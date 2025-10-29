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
        Function *versions[2]; // v0 and v1
        Function *wrapper;
        uint32_t functionId;
        FunctionType *signature;

        // Per-function static globals for adaptive runtime tracking
        GlobalVariable *callCounter;  // call count
        GlobalVariable *cpuCycles[2]; // accumulated cpu cycles per version
        GlobalVariable *runCount[2];  // runs per version
        GlobalVariable *bestVersion;  // best version index after profiling
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
        errs() << "=== SimpleAdaptive Pass Running ===\n";
        functionMap.clear();

        // Collect adaptive functions
        std::vector<Function *> adaptiveFunctions;
        for (Function &F : M)
        {
            if (!F.isDeclaration() && F.hasFnAttribute("adaptive"))
            {
                errs() << "[ADAPTIVE] Found: " << F.getName() << "\n";
                adaptiveFunctions.push_back(&F);
            }
        }
        if (adaptiveFunctions.empty())
        {
            errs() << "No adaptive functions found in this module\n";
            return PreservedAnalyses::all();
        }

        uint32_t funcId = 0;
        for (Function *F : adaptiveFunctions)
        {
            processFunctionWithDirectDispatch(F, funcId++, M);
        }

        createInitializationFunction(M);

        errs() << "=== Transformation Complete ===\n";
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

        for (int v = 0; v < 2; v++)
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

        errs() << "  Created static variables for " << baseName << "\n";
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
			Value *FuncName = RetBuilder.CreateGlobalStringPtr(F->getName());
			Value *CyclesMsg = RetBuilder.CreateGlobalStringPtr(
				"[CYCLES] %s_v%d: This run=%lu cycles, Total cycles=%lu, Total runs=%d\n");
			RetBuilder.CreateCall(Printf, {CyclesMsg, FuncName, 
										ConstantInt::get(Type::getInt32Ty(Ctx), versionId),
										ElapsedCycles, NewCycles, NewRuns});
		}
	}

    Function *SimpleAdaptivePassImpl::createVersion(Function *F, int versionId, Module &M)
    {
        ValueToValueMapTy VMap;
        Function *NewF = CloneFunction(F, VMap);
        NewF->setName(F->getName() + "_v" + std::to_string(versionId));
        NewF->setLinkage(GlobalValue::InternalLinkage);

        if (versionId == 1)
        {
            NewF->addFnAttr("prefer-vector-width", "256");
            NewF->addFnAttr("target-features", "+avx2");
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

        metadata.versions[0] = createVersion(F, 0, M);
        metadata.versions[1] = createVersion(F, 1, M);

        createWrapperStaticVars(metadata, M);
//         addCPUCycleMeasurement(metadata.versions[0], metadata, 0, M);
//         addCPUCycleMeasurement(metadata.versions[1], metadata, 1, M);

        metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

        F->replaceAllUsesWith(metadata.wrapper);
        metadata.wrapper->takeName(F);

        F->setName(F->getName().str() + "_original");
        F->setLinkage(GlobalValue::InternalLinkage);

        functionMap[F] = metadata;

        errs() << "Processed function " << funcId << ": " << F->getName() << "\n";
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
	{
		LLVMContext &Ctx = M.getContext();
		Function *Orig = metadata.original;

		Function *Wrapper = Function::Create(
			Orig->getFunctionType(), Orig->getLinkage(), Orig->getName() + "_wrapper", &M);

		BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
		IRBuilder<> Builder(Entry);

		FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
													{PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
		FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

		// ADD: Get rdtsc intrinsic for cycle measurement in wrapper
		FunctionType *RdtscType = FunctionType::get(Type::getInt64Ty(Ctx), {}, false);
		FunctionCallee Rdtsc = M.getOrInsertFunction("llvm.readcyclecounter", RdtscType);

		Value *Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.callCounter);
		Value *NewCount = Builder.CreateAdd(Count, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
		Builder.CreateStore(NewCount, metadata.callCounter);

		// ADDED: Print total call count
		printf("check\n");
		// Value *CallCountMsg = Builder.CreateGlobalStringPtr("[CALL_COUNT] %s: Total calls=%d\n");
		Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName());
		// Builder.CreateCall(Printf, {CallCountMsg, FuncName, NewCount});

		Value *Phase = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.currentPhase);

		BasicBlock *ProfPhaseBB = BasicBlock::Create(Ctx, "profiling_phase", Wrapper);
		BasicBlock *OptPhaseBB = BasicBlock::Create(Ctx, "optimal_phase", Wrapper);
		Builder.CreateCondBr(
			Builder.CreateICmpEQ(Phase, ConstantInt::get(Type::getInt32Ty(Ctx), 1)), OptPhaseBB, ProfPhaseBB);

		// Profiling phase: alternate versions and measure
		Builder.SetInsertPoint(ProfPhaseBB);
		Value *Version = Builder.CreateURem(Count, ConstantInt::get(Type::getInt32Ty(Ctx), 2));

		Value *ShouldUpdateBest = Builder.CreateICmpEQ(NewCount, ConstantInt::get(Type::getInt32Ty(Ctx), 101));   // Best version should be used from 101st run

		BasicBlock *ContinueProfBB = BasicBlock::Create(Ctx, "continue_prof", Wrapper);
		BasicBlock *UpdateBestBB = BasicBlock::Create(Ctx, "update_best", Wrapper);

		Builder.CreateCondBr(ShouldUpdateBest, UpdateBestBB, ContinueProfBB);

		Builder.SetInsertPoint(ContinueProfBB);
		{
			Value *IsVersion1 = Builder.CreateICmpEQ(Version, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
			Value *FuncV0 = Builder.CreatePointerCast(metadata.versions[0], Orig->getFunctionType()->getPointerTo());
			Value *FuncV1 = Builder.CreatePointerCast(metadata.versions[1], Orig->getFunctionType()->getPointerTo());
			Value *FuncPtr = Builder.CreateSelect(IsVersion1, FuncV1, FuncV0);
			
			// ADD: Measure cycles in wrapper before calling
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

			// ADD: Measure end time and print cycles
			Value *EndTime = Builder.CreateCall(Rdtsc);
			Value *ElapsedCycles = Builder.CreateSub(EndTime, StartTime);
			
			// Value *CycleMsg = Builder.CreateGlobalStringPtr("[CYCLES] Call %d took %lu cycles (version %d)\n");
			// Builder.CreateCall(Printf, {CycleMsg, NewCount, ElapsedCycles, Version});

			// ADD: Update cycle counters for the called version
			// Create a select to choose which counter to update
			Value *CurrentCyclesV0 = Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[0]);
			Value *CurrentCyclesV1 = Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[1]);
			Value *NewCyclesV0 = Builder.CreateAdd(CurrentCyclesV0, ElapsedCycles);
			Value *NewCyclesV1 = Builder.CreateAdd(CurrentCyclesV1, ElapsedCycles);
			
			// Store to appropriate version based on selection
			Builder.CreateStore(Builder.CreateSelect(IsVersion1, CurrentCyclesV0, NewCyclesV0), metadata.cpuCycles[0]);
			Builder.CreateStore(Builder.CreateSelect(IsVersion1, NewCyclesV1, CurrentCyclesV1), metadata.cpuCycles[1]);

			// ADD: Update run counts
			Value *CurrentRunsV0 = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[0]);
			Value *CurrentRunsV1 = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[1]);
			Value *NewRunsV0 = Builder.CreateAdd(CurrentRunsV0, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
			Value *NewRunsV1 = Builder.CreateAdd(CurrentRunsV1, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
			
			Builder.CreateStore(Builder.CreateSelect(IsVersion1, CurrentRunsV0, NewRunsV0), metadata.runCount[0]);
			Builder.CreateStore(Builder.CreateSelect(IsVersion1, NewRunsV1, CurrentRunsV1), metadata.runCount[1]);

			// Print updated counters
			Value *UpdatedMsg = Builder.CreateGlobalStringPtr("[UPDATED] V0: %lu cycles, %d runs | V1: %lu cycles, %d runs\n");
			Builder.CreateCall(Printf, {UpdatedMsg, 
									Builder.CreateSelect(IsVersion1, CurrentCyclesV0, NewCyclesV0),
									Builder.CreateSelect(IsVersion1, CurrentRunsV0, NewRunsV0),
									Builder.CreateSelect(IsVersion1, NewCyclesV1, CurrentCyclesV1),
									Builder.CreateSelect(IsVersion1, NewRunsV1, CurrentRunsV1)});

			if (Orig->getReturnType()->isVoidTy()) {
				Builder.CreateRetVoid();
			} else {
				Builder.CreateRet(Result);
			}
		}

		Builder.SetInsertPoint(UpdateBestBB);
		{
			Value *V0Cycles = Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[0]);
			Value *V1Cycles = Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[1]);
			Value *V0Runs = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[0]);
			Value *V1Runs = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[1]);
			Value *V0Runs64 = Builder.CreateZExt(V0Runs, Type::getInt64Ty(Ctx));
			Value *V1Runs64 = Builder.CreateZExt(V1Runs, Type::getInt64Ty(Ctx));
			Value *V0Avg = Builder.CreateUDiv(V0Cycles, V0Runs64);
			Value *V1Avg = Builder.CreateUDiv(V1Cycles, V1Runs64);

			// ADDED: Print detailed statistics before computing best version
			Value *StatsMsg = Builder.CreateGlobalStringPtr(
				"[STATS] %s - V0: total_cycles=%lu, runs=%d, avg=%lu | V1: total_cycles=%lu, runs=%d, avg=%lu\n");
			Builder.CreateCall(Printf, {StatsMsg, FuncName, 
									V0Cycles, V0Runs, V0Avg, 
									V1Cycles, V1Runs, V1Avg});

			Value *V0Better = Builder.CreateICmpULT(V0Avg, V1Avg);
			Value *BestVersion = Builder.CreateSelect(V0Better, ConstantInt::get(Type::getInt32Ty(Ctx), 0), ConstantInt::get(Type::getInt32Ty(Ctx), 1));
			Builder.CreateStore(BestVersion, metadata.bestVersion);
			Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Ctx), 1), metadata.currentPhase);

			Value *Msg = Builder.CreateGlobalStringPtr("[BEST] Computed best version %d\n");
			Builder.CreateCall(Printf, {Msg, BestVersion});

			// ADD: Measure cycles for this best version call too
			Value *BestStartTime = Builder.CreateCall(Rdtsc);
			
			// Dispatch to best version now
			Value *IsBestVersion1 = Builder.CreateICmpEQ(BestVersion, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
			Value *FuncV0 = Builder.CreatePointerCast(metadata.versions[0], Orig->getFunctionType()->getPointerTo());
			Value *FuncV1 = Builder.CreatePointerCast(metadata.versions[1], Orig->getFunctionType()->getPointerTo());
			Value *FuncPtr = Builder.CreateSelect(IsBestVersion1, FuncV1, FuncV0);
			
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

			// ADD: Measure best version cycles
			Value *BestEndTime = Builder.CreateCall(Rdtsc);
			Value *BestElapsed = Builder.CreateSub(BestEndTime, BestStartTime);
			Value *BestCycleMsg = Builder.CreateGlobalStringPtr("[BEST_CYCLES] Best version call took %lu cycles\n");
			Builder.CreateCall(Printf, {BestCycleMsg, BestElapsed});

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
			
			// ADD: Measure cycles in optimal phase
			Value *OptimalStartTime = Builder.CreateCall(Rdtsc);
			
			// ADDED: Print current run counts and cycles in optimal phase
			Value *V0Cycles = Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[0]);
			Value *V1Cycles = Builder.CreateLoad(Type::getInt64Ty(Ctx), metadata.cpuCycles[1]);
			Value *V0Runs = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[0]);
			Value *V1Runs = Builder.CreateLoad(Type::getInt32Ty(Ctx), metadata.runCount[1]);
			
			Value *OptimalStatsMsg = Builder.CreateGlobalStringPtr(
				"[OPTIMAL_STATS] %s - V0: cycles=%lu, runs=%d | V1: cycles=%lu, runs=%d | Using version %d\n");
			Builder.CreateCall(Printf, {OptimalStatsMsg, FuncName, 
									V0Cycles, V0Runs, V1Cycles, V1Runs, BestVersion});

			Value *FuncV0 = Builder.CreatePointerCast(metadata.versions[0], Orig->getFunctionType()->getPointerTo());
			Value *FuncV1 = Builder.CreatePointerCast(metadata.versions[1], Orig->getFunctionType()->getPointerTo());

			Value *FuncPtr = Builder.CreateSelect(
				Builder.CreateICmpEQ(BestVersion, ConstantInt::get(BestVersion->getType(), 0)),
				FuncV0,
				FuncV1);

			Value *Msg = Builder.CreateGlobalStringPtr("[OPTIMAL] Using best version %d\n");
			Builder.CreateCall(Printf, {Msg, BestVersion});

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

			// ADD: Measure optimal phase cycles
			Value *OptimalEndTime = Builder.CreateCall(Rdtsc);
			Value *OptimalElapsed = Builder.CreateSub(OptimalEndTime, OptimalStartTime);
			Value *OptimalCycleMsg = Builder.CreateGlobalStringPtr("[OPTIMAL_CYCLES] Call took %lu cycles\n");
			Builder.CreateCall(Printf, {OptimalCycleMsg, OptimalElapsed});

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
        FunctionCallee Printf = M.getOrInsertFunction("printf",
                                                  FunctionType::get(Type::getInt32Ty(Ctx), {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true));
        Value *InitMsg = Builder.CreateGlobalStringPtr("[INIT] Adaptive dispatcher initialized\n");
        Builder.CreateCall(Printf, {InitMsg});
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm
