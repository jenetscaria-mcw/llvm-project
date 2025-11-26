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
#include <string>

using namespace llvm;

namespace llvm
{
    // Configuration structure for function groups
    struct FunctionGroupConfig {
        int warmupRuns;
        int sampleRate;
        int profilingThreshold;
        std::string description;
    };

    // Function group configurations - UPDATED with your requirements
    static std::map<std::string, FunctionGroupConfig> FUNCTION_GROUPS = {
        {"HIGH_FREQUENCY", {10, 5, 50000, "Functions with millions of calls"}},
        {"MEDIUM_FREQUENCY", {10, 2, 1200, "Functions with 10k-1M calls"}},
        {"LOW_FREQUENCY", {10, 2, 500, "Functions with less than 10k calls"}}
    };

    // Default configuration
    static const FunctionGroupConfig DEFAULT_CONFIG = {10, 2, 1200, "Default configuration"};

    struct FunctionMetadata
    {
        Function *original;
        Function *versions[6]; // v0 to v5
        Function *wrapper;
        Function *dispatcher; // NEW: Thin dispatcher function
        uint32_t functionId;
        FunctionType *signature;
        std::string groupName; // NEW: Store which group this function belongs to

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
        
        // NEW: Helper functions for group configuration
        std::string determineFunctionGroup(Function *F);
        const FunctionGroupConfig& getGroupConfig(const std::string& groupName);
        void printFunctionGroupInfo(Function *F, const std::string& groupName, 
                                   const FunctionGroupConfig& config, raw_ostream &OS);

    public:
        SimpleAdaptivePassImpl() {}
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
    };

    PreservedAnalyses SimpleAdaptivePass::run(Module &M, ModuleAnalysisManager &AM)
    {
        SimpleAdaptivePassImpl impl;
        return impl.run(M, AM);
    }

    // NEW: Determine which group a function belongs to based on manual assignments
    std::string SimpleAdaptivePassImpl::determineFunctionGroup(Function *F)
    {
        std::string funcName = F->getName().str();
        
        // MANUAL GROUP ASSIGNMENTS BASED ON FUNCTION NAMES
        // Low frequency group (call count < 10k)
        if (funcName.find("_ZN12_GLOBAL__N_115tinyBLAS_Q0_AVXI10block_q4_010block_q8_0fE7gemm4xNILi4EEEvllll") != std::string::npos) {
            return "LOW_FREQUENCY";
        }
        
        // Medium frequency group (call count 10k - 1M)  
        if (funcName.find("_ZN12_GLOBAL__N_18tinyBLASILi16EDv16_fS1_tffE6mnpackEllll") != std::string::npos ||
            // funcName.find("gemm<5>") != std::string::npos ||
            funcName.find("quantize_row_q8_0") != std::string::npos ||
            funcName.find("ggml_compute_forward_dup") != std::string::npos ||
            funcName.find("ggml_compute_forward_rope_f32") != std::string::npos) {
            return "MEDIUM_FREQUENCY";
        }
        
        // High frequency group (call count > 1M)
        if (funcName.find("ggml_fp32_to_fp16_row") != std::string::npos ||
            funcName.find("ggml_vec_dot_q4_0_q8_0") != std::string::npos ||
            funcName.find("ggml_vec_dot_q6_K_q8_K") != std::string::npos ||
            funcName.find("ggml_vec_dot_f16") != std::string::npos) {
            return "HIGH_FREQUENCY";
        }
        
        // Check for explicit group attribute (override manual assignments)
        if (F->hasFnAttribute("adaptive-group")) {
            Attribute groupAttr = F->getFnAttribute("adaptive-group");
            std::string groupName = groupAttr.getValueAsString().str();
            if (FUNCTION_GROUPS.find(groupName) != FUNCTION_GROUPS.end()) {
                return groupName;
            }
        }
        
        // Default to medium frequency for any unclassified adaptive functions
        return "MEDIUM_FREQUENCY";
    }

    // NEW: Get configuration for a group
    const FunctionGroupConfig& SimpleAdaptivePassImpl::getGroupConfig(const std::string& groupName)
    {
        auto it = FUNCTION_GROUPS.find(groupName);
        if (it != FUNCTION_GROUPS.end()) {
            return it->second;
        }
        return DEFAULT_CONFIG;
    }

    // NEW: Print function group information
    void SimpleAdaptivePassImpl::printFunctionGroupInfo(Function *F, const std::string& groupName, 
                                                       const FunctionGroupConfig& config, raw_ostream &OS)
    {
        OS << "[ADAPTIVE] Function: " << F->getName() 
           << " | Group: " << groupName 
           << " | Warmup: " << config.warmupRuns 
           << " | Sample Rate: 1/" << config.sampleRate
           << " | Threshold: " << config.profilingThreshold
           << " | Desc: " << config.description << "\n";
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
            // NEW: Determine group for this function
            std::string groupName = determineFunctionGroup(F);
            const FunctionGroupConfig& config = getGroupConfig(groupName);
            
            // Print group information
            printFunctionGroupInfo(F, groupName, config, errs());
            
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

        auto *ptrToFuncTy = PointerType::get(funcPtrTy, 0);   // i.e. T*
        auto *nullFuncPtr = ConstantPointerNull::get(ptrToFuncTy);

        metadata.functionPtr = new GlobalVariable(
            M,
            ptrToFuncTy,                       // The type of the global var
            false,
            GlobalValue::InternalLinkage,
            nullFuncPtr,                       // initializer
            funcPtrName
        );

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

        switch (versionId)
        {
        case 0: // BASELINE - Conservative
            break;
        case 1: // VECTORIZED - Maximum SIMD
            NewF->addFnAttr("prefer-vector-width", "512");
            NewF->addFnAttr("min-legal-vector-width", "256");
            NewF->addFnAttr("target-features", "+avx512f,+avx512vl,+avx2,+fma");
            NewF->addFnAttr("vectorize-predicate", "enable");
            NewF->addFnAttr("force-vector-width", "8");
            NewF->addFnAttr("force-vector-interleave", "4");
            break;
        case 2: // LOOP OPTIMIZED - Aggressive Unrolling
            NewF->addFnAttr("unroll-threshold", "100000");
            NewF->addFnAttr("unroll-count", "16");
            NewF->addFnAttr("unroll-allow-partial", "true");
            NewF->addFnAttr("unroll-runtime", "true");
            NewF->addFnAttr("unroll-full-unroll-max", "1024");
            NewF->addFnAttr("interleave-count", "4");
            NewF->addFnAttr("tail-merge-threshold", "2");
            NewF->addFnAttr("machine-block-placement", "true");
            break;
        case 3: // AGGRESSIVE INLINE - Maximum Inlining
            NewF->addFnAttr("unsafe-fp-math", "true");
            NewF->addFnAttr("no-nans-fp-math", "true");
            NewF->addFnAttr("no-infs-fp-math", "true");
            NewF->addFnAttr("approx-func-fp-math", "true");
            break;
        case 4: // BRANCH OPTIMIZED - Control Flow Focus
            NewF->addFnAttr("branch-weights", "likely");
            NewF->addFnAttr("profile-likely-prob", "0.9");
            NewF->addFnAttr("jump-table-density", "10");
            NewF->addFnAttr("min-jump-table-entries", "4");
            NewF->addFnAttr("tail-call-opt", "true");
            NewF->addFnAttr("x86-cmov-converter", "true");
            NewF->addFnAttr("select-optimize", "true");
            break;
        case 5: // Math optimizations
            NewF->addFnAttr(Attribute::AlwaysInline);
            NewF->addFnAttr("inline-threshold", "100000");
            NewF->addFnAttr("inlinehint-threshold", "50000");
            NewF->addFnAttr("hot-call-site-threshold", "100000");
            NewF->addFnAttr(Attribute::NoRecurse);
            NewF->addFnAttr(Attribute::NoUnwind);
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
        metadata.groupName = determineFunctionGroup(F); // NEW: Store group name
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

        functionMap[F] = metadata;
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;

        // NEW: Get configuration for this function's group
        const FunctionGroupConfig& config = getGroupConfig(metadata.groupName);
        
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
        // Constants - NEW: Use group-specific values
        ConstantInt *zero32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 0));
        ConstantInt *one32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 1));
        ConstantInt *two32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 2));
        ConstantInt *warmupThreshold = cast<ConstantInt>(ConstantInt::get(i32Ty, config.warmupRuns * 6)); // Total warmup runs
        ConstantInt *profilingThreshold = cast<ConstantInt>(ConstantInt::get(i32Ty, config.profilingThreshold));
        ConstantInt *sampleRate = cast<ConstantInt>(ConstantInt::get(i32Ty, config.sampleRate));

        // Get printf for debugging
        FunctionType *PrintfType = FunctionType::get(i32Ty,
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName().str());

        // NEW: Print group configuration at first call
        // Value *GroupConfigMsg = Builder.CreateGlobalStringPtr(
        //     "[GROUP CONFIG] %s - Group: %s, Warmup: %d, Sample Rate: 1/%d, Threshold: %d\n");
        // Value *GroupNameStr = Builder.CreateGlobalStringPtr(metadata.groupName);
        // Builder.CreateCall(Printf, {GroupConfigMsg, FuncName, GroupNameStr, 
        //                            ConstantInt::get(i32Ty, config.warmupRuns),
        //                            ConstantInt::get(i32Ty, config.sampleRate),
        //                            ConstantInt::get(i32Ty, config.profilingThreshold)});

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
            // Print transition message
            Value *TransMsg = Builder.CreateGlobalStringPtr(
                "[WARMUP COMPLETE] %s - Transitioning to profiling phase after %d warmup runs\n");
            Builder.CreateCall(Printf, {TransMsg, FuncName, warmupThreshold});

            // Update phase to profiling
            StoreInst *PhaseStore = Builder.CreateStore(one32, metadata.currentPhase);
            PhaseStore->setAtomic(AtomicOrdering::Release);
            PhaseStore->setAlignment(Align(4));

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
                // Increment profile counter (only sampled calls)
                Value *ProfileCount = Builder.CreateAtomicRMW(
                    AtomicRMWInst::Add, metadata.profileCounter, one32,
                    MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

                // Calculate which version to run (round-robin for sampled calls)
                Value *VersionToProfile = Builder.CreateURem(ProfileCount, ConstantInt::get(i32Ty, NUM_VERSIONS));

                // Store current version being tested
                StoreInst *VersionStore = Builder.CreateStore(VersionToProfile, metadata.currentVersion);
                VersionStore->setAtomic(AtomicOrdering::Release);
                VersionStore->setAlignment(Align(4));

                // Check if profiling is complete
                Value *IsProfilingComplete = Builder.CreateICmpUGE(ProfileCount, profilingThreshold);
                Builder.CreateCondBr(IsProfilingComplete, TransitionToOptimalBB, UpdateStatsBB);
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
                // Atomic add for cycles
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.cpuCycles[i],
                                        ElapsedCycles, MaybeAlign(8), AtomicOrdering::SequentiallyConsistent);
                // Atomic add for run count
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.runCount[i],
                                        one32, MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);
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

            // Update phase to optimal
            StoreInst *PhaseStore = Builder.CreateStore(two32, metadata.currentPhase);
            PhaseStore->setAtomic(AtomicOrdering::Release);
            PhaseStore->setAlignment(Align(4));

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
        
        // NEW: Print group configurations
        Value *GroupHeaderMsg = Builder.CreateGlobalStringPtr(
            "[INIT] Adaptive dispatcher with MANUAL GROUP CONFIGURATIONS initialized:\n");
        Builder.CreateCall(Printf, {GroupHeaderMsg});
        
        // Print each group configuration
        for (const auto& [groupName, config] : FUNCTION_GROUPS) {
            Value *GroupMsg = Builder.CreateGlobalStringPtr(
                "  - Group %s: Warmup=%d, Sample Rate=1/%d, Threshold=%d (%s)\n");
            Value *GroupNameStr = Builder.CreateGlobalStringPtr(groupName);
            Value *DescStr = Builder.CreateGlobalStringPtr(config.description);
            Builder.CreateCall(Printf, {GroupMsg, GroupNameStr, 
                                       ConstantInt::get(Type::getInt32Ty(Ctx), config.warmupRuns),
                                       ConstantInt::get(Type::getInt32Ty(Ctx), config.sampleRate),
                                       ConstantInt::get(Type::getInt32Ty(Ctx), config.profilingThreshold),
                                       DescStr});
        }
        
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm
