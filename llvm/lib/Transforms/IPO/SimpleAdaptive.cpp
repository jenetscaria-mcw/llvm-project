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

    struct FunctionMetadata
    {
        Function *original;
        Function *versions[6]; // v0 to v5
        Function *wrapper;
        uint32_t functionId;
        FunctionType *signature;

        // Per-function static globals for adaptive runtime tracking
        GlobalVariable *callCounter;  // call count
        GlobalVariable *cpuCycles[6]; // accumulated cpu cycles per version
        GlobalVariable *runCount[6];  // runs per version
        GlobalVariable *bestVersion;  // best version index after profiling
        GlobalVariable *currentPhase; // 0=profiling, 1=optimal
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
        const std::string bestName = "__static_best_" + baseName;
        const std::string phaseName = "__static_phase_" + baseName;
        const std::string cyclesBase = "__static_cycles_" + baseName + "_v";
        const std::string runsBase = "__static_runs_" + baseName + "_v";

        // Create variables with atomic alignment
        metadata.callCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), counterName);
        metadata.callCounter->setAlignment(Align(4));

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
            ConstantInt::get(i32Ty, 0), phaseName);
        metadata.currentPhase->setAlignment(Align(4));
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

        M.getFunctionList().push_back(NewF);
        return NewF;
    }

    void SimpleAdaptivePassImpl::processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M)
    {
        FunctionMetadata metadata;
        metadata.original = F;
        metadata.functionId = funcId;
        metadata.signature = F->getFunctionType();
        auto funcName = F->getName().str();
        // errs() << "Original function: " << F->getName() << "\n";

        // Create wrapper static variables
        createWrapperStaticVars(metadata, M);

        // errs() << "Processing adaptive function: " << F->getName() << "\n";

        // Create 6 versions (v0 - v5)
        for (int v = 0; v < 6; v++)
        {
            metadata.versions[v] = createVersion(F, v, M);
        }

        // Create optimized direct dispatch wrapper
        metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

        F->replaceAllUsesWith(metadata.wrapper);
        metadata.wrapper->takeName(F);

        F->setName(funcName + "_original");
        F->setLinkage(GlobalValue::InternalLinkage);

        functionMap[F] = metadata;

        // errs() << "Processed function " << funcId << ": " << F->getName() << "\n";
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;
        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        Type *i8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);

        const int NUM_VERSIONS = 6;
        const int PROFILING_CALLS = 600;

        ConstantInt *zero32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 0));
        ConstantInt *one32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 1));

        /*
        // Create intrinsics for serialized RDTSC
        FunctionType *RdtscType = FunctionType::get(i64Ty, {}, false);

        // RDTSC_START: CPUID + RDTSC (serialize before reading)
        InlineAsm *RdtscStart = InlineAsm::get(
            RdtscType,
            "xor %rax, %rax\n\t" // Zero out RAX for CPUID
            "cpuid\n\t"          // Serialize execution
            "rdtsc\n\t"          // Read timestamp
            "shl $$32, %rdx\n\t" // Shift high bits
            "or %rdx, %rax",     // Combine into 64-bit value
            "={rax},~{rbx},~{rcx},~{rdx}",
            true // hasSideEffects
        );

        // RDTSC_END: RDTSCP + CPUID (serialize after reading)
        InlineAsm *RdtscEnd = InlineAsm::get(
            RdtscType,
            "rdtscp\n\t"         // Read timestamp (serializing)
            "shl $$32, %rdx\n\t" // Shift high bits
            "or %rdx, %rax\n\t"  // Combine
            "mov %rax, %rcx\n\t" // Save result
            "xor %rax, %rax\n\t" // Zero RAX for CPUID
            "cpuid\n\t"          // Serialize
            "mov %rcx, %rax",    // Restore result
            "={rax},~{rbx},~{rcx},~{rdx}",
            true);

        FunctionCallee RdtscStartCallee(RdtscType, RdtscStart);
        FunctionCallee RdtscEndCallee(RdtscType, RdtscEnd);
        */

        Function *Wrapper = Function::Create(
            Orig->getFunctionType(), Orig->getLinkage(), Orig->getName() + "_wrapper", &M);

        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
        IRBuilder<> Builder(Entry);

        FunctionType *PrintfType = FunctionType::get(i32Ty,
                                                     {i8PtrTy}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

        // Get rdtsc intrinsic for cycle measurement in wrapper
        //FunctionType *RdtscType = FunctionType::get(i64Ty, {}, false);
        //FunctionCallee Rdtsc = M.getOrInsertFunction("llvm.readcyclecounter", RdtscType);
        
        // Declare the readcyclecounter intrinsic
        Function *ReadCycleCounter = Intrinsic::getDeclaration(&M, Intrinsic::readcyclecounter);

        // Atomic increment of call counter
        Value *Count = Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.callCounter,
                                               one32, MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);
        Value *NewCount = Builder.CreateAdd(Count, one32);

        Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName());

        // Atomic load of current phase
        LoadInst *PhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
        PhaseLoad->setAtomic(AtomicOrdering::Acquire);
        PhaseLoad->setAlignment(Align(4));
        Value *Phase = PhaseLoad;

        BasicBlock *ProfPhaseBB = BasicBlock::Create(Ctx, "profiling_phase", Wrapper);
        BasicBlock *OptPhaseBB = BasicBlock::Create(Ctx, "optimal_phase", Wrapper);
        Builder.CreateCondBr(
            Builder.CreateICmpEQ(Phase, one32), OptPhaseBB, ProfPhaseBB);

        // Profiling phase: alternate versions and measure
        Builder.SetInsertPoint(ProfPhaseBB);
        Value *Version = Builder.CreateURem(Count, ConstantInt::get(i32Ty, NUM_VERSIONS));

        Value *ShouldUpdateBest = Builder.CreateICmpUGE(NewCount, ConstantInt::get(i32Ty, PROFILING_CALLS + 1));

        BasicBlock *ContinueProfBB = BasicBlock::Create(Ctx, "continue_prof", Wrapper);
        BasicBlock *UpdateBestBB = BasicBlock::Create(Ctx, "update_best", Wrapper);

        Builder.CreateCondBr(ShouldUpdateBest, UpdateBestBB, ContinueProfBB);

        Builder.SetInsertPoint(ContinueProfBB);
        {
            // Create function pointers for all versions
            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            // Create selection logic for all versions
            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsVersionI = Builder.CreateICmpEQ(Version, ConstantInt::get(i32Ty, i));
                FuncPtr = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtr);
            }

        // Measure cycles before calling
        Value *StartTime = Builder.CreateCall(ReadCycleCounter);

            // Debug message
            // Value *FormatStr = Builder.CreateGlobalStringPtr("[PROFILING] %s: Call %d, using version %d\n");
            // Builder.CreateCall(Printf, {FormatStr, FuncName, NewCount, Version});

            // Call version
            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            Value *Result;
            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                Result = nullptr;
            }
            else
            {
                Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
            }

            // Measure end time and calculate cycles
            Value *EndTime = Builder.CreateCall(ReadCycleCounter);
            
            // FIXED: Check for TSC wraparound (EndTime < StartTime)
            Value *DidWrap = Builder.CreateICmpULT(EndTime, StartTime);
            
            // Calculate elapsed cycles - handle wraparound
            Value *ElapsedNormal = Builder.CreateSub(EndTime, StartTime);
            
            // If wrapped: elapsed = (UINT64_MAX - start) + end + 1
            Value *MaxU64 = ConstantInt::get(i64Ty, UINT64_MAX);
            Value *WrapDiff = Builder.CreateSub(MaxU64, StartTime);
            Value *ElapsedWrapped = Builder.CreateAdd(WrapDiff, Builder.CreateAdd(EndTime, ConstantInt::get(i64Ty, 1)));
            
            Value *ElapsedCycles = Builder.CreateSelect(DidWrap, ElapsedWrapped, ElapsedNormal);

            
            // Use atomic operations to update only the called version
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                Value *IsThisVersion = Builder.CreateICmpEQ(Version, ConstantInt::get(i32Ty, i));

                // Create a basic block for this version's update
                BasicBlock *UpdateVersionBB = BasicBlock::Create(Ctx, "update_v" + std::to_string(i), Wrapper);
                BasicBlock *SkipUpdateBB = BasicBlock::Create(Ctx, "skip_v" + std::to_string(i), Wrapper);

                Builder.CreateCondBr(IsThisVersion, UpdateVersionBB, SkipUpdateBB);

                Builder.SetInsertPoint(UpdateVersionBB);
                // FIXED: Atomic add for cycles (treating as unsigned)
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.cpuCycles[i],
                                        ElapsedCycles, MaybeAlign(8), AtomicOrdering::SequentiallyConsistent);
                // Atomic add for run count
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.runCount[i],
                                        one32, MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);
                Builder.CreateBr(SkipUpdateBB);

                Builder.SetInsertPoint(SkipUpdateBB);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateRetVoid();
            }
            else
            {
                Builder.CreateRet(Result);
            }
        }

        Builder.SetInsertPoint(UpdateBestBB);
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
                Value *Runs64 = Builder.CreateZExt(CurrentRuns[i], i64Ty);
                Value *Avg = Builder.CreateUDiv(CurrentCycles[i], Runs64);
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
                // FIXED: Use %llu (unsigned) instead of %lld (signed)
                Value *StatsMsg = Builder.CreateGlobalStringPtr(
                    "[STATS] %s - V%d: total_cycles=%llu, runs=%u, avg=%llu\n");
                Builder.CreateCall(Printf, {StatsMsg, FuncName,
                                            ConstantInt::get(i32Ty, i),
                                            CurrentCycles[i], CurrentRuns[i], AvgCycles[i]});
            }
            // Print current statistics
            Value *OptimalStatsMsg = Builder.CreateGlobalStringPtr(
                "[OPTIMAL] %s - Using best version %d\n");
            Builder.CreateCall(Printf, {OptimalStatsMsg, FuncName, BestVersion});
            // Atomic store of best version
            StoreInst *BestStore = Builder.CreateStore(BestVersion, metadata.bestVersion);
            BestStore->setAtomic(AtomicOrdering::Release);
            BestStore->setAlignment(Align(4));

            // Atomic store of phase transition
            StoreInst *PhaseStore = Builder.CreateStore(one32, metadata.currentPhase);
            PhaseStore->setAtomic(AtomicOrdering::Release);
            PhaseStore->setAlignment(Align(4));

            // Dispatch to best version
            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsBestVersionI = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(i32Ty, i));
                FuncPtr = Builder.CreateSelect(IsBestVersionI, VersionFuncs[i], FuncPtr);
            }

            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            Value *Result;
            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                Result = nullptr;
            }
            else
            {
                Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateRetVoid();
            }
            else
            {
                Builder.CreateRet(Result);
            }
        }

        // Optimal phase: always use bestVersion
        Builder.SetInsertPoint(OptPhaseBB);
        {
            // Atomic load of best version
            LoadInst *BestLoad = Builder.CreateLoad(i32Ty, metadata.bestVersion);
            BestLoad->setAtomic(AtomicOrdering::Acquire);
            BestLoad->setAlignment(Align(4));
            Value *BestVersion = BestLoad;


            // Create function pointer selection for best version
            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsBestVersionI = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(i32Ty, i));
                FuncPtr = Builder.CreateSelect(IsBestVersionI, VersionFuncs[i], FuncPtr);
            }

            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            Value *Result;
            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                Result = nullptr;
            }
            else
            {
                Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateRetVoid();
            }
            else
            {
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
        // Value *InitMsg = Builder.CreateGlobalStringPtr("[INIT] Adaptive dispatcher initialized\n");
        // Builder.CreateCall(Printf, {InitMsg});
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm