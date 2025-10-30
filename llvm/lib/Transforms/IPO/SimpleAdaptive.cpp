#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
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
    void addCPUCycleMeasurement(Function *F, FunctionMetadata &metadata, int versionId, Module &M);
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
    std::string baseName = metadata.original->getName().str();

    metadata.callCounter = new GlobalVariable(
        M, Type::getInt32Ty(Ctx), false, GlobalValue::InternalLinkage,
        ConstantInt::get(Type::getInt32Ty(Ctx), 0),
        "__static_counter_" + baseName);

    // FIXED: Create arrays for all 6 versions, not just 2
    for (int v = 0; v < 6; v++)
    {
        metadata.cpuCycles[v] = new GlobalVariable(
            M, Type::getInt64Ty(Ctx), false, GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt64Ty(Ctx), 0),
            "__static_cycles_" + baseName + "_v" + std::to_string(v));

        metadata.runCount[v] = new GlobalVariable(
            M, Type::getInt32Ty(Ctx), false, GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            "__static_runs_" + baseName + "_v" + std::to_string(v));
    }

    metadata.bestVersion = new GlobalVariable(
        M, Type::getInt32Ty(Ctx), false, GlobalValue::InternalLinkage,
        ConstantInt::get(Type::getInt32Ty(Ctx), -1),
        "__static_best_" + baseName);

    metadata.currentPhase = new GlobalVariable(
        M, Type::getInt32Ty(Ctx), false, GlobalValue::InternalLinkage,
        ConstantInt::get(Type::getInt32Ty(Ctx), 0),
        "__static_phase_" + baseName);

    // errs() << "  Created static variables for " << baseName << "\n";
}

void SimpleAdaptivePassImpl::addCPUCycleMeasurement(Function *F, FunctionMetadata &metadata, int versionId, Module &M)
{
    LLVMContext &Ctx = M.getContext();

    FunctionType *RdtscType = FunctionType::get(Type::getInt64Ty(Ctx), {}, false);
    FunctionCallee Rdtsc = M.getOrInsertFunction("llvm.readcyclecounter", RdtscType);

    // Get printf function for debugging
    FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx), 
                                                {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    BasicBlock &EntryBB = F->getEntryBlock();
    IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());

    Value *StartTime = Builder.CreateCall(Rdtsc);

    std::vector<ReturnInst*> returns;
    for (BasicBlock &BB : *F)
        if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
            returns.push_back(RI);

    for (ReturnInst *RI : returns)
    {
        IRBuilder<> RetBuilder(RI);
        Value *EndTime = RetBuilder.CreateCall(Rdtsc);
        Value *ElapsedCycles = RetBuilder.CreateSub(EndTime, StartTime);

        // Load, update, and store cycles
        Value *CurCycles = RetBuilder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[versionId]);
        Value *NewCycles = RetBuilder.CreateAdd(CurCycles, ElapsedCycles);
        RetBuilder.CreateStore(NewCycles, metadata.cpuCycles[versionId]);

        // Load, update, and store runs
        Value *CurRuns = RetBuilder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[versionId]);
        Value *NewRuns = RetBuilder.CreateAdd(CurRuns, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        RetBuilder.CreateStore(NewRuns, metadata.runCount[versionId]);

        // ADDED: Print CPU cycles and run counts after each execution
        // Value *FuncName = RetBuilder.CreateGlobalStringPtr(F->getName());
        // Value *CyclesMsg = RetBuilder.CreateGlobalStringPtr(
        //     "[CYCLES] %s_v%d: This run=%lu cycles, Total cycles=%lu, Total runs=%d\n");
        // RetBuilder.CreateCall(Printf, {CyclesMsg, FuncName, 
        //                             ConstantInt::get(Type::getInt32Ty(Ctx), versionId),
        //                             ElapsedCycles, NewCycles, NewRuns});
    }
}

Function *SimpleAdaptivePassImpl::createVersion(Function *F, int versionId, Module &M)
{
    ValueToValueMapTy VMap;
    Function *NewF = CloneFunction(F, VMap);
    NewF->setName(F->getName() + "_v" + std::to_string(versionId));
    NewF->setLinkage(GlobalValue::InternalLinkage);

    switch(versionId) {
        case 0:  // BASELINE - Conservative
            // errs() << "  V0: Baseline (conservative)\n";
            break;
        case 1:  // VECTORIZED - Maximum SIMD
            NewF->addFnAttr("prefer-vector-width", "512");
            NewF->addFnAttr("min-legal-vector-width", "256");
            NewF->addFnAttr("target-features", "+avx512f,+avx512vl,+avx2,+fma");
            // NewF->addFnAttr("target-cpu", "skylake-avx512");
            NewF->addFnAttr("vectorize-predicate", "enable");
            NewF->addFnAttr("force-vector-width", "8");
            NewF->addFnAttr("force-vector-interleave", "4");
            // errs() << "  V1: Maximum Vectorized (AVX512)\n";
            break;
        case 2:  // LOOP OPTIMIZED - Aggressive Unrolling
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
        case 3:  // AGGRESSIVE INLINE - Maximum Inlining
            NewF->addFnAttr(Attribute::AlwaysInline);
            NewF->addFnAttr("inline-threshold", "100000");
            NewF->addFnAttr("inlinehint-threshold", "50000");
            NewF->addFnAttr("hot-call-site-threshold", "100000");
            NewF->addFnAttr("unsafe-fp-math", "true");
            NewF->addFnAttr("no-nans-fp-math", "true");
            NewF->addFnAttr("no-infs-fp-math", "true");
            NewF->addFnAttr("approx-func-fp-math", "true");
            NewF->addFnAttr(Attribute::NoRecurse);
            NewF->addFnAttr(Attribute::NoUnwind);
            // errs() << "  V3: Maximum Aggressive (inline + fast-math)\n";
            break;
        case 4:  // BRANCH OPTIMIZED - Control Flow Focus
            NewF->addFnAttr("branch-weights", "likely");
            NewF->addFnAttr("profile-likely-prob", "0.9");
            NewF->addFnAttr("jump-table-density", "10");
            NewF->addFnAttr("min-jump-table-entries", "4");
            NewF->addFnAttr("tail-call-opt", "true");
            NewF->addFnAttr("x86-cmov-converter", "true");
            NewF->addFnAttr("select-optimize", "true");
            // errs() << "  V4: Branch Optimized (predictable branches)\n";
            break;
        case 5:  // HYBRID - Best of Multiple Optimizations
            NewF->addFnAttr("unsafe-fp-math", "true");
            NewF->addFnAttr("no-nans-fp-math", "true");
            NewF->addFnAttr("no-infs-fp-math", "true");
            NewF->addFnAttr("approx-func-fp-math", "true");
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

    for (int i = 0; i <= 5; i++) {
        metadata.versions[i] = createVersion(F, i, M);
    }

    createWrapperStaticVars(metadata, M);

    metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

    F->replaceAllUsesWith(metadata.wrapper);
    metadata.wrapper->takeName(F);

    F->setName(F->getName().str() + "_original");
    F->setLinkage(GlobalValue::InternalLinkage);

    functionMap[F] = metadata;

    // errs() << "Processed function " << funcId << ": " << F->getName() << "\n";
}

Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
{
    LLVMContext &Ctx = M.getContext();
    Function *Orig = metadata.original;
    const int NUM_VERSIONS = 6;

    Function *Wrapper = Function::Create(
        Orig->getFunctionType(), Orig->getLinkage(), Orig->getName() + "_wrapper", &M);

    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
    IRBuilder<> Builder(Entry);

    FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
                                                {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    // Get rdtsc intrinsic for cycle measurement in wrapper
    FunctionType *RdtscType = FunctionType::get(Type::getInt64Ty(Ctx), {}, false);
    FunctionCallee Rdtsc = M.getOrInsertFunction("llvm.readcyclecounter", RdtscType);

    Value *Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.callCounter);
    Value *NewCount = Builder.CreateAdd(Count, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
    Builder.CreateStore(NewCount, metadata.callCounter);

    Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName());

    Value *Phase = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.currentPhase);

    BasicBlock *ProfPhaseBB = BasicBlock::Create(Ctx, "profiling_phase", Wrapper);
    BasicBlock *OptPhaseBB = BasicBlock::Create(Ctx, "optimal_phase", Wrapper);
    Builder.CreateCondBr(
        Builder.CreateICmpEQ(Phase, ConstantInt::get(Type::getInt32Ty(Ctx), 1)), OptPhaseBB, ProfPhaseBB);

    // Profiling phase: alternate versions and measure
    Builder.SetInsertPoint(ProfPhaseBB);
    Value *Version = Builder.CreateURem(Count, ConstantInt::get(Type::getInt32Ty(Ctx), NUM_VERSIONS));

    Value *ShouldUpdateBest = Builder.CreateICmpUGE(NewCount, ConstantInt::get(Type::getInt32Ty(Ctx), 61));

    BasicBlock *ContinueProfBB = BasicBlock::Create(Ctx, "continue_prof", Wrapper);
    BasicBlock *UpdateBestBB = BasicBlock::Create(Ctx, "update_best", Wrapper);

    Builder.CreateCondBr(ShouldUpdateBest, UpdateBestBB, ContinueProfBB);

    Builder.SetInsertPoint(ContinueProfBB);
    {
        // Create function pointers for all versions
        std::vector<Value*> VersionFuncs;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
        }

        // Create selection logic for all versions
        Value *FuncPtr = VersionFuncs[0];
        for (int i = 1; i < NUM_VERSIONS; i++) {
            Value *IsVersionI = Builder.CreateICmpEQ(Version, ConstantInt::get(Type::getInt32Ty(Ctx), i));
            FuncPtr = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtr);
        }

        // Measure cycles before calling
        Value *StartTime = Builder.CreateCall(Rdtsc);
        
        // Debug message
        Value *FormatStr = Builder.CreateGlobalStringPtr("[PROFILING] %s: Call %d, using version %d\n");
        Builder.CreateCall(Printf, {FormatStr, FuncName, NewCount, Version});
        
        // Call version
        std::vector<Value *> Args;
        for (auto &Arg : Wrapper->args())
            Args.push_back(&Arg);

        Value *Result;
        if (Orig->getReturnType()->isVoidTy()) {
            Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
            Result = nullptr;
        } else {
            Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
        }

        // Measure end time and calculate cycles
        Value *EndTime = Builder.CreateCall(Rdtsc);
        Value *ElapsedCycles = Builder.CreateSub(EndTime, StartTime);
        
        // Update cycle counters and run counts for all versions
        std::vector<Value*> CurrentCycles;
        std::vector<Value*> CurrentRuns;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            CurrentCycles.push_back(Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[i]));
            CurrentRuns.push_back(Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[i]));
        }

        std::vector<Value*> NewCycles;
        std::vector<Value*> NewRuns;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            NewCycles.push_back(Builder.CreateAdd(CurrentCycles[i], ElapsedCycles));
            NewRuns.push_back(Builder.CreateAdd(CurrentRuns[i], ConstantInt::get(Type::getInt32Ty(Ctx), 1)));
        }

        // Store updated values - only update the called version
        for (int i = 0; i < NUM_VERSIONS; i++) {
            Value *IsThisVersion = Builder.CreateICmpEQ(Version, ConstantInt::get(Type::getInt32Ty(Ctx), i));
            Value *CyclesToStore = Builder.CreateSelect(IsThisVersion, NewCycles[i], CurrentCycles[i]);
            Value *RunsToStore = Builder.CreateSelect(IsThisVersion, NewRuns[i], CurrentRuns[i]);
            Builder.CreateStore(CyclesToStore, metadata.cpuCycles[i]);
            Builder.CreateStore(RunsToStore, metadata.runCount[i]);
        }

        // Print updated counters for first few versions for debugging
        // Value *UpdatedMsg = Builder.CreateGlobalStringPtr("[UPDATED] Version %d: %lu cycles, %d runs\n");
        // for (int i = 0; i < std::min(2, NUM_VERSIONS); i++) { // Only show first 2 to avoid clutter
        //     Builder.CreateCall(Printf, {UpdatedMsg, 
        //         ConstantInt::get(Type::getInt32Ty(Ctx), i),
        //         Builder.CreateSelect(
        //             Builder.CreateICmpEQ(Version, ConstantInt::get(Type::getInt32Ty(Ctx), i)),
        //             NewCycles[i], CurrentCycles[i]),
        //         Builder.CreateSelect(
        //             Builder.CreateICmpEQ(Version, ConstantInt::get(Type::getInt32Ty(Ctx), i)),
        //             NewRuns[i], CurrentRuns[i])
        //     });
        // }

        if (Orig->getReturnType()->isVoidTy()) {
            Builder.CreateRetVoid();
        } else {
            Builder.CreateRet(Result);
        }
    }

    Builder.SetInsertPoint(UpdateBestBB);
    {
        // Load all version data
        std::vector<Value*> CurrentCycles;
        std::vector<Value*> CurrentRuns;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            CurrentCycles.push_back(Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[i]));
            CurrentRuns.push_back(Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[i]));
        }

        // Calculate averages and find best version
        std::vector<Value*> AvgCycles;
        Value *BestVersion = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
        Value *BestAvg = nullptr;

        for (int i = 0; i < NUM_VERSIONS; i++) {
            Value *Runs64 = Builder.CreateZExt(CurrentRuns[i], Type::getInt64Ty(Ctx));
            Value *Avg = Builder.CreateUDiv(CurrentCycles[i], Runs64);
            AvgCycles.push_back(Avg);

            if (i == 0) {
                BestAvg = Avg;
            } else {
                Value *IsBetter = Builder.CreateICmpULT(Avg, BestAvg);
                BestVersion = Builder.CreateSelect(IsBetter, ConstantInt::get(Type::getInt32Ty(Ctx), i), BestVersion);
                BestAvg = Builder.CreateSelect(IsBetter, Avg, BestAvg);
            }

            // Print statistics for each version
            // Value *StatsMsg = Builder.CreateGlobalStringPtr(
            //     "[STATS] %s - V%d: total_cycles=%lu, runs=%d, avg=%lu\n");
            // Builder.CreateCall(Printf, {StatsMsg, FuncName, 
            //     ConstantInt::get(Type::getInt32Ty(Ctx), i),
            //     CurrentCycles[i], CurrentRuns[i], AvgCycles[i]});
        }

        Builder.CreateStore(BestVersion, metadata.bestVersion);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Ctx), 1), metadata.currentPhase);

        // Value *Msg = Builder.CreateGlobalStringPtr("[BEST] Computed best version %d with avg %lu cycles\n");
        // Builder.CreateCall(Printf, {Msg, BestVersion, BestAvg});

        // Dispatch to best version
        std::vector<Value*> VersionFuncs;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
        }

        Value *FuncPtr = VersionFuncs[0];
        for (int i = 1; i < NUM_VERSIONS; i++) {
            Value *IsBestVersionI = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(Type::getInt32Ty(Ctx), i));
            FuncPtr = Builder.CreateSelect(IsBestVersionI, VersionFuncs[i], FuncPtr);
        }
        
        std::vector<Value *> Args;
        for (auto &Arg : Wrapper->args())
            Args.push_back(&Arg);

        Value *Result;
        if (Orig->getReturnType()->isVoidTy()) {
            Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
            Result = nullptr;
        } else {
            Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
        }

        if (Orig->getReturnType()->isVoidTy()) {
            Builder.CreateRetVoid();
        } else {
            Builder.CreateRet(Result);
        }
    }

    // Optimal phase: always use bestVersion
    Builder.SetInsertPoint(OptPhaseBB);
    {
        Value *BestVersion = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.bestVersion);
        
        // Print current statistics
        // Value *OptimalStatsMsg = Builder.CreateGlobalStringPtr(
        //     "[OPTIMAL] %s - Using best version %d\n");
        // Builder.CreateCall(Printf, {OptimalStatsMsg, FuncName, BestVersion});

        // Create function pointer selection for best version
        std::vector<Value*> VersionFuncs;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
        }

        Value *FuncPtr = VersionFuncs[0];
        for (int i = 1; i < NUM_VERSIONS; i++) {
            Value *IsBestVersionI = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(Type::getInt32Ty(Ctx), i));
            FuncPtr = Builder.CreateSelect(IsBestVersionI, VersionFuncs[i], FuncPtr);
        }

        std::vector<Value *> Args;
        for (auto &Arg : Wrapper->args())
            Args.push_back(&Arg);

        Value *Result;
        if (Orig->getReturnType()->isVoidTy()) {
            Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
            Result = nullptr;
        } else {
            Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
        }

        if (Orig->getReturnType()->isVoidTy()) {
            Builder.CreateRetVoid();
        } else {
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
    // FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
    // Value *InitMsg = Builder.CreateGlobalStringPtr("[INIT] Adaptive dispatcher initialized\n");
    // Builder.CreateCall(Printf, {InitMsg});
    Builder.CreateRetVoid();

    appendToGlobalCtors(M, InitFunc, 65535);
}

} // end namespace llvm