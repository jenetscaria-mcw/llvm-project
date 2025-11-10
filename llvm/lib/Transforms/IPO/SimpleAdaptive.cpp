#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
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

    // Validate IR after transformations to catch issues early
    if (verifyModule(M, &errs())) {
        report_fatal_error("SimpleAdaptive produced invalid IR");
    }

    errs() << "=== Transformation Complete ===\n";
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
    
    // Create variables
    metadata.callCounter = new GlobalVariable(
        M, i32Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i32Ty, 0), counterName);
    // Make call counter thread-local to avoid global contention
    metadata.callCounter->setThreadLocal(true);

    // Optimized loop - precompute version strings
    const std::array<std::string, 6> versionStrs = {
        "0", "1", "2", "3", "4", "5"
    };
    
    for (int v = 0; v < 6; v++)
    {
        metadata.cpuCycles[v] = new GlobalVariable(
            M, i64Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i64Ty, 0), cyclesBase + versionStrs[v]);

        metadata.runCount[v] = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), runsBase + versionStrs[v]);
    }

    metadata.bestVersion = new GlobalVariable(
        M, i32Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i32Ty, -1), bestName);

    metadata.currentPhase = new GlobalVariable(
        M, i32Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i32Ty, 0), phaseName);

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
        case 5:  // Math optimizations
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
    std::string baseName = F->getName().str();

    for (int i = 0; i <= 5; i++) {
        metadata.versions[i] = createVersion(F, i, M);
        errs() << "Version " << i << ": " << metadata.versions[i]->getName() << "\n";
    }

    createWrapperStaticVars(metadata, M);

    metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);
    errs() << "Wrapper: " << metadata.wrapper->getName() << "\n";
    errs() << "Original: " << F->getName() << "\n";

    // 1) Fix recursion inside each cloned version: calls to original become calls to that version
    for (int i = 0; i <= 5; i++) {
        Function *Ver = metadata.versions[i];
        for (auto &BB : *Ver) {
            for (auto &I : BB) {
                if (auto *CB = dyn_cast<CallBase>(&I)) {
                    Value *Callee = CB->getCalledOperand();
                    Value *Target = nullptr;
                    if (Callee == F) {
                        Target = Ver;
                    } else if (auto *CE = dyn_cast<ConstantExpr>(Callee)) {
                        if (CE->getOpcode() == Instruction::BitCast && CE->getOperand(0) == F) {
                            Target = ConstantExpr::getBitCast(Ver, Callee->getType());
                        }
                    }
                    if (Target) {
                        CB->setCalledOperand(Target);
                    }
                }
            }
        }
    }

    // 2) Redirect only external uses of original to wrapper (not inside versions or original itself)
    SmallPtrSet<const Function*, 8> SkipFuncs;
    for (int i = 0; i <= 5; i++) SkipFuncs.insert(metadata.versions[i]);
    SkipFuncs.insert(F);
    SkipFuncs.insert(metadata.wrapper);

    SmallVector<User*, 16> Users;
    for (User *U : F->users()) Users.push_back(U);
    for (User *U : Users) {
        if (auto *I = dyn_cast<Instruction>(U)) {
            Function *UF = I->getFunction();
            if (SkipFuncs.count(UF)) continue;
            I->replaceUsesOfWith(F, metadata.wrapper);
        } else if (auto *CE = dyn_cast<ConstantExpr>(U)) {
            // Replace constant expr users outside of disallowed functions
            SmallVector<User*, 8> CEUsers;
            for (User *CU : CE->users()) CEUsers.push_back(CU);
            for (User *CU : CEUsers) {
                if (auto *CI = dyn_cast<Instruction>(CU)) {
                    if (SkipFuncs.count(CI->getFunction())) continue;
                    Value *Replacement = CE;
                    if (CE->getOpcode() == Instruction::BitCast && CE->getOperand(0) == F) {
                        Replacement = ConstantExpr::getBitCast(metadata.wrapper, CE->getType());
                    } else {
                        Replacement = metadata.wrapper;
                    }
                    CI->replaceUsesOfWith(CE, Replacement);
                }
            }
        }
    }

    // 3) Now move the public name to the wrapper and hide the original
    metadata.wrapper->takeName(F);

    F->setName(baseName + "_original");
    errs() << "Original: " << F->getName() << "\n";
    F->setLinkage(GlobalValue::InternalLinkage);

    functionMap[F] = metadata;

    errs() << "Processed function " << funcId << ": " << F->getName() << "\n";
}

/*
Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
{
    LLVMContext &Ctx = M.getContext();
    Function *Orig = metadata.original;
    const int NUM_VERSIONS = 6;
    const int PROFILING_CALLS = 60;
    
    Type *i32Ty = Type::getInt32Ty(Ctx);
    Type *i64Ty = Type::getInt64Ty(Ctx);
    // Type *voidTy = Type::getVoidTy(Ctx);
    PointerType *i8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
    
    // Precompute common constants
    ConstantInt *zero32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 0));
    ConstantInt *one32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 1));
    // ConstantInt *profilingcallse32 = cast<ConstantInt>(ConstantInt::get(i32Ty, PROFILING_CALLS + 1));
    // ConstantInt *minusOne32 = cast<ConstantInt>(ConstantInt::get(i32Ty, -1));

    Function *Wrapper = Function::Create(
        Orig->getFunctionType(), Orig->getLinkage(), Orig->getName() + "_wrapper", &M);

    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
    IRBuilder<> Builder(Entry);

    FunctionType *PrintfType = FunctionType::get(i32Ty,
                                                {i8PtrTy}, true);
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    // Get rdtsc intrinsic for cycle measurement in wrapper
    FunctionType *RdtscType = FunctionType::get(i64Ty, {}, false);
    FunctionCallee Rdtsc = M.getOrInsertFunction("llvm.readcyclecounter", RdtscType);

    Value *Count = Builder.CreateLoad(i32Ty, metadata.callCounter);
    Value *NewCount = Builder.CreateAdd(Count, one32);
    Builder.CreateStore(NewCount, metadata.callCounter);

    Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName());

    Value *Phase = Builder.CreateLoad(i32Ty, metadata.currentPhase);

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
        std::vector<Value*> VersionFuncs;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
        }

        // Create selection logic for all versions
        Value *FuncPtr = VersionFuncs[0];
        for (int i = 1; i < NUM_VERSIONS; i++) {
            Value *IsVersionI = Builder.CreateICmpEQ(Version, ConstantInt::get(i32Ty, i));
            FuncPtr = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtr);
        }

        // Measure cycles before calling
        Value *StartTime = Builder.CreateCall(Rdtsc);
        
        // Debug message
        // Value *FormatStr = Builder.CreateGlobalStringPtr("[PROFILING] %s: Call %d, using version %d\n");
        // Builder.CreateCall(Printf, {FormatStr, FuncName, NewCount, Version});
        
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
            CurrentCycles.push_back(Builder.CreateLoad(i64Ty, metadata.cpuCycles[i]));
            CurrentRuns.push_back(Builder.CreateLoad(i32Ty, metadata.runCount[i]));
        }

        std::vector<Value*> NewCycles;
        std::vector<Value*> NewRuns;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            NewCycles.push_back(Builder.CreateAdd(CurrentCycles[i], ElapsedCycles));
            NewRuns.push_back(Builder.CreateAdd(CurrentRuns[i], one32));
        }

        // Store updated values - only update the called version
        for (int i = 0; i < NUM_VERSIONS; i++) {
            Value *IsThisVersion = Builder.CreateICmpEQ(Version, ConstantInt::get(i32Ty, i));
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
            CurrentCycles.push_back(Builder.CreateLoad(i64Ty, metadata.cpuCycles[i]));
            CurrentRuns.push_back(Builder.CreateLoad(i32Ty, metadata.runCount[i]));
        }

        // Calculate averages and find best version
        std::vector<Value*> AvgCycles;
        Value *BestVersion = zero32;
        Value *BestAvg = nullptr;

        for (int i = 0; i < NUM_VERSIONS; i++) {
            Value *Runs64 = Builder.CreateZExt(CurrentRuns[i], i64Ty);
            Value *Avg = Builder.CreateUDiv(CurrentCycles[i], Runs64);
            AvgCycles.push_back(Avg);

            if (i == 0) {
                BestAvg = Avg;
            } else {
                Value *IsBetter = Builder.CreateICmpULT(Avg, BestAvg);
                BestVersion = Builder.CreateSelect(IsBetter, ConstantInt::get(i32Ty, i), BestVersion);
                BestAvg = Builder.CreateSelect(IsBetter, Avg, BestAvg);
            }

            // Print statistics for each version
            // Value *StatsMsg = Builder.CreateGlobalStringPtr(
            //     "[STATS] %s - V%d: total_cycles=%lu, runs=%d, avg=%lu\n");
            // Builder.CreateCall(Printf, {StatsMsg, FuncName, 
            //     ConstantInt::get(i32Ty, i),
            //     CurrentCycles[i], CurrentRuns[i], AvgCycles[i]});
        }

        Builder.CreateStore(BestVersion, metadata.bestVersion);
        Builder.CreateStore(one32, metadata.currentPhase);

        // Value *Msg = Builder.CreateGlobalStringPtr("[BEST] Computed best version %d with avg %lu cycles\n");
        // Builder.CreateCall(Printf, {Msg, BestVersion, BestAvg});

        // Dispatch to best version
        std::vector<Value*> VersionFuncs;
        for (int i = 0; i < NUM_VERSIONS; i++) {
            VersionFuncs.push_back(Builder.CreatePointerCast(metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
        }

        Value *FuncPtr = VersionFuncs[0];
        for (int i = 1; i < NUM_VERSIONS; i++) {
            Value *IsBestVersionI = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(i32Ty, i));
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
        Value *BestVersion = Builder.CreateLoad(i32Ty, metadata.bestVersion);
        
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
            Value *IsBestVersionI = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(i32Ty, i));
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
*/

Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
{
    LLVMContext &Ctx = M.getContext();
    Function *Orig = metadata.original;

    const int NUM_VERSIONS = 6;
    const int SAMPLE_RATE = 10;
    const int MIN_SAMPLES_PER_VERSION = 10;
    const int MAX_TOTAL_SAMPLES = 600; // FIX 1: Prevent infinite profiling

    Type *i32Ty = Type::getInt32Ty(Ctx);
    Type *i64Ty = Type::getInt64Ty(Ctx);

    // Precompute constants
    ConstantInt *zero32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 0));
    ConstantInt *one32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 1));
    ConstantInt *sampleRate = cast<ConstantInt>(ConstantInt::get(i32Ty, SAMPLE_RATE));
    ConstantInt *minSamples = cast<ConstantInt>(ConstantInt::get(i32Ty, MIN_SAMPLES_PER_VERSION));
    ConstantInt *numVersions = cast<ConstantInt>(ConstantInt::get(i32Ty, NUM_VERSIONS));
    ConstantInt *maxSamples = cast<ConstantInt>(ConstantInt::get(i32Ty, MAX_TOTAL_SAMPLES));

    Function *Rdtsc = Intrinsic::getDeclaration(&M, Intrinsic::readcyclecounter);

    // Create wrapper function
    Function *Wrapper = Function::Create(
        Orig->getFunctionType(),
        Orig->getLinkage(),
        Orig->getName() + "_wrapper",
        &M);

    errs() << "Wrapper: " << Wrapper->getName() << "\n";

    Wrapper->setAttributes(Orig->getAttributes());
    Wrapper->setCallingConv(Orig->getCallingConv());

    // Create basic blocks
    BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", Wrapper);
    BasicBlock *CheckPhaseBB = BasicBlock::Create(Ctx, "check_phase", Wrapper);
    BasicBlock *ProfilingBB = BasicBlock::Create(Ctx, "profiling", Wrapper);
    BasicBlock *ShouldSampleBB = BasicBlock::Create(Ctx, "should_sample", Wrapper);
    BasicBlock *DoSampleBB = BasicBlock::Create(Ctx, "do_sample", Wrapper);
    BasicBlock *SkipSampleBB = BasicBlock::Create(Ctx, "skip_sample", Wrapper);
    // Removed unused CheckConvergenceBB
    BasicBlock *FindBestBB = BasicBlock::Create(Ctx, "find_best", Wrapper);
    BasicBlock *OptimalBB = BasicBlock::Create(Ctx, "optimal", Wrapper);

    IRBuilder<> Builder(Ctx);

    // =========================================================================
    // ENTRY: Atomic counter increment
    // =========================================================================
    Builder.SetInsertPoint(EntryBB);

    // callCounter is thread-local; use regular load/add/store
    Value *OldCount = Builder.CreateLoad(i32Ty, metadata.callCounter);
    Value *Count = Builder.CreateAdd(OldCount, one32, "call_count");
    Builder.CreateStore(Count, metadata.callCounter);

    Builder.CreateBr(CheckPhaseBB);

    // =========================================================================
    // CHECK PHASE: Profiling or Optimal?
    // =========================================================================
    Builder.SetInsertPoint(CheckPhaseBB);

    LoadInst *PhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase, "phase");
    PhaseLoad->setAtomic(AtomicOrdering::Acquire);

    Value *IsOptimal = Builder.CreateICmpEQ(PhaseLoad, one32, "is_optimal");
    Builder.CreateCondBr(IsOptimal, OptimalBB, ProfilingBB);

    // =========================================================================
    // PROFILING PHASE
    // =========================================================================
    Builder.SetInsertPoint(ProfilingBB);

    // FIX 2: Safety check - force finalization if max samples exceeded
    BasicBlock *ForceFindBestBB = BasicBlock::Create(Ctx, "force_find_best", Wrapper);
    Value *ExceededMax = Builder.CreateICmpUGE(Count, maxSamples, "exceeded_max");
    Builder.CreateCondBr(ExceededMax, ForceFindBestBB, ShouldSampleBB);

    // =========================================================================
    // SHOULD SAMPLE: Check if count % SAMPLE_RATE == 0
    // =========================================================================
    Builder.SetInsertPoint(ShouldSampleBB);

    Value *Remainder = Builder.CreateURem(Count, sampleRate, "remainder");
    Value *ShouldSample = Builder.CreateICmpEQ(Remainder, zero32, "should_sample");

    // If we should sample, go sample; otherwise skip
    Builder.CreateCondBr(ShouldSample, DoSampleBB, SkipSampleBB);

    // =========================================================================
    // DO SAMPLE: Profile one version
    // =========================================================================
    Builder.SetInsertPoint(DoSampleBB);

    Value *SampleNum = Builder.CreateUDiv(Count, sampleRate, "sample_num");
    Value *VersionIdx = Builder.CreateURem(SampleNum, numVersions, "version_idx");

    Value *StartTime = Builder.CreateCall(Rdtsc, {}, "start_time");

    BasicBlock *CallMergeBB = BasicBlock::Create(Ctx, "call_merge", Wrapper);
    std::vector<BasicBlock *> CallBBs;

    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        CallBBs.push_back(BasicBlock::Create(Ctx, "call_v" + std::to_string(i), Wrapper));
    }

    SwitchInst *CallSwitch = Builder.CreateSwitch(VersionIdx, CallBBs[0], NUM_VERSIONS);
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        CallSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), CallBBs[i]);
    }

    // Generate call blocks for each version
    std::vector<Value *> CallResults;
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        Builder.SetInsertPoint(CallBBs[i]);

        std::vector<Value *> Args;
        for (auto &Arg : Wrapper->args())
        {
            Args.push_back(&Arg);
        }

        CallInst *Call = Builder.CreateCall(
            metadata.versions[i]->getFunctionType(),
            metadata.versions[i],
            Args,
            Orig->getReturnType()->isVoidTy() ? "" : "result");
        Call->setCallingConv(metadata.versions[i]->getCallingConv());
        Call->setAttributes(metadata.versions[i]->getAttributes());

        CallResults.push_back(Call);
        Builder.CreateBr(CallMergeBB);
    }

    // =========================================================================
    // UPDATE STATS: Only update the executed version
    // =========================================================================
    Builder.SetInsertPoint(CallMergeBB);

    PHINode *ResultPhi = nullptr;
    if (!Orig->getReturnType()->isVoidTy())
    {
        ResultPhi = Builder.CreatePHI(Orig->getReturnType(), NUM_VERSIONS, "result");
        for (int i = 0; i < NUM_VERSIONS; i++)
        {
            ResultPhi->addIncoming(CallResults[i], CallBBs[i]);
        }
    }

    Value *EndTime = Builder.CreateCall(Rdtsc, {}, "end_time");
    Value *ElapsedCycles = Builder.CreateSub(EndTime, StartTime, "elapsed");

    BasicBlock *UpdateMergeBB = BasicBlock::Create(Ctx, "update_merge", Wrapper);
    std::vector<BasicBlock *> UpdateBBs;

    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        UpdateBBs.push_back(BasicBlock::Create(Ctx, "update_v" + std::to_string(i), Wrapper));
    }

    SwitchInst *UpdateSwitch = Builder.CreateSwitch(VersionIdx, UpdateBBs[0], NUM_VERSIONS);
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        UpdateSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), UpdateBBs[i]);
    }

    std::vector<Value *> NewRunCounts;
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        Builder.SetInsertPoint(UpdateBBs[i]);

        // Update with atomics since we removed sampling lock
        Builder.CreateAtomicRMW(
            AtomicRMWInst::Add,
            metadata.cpuCycles[i],
            ElapsedCycles,
            MaybeAlign(8),
            AtomicOrdering::Monotonic);

        Value *NewRuns = Builder.CreateAtomicRMW(
            AtomicRMWInst::Add,
            metadata.runCount[i],
            one32,
            MaybeAlign(4),
            AtomicOrdering::Monotonic);
        NewRuns = Builder.CreateAdd(NewRuns, one32, "new_runs");
        NewRunCounts.push_back(NewRuns);

        Builder.CreateBr(UpdateMergeBB);
    }

    // =========================================================================
    // CHECK CONVERGENCE: Should we find best version?
    // =========================================================================
    Builder.SetInsertPoint(UpdateMergeBB);

    PHINode *RunCountPhi = Builder.CreatePHI(i32Ty, NUM_VERSIONS, "run_count");
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        RunCountPhi->addIncoming(NewRunCounts[i], UpdateBBs[i]);
    }

    Value *ShouldCheck = Builder.CreateICmpEQ(
        Builder.CreateURem(RunCountPhi, ConstantInt::get(i32Ty, 10)),
        zero32,
        "should_check");

    BasicBlock *DoCheckBB = BasicBlock::Create(Ctx, "do_check", Wrapper);
    BasicBlock *ReturnSampleBB = BasicBlock::Create(Ctx, "return_sample", Wrapper);

    Builder.CreateCondBr(ShouldCheck, DoCheckBB, ReturnSampleBB);

    Builder.SetInsertPoint(DoCheckBB);

    Value *AllHaveMin = Builder.getTrue();
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        LoadInst *Runs = Builder.CreateLoad(i32Ty, metadata.runCount[i]);
        Runs->setAtomic(AtomicOrdering::Monotonic);

        Value *HasMin = Builder.CreateICmpUGE(Runs, minSamples);
        AllHaveMin = Builder.CreateAnd(AllHaveMin, HasMin);
    }

    Builder.CreateCondBr(AllHaveMin, FindBestBB, ReturnSampleBB);

    // =========================================================================
    // FORCE FIND BEST: Safety mechanism when max samples exceeded
    // =========================================================================
    Builder.SetInsertPoint(ForceFindBestBB);
    Builder.CreateBr(FindBestBB);

    // =========================================================================
    // FIND BEST: Calculate averages and select best version
    // =========================================================================
    Builder.SetInsertPoint(FindBestBB);

    std::vector<Value *> Cycles;
    std::vector<Value *> Runs;

    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        LoadInst *CyclesLoad = Builder.CreateLoad(i64Ty, metadata.cpuCycles[i]);
        CyclesLoad->setAtomic(AtomicOrdering::Monotonic);
        Cycles.push_back(CyclesLoad);

        LoadInst *RunsLoad = Builder.CreateLoad(i32Ty, metadata.runCount[i]);
        RunsLoad->setAtomic(AtomicOrdering::Monotonic);
        Runs.push_back(RunsLoad);
    }

    // Calculate averages and find best
    std::vector<Value *> Averages;
    Value *BestVersion = zero32;
    Value *BestAvg = nullptr;

    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        // FIX 3: Division-by-zero protection
        Value *IsZero = Builder.CreateICmpEQ(Runs[i], zero32, "is_zero");
        Value *SafeRuns = Builder.CreateSelect(IsZero, one32, Runs[i], "safe_runs");

        Value *Runs64 = Builder.CreateZExt(SafeRuns, i64Ty);
        Value *Avg = Builder.CreateUDiv(Cycles[i], Runs64);

        // If runs was zero, set average to max value (worst performance)
        Value *MaxAvg = ConstantInt::get(i64Ty, UINT64_MAX);
        Avg = Builder.CreateSelect(IsZero, MaxAvg, Avg, "safe_avg");

        Averages.push_back(Avg);

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
    }

    // FIX 4: REMOVED printf - it causes hanging in multi-threaded builds
    // Original code (REMOVED):
    // FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
    //                                              {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
    // FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
    // Value *Msg = Builder.CreateGlobalStringPtr("[BEST] Version %d selected...\n");
    // Builder.CreateCall(Printf, {Msg, BestVersion, ...});

    // Store best version
    StoreInst *StoreBest = Builder.CreateStore(BestVersion, metadata.bestVersion);
    StoreBest->setAtomic(AtomicOrdering::Release);

    // Mark as optimal phase
    StoreInst *StorePhase = Builder.CreateStore(one32, metadata.currentPhase);
    StorePhase->setAtomic(AtomicOrdering::Release);

    Builder.CreateBr(ReturnSampleBB);

    // =========================================================================
    // RETURN from sampling
    // =========================================================================
    Builder.SetInsertPoint(ReturnSampleBB);
    // No lock to release

    if (Orig->getReturnType()->isVoidTy())
    {
        Builder.CreateRetVoid();
    }
    else
    {
        Builder.CreateRet(ResultPhi);
    }

    // =========================================================================
    // SKIP SAMPLE: Just call baseline version
    // =========================================================================
    Builder.SetInsertPoint(SkipSampleBB);

    std::vector<Value *> BaselineArgs;
    for (auto &Arg : Wrapper->args())
    {
        BaselineArgs.push_back(&Arg);
    }

    CallInst *BaselineCall = Builder.CreateCall(
        metadata.versions[0]->getFunctionType(),
        metadata.versions[0],
        BaselineArgs,
        Orig->getReturnType()->isVoidTy() ? "" : "baseline_result");
    BaselineCall->setCallingConv(metadata.versions[0]->getCallingConv());
    BaselineCall->setTailCall(true);

    if (Orig->getReturnType()->isVoidTy())
    {
        Builder.CreateRetVoid();
    }
    else
    {
        Builder.CreateRet(BaselineCall);
    }

    // =========================================================================
    // OPTIMAL PHASE: Direct dispatch to best version
    // =========================================================================
    Builder.SetInsertPoint(OptimalBB);

    LoadInst *BestVersionLoad = Builder.CreateLoad(i32Ty, metadata.bestVersion, "best");
    BestVersionLoad->setAtomic(AtomicOrdering::Acquire); // FIX 5: Changed from Monotonic to Acquire

    BasicBlock *OptDefaultBB = BasicBlock::Create(Ctx, "opt_default", Wrapper);
    std::vector<BasicBlock *> OptBBs;

    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        OptBBs.push_back(BasicBlock::Create(Ctx, "opt_v" + std::to_string(i), Wrapper));
    }

    SwitchInst *OptSwitch = Builder.CreateSwitch(BestVersionLoad, OptDefaultBB, NUM_VERSIONS);
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        OptSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), OptBBs[i]);
    }

    // Generate optimal call blocks
    for (int i = 0; i < NUM_VERSIONS; i++)
    {
        Builder.SetInsertPoint(OptBBs[i]);

        std::vector<Value *> Args;
        for (auto &Arg : Wrapper->args())
        {
            Args.push_back(&Arg);
        }

        CallInst *Call = Builder.CreateCall(
            metadata.versions[i]->getFunctionType(),
            metadata.versions[i],
            Args,
            Orig->getReturnType()->isVoidTy() ? "" : "result");
        Call->setCallingConv(metadata.versions[i]->getCallingConv());
        Call->setAttributes(metadata.versions[i]->getAttributes());
        Call->setTailCall(true);

        if (Orig->getReturnType()->isVoidTy())
        {
            Builder.CreateRetVoid();
        }
        else
        {
            Builder.CreateRet(Call);
        }
    }

    // Default case (fallback to version 0)
    Builder.SetInsertPoint(OptDefaultBB);

    std::vector<Value *> DefaultArgs;
    for (auto &Arg : Wrapper->args())
    {
        DefaultArgs.push_back(&Arg);
    }

    CallInst *DefaultCall = Builder.CreateCall(
        metadata.versions[0]->getFunctionType(),
        metadata.versions[0],
        DefaultArgs);
    DefaultCall->setTailCall(true);

    if (Orig->getReturnType()->isVoidTy())
    {
        Builder.CreateRetVoid();
    }
    else
    {
        Builder.CreateRet(DefaultCall);
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

    // Print initialization message (intentionally disabled to avoid I/O during init)
    Builder.CreateRetVoid();

    appendToGlobalCtors(M, InitFunc, 65535);
}

} // end namespace llvm