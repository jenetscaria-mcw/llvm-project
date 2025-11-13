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
    };

    class SimpleAdaptivePassImpl
    {
    private:
        std::map<Function *, FunctionMetadata> functionMap;

        Function *createVersion(Function *F, int versionId, Module &M);
        void createWrapperStaticVars(FunctionMetadata &metadata, Module &M);
        void processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M);
        Function *createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M);
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
        // errs() << "=== SimpleAdaptive Pass Running ===\n";
        functionMap.clear();

        // Collect adaptive functions
        std::vector<Function *> adaptiveFunctions;
        for (Function &F : M)
        {
            if (!F.isDeclaration() && F.hasFnAttribute("adaptive"))
            {
                // errs() << "[ADAPTIVE] Found: " << F.getName() << "\n";
                adaptiveFunctions.push_back(&F);
            }
        }
        if (adaptiveFunctions.empty())
        {
            // errs() << "No adaptive functions found in this module\n";
            return PreservedAnalyses::all();
        }

        uint32_t funcId = 0;
        for (Function *F : adaptiveFunctions)
        {
            processFunctionWithDirectDispatch(F, funcId++, M);
        }

        createInitializationFunction(M);

        // errs() << "=== Transformation Complete ===\n";
        return PreservedAnalyses::none();
    }

    void SimpleAdaptivePassImpl::createWrapperStaticVars(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        const std::string baseName = metadata.original->getName().str();

        // Precompute everything upfront
        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);

        const std::string counterName = "__static_counter_" + baseName;
        const std::string warmupName = "__static_warmup_" + baseName;
        const std::string profileName = "__static_profile_" + baseName;
        const std::string bestName = "__static_best_" + baseName;
        const std::string phaseName = "__static_phase_" + baseName;
        const std::string currentVersionName = "__static_current_version_" + baseName;
        const std::string sampleCounterName = "__static_sample_counter_" + baseName;
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

        // Optimized loop - precompute version strings
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
            // errs() << "  V0: Baseline (conservative)\n";
            break;
        case 1: // VECTORIZED - Maximum SIMD
            NewF->addFnAttr("prefer-vector-width", "512");
            NewF->addFnAttr("min-legal-vector-width", "256");
            NewF->addFnAttr("target-features", "+avx512f,+avx512vl,+avx2,+fma");
            // NewF->addFnAttr("target-cpu", "skylake-avx512");
            NewF->addFnAttr("vectorize-predicate", "enable");
            NewF->addFnAttr("force-vector-width", "8");
            NewF->addFnAttr("force-vector-interleave", "4");
            // errs() << "  V1: Maximum Vectorized (AVX512)\n";
            break;
        case 2: // LOOP OPTIMIZED - Aggressive Unrolling
            NewF->addFnAttr("unroll-threshold", "100000");
            NewF->addFnAttr("unroll-count", "16");
            NewF->addFnAttr("unroll-allow-partial", "true");
            NewF->addFnAttr("unroll-runtime", "true");
            NewF->addFnAttr("unroll-full-unroll-max", "1024");
            NewF->addFnAttr("interleave-count", "4");
            // Branch optimization for loops
            NewF->addFnAttr("tail-merge-threshold", "2");
            NewF->addFnAttr("machine-block-placement", "true");
            // errs() << "  V2: Aggressive Loop Optimized\n";
            break;
        case 3: // AGGRESSIVE INLINE - Maximum Inlining
            NewF->addFnAttr("unsafe-fp-math", "true");
            NewF->addFnAttr("no-nans-fp-math", "true");
            NewF->addFnAttr("no-infs-fp-math", "true");
            NewF->addFnAttr("approx-func-fp-math", "true");
            // errs() << "  V3: Maximum Aggressive (inline + fast-math)\n";
            break;
        case 4: // BRANCH OPTIMIZED - Control Flow Focus
            NewF->addFnAttr("branch-weights", "likely");
            NewF->addFnAttr("profile-likely-prob", "0.9");
            NewF->addFnAttr("jump-table-density", "10");
            NewF->addFnAttr("min-jump-table-entries", "4");
            NewF->addFnAttr("tail-call-opt", "true");
            NewF->addFnAttr("x86-cmov-converter", "true");
            NewF->addFnAttr("select-optimize", "true");
            // errs() << "  V4: Branch Optimized (predictable branches)\n";
            break;
        case 5: // Math optimizations
            NewF->addFnAttr(Attribute::AlwaysInline);
            NewF->addFnAttr("inline-threshold", "100000");
            NewF->addFnAttr("inlinehint-threshold", "50000");
            NewF->addFnAttr("hot-call-site-threshold", "100000");
            NewF->addFnAttr(Attribute::NoRecurse);
            NewF->addFnAttr(Attribute::NoUnwind);
            // errs() << "  V5: Math optimizations\n";
            break;
        }

        return NewF;
    }

    void SimpleAdaptivePassImpl::processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M)
    {
        FunctionMetadata metadata;
        metadata.original = F;
        metadata.functionId = funcId;
        metadata.signature = F->getFunctionType();
        auto funcName = F->getName().str();

        // errs() << "\n[ADAPTIVE] Processing: " << F->getName() << " (ID: " << funcId << ")\n";

        // Create versions
        for (int i = 0; i < 6; i++)
        {
            metadata.versions[i] = createVersion(F, i, M);
        }

        // Create static vars for wrapper
        createWrapperStaticVars(metadata, M);

        // Create wrapper
        metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

        // Replace all uses
        F->replaceAllUsesWith(metadata.wrapper);
        metadata.wrapper->takeName(F);

        F->setName(funcName + "_original");
        F->setLinkage(GlobalValue::InternalLinkage);

        functionMap[F] = metadata;
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;

        // errs() << "[WRAPPER] Creating direct dispatcher for: " << Orig->getName() << "\n";

        // Create wrapper function
        Function *Wrapper = Function::Create(
            Orig->getFunctionType(),
            Orig->getLinkage(),
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

        // Types
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
        FunctionType *PrintfType = FunctionType::get(i32Ty, {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName().str());

        // Entry block: Atomic increment of call counter and phase check
        LoadInst *PhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
        PhaseLoad->setAtomic(AtomicOrdering::Acquire);
        PhaseLoad->setAlignment(Align(4));

        // Branch based on phase
        SwitchInst *PhaseSwitch = Builder.CreateSwitch(PhaseLoad, OptimalPhaseBB, 3);
        PhaseSwitch->addCase(zero32, WarmupPhaseBB);   // Phase 0: Warmup
        PhaseSwitch->addCase(one32, ProfilingPhaseBB); // Phase 1: Profiling
        PhaseSwitch->addCase(two32, OptimalPhaseBB);   // Phase 2: Optimal

        const int NUM_VERSIONS = 6;

        // Warmup Phase
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
/*
        // Transition to Optimal
        Builder.SetInsertPoint(TransitionToOptimalBB);
        {
            // Load all version data with atomic loads
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

            // Calculate averages and find best version
            std::vector<Value *> AvgCycles;
            Value *BestVersion = zero32;
            Value *BestAvg = nullptr;

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                // Check if we have runs for this version
                Value *HasRuns = Builder.CreateICmpUGT(CurrentRuns[i], zero32);

                // Calculate average (protect against division by zero)
                Value *Runs64 = Builder.CreateZExt(CurrentRuns[i], i64Ty);
                Value *SafeRuns = Builder.CreateSelect(HasRuns, Runs64, ConstantInt::get(i64Ty, 1));
                Value *Avg = Builder.CreateUDiv(CurrentCycles[i], SafeRuns);

                // Use max value if no runs
                Value *MaxVal = ConstantInt::get(i64Ty, UINT64_MAX);
                Avg = Builder.CreateSelect(HasRuns, Avg, MaxVal);
                AvgCycles.push_back(Avg);

                if (i == 0)
                {
                    BestAvg = Avg;
                }
                else
                {
                    Value *IsBetter = Builder.CreateICmpULT(Avg, BestAvg);
                    BestVersion = Builder.CreateSelect(IsBetter, ConstantInt::get(i32Ty, i), BestVersion);
                    BestAvg = Builder.CreateSelect(IsBetter, Avg, BestAvg);
                }

                // Print statistics for each version
                Value *StatsMsg = Builder.CreateGlobalStringPtr(
                    "[STATS] %s - V%d: total_cycles=%llu, runs=%u, avg=%llu\n");
                Builder.CreateCall(Printf, {StatsMsg, FuncName,
                                            ConstantInt::get(i32Ty, i),
                                            CurrentCycles[i], CurrentRuns[i], AvgCycles[i]});
            }

            // Print final decision
            Value *OptimalStatsMsg = Builder.CreateGlobalStringPtr(
                "[OPTIMAL] %s - Selected best version: V%d (sampled %d/%d calls)\n");
            LoadInst *FinalProfileCount = Builder.CreateLoad(i32Ty, metadata.profileCounter);
            FinalProfileCount->setAtomic(AtomicOrdering::Acquire);
            FinalProfileCount->setAlignment(Align(4));
            Builder.CreateCall(Printf, {OptimalStatsMsg, FuncName, BestVersion, FinalProfileCount, profilingThreshold});

            // Store best version
            StoreInst *BestStore = Builder.CreateStore(BestVersion, metadata.bestVersion);
            BestStore->setAtomic(AtomicOrdering::Release);
            BestStore->setAlignment(Align(4));

            // Update phase to optimal
            StoreInst *PhaseStore = Builder.CreateStore(two32, metadata.currentPhase);
            PhaseStore->setAtomic(AtomicOrdering::Release);
            PhaseStore->setAlignment(Align(4));

            Builder.CreateBr(OptimalPhaseBB);
        }
*/

        Builder.SetInsertPoint(TransitionToOptimalBB);
        {
            // Load all version data with atomic loads
            std::vector<Value *> CurrentCycles;
            std::vector<Value *> CurrentRuns;
            for (int i = 0; i < NUM_VERSIONS; i++) {
                LoadInst *CyclesLoad = Builder.CreateLoad(i64Ty, metadata.cpuCycles[i]);
                CyclesLoad->setAtomic(AtomicOrdering::Acquire);
                CyclesLoad->setAlignment(Align(8));
                CurrentCycles.push_back(CyclesLoad);

                LoadInst *RunsLoad = Builder.CreateLoad(i32Ty, metadata.runCount[i]);
                RunsLoad->setAtomic(AtomicOrdering::Acquire);
                RunsLoad->setAlignment(Align(4));
                CurrentRuns.push_back(RunsLoad);
            }

            // Calculate averages using floating point for precision
            std::vector<Value *> AvgCycles;
            Value *BestVersion = zero32;
            Value *BestAvg = nullptr;

            for (int i = 0; i < NUM_VERSIONS; i++) {
                // Check if we have runs for this version
                Value *HasRuns = Builder.CreateICmpUGT(CurrentRuns[i], zero32);

                // Convert to floating point for proper division
                Value *CyclesFloat = Builder.CreateUIToFP(CurrentCycles[i], Builder.getDoubleTy());
                Value *RunsFloat = Builder.CreateUIToFP(CurrentRuns[i], Builder.getDoubleTy());
                
                // Use 1.0 if no runs to avoid division by zero
                Value *SafeRunsFloat = Builder.CreateSelect(
                    HasRuns, 
                    RunsFloat, 
                    ConstantFP::get(Builder.getDoubleTy(), 1.0)
                );
                
                Value *AvgFloat = Builder.CreateFDiv(CyclesFloat, SafeRunsFloat);
                
                // Use max value if no runs
                Value *MaxValFloat = ConstantFP::get(Builder.getDoubleTy(), std::numeric_limits<double>::max());
                Value *FinalAvg = Builder.CreateSelect(HasRuns, AvgFloat, MaxValFloat);
                AvgCycles.push_back(FinalAvg);

                // Initialize or compare best version
                if (i == 0) {
                    BestAvg = FinalAvg;
                    BestVersion = ConstantInt::get(i32Ty, 0);
                } else {
                    Value *IsBetter = Builder.CreateFCmpOLT(FinalAvg, BestAvg);
                    BestVersion = Builder.CreateSelect(IsBetter, ConstantInt::get(i32Ty, i), BestVersion);
                    BestAvg = Builder.CreateSelect(IsBetter, FinalAvg, BestAvg);
                }

                // Print statistics - convert back to integer for display if needed
                Value *AvgForDisplay = Builder.CreateFPToUI(FinalAvg, i64Ty);
                Value *StatsMsg = Builder.CreateGlobalStringPtr(
                    "[STATS] %s - V%d: total_cycles=%llu, runs=%u, avg=%llu\n");
                Builder.CreateCall(Printf, {StatsMsg, FuncName,
                                            ConstantInt::get(i32Ty, i),
                                            CurrentCycles[i], CurrentRuns[i], AvgForDisplay});
            }

            // Print final decision
            Value *OptimalStatsMsg = Builder.CreateGlobalStringPtr(
                "[OPTIMAL] %s - Selected best version: V%d (sampled %d/%d calls)\n");
            LoadInst *FinalProfileCount = Builder.CreateLoad(i32Ty, metadata.profileCounter);
            FinalProfileCount->setAtomic(AtomicOrdering::Acquire);
            FinalProfileCount->setAlignment(Align(4));
            Builder.CreateCall(Printf, {OptimalStatsMsg, FuncName, BestVersion, FinalProfileCount, profilingThreshold});

            // Store best version
            StoreInst *BestStore = Builder.CreateStore(BestVersion, metadata.bestVersion);
            BestStore->setAtomic(AtomicOrdering::Release);
            BestStore->setAlignment(Align(4));

            // Update phase to optimal
            StoreInst *PhaseStore = Builder.CreateStore(two32, metadata.currentPhase);
            PhaseStore->setAtomic(AtomicOrdering::Release);
            PhaseStore->setAlignment(Align(4));

            Builder.CreateBr(OptimalPhaseBB);
        }
        // Optimal Phase: always use bestVersion
        Builder.SetInsertPoint(OptimalPhaseBB);
        {
            // Load best version
            LoadInst *BestLoad = Builder.CreateLoad(i32Ty, metadata.bestVersion);
            BestLoad->setAtomic(AtomicOrdering::Acquire);
            BestLoad->setAlignment(Align(4));

            // Create function pointer selection for best version
            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(
                    metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsBestVersionI = Builder.CreateICmpEQ(BestLoad, ConstantInt::get(i32Ty, i));
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
        Function *InitFunc = Function::Create(InitType, GlobalValue::InternalLinkage, "__adaptive_init", &M);
        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", InitFunc);
        IRBuilder<> Builder(Entry);

        // Print initialization message
        FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        Value *InitMsg = Builder.CreateGlobalStringPtr(
            "[INIT] Adaptive dispatcher initialized with:\n"
            "  - Warmup runs: %d per version (%d total)\n"
            "  - Sample rate: 1 in %d calls\n"
            "  - Profiling threshold: %d samples\n");
        Builder.CreateCall(Printf, {InitMsg,
                                    ConstantInt::get(Type::getInt32Ty(Ctx), WARMUP_RUNS),
                                    ConstantInt::get(Type::getInt32Ty(Ctx), WARMUP_RUNS * 6),
                                    ConstantInt::get(Type::getInt32Ty(Ctx), SAMPLE_RATE),
                                    ConstantInt::get(Type::getInt32Ty(Ctx), PROFILING_THRESHOLD)});
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm