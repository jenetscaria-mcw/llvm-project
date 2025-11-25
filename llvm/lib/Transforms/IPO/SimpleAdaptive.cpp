#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <vector>

using namespace llvm;

namespace llvm
{
    // Configuration constants
    static constexpr int WARMUP_RUNS = 10;           // Number of warmup runs per version
    static constexpr int SAMPLE_RATE = 5;            // Sample 1 in N calls during profiling
    static constexpr int PROFILING_THRESHOLD = 1200; // Total calls needed for profiling (after warmup)

    struct FunctionMetadata
    {
        Function *original;
        Function *versions[6]; // v0 to v5
        Function *wrapper;
        Function *dispatcher; // NEW: Thin dispatcher function
        uint32_t functionId;
        FunctionType *signature;

        // Per-function static globals for adaptive runtime tracking
        GlobalVariable *callCounter;    // total call count
        GlobalVariable *warmupCounter;  // warmup call count
        GlobalVariable *profileCounter; // profiled call count (sampled)
        GlobalVariable *cpuCycles[6];   // accumulated cpu cycles per version
        GlobalVariable *runCount[6];    // actual profiled runs per version
        GlobalVariable *bestVersion;    // best version index after profiling
        GlobalVariable *currentPhase;   // 0=warmup, 1=profiling, 2=optimal
        GlobalVariable *currentVersion; // current version being tested
        GlobalVariable *sampleCounter;  // counter for sampling
        GlobalVariable *functionPtr;    // NEW: Function pointer for direct dispatch
    };

    class SimpleAdaptivePassImpl
    {
    private:
        std::map<Function *, FunctionMetadata> functionMap;

        Function *createVersion(Function *F, int versionId, Module &M);
        void createWrapperStaticVars(FunctionMetadata &metadata, Module &M);
        void processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M);
        Function *createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M);
        Function *createThinDispatcher(FunctionMetadata &metadata, Module &M);
        void createInitializationFunction(Module &M);

    public:
        SimpleAdaptivePassImpl() {}
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
    };

    PreservedAnalyses SimpleAdaptivePass::run(Module &M, ModuleAnalysisManager &AM)
    {
        SimpleAdaptivePassImpl impl;
        return impl.run(M, AM);
    }

    PreservedAnalyses SimpleAdaptivePassImpl::run(Module &M, ModuleAnalysisManager &)
    {
        functionMap.clear();

        // Collect adaptive functions
        std::vector<Function *> adaptiveFunctions;
        for (Function &F : M)
        {
            if (!F.isDeclaration() && F.hasFnAttribute("adaptive"))
            {
                adaptiveFunctions.push_back(&F);
            }
        }
        if (adaptiveFunctions.empty())
        {
            return PreservedAnalyses::all();
        }

        uint32_t funcId = 0;
        for (Function *F : adaptiveFunctions)
        {
            processFunctionWithDirectDispatch(F, funcId++, M);
        }

        createInitializationFunction(M);

        return PreservedAnalyses::none();
    }

    void SimpleAdaptivePassImpl::createWrapperStaticVars(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        const std::string baseName = metadata.original->getName().str();

        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        Type *funcPtrTy = metadata.original->getFunctionType()->getPointerTo();

        const std::string counterName = "__static_counter_" + baseName;
        const std::string warmupName = "__static_warmup_" + baseName;
        const std::string profileName = "__static_profile_" + baseName;
        const std::string bestName = "__static_best_" + baseName;
        const std::string phaseName = "__static_phase_" + baseName;
        const std::string currentVersionName = "__static_current_version_" + baseName;
        const std::string sampleCounterName = "__static_sample_counter_" + baseName;
        const std::string funcPtrName = "__static_funcptr_" + baseName;
        const std::string cyclesBase = "__static_cycles_" + baseName + "_v";
        const std::string runsBase = "__static_runs_" + baseName + "_v";

        // Create variables with atomic alignment
        metadata.callCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), counterName);
        metadata.callCounter->setAlignment(Align(4));

        metadata.warmupCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), warmupName);
        metadata.warmupCounter->setAlignment(Align(4));

        metadata.profileCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), profileName);
        metadata.profileCounter->setAlignment(Align(4));

        metadata.sampleCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), sampleCounterName);
        metadata.sampleCounter->setAlignment(Align(4));

        auto *ptrToFuncTy = PointerType::get(funcPtrTy, 0); // i.e. T*
        auto *nullFuncPtr = ConstantPointerNull::get(ptrToFuncTy);

        metadata.functionPtr = new GlobalVariable(
            M,
            ptrToFuncTy, // The type of the global var
            false,
            GlobalValue::InternalLinkage,
            nullFuncPtr, // initializer
            funcPtrName);

        metadata.functionPtr->setAlignment(Align(8));

        const std::array<std::string, 6> versionStrs = {
            "0", "1", "2", "3", "4", "5"};

        for (int v = 0; v < 6; v++)
        {
            metadata.cpuCycles[v] = new GlobalVariable(
                M, i64Ty, false, GlobalValue::InternalLinkage,
                ConstantInt::get(i64Ty, 0), cyclesBase + versionStrs[v]);
            metadata.cpuCycles[v]->setAlignment(Align(8));

            metadata.runCount[v] = new GlobalVariable(
                M, i32Ty, false, GlobalValue::InternalLinkage,
                ConstantInt::get(i32Ty, 0), runsBase + versionStrs[v]);
            metadata.runCount[v]->setAlignment(Align(4));
        }

        metadata.bestVersion = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, -1), bestName);
        metadata.bestVersion->setAlignment(Align(4));

        metadata.currentPhase = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), phaseName); // 0=warmup, 1=profiling, 2=optimal
        metadata.currentPhase->setAlignment(Align(4));

        metadata.currentVersion = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), currentVersionName);
        metadata.currentVersion->setAlignment(Align(4));
    }

    Function *SimpleAdaptivePassImpl::createVersion(Function *F, int versionId, Module &M)
    {
        ValueToValueMapTy VMap;
        Function *NewF = CloneFunction(F, VMap);
        NewF->setName(F->getName() + "_v" + std::to_string(versionId));
        NewF->setLinkage(GlobalValue::InternalLinkage);

        LLVMContext &Ctx = M.getContext();

        switch (versionId)
        {
        case 0: // BASELINE - No optimizations
            NewF->addFnAttr(Attribute::OptimizeNone);
            NewF->addFnAttr(Attribute::NoInline);
            break;

        case 1: // VECTORIZED - Force vectorization
            NewF->addFnAttr("target-features", "+avx2,+fma");
            // NewF->addFnAttr("target-cpu", "haswell");
            // Add loop metadata for vectorization
            for (BasicBlock &BB : *NewF)
            {
                for (Instruction &I : BB)
                {
                    if (auto *Loop = dyn_cast<BranchInst>(&I))
                    {
                        // Add vectorization metadata to loops
                        MDNode *VecMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop.vectorize.enable"),
                                                          ConstantAsMetadata::get(ConstantInt::get(Type::getInt1Ty(Ctx), 1))});
                        MDNode *WidthMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop.vectorize.width"),
                                                            ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(Ctx), 8))});
                        MDNode *LoopMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop"), VecMD, WidthMD});
                        I.setMetadata("llvm.loop", LoopMD);
                    }
                }
            }
            break;

        case 2: // LOOP OPTIMIZED - Aggressive unrolling
            // NewF->addFnAttr("target-cpu", "native");
            // Add unroll metadata to all loops
            for (Function::iterator BB = NewF->begin(); BB != NewF->end(); ++BB)
            {
                if (BranchInst *BI = dyn_cast<BranchInst>(BB->getTerminator()))
                {
                    MDNode *UnrollMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop.unroll.enable"),
                                                         ConstantAsMetadata::get(ConstantInt::get(Type::getInt1Ty(Ctx), 1))});
                    MDNode *UnrollCount = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop.unroll.count"),
                                                            ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(Ctx), 8))});
                    MDNode *LoopMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop"), UnrollMD, UnrollCount});
                    BI->setMetadata("llvm.loop", LoopMD);
                }
            }
            break;

        case 3: // FAST MATH - Aggressive FP optimizations
            NewF->addFnAttr("unsafe-fp-math", "true");
            NewF->addFnAttr("no-nans-fp-math", "true");
            NewF->addFnAttr("no-infs-fp-math", "true");
            NewF->addFnAttr("no-signed-zeros-fp-math", "true");
            // Set fast math flags on all FP instructions
            for (BasicBlock &BB : *NewF)
            {
                for (Instruction &I : BB)
                {
                    if (I.getType()->isFloatingPointTy())
                    {
                        FastMathFlags FMF;
                        FMF.setFast(true);
                        I.setFastMathFlags(FMF);
                    }
                }
            }
            break;

        case 4: // SIZE OPTIMIZED - Prefer smaller code
            NewF->addFnAttr(Attribute::OptimizeForSize);
            NewF->addFnAttr(Attribute::MinSize);
            // Disable unrolling and inlining
            for (Function::iterator BB = NewF->begin(); BB != NewF->end(); ++BB)
            {
                if (BranchInst *BI = dyn_cast<BranchInst>(BB->getTerminator()))
                {
                    MDNode *UnrollMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop.unroll.disable"),
                                                         ConstantAsMetadata::get(ConstantInt::get(Type::getInt1Ty(Ctx), 1))});
                    MDNode *LoopMD = MDNode::get(Ctx, {MDString::get(Ctx, "llvm.loop"), UnrollMD});
                    BI->setMetadata("llvm.loop", LoopMD);
                }
            }
            break;

        case 5: // AGGRESSIVE INLINE
            NewF->addFnAttr(Attribute::AlwaysInline);
            NewF->addFnAttr(Attribute::NoUnwind);
            NewF->addFnAttr(Attribute::NoRecurse);
            // Inline all call instructions in this function
            for (BasicBlock &BB : *NewF)
            {
                for (Instruction &I : BB)
                {
                    if (CallInst *CI = dyn_cast<CallInst>(&I))
                    {
                        if (Function *Callee = CI->getCalledFunction())
                        {
                            if (!Callee->isDeclaration())
                            {
                                Callee->addFnAttr(Attribute::AlwaysInline);
                            }
                        }
                    }
                }
            }
            break;
        }

        return NewF;
    }

    Function *SimpleAdaptivePassImpl::createThinDispatcher(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;

        // Create dispatcher function with same signature as original
        Function *Dispatcher = Function::Create(
            Orig->getFunctionType(),
            Orig->getLinkage(),
            Orig->getName() + "_dispatcher",
            &M);
        Dispatcher->copyAttributesFrom(Orig);

        // Mark as always inline for minimal overhead
        Dispatcher->addFnAttr(Attribute::AlwaysInline);
        // Dispatcher->setLinkage(GlobalValue::InternalLinkage);

        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
        IRBuilder<> Builder(Entry);

        // Load the function pointer
        LoadInst *FuncPtrLoad = Builder.CreateLoad(
            metadata.original->getFunctionType()->getPointerTo(),
            metadata.functionPtr);
        FuncPtrLoad->setAtomic(AtomicOrdering::Acquire);
        FuncPtrLoad->setAlignment(Align(8));

        // Collect arguments
        std::vector<Value *> Args;
        for (auto &Arg : Dispatcher->args())
            Args.push_back(&Arg);

        // Call through the function pointer
        if (Orig->getReturnType()->isVoidTy())
        {
            Builder.CreateCall(Orig->getFunctionType(), FuncPtrLoad, Args);
            Builder.CreateRetVoid();
        }
        else
        {
            Value *Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtrLoad, Args);
            Builder.CreateRet(Result);
        }

        return Dispatcher;
    }

    void SimpleAdaptivePassImpl::processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M)
    {
        FunctionMetadata metadata;
        metadata.original = F;
        metadata.functionId = funcId;
        metadata.signature = F->getFunctionType();
        auto funcName = F->getName().str();

        // Create versions
        for (int i = 0; i < 6; i++)
        {
            metadata.versions[i] = createVersion(F, i, M);
        }

        // Create static vars for wrapper
        createWrapperStaticVars(metadata, M);

        // Create wrapper (profiling logic)
        metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

        // Create thin dispatcher
        metadata.dispatcher = createThinDispatcher(metadata, M);

        // Initialize function pointer to point to wrapper
        metadata.functionPtr->setInitializer(
            ConstantExpr::getPointerCast(metadata.wrapper,
                                         metadata.original->getFunctionType()->getPointerTo()));

        // Replace all uses with the dispatcher
        F->replaceAllUsesWith(metadata.dispatcher);
        metadata.dispatcher->takeName(F);

        F->setName(funcName + "_original");
        F->setLinkage(GlobalValue::InternalLinkage);

        errs() << F->getName().str() << "\n";
        errs() << metadata.wrapper->getName().str() << "\n";
        errs() << metadata.dispatcher->getName().str() << "\n";
        errs() << metadata.dispatcher->getName().str() << "\n";

        functionMap[F] = metadata;
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;

        // Create wrapper function (internal, not exposed)
        Function *Wrapper = Function::Create(
            Orig->getFunctionType(),
            GlobalValue::InternalLinkage, // Internal linkage
            Orig->getName() + "_wrapper",
            &M);
        Wrapper->copyAttributesFrom(Orig);

        // Set up basic blocks
        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
        BasicBlock *WarmupPhaseBB = BasicBlock::Create(Ctx, "warmup_phase", Wrapper);
        BasicBlock *ProfilingPhaseBB = BasicBlock::Create(Ctx, "profiling_phase", Wrapper);
        BasicBlock *OptimalPhaseBB = BasicBlock::Create(Ctx, "optimal_phase", Wrapper);
        BasicBlock *UpdateStatsBB = BasicBlock::Create(Ctx, "update_stats", Wrapper);
        BasicBlock *TransitionToProfilingBB = BasicBlock::Create(Ctx, "transition_to_profiling", Wrapper);
        BasicBlock *TransitionToOptimalBB = BasicBlock::Create(Ctx, "transition_to_optimal", Wrapper);

        IRBuilder<> Builder(Entry);

        // Types and constants
        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        // Constants
        ConstantInt *zero32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 0));
        ConstantInt *one32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 1));
        ConstantInt *two32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 2));
        ConstantInt *warmupThreshold = cast<ConstantInt>(ConstantInt::get(i32Ty, WARMUP_RUNS * 6)); // Total warmup runs
        ConstantInt *profilingThreshold = cast<ConstantInt>(ConstantInt::get(i32Ty, PROFILING_THRESHOLD));
        ConstantInt *sampleRate = cast<ConstantInt>(ConstantInt::get(i32Ty, SAMPLE_RATE));

        // Get printf for debugging
        FunctionType *PrintfType = FunctionType::get(i32Ty,
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName().str());

        // Entry: Check current phase
        LoadInst *PhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
        PhaseLoad->setAtomic(AtomicOrdering::Acquire);
        PhaseLoad->setAlignment(Align(4));

        // Branch based on phase
        SwitchInst *PhaseSwitch = Builder.CreateSwitch(PhaseLoad, OptimalPhaseBB, 3);
        PhaseSwitch->addCase(zero32, WarmupPhaseBB);
        PhaseSwitch->addCase(one32, ProfilingPhaseBB);
        PhaseSwitch->addCase(two32, OptimalPhaseBB);

        const int NUM_VERSIONS = 6;

        // Warmpup phase
        Builder.SetInsertPoint(WarmupPhaseBB);
        {
            // Increment warmup counter
            Value *WarmupCount = Builder.CreateAtomicRMW(
                AtomicRMWInst::Add, metadata.warmupCounter, one32,
                MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

            // Calculate which version to run (round-robin during warmup)
            Value *VersionToRun = Builder.CreateURem(WarmupCount, ConstantInt::get(i32Ty, NUM_VERSIONS));

            // Store current version
            StoreInst *VersionStore = Builder.CreateStore(VersionToRun, metadata.currentVersion);
            VersionStore->setAtomic(AtomicOrdering::Release);
            VersionStore->setAlignment(Align(4));

            // Check if warmup is complete
            Value *IsWarmupComplete = Builder.CreateICmpUGE(WarmupCount, warmupThreshold);
            Builder.CreateCondBr(IsWarmupComplete, TransitionToProfilingBB, UpdateStatsBB);
        }

        // Transition to Profiling
        Builder.SetInsertPoint(TransitionToProfilingBB);
        {
            // RACE CONDITION FIX: Use atomic CAS to ensure only ONE thread performs transition
            BasicBlock *DoWarmupTransitionBB = BasicBlock::Create(Ctx, "do_warmup_transition", Wrapper);
            BasicBlock *AlreadyInProfilingBB = BasicBlock::Create(Ctx, "already_in_profiling", Wrapper);
            
            // Attempt atomic compare-exchange: if currentPhase == 0, set it to 1
            AtomicCmpXchgInst *CASWarmup = Builder.CreateAtomicCmpXchg(
                metadata.currentPhase, zero32, one32,
                MaybeAlign(4), 
                AtomicOrdering::SequentiallyConsistent,
                AtomicOrdering::SequentiallyConsistent);
            CASWarmup->setWeak(false);  // Strong CAS
            
            // Extract the success flag from the result
            Value *CASWarmupResult = Builder.CreateExtractValue(CASWarmup, 1);
            Builder.CreateCondBr(CASWarmupResult, DoWarmupTransitionBB, AlreadyInProfilingBB);
            
            // If CAS failed, another thread already transitioned - go to profiling
            Builder.SetInsertPoint(AlreadyInProfilingBB);
            Builder.CreateBr(ProfilingPhaseBB);
            
            // Only the winning thread performs the transition
            Builder.SetInsertPoint(DoWarmupTransitionBB);
            
            // Print transition message
            Value *TransMsg = Builder.CreateGlobalStringPtr(
                "[WARMUP COMPLETE] %s - Transitioning to profiling phase after %d warmup runs\n");
            Builder.CreateCall(Printf, {TransMsg, FuncName, warmupThreshold});

            // Phase already updated by CAS above - no need to update again
            
            // Reset counters for profiling phase  
            StoreInst *ResetProfile = Builder.CreateStore(zero32, metadata.profileCounter);
            ResetProfile->setAtomic(AtomicOrdering::Release);
            ResetProfile->setAlignment(Align(4));

            Builder.CreateBr(ProfilingPhaseBB);
        }

        // Profiling Phase with Sampling
        Builder.SetInsertPoint(ProfilingPhaseBB);
        {
            // Increment sample counter for sampling decision
            Value *SampleCount = Builder.CreateAtomicRMW(
                AtomicRMWInst::Add, metadata.sampleCounter, one32,
                MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

            // Check if this call should be sampled (every Nth call)
            Value *ShouldSample = Builder.CreateICmpEQ(
                Builder.CreateURem(SampleCount, sampleRate), zero32);

            // Create blocks for sampling decision
            BasicBlock *DoSampleBB = BasicBlock::Create(Ctx, "do_sample", Wrapper);
            BasicBlock *SkipSampleBB = BasicBlock::Create(Ctx, "skip_sample", Wrapper);
            Builder.CreateCondBr(ShouldSample, DoSampleBB, SkipSampleBB);

            // Do Sample Block - measure performance
            Builder.SetInsertPoint(DoSampleBB);
            {
                // Try to find a version that still needs profiling (< 200 runs)
                // We check versions in order and atomically claim a run
                
                BasicBlock *CheckVersionBBs[NUM_VERSIONS];
                for (int v = 0; v < NUM_VERSIONS; v++)
                {
                    CheckVersionBBs[v] = BasicBlock::Create(Ctx, "check_v" + std::to_string(v), Wrapper);
                }
                
                Builder.CreateBr(CheckVersionBBs[0]);
                
                // Try each version sequentially
                for (int v = 0; v < NUM_VERSIONS; v++)
                {
                    Builder.SetInsertPoint(CheckVersionBBs[v]);
                    
                    // Atomically try to claim a run for this version
                    Value *OldRunCount = Builder.CreateAtomicRMW(
                        AtomicRMWInst::Add, metadata.runCount[v], one32,
                        MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);
                    
                    // Check if this version still needs runs (old count < 200)
                    Value *NeedsRun = Builder.CreateICmpULT(OldRunCount, ConstantInt::get(i32Ty, 200));
                    
                    BasicBlock *UseVersionBB = BasicBlock::Create(Ctx, "use_v" + std::to_string(v), Wrapper);
                    BasicBlock *TryNextBB = (v < NUM_VERSIONS - 1) ? 
                        CheckVersionBBs[v + 1] : TransitionToOptimalBB;
                    
                    Builder.CreateCondBr(NeedsRun, UseVersionBB, TryNextBB);
                    
                    // Use this version - store it and profile
                    Builder.SetInsertPoint(UseVersionBB);
                    StoreInst *VersionStore = Builder.CreateStore(ConstantInt::get(i32Ty, v), metadata.currentVersion);
                    VersionStore->setAtomic(AtomicOrdering::Release);
                    VersionStore->setAlignment(Align(4));
                    Builder.CreateBr(UpdateStatsBB);
                    
                    // Didn't use this version - decrement back
                    if (v < NUM_VERSIONS - 1)
                    {
                        Builder.SetInsertPoint(TryNextBB);
                        Builder.CreateAtomicRMW(
                            AtomicRMWInst::Sub, metadata.runCount[v], one32,
                            MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);
                    }
                    else
                    {
                        // Last version is also full - decrement and transition
                        Builder.SetInsertPoint(TransitionToOptimalBB);
                        Builder.CreateAtomicRMW(
                            AtomicRMWInst::Sub, metadata.runCount[v], one32,
                            MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);
                    }
                }
            }

            // Skip Sample Block - run current best or default version without measurement
            Builder.SetInsertPoint(SkipSampleBB);
            {
                // Load current version (last profiled version or default)
                LoadInst *CurrentVersionLoad = Builder.CreateLoad(i32Ty, metadata.currentVersion);
                CurrentVersionLoad->setAtomic(AtomicOrdering::Acquire);
                CurrentVersionLoad->setAlignment(Align(4));

                // Dispatch to current version without timing
                std::vector<Value *> VersionFuncs;
                for (int i = 0; i < NUM_VERSIONS; i++)
                {
                    VersionFuncs.push_back(Builder.CreatePointerCast(
                        metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
                }

                Value *FuncPtr = VersionFuncs[0];
                for (int i = 1; i < NUM_VERSIONS; i++)
                {
                    Value *IsVersionI = Builder.CreateICmpEQ(CurrentVersionLoad, ConstantInt::get(i32Ty, i));
                    FuncPtr = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtr);
                }

                std::vector<Value *> Args;
                for (auto &Arg : Wrapper->args())
                    Args.push_back(&Arg);

                if (Orig->getReturnType()->isVoidTy())
                {
                    Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                    Builder.CreateRetVoid();
                }
                else
                {
                    Value *Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                    Builder.CreateRet(Result);
                }
            }
        }

        // Update Stats Block (for sampled profiling calls and warmup)
        Builder.SetInsertPoint(UpdateStatsBB);
        {
            // Load the current version
            LoadInst *CurrentVersionLoad = Builder.CreateLoad(i32Ty, metadata.currentVersion);
            CurrentVersionLoad->setAtomic(AtomicOrdering::Acquire);
            CurrentVersionLoad->setAlignment(Align(4));

            // Load current phase to determine if we should measure
            LoadInst *CurrentPhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
            CurrentPhaseLoad->setAtomic(AtomicOrdering::Acquire);
            CurrentPhaseLoad->setAlignment(Align(4));

            // Only measure during profiling phase (not warmup)
            Value *IsProfilingPhase = Builder.CreateICmpEQ(CurrentPhaseLoad, one32);

            // Create dispatch logic with optional timing
            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(
                    metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            // Get RDTSC inline assembly for x86-64
            /*
            FunctionType *RdtscType = FunctionType::get(i64Ty, {}, false);
            InlineAsm *Rdtsc = InlineAsm::get(RdtscType,
                                             "rdtsc; shl $$32, %rdx; or %rdx, %rax",
                                             "=A,~{rdx}", false);
            */
            Function *ReadCycleCounter = Intrinsic::getDeclaration(&M, Intrinsic::readcyclecounter);

            // Conditional timing based on phase
            BasicBlock *MeasureBB = BasicBlock::Create(Ctx, "measure", Wrapper);
            BasicBlock *NoMeasureBB = BasicBlock::Create(Ctx, "no_measure", Wrapper);
            BasicBlock *UpdateDispatchBB = BasicBlock::Create(Ctx, "update_dispatch", Wrapper);
            BasicBlock *ContinueBB = BasicBlock::Create(Ctx, "continue", Wrapper);

            std::vector<Value *> WrapperArgs;
            for (auto &Arg : Wrapper->args())
                WrapperArgs.push_back(&Arg);

            Value *MeasuredResult = nullptr;
            Value *NoMeasureResult = nullptr;
            std::vector<BasicBlock *> UpdateBlocks;
            UpdateBlocks.reserve(NUM_VERSIONS + 1);

            Builder.CreateCondBr(IsProfilingPhase, MeasureBB, NoMeasureBB);

            // Measure block - with timing
            Builder.SetInsertPoint(MeasureBB);
            Value *StartCycles = Builder.CreateCall(ReadCycleCounter);

            Value *FuncPtrMeasure = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsVersionI = Builder.CreateICmpEQ(CurrentVersionLoad, ConstantInt::get(i32Ty, i));
                FuncPtrMeasure = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtrMeasure);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtrMeasure, WrapperArgs);
            }
            else
            {
                MeasuredResult = Builder.CreateCall(Orig->getFunctionType(), FuncPtrMeasure, WrapperArgs);
            }

            Value *EndCycles = Builder.CreateCall(ReadCycleCounter);
            Value *ElapsedCycles = Builder.CreateSub(EndCycles, StartCycles);

            Builder.CreateBr(UpdateDispatchBB);

            // Update statistics dispatch
            Builder.SetInsertPoint(UpdateDispatchBB);
            BasicBlock *DefaultUpdateBB = BasicBlock::Create(Ctx, "update_default", Wrapper);
            SwitchInst *VersionSwitch = Builder.CreateSwitch(CurrentVersionLoad, DefaultUpdateBB, NUM_VERSIONS);

            Builder.SetInsertPoint(DefaultUpdateBB);
            UpdateBlocks.push_back(DefaultUpdateBB);
            Builder.CreateBr(ContinueBB);

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                BasicBlock *UpdateVersionBB = BasicBlock::Create(Ctx, "update_v" + std::to_string(i), Wrapper);
                VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), UpdateVersionBB);
                Builder.SetInsertPoint(UpdateVersionBB);
                // Atomic add for cycles (runCount already incremented in DoSampleBB)
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.cpuCycles[i],
                                        ElapsedCycles, MaybeAlign(8), AtomicOrdering::SequentiallyConsistent);
                Builder.CreateBr(ContinueBB);
                UpdateBlocks.push_back(UpdateVersionBB);
            }

            // No measure block - warmup execution without timing
            Builder.SetInsertPoint(NoMeasureBB);
            Value *FuncPtrNoMeasure = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsVersionI = Builder.CreateICmpEQ(CurrentVersionLoad, ConstantInt::get(i32Ty, i));
                FuncPtrNoMeasure = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtrNoMeasure);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtrNoMeasure, WrapperArgs);
            }
            else
            {
                NoMeasureResult = Builder.CreateCall(Orig->getFunctionType(), FuncPtrNoMeasure, WrapperArgs);
            }
            Builder.CreateBr(ContinueBB);

            // Continue block
            Builder.SetInsertPoint(ContinueBB);
            if (!Orig->getReturnType()->isVoidTy())
            {
                PHINode *ResultPhi = Builder.CreatePHI(Orig->getReturnType(), UpdateBlocks.size() + 1);
                for (BasicBlock *Pred : UpdateBlocks)
                {
                    ResultPhi->addIncoming(MeasuredResult, Pred);
                }
                ResultPhi->addIncoming(NoMeasureResult, NoMeasureBB);
                Builder.CreateRet(ResultPhi);
            }
            else
            {
                Builder.CreateRetVoid();
            }
        }

        // Key change: Transition to Optimal
        Builder.SetInsertPoint(TransitionToOptimalBB);
        {
            // RACE CONDITION FIX: Use atomic CAS to ensure only ONE thread performs transition
            BasicBlock *DoTransitionBB = BasicBlock::Create(Ctx, "do_transition", Wrapper);
            BasicBlock *AlreadyTransitionedBB = BasicBlock::Create(Ctx, "already_transitioned", Wrapper);
            
            // Attempt atomic compare-exchange: if currentPhase == 1, set it to 2
            AtomicCmpXchgInst *CAS = Builder.CreateAtomicCmpXchg(
                metadata.currentPhase, one32, two32,
                MaybeAlign(4), 
                AtomicOrdering::SequentiallyConsistent,
                AtomicOrdering::SequentiallyConsistent);
            CAS->setWeak(false);  // Strong CAS
            
            // Extract the success flag from the result
            Value *CASResult = Builder.CreateExtractValue(CAS, 1);
            Builder.CreateCondBr(CASResult, DoTransitionBB, AlreadyTransitionedBB);
            
            // If CAS failed, another thread already transitioned - go to optimal
            Builder.SetInsertPoint(AlreadyTransitionedBB);
            Builder.CreateBr(OptimalPhaseBB);
            
            // Only the winning thread performs the transition
            Builder.SetInsertPoint(DoTransitionBB);
            
            // [Calculate best version as before...]
            // Load all version data
            std::vector<Value *> CurrentCycles;
            std::vector<Value *> CurrentRuns;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                LoadInst *CyclesLoad = Builder.CreateLoad(i64Ty, metadata.cpuCycles[i]);
                CyclesLoad->setAtomic(AtomicOrdering::Acquire);
                CyclesLoad->setAlignment(Align(8));
                CurrentCycles.push_back(CyclesLoad);

                LoadInst *RunsLoad = Builder.CreateLoad(i32Ty, metadata.runCount[i]);
                RunsLoad->setAtomic(AtomicOrdering::Acquire);
                RunsLoad->setAlignment(Align(4));
                CurrentRuns.push_back(RunsLoad);
            }

            // Find best version
            Value *BestVersion = zero32;
            Value *BestAvg = nullptr;

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                Value *HasRuns = Builder.CreateICmpUGT(CurrentRuns[i], zero32);
                Value *CyclesFloat = Builder.CreateUIToFP(CurrentCycles[i], Builder.getDoubleTy());
                Value *RunsFloat = Builder.CreateUIToFP(CurrentRuns[i], Builder.getDoubleTy());
                Value *SafeRunsFloat = Builder.CreateSelect(
                    HasRuns, RunsFloat,
                    ConstantFP::get(Builder.getDoubleTy(), 1.0));
                Value *AvgFloat = Builder.CreateFDiv(CyclesFloat, SafeRunsFloat);
                Value *MaxValFloat = ConstantFP::get(Builder.getDoubleTy(),
                                                     std::numeric_limits<double>::max());
                Value *FinalAvg = Builder.CreateSelect(HasRuns, AvgFloat, MaxValFloat);

                if (i == 0)
                {
                    BestAvg = FinalAvg;
                    BestVersion = ConstantInt::get(i32Ty, 0);
                }
                else
                {
                    Value *IsBetter = Builder.CreateFCmpOLT(FinalAvg, BestAvg);
                    BestVersion = Builder.CreateSelect(IsBetter,
                                                       ConstantInt::get(i32Ty, i), BestVersion);
                    BestAvg = Builder.CreateSelect(IsBetter, FinalAvg, BestAvg);
                }

                // Print stats
                Value *AvgForDisplay = Builder.CreateFPToUI(FinalAvg, i64Ty);
                Value *StatsMsg = Builder.CreateGlobalStringPtr(
                    "[STATS] %s - V%d: total_cycles=%llu, runs=%u, avg=%llu\n");
                Builder.CreateCall(Printf, {StatsMsg, FuncName,
                                            ConstantInt::get(i32Ty, i),
                                            CurrentCycles[i], CurrentRuns[i], AvgForDisplay});
            }

            // Store best version
            StoreInst *BestStore = Builder.CreateStore(BestVersion, metadata.bestVersion);
            BestStore->setAtomic(AtomicOrdering::Release);
            BestStore->setAlignment(Align(4));

            // Phase already updated by CAS above - no need to update again

            // Create blocks for each version
            BasicBlock *PatchDoneBB = BasicBlock::Create(Ctx, "patch_done", Wrapper);
            BasicBlock *DefaultPatchBB = BasicBlock::Create(Ctx, "default_patch", Wrapper);

            SwitchInst *PatchSwitch = Builder.CreateSwitch(BestVersion, DefaultPatchBB, 6);

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                BasicBlock *PatchBB = BasicBlock::Create(Ctx,
                                                         "patch_v" + std::to_string(i), Wrapper);
                PatchSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), PatchBB);

                Builder.SetInsertPoint(PatchBB);

                // Patch the function pointer to point directly to the best version
                Value *VersionPtr = Builder.CreatePointerCast(
                    metadata.versions[i],
                    metadata.original->getFunctionType()->getPointerTo());

                StoreInst *PatchStore = Builder.CreateStore(VersionPtr, metadata.functionPtr);
                PatchStore->setAtomic(AtomicOrdering::Release);
                PatchStore->setAlignment(Align(8));

                // Print patch message
                Value *PatchMsg = Builder.CreateGlobalStringPtr(
                    "[PATCH] %s - Function pointer patched to V%d, wrapper bypassed!\n");
                Builder.CreateCall(Printf, {PatchMsg, FuncName, ConstantInt::get(i32Ty, i)});

                Builder.CreateBr(PatchDoneBB);
            }

            Builder.SetInsertPoint(DefaultPatchBB);
            Builder.CreateBr(PatchDoneBB);

            Builder.SetInsertPoint(PatchDoneBB);
            Builder.CreateBr(OptimalPhaseBB);
        }

        // Optimal Phase: Call best version (only used once before patching)
        Builder.SetInsertPoint(OptimalPhaseBB);
        {
            LoadInst *BestLoad = Builder.CreateLoad(i32Ty, metadata.bestVersion);
            BestLoad->setAtomic(AtomicOrdering::Acquire);
            BestLoad->setAlignment(Align(4));

            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(
                    metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsBestVersionI = Builder.CreateICmpEQ(BestLoad,
                                                             ConstantInt::get(i32Ty, i));
                FuncPtr = Builder.CreateSelect(IsBestVersionI, VersionFuncs[i], FuncPtr);
            }

            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                Builder.CreateRetVoid();
            }
            else
            {
                Value *Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                Builder.CreateRet(Result);
            }
        }

        return Wrapper;
    }

    void SimpleAdaptivePassImpl::createInitializationFunction(Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        if (M.getFunction("__adaptive_init"))
            return;

        FunctionType *InitType = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
        Function *InitFunc = Function::Create(InitType, GlobalValue::InternalLinkage,
                                              "__adaptive_init", &M);
        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", InitFunc);
        IRBuilder<> Builder(Entry);

        // Print initialization message
        FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        Value *InitMsg = Builder.CreateGlobalStringPtr(
            "[INIT] Adaptive dispatcher with DIRECT PATCHING initialized:\n"
            "  - Warmup runs: %d per version (%d total)\n"
            "  - Sample rate: 1 in %d calls\n"
            "  - Profiling threshold: %d samples\n"
            "  - Production mode: DIRECT CALLS (wrapper bypassed)\n");
        Builder.CreateCall(Printf, {InitMsg,
                                    ConstantInt::get(Type::getInt32Ty(Ctx), WARMUP_RUNS),
                                    ConstantInt::get(Type::getInt32Ty(Ctx), WARMUP_RUNS * 6),
                                    ConstantInt::get(Type::getInt32Ty(Ctx), SAMPLE_RATE),
                                    ConstantInt::get(Type::getInt32Ty(Ctx), PROFILING_THRESHOLD)});
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm