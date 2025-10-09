#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/raw_ostream.h"
#include <typeinfo>

using namespace llvm;

// Performance tracking structure
struct PerformanceMetrics {
    int call_count_v0;
    int call_count_v1;
    Function* func_v0;
    Function* func_v1;
    long long total_cycles_v0;
    long long total_cycles_v1;
    Function* best_version;
    double avg_cycles_v0;
    double avg_cycles_v1;
    
    PerformanceMetrics() : call_count_v0(0), call_count_v1(0), 
                          func_v0(nullptr), func_v1(nullptr),
                          total_cycles_v0(0), total_cycles_v1(0),
                          best_version(nullptr), avg_cycles_v0(0.0), avg_cycles_v1(0.0) {}
    
    void updateMetrics(int version, long long cycles) {
        if (version == 0) {
            call_count_v0++;
            total_cycles_v0 += cycles;
            avg_cycles_v0 = (double)total_cycles_v0 / call_count_v0;
        } else {
            call_count_v1++;
            total_cycles_v1 += cycles;
            avg_cycles_v1 = (double)total_cycles_v1 / call_count_v1;
        }
        
        // Update best version based on average cycles (lower is better)
        if (call_count_v0 > 0 && call_count_v1 > 0) {
            best_version = (avg_cycles_v0 <= avg_cycles_v1) ? func_v0 : func_v1;
        } else if (call_count_v0 > 0) {
            best_version = func_v0;
        } else if (call_count_v1 > 0) {
            best_version = func_v1;
        }
    }
    
    void setFunctions(Function* v0, Function* v1) {
        func_v0 = v0;
        func_v1 = v1;
        if (!best_version) best_version = v0; // Default to v0
    }
};

namespace {

class SimpleAdaptivePass : public PassInfoMixin<SimpleAdaptivePass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        errs() << "=== SimpleAdaptive Pass Running ===\n";
        
        // Find the target function
        Function* AdaptiveFunc = nullptr;
        /*
        for (Function &F : M) {
            errs() << "Attribute:" << F.hasFnAttribute("adaptive") << " " << F.getName() << "\n";
        }
        */
        // lowered to the IR string attribute "adaptive".
        for (Function &F : M) {
            if (!F.isDeclaration() && F.hasFnAttribute("adaptive")) {
                errs() << "[ADAPTIVE] Found annotated function 1: "
                       << F.getName() << "\n";
                AdaptiveFunc = &F;
                break; // Use the first match; adjust if you want all
            }
        }
        
        if (!AdaptiveFunc) {
            errs() << "Target function not found!\n";
            return PreservedAnalyses::all();
        }
        
        errs() << "Found function: " << AdaptiveFunc->getName() << "\n";
        StringRef name = AdaptiveFunc->getName();
        //errs() << name << "\n";
        std::string s = name.str();
        //errs() << s << "\n";

        Function* V0 = createBaselineVersion(AdaptiveFunc, 0, M);
        Function* V1 = createOptimizedVersion(AdaptiveFunc, 1, M);

        // Add print statements to versions
        addPrintToVersion(V0, 0, M);
        addPrintToVersion(V1, 1, M);
        
       // Create global performance metrics structure
       GlobalVariable* GlobalMetrics = createGlobalMetrics(V0, V1, M);
        
        // Create dispatcher with debug output
        // Function* Dispatcher = createDispatcherWithDebug(AdaptiveFunc, V0, V1, M);
        
        
        Function* Dispatcher = createDispatcher(AdaptiveFunc, V0, V1, M);
        
        Function* Wrapper = createWrapper(AdaptiveFunc, Dispatcher, M);
        
        AdaptiveFunc->replaceAllUsesWith(Wrapper);
        Wrapper->takeName(AdaptiveFunc);
        AdaptiveFunc->setName(s + "_original");
        AdaptiveFunc->setLinkage(GlobalValue::InternalLinkage);
        
        errs() << "=== Transformation Complete ===\n";
        return PreservedAnalyses::none();
    }
    
private:

    Function* createBaselineVersion(Function* F, int versionId, Module& M) {
        ValueToValueMapTy VMap;
        Function* NewF = CloneFunction(F, VMap);
        NewF->setName(F->getName() + "_v" + std::to_string(versionId));
        M.getFunctionList().push_back(NewF);
        return NewF;
    }
    
    Function* createOptimizedVersion(Function* F, int versionId, Module& M) {
        ValueToValueMapTy VMap;
        Function* NewF = CloneFunction(F, VMap);
        NewF->setName(F->getName() + "_v" + std::to_string(versionId));
        
        NewF->addFnAttr("prefer-vector-width", "256");
        NewF->addFnAttr("no-unroll-loops", "false");
        
        M.getFunctionList().push_back(NewF);
        return NewF;
    }
    
    // Create global PerformanceMetrics structure
    GlobalVariable* createGlobalMetrics(Function* V0, Function* V1, Module& M) {
        LLVMContext &Ctx = M.getContext();
        
        // Create a constant struct initializer for PerformanceMetrics
        StructType* MetricsType = createMetricsStructType(Ctx);
        
        // Initialize with zeros and function pointers
        Constant* Zero32 = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
        Constant* Zero64 = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
        Constant* ZeroDouble = ConstantFP::get(Type::getDoubleTy(Ctx), 0.0);
        Constant* NullFunc = ConstantPointerNull::get(Type::getInt8PtrTy(Ctx));
        
        std::vector<Constant*> Elements = {
            Zero32,     // call_count_v0
            Zero32,     // call_count_v1
            ConstantExpr::getBitCast(V0, Type::getInt8PtrTy(Ctx)),  // func_v0
            ConstantExpr::getBitCast(V1, Type::getInt8PtrTy(Ctx)),  // func_v1
            Zero64,     // total_cycles_v0
            Zero64,     // total_cycles_v1
            ConstantExpr::getBitCast(V0, Type::getInt8PtrTy(Ctx)),  // best_version (default to V0)
            ZeroDouble, // avg_cycles_v0
            ZeroDouble  // avg_cycles_v1
        };
        
        Constant* Init = ConstantStruct::get(MetricsType, Elements);
        
        GlobalVariable* GlobalMetrics = new GlobalVariable(
            M,
            MetricsType,
            false, // isConstant
            GlobalValue::InternalLinkage,
            Init,
            "adaptive_metrics"
        );
        
        return GlobalMetrics;
    }

    // Create metrics struct type
    StructType* createMetricsStructType(LLVMContext &Ctx) {
        std::vector<Type*> Elements = {
            Type::getInt32Ty(Ctx),    // call_count_v0
            Type::getInt32Ty(Ctx),    // call_count_v1
            Type::getInt8PtrTy(Ctx)->getPointerTo(),  // func_v0
            Type::getInt8PtrTy(Ctx)->getPointerTo(),  // func_v1
            Type::getInt64Ty(Ctx),    // total_cycles_v0
            Type::getInt64Ty(Ctx),    // total_cycles_v1
            Type::getInt8PtrTy(Ctx)->getPointerTo(),  // best_version (default to V0)
            Type::getDoubleTy(Ctx),   // avg_cycles_v0
            Type::getDoubleTy(Ctx)    // avg_cycles_v1
        };
        return StructType::create(Ctx, Elements);
    }

    // Add printf at the beginning of each version
    void addPrintToVersion(Function* F, int versionId, Module& M) {
        LLVMContext &Ctx = M.getContext();
        
        // Get or declare printf
        FunctionType* PrintfType = FunctionType::get(
            Type::getInt32Ty(Ctx),
            {Type::getInt32Ty(Ctx)->getPointerTo()},
            true  // variadic
        );
        
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        
        // Get entry block
        BasicBlock &EntryBB = F->getEntryBlock();
        IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());
        
        // Create format string
        std::string FormatStr = "[DISPATCHER] Calling Version " + 
                               std::to_string(versionId) + " of %s\n";
        Value* FormatStrVal = Builder.CreateGlobalStringPtr(FormatStr);
        
        // Create function name string
        Value* FuncNameVal = Builder.CreateGlobalStringPtr(F->getName().str());
        
        // Call printf
        Builder.CreateCall(Printf, {FormatStrVal, FuncNameVal});
    }
    
    Function* createDispatcherWithDebug(Function* Original, Function* V0, 
                                        Function* V1, Module& M) {
        LLVMContext &Ctx = M.getContext();
        
        Function* Dispatcher = Function::Create(
            Original->getFunctionType(),
            GlobalValue::InternalLinkage,
            Original->getName() + "_dispatcher",
            &M
        );
        
        // Get printf
        FunctionType* PrintfType = FunctionType::get(
            Type::getInt32Ty(Ctx),
            {Type::getInt32Ty(Ctx)->getPointerTo()},
            true
        );
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

        // Declare a clock function to measure time (mangled system_clock::now)
        FunctionCallee ChronoNow = M.getOrInsertFunction(
            "_ZNSt6chrono3_V212system_clock3nowEv",
            FunctionType::get(Type::getInt64Ty(Ctx), false)
        );

        // Performance tracking globals (match sample structure)
        GlobalVariable* CallCountV0 = new GlobalVariable(
            M,
            Type::getInt32Ty(Ctx),
            false,
            GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            "call_count_v0"
        );

        GlobalVariable* CallCountV1 = new GlobalVariable(
            M,
            Type::getInt32Ty(Ctx),
            false,
            GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            "call_count_v1"
        );

        GlobalVariable* TotalCyclesV0 = new GlobalVariable(
            M,
            Type::getInt64Ty(Ctx),
            false,
            GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt64Ty(Ctx), 0),
            "total_cycles_v0"
        );

        GlobalVariable* TotalCyclesV1 = new GlobalVariable(
            M,
            Type::getInt64Ty(Ctx),
            false,
            GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt64Ty(Ctx), 0),
            "total_cycles_v1"
        );

        BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
        IRBuilder<> Builder(Entry);

        // Load current counters and totals
        Value* CountV0 = Builder.CreateLoad(Type::getInt32Ty(Ctx), CallCountV0);
        Value* CountV1 = Builder.CreateLoad(Type::getInt32Ty(Ctx), CallCountV1);
        Value* TotalV0 = Builder.CreateLoad(Type::getInt64Ty(Ctx), TotalCyclesV0);
        Value* TotalV1 = Builder.CreateLoad(Type::getInt64Ty(Ctx), TotalCyclesV1);

        // Compute whether we have enough samples from both versions
        Value* MinSamples = ConstantInt::get(Type::getInt32Ty(Ctx), 10);
        Value* HasEnoughV0 = Builder.CreateICmpUGE(CountV0, MinSamples);
        Value* HasEnoughV1 = Builder.CreateICmpUGE(CountV1, MinSamples);
        Value* HasEnoughBoth = Builder.CreateAnd(HasEnoughV0, HasEnoughV1);

        // Compute average cycles for each version (integer division)
        Value* CountV0_i64 = Builder.CreateZExt(CountV0, Type::getInt64Ty(Ctx));
        Value* CountV1_i64 = Builder.CreateZExt(CountV1, Type::getInt64Ty(Ctx));
        Value* AvgV0 = Builder.CreateUDiv(TotalV0, CountV0_i64);
        Value* AvgV1 = Builder.CreateUDiv(TotalV1, CountV1_i64);
        Value* V1IsBetter = Builder.CreateICmpULT(AvgV1, AvgV0);

        // Fallback alternating strategy when not enough samples: use total parity
        Value* TotalCalls = Builder.CreateAdd(CountV0, CountV1);
        Value* One = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
        Value* Parity = Builder.CreateAnd(TotalCalls, One);
        Value* AlternateUseV1 = Builder.CreateICmpEQ(Parity, One);

        // Final decision: if enough samples choose best, else alternate
        Value* UseV1 = Builder.CreateSelect(HasEnoughBoth, V1IsBetter, AlternateUseV1);

        // Blocks
        BasicBlock* PrintBestVersion = BasicBlock::Create(Ctx, "print_best_version", Dispatcher);
        BasicBlock* VersionSelection = BasicBlock::Create(Ctx, "version_selection", Dispatcher);
        BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
        BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
        BasicBlock* EndBlock = BasicBlock::Create(Ctx, "end", Dispatcher);

        // Print call number / basic header
        Value* CallNumStr = Builder.CreateGlobalStringPtr("[DISPATCHER] Total calls so far: %d\n");
        Builder.CreateCall(Printf, {CallNumStr, TotalCalls});

        // Branch: if enough samples, print best version first, else go to selection
        Builder.CreateCondBr(HasEnoughBoth, PrintBestVersion, VersionSelection);

        // PrintBestVersion block
        Builder.SetInsertPoint(PrintBestVersion);
        Value* BestVersionMsg = Builder.CreateGlobalStringPtr("BEST VERSION: %s (Avg: %lld cycles)\n");
        Value* BestVersionName = Builder.CreateSelect(
            V1IsBetter,
            Builder.CreateGlobalStringPtr("V1 (Optimized)"),
            Builder.CreateGlobalStringPtr("V0 (Baseline)")
        );
        Value* BestAvgCycles = Builder.CreateSelect(V1IsBetter, AvgV1, AvgV0);
        Builder.CreateCall(Printf, {BestVersionMsg, BestVersionName, BestAvgCycles});
        Builder.CreateCondBr(UseV1, CallV1, CallV0);

        // VersionSelection block
        Builder.SetInsertPoint(VersionSelection);
        Builder.CreateCondBr(UseV1, CallV1, CallV0);

        // Prepare call arguments once
        std::vector<Value*> Args;
        for (auto &Arg : Dispatcher->args()) {
            Args.push_back(&Arg);
        }

        // CallV0 block
        Builder.SetInsertPoint(CallV0);
        Value* StartTimeV0 = Builder.CreateCall(ChronoNow);
        Value* MsgV0 = Builder.CreateGlobalStringPtr("DISPATCH: V0 (Baseline) - ");
        Builder.CreateCall(Printf, {MsgV0});
        Builder.CreateCall(V0, Args);
        Value* EndTimeV0 = Builder.CreateCall(ChronoNow);
        Value* CyclesV0 = Builder.CreateSub(EndTimeV0, StartTimeV0);
        
        // Update V0 metrics
        Value* NewCountV0 = Builder.CreateAdd(CountV0, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        Value* NewTotalV0 = Builder.CreateAdd(TotalV0, CyclesV0);
        Builder.CreateStore(NewCountV0, CallCountV0);
        Builder.CreateStore(NewTotalV0, TotalCyclesV0);
        
        // Log performance metrics
        Value* LogMsgV0 = Builder.CreateGlobalStringPtr("Cycles: %lld, Total: %lld, Count: %d\n");
        Builder.CreateCall(Printf, {LogMsgV0, CyclesV0, NewTotalV0, NewCountV0});
        Builder.CreateBr(EndBlock);

        // CallV1 block
        Builder.SetInsertPoint(CallV1);
        Value* StartTimeV1 = Builder.CreateCall(ChronoNow);
        Value* MsgV1 = Builder.CreateGlobalStringPtr("DISPATCH: V1 (Optimized) - ");
        Builder.CreateCall(Printf, {MsgV1});
        Builder.CreateCall(V1, Args);
        Value* EndTimeV1 = Builder.CreateCall(ChronoNow);
        Value* CyclesV1 = Builder.CreateSub(EndTimeV1, StartTimeV1);
        
        // Update V1 metrics
        Value* NewCountV1 = Builder.CreateAdd(CountV1, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        Value* NewTotalV1 = Builder.CreateAdd(TotalV1, CyclesV1);
        Builder.CreateStore(NewCountV1, CallCountV1);
        Builder.CreateStore(NewTotalV1, TotalCyclesV1);
        
        // Log performance metrics
        Value* LogMsgV1 = Builder.CreateGlobalStringPtr("Cycles: %lld, Total: %lld, Count: %d\n");
        Builder.CreateCall(Printf, {LogMsgV1, CyclesV1, NewTotalV1, NewCountV1});
        Builder.CreateBr(EndBlock);

        // EndBlock
        Builder.SetInsertPoint(EndBlock);
        Builder.CreateRetVoid();

        return Dispatcher;
    }
    
    Function* createDispatcher(Function* Original, Function* V0, Function* V1, Module& M) {
        LLVMContext &Ctx = M.getContext();
        
        Function* Dispatcher = Function::Create(
            Original->getFunctionType(),
            GlobalValue::InternalLinkage,
            Original->getName() + "_dispatcher",
            &M
        );
        
        // Declare printf (match usage in createDispatcherWithDebug)
        FunctionType* PrintfType = FunctionType::get(
            Type::getInt32Ty(Ctx),
            {Type::getInt32Ty(Ctx)->getPointerTo()},
            true
        );
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

        // Declare clock function for timing (same as createDispatcherWithDebug)
        FunctionCallee ChronoNow = M.getOrInsertFunction(
            "_ZNSt6chrono3_V212system_clock3nowEv",
            FunctionType::get(Type::getInt64Ty(Ctx), false)
        );

        // Get the metrics struct type
        StructType* MetricsType = createMetricsStructType(Ctx);
        
        BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
        IRBuilder<> Builder(Entry);

        // Load metrics from global struct
        Value* MetricsPtr = Builder.CreateBitCast(GlobalMetrics, 
            MetricsType->getPointerTo());
        
        // Load individual fields from the struct
        Value* CountV0 = Builder.CreateLoad(Type::getInt32Ty(Ctx), 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 0));
        Value* CountV1 = Builder.CreateLoad(Type::getInt32Ty(Ctx), 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 1));
        Value* TotalV0 = Builder.CreateLoad(Type::getInt64Ty(Ctx), 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 4));
        Value* TotalV1 = Builder.CreateLoad(Type::getInt64Ty(Ctx), 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 5));
        Value* BestVersionPtr = Builder.CreateLoad(Type::getInt8PtrTy(Ctx), 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 6));

        // Adaptive decision
        Value* MinSamples = ConstantInt::get(Type::getInt32Ty(Ctx), 10);
        Value* HasEnoughV0 = Builder.CreateICmpUGE(CountV0, MinSamples);
        Value* HasEnoughV1 = Builder.CreateICmpUGE(CountV1, MinSamples);
        Value* HasEnoughBoth = Builder.CreateAnd(HasEnoughV0, HasEnoughV1);

        Value* CountV0_i64 = Builder.CreateZExt(CountV0, Type::getInt64Ty(Ctx));
        Value* CountV1_i64 = Builder.CreateZExt(CountV1, Type::getInt64Ty(Ctx));
        Value* AvgV0 = Builder.CreateUDiv(TotalV0, CountV0_i64);
        Value* AvgV1 = Builder.CreateUDiv(TotalV1, CountV1_i64);
        Value* V1IsBetter = Builder.CreateICmpULT(AvgV1, AvgV0);

        Value* TotalCalls = Builder.CreateAdd(CountV0, CountV1);
        Value* One = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
        Value* Parity = Builder.CreateAnd(TotalCalls, One);
        Value* AlternateUseV1 = Builder.CreateICmpEQ(Parity, One);
        Value* UseV1 = Builder.CreateSelect(HasEnoughBoth, V1IsBetter, AlternateUseV1);

        // Blocks
        BasicBlock* PrintBestVersion = BasicBlock::Create(Ctx, "print_best_version", Dispatcher);
        BasicBlock* VersionSelection = BasicBlock::Create(Ctx, "version_selection", Dispatcher);
        BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
        BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
        BasicBlock* EndBlock = BasicBlock::Create(Ctx, "end", Dispatcher);

        // Log total calls
        Value* CallNumStr = Builder.CreateGlobalStringPtr("[DISPATCHER] Total calls so far: %d\n");
        Builder.CreateCall(Printf, {CallNumStr, TotalCalls});

        // Branch to print best version if enough samples
        Builder.CreateCondBr(HasEnoughBoth, PrintBestVersion, VersionSelection);

        // Print best version and branch to chosen path
        Builder.SetInsertPoint(PrintBestVersion);
        Value* BestVersionMsg = Builder.CreateGlobalStringPtr("BEST VERSION: %s (Avg: %lld cycles)\n");
        Value* BestVersionName = Builder.CreateSelect(
            V1IsBetter,
            Builder.CreateGlobalStringPtr("V1 (Optimized)"),
            Builder.CreateGlobalStringPtr("V0 (Baseline)")
        );
        Value* BestAvgCycles = Builder.CreateSelect(V1IsBetter, AvgV1, AvgV0);
        Builder.CreateCall(Printf, {BestVersionMsg, BestVersionName, BestAvgCycles});
        Builder.CreateCondBr(UseV1, CallV1, CallV0);

        // Version selection (not enough samples)
        Builder.SetInsertPoint(VersionSelection);
        Builder.CreateCondBr(UseV1, CallV1, CallV0);
        
        Builder.SetInsertPoint(CallV0);
        std::vector<Value*> Args;
        for (auto &Arg : Dispatcher->args()) {
            Args.push_back(&Arg);
        }
        
        // Helper function to update metrics in the struct
        auto updateMetricsInStruct = [&](int version, Value* Cycles, 
            Value* OldCount, Value* OldTotal) {
            // Calculate new values
            Value* NewCount = Builder.CreateAdd(OldCount, 
            ConstantInt::get(Type::getInt32Ty(Ctx), 1));
            Value* NewTotal = Builder.CreateAdd(OldTotal, Cycles);

            // Update count and total in struct
            Builder.CreateStore(NewCount, 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, version));
            Builder.CreateStore(NewTotal, 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, version + 4));

            // Calculate and update average
            Value* NewCount_i64 = Builder.CreateZExt(NewCount, Type::getInt64Ty(Ctx));
            Value* NewAvg = Builder.CreateUIToFP(
            Builder.CreateUDiv(NewTotal, NewCount_i64), 
            Type::getDoubleTy(Ctx)
            );
            Builder.CreateStore(NewAvg, 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, version + 7));

            // Update best version if needed
            Value* OtherCount = (version == 0) ? CountV1 : CountV0;
            Value* HasEnoughOther = Builder.CreateICmpUGE(OtherCount, MinSamples);

            BasicBlock* UpdateBest = BasicBlock::Create(Ctx, "update_best", Dispatcher);
            BasicBlock* SkipUpdate = BasicBlock::Create(Ctx, "skip_update", Dispatcher);

            Builder.CreateCondBr(HasEnoughBoth, UpdateBest, SkipUpdate);

            Builder.SetInsertPoint(UpdateBest);
            Value* CurrentBest = Builder.CreateLoad(Type::getInt8PtrTy(Ctx),
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 6));
            Value* Avg0 = Builder.CreateLoad(Type::getDoubleTy(Ctx),
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 7));
            Value* Avg1 = Builder.CreateLoad(Type::getDoubleTy(Ctx),
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 8));
            Value* NewBestV1 = Builder.CreateFCmpULT(Avg1, Avg0);
            Value* NewBest = Builder.CreateSelect(NewBestV1,
            ConstantExpr::getBitCast(V1, Type::getInt8PtrTy(Ctx)),
            ConstantExpr::getBitCast(V0, Type::getInt8PtrTy(Ctx)));
            Builder.CreateStore(NewBest, 
            Builder.CreateStructGEP(MetricsType, MetricsPtr, 6));
            Builder.CreateBr(SkipUpdate);

            Builder.SetInsertPoint(SkipUpdate);

            return std::make_pair(NewCount, NewTotal);
        };

        // V0 path
        Builder.SetInsertPoint(CallV0);
        Value* StartTimeV0 = Builder.CreateCall(ChronoNow);
        Value* MsgV0 = Builder.CreateGlobalStringPtr("DISPATCH: V0 (Baseline) - ");
        Builder.CreateCall(Printf, {MsgV0});
        Builder.CreateCall(V0, Args);
        Value* EndTimeV0 = Builder.CreateCall(ChronoNow);
        Value* CyclesV0 = Builder.CreateSub(EndTimeV0, StartTimeV0);
        
        auto [NewCountV0, NewTotalV0] = updateMetricsInStruct(0, CyclesV0, CountV0, TotalV0);
        
        Builder.CreateStore(NewCountV0, CallCountV0);
        Builder.CreateStore(NewTotalV0, TotalCyclesV0);
        
        Value* LogMsgV0 = Builder.CreateGlobalStringPtr("Cycles: %lld, Total: %lld, Count: %d\n");
        Builder.CreateCall(Printf, {LogMsgV0, CyclesV0, NewTotalV0, NewCountV0});
        Builder.CreateBr(EndBlock);

        // V1 path
        Builder.SetInsertPoint(CallV1);
        Value* StartTimeV1 = Builder.CreateCall(ChronoNow);
        Value* MsgV1 = Builder.CreateGlobalStringPtr("DISPATCH: V1 (Optimized) - ");
        Builder.CreateCall(Printf, {MsgV1});
        Builder.CreateCall(V1, Args);
        Value* EndTimeV1 = Builder.CreateCall(ChronoNow);
        Value* CyclesV1 = Builder.CreateSub(EndTimeV1, StartTimeV1);
        auto [NewCountV1, NewTotalV1] = updateMetricsInStruct(1, CyclesV1, CountV1, TotalV1);
        
        Builder.CreateStore(NewCountV1, CallCountV1);
        Builder.CreateStore(NewTotalV1, TotalCyclesV1);
        Value* LogMsgV1 = Builder.CreateGlobalStringPtr("Cycles: %lld, Total: %lld, Count: %d\n");
        Builder.CreateCall(Printf, {LogMsgV1, CyclesV1, NewTotalV1, NewCountV1});
        Builder.CreateBr(EndBlock);

        // End
        Builder.SetInsertPoint(EndBlock);
        Builder.CreateRetVoid();
        
        errs() << "  Created dispatcher: " << Dispatcher->getName() << "\n";
        return Dispatcher;
    }
    
    Function* createWrapper(Function* Original, Function* Dispatcher, Module& M) {
        Function* Wrapper = Function::Create(
            Original->getFunctionType(),
            Original->getLinkage(),
            "temp_wrapper",  // Temporary name
            &M
        );
        
        BasicBlock* Entry = BasicBlock::Create(M.getContext(), "entry", Wrapper);
        IRBuilder<> Builder(Entry);
        
        std::vector<Value*> Args;
        for (auto &Arg : Wrapper->args()) {
            Args.push_back(&Arg);
        }
        
        Builder.CreateCall(Dispatcher, Args);
        Builder.CreateRetVoid();
        
        errs() << "  Created wrapper\n";
        return Wrapper;
    }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION, "simple-adaptive", LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "simple-adaptive") {
                        MPM.addPass(SimpleAdaptivePass());
                        return true;
                    }
                    return false;
                });
        }
    };
}