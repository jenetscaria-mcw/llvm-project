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
#include <chrono>

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
        Function* MatrixFunc = M.getFunction("_Z15matrix_multiplyPfS_S_i");
        if (!MatrixFunc) {
            // Try C-style name if C++ mangled name not found
            MatrixFunc = M.getFunction("matrix_multiply");
        }
        
        if (!MatrixFunc) {
            errs() << "Target function not found!\n";
            return PreservedAnalyses::all();
        }
        
        errs() << "Found function: " << MatrixFunc->getName() << "\n";
        //errs() << typeid(MatrixFunc->getName()).name() << "\n";
        StringRef name = MatrixFunc->getName();
        errs() << name << "\n";
        std::string s = name.str();
        errs() << s << "\n";
        Function* V0 = createBaselineVersion(MatrixFunc, 0, M);
        Function* V1 = createOptimizedVersion(MatrixFunc, 1, M);
        
        // Create performance metrics structure
        PerformanceMetrics metrics;
        metrics.setFunctions(V0, V1);
        
        Function* Dispatcher = createDispatcher(MatrixFunc, V0, V1, M);
        
        Function* Wrapper = createWrapper(MatrixFunc, Dispatcher, M);
        
        // Create performance summary function
        Function* SummaryFunc = createPerformanceSummary(MatrixFunc, M);
        
        MatrixFunc->replaceAllUsesWith(Wrapper);
        Wrapper->takeName(MatrixFunc);
        MatrixFunc->setName(s + "_original");
        MatrixFunc->setLinkage(GlobalValue::InternalLinkage);
        
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
    
    Function* createDispatcher(Function* Original, Function* V0, Function* V1, Module& M) {
        LLVMContext &Ctx = M.getContext();
        
        Function* Dispatcher = Function::Create(
            Original->getFunctionType(),
            GlobalValue::InternalLinkage,
            Original->getName() + "_dispatcher",
            &M
        );
        
        // Declare printf for runtime logging
        FunctionCallee Printf = M.getOrInsertFunction(
            "printf",
            FunctionType::get(
                Type::getInt32Ty(Ctx),
                PointerType::get(Type::getInt8Ty(Ctx), 0),
                /*isVarArg=*/true)
        );
        
        // Declare std::chrono::high_resolution_clock::now
        FunctionCallee ChronoNow = M.getOrInsertFunction(
            "_ZNSt6chrono3_V212system_clock3nowEv",
            FunctionType::get(
                Type::getInt64Ty(Ctx),
                false)
        );
        
        // Global variables for performance tracking
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
        
        GlobalVariable* BestVersion = new GlobalVariable(
            M,
            PointerType::get(Original->getFunctionType(), 0),
            false,
            GlobalValue::InternalLinkage,
            ConstantPointerNull::get(PointerType::get(Original->getFunctionType(), 0)),
            "best_version"
        );
        
        BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
        IRBuilder<> Builder(Entry);
        
        // Load current call counts
        Value* CountV0 = Builder.CreateLoad(Type::getInt32Ty(Ctx), CallCountV0);
        Value* CountV1 = Builder.CreateLoad(Type::getInt32Ty(Ctx), CallCountV1);
        Value* TotalV0 = Builder.CreateLoad(Type::getInt64Ty(Ctx), TotalCyclesV0);
        Value* TotalV1 = Builder.CreateLoad(Type::getInt64Ty(Ctx), TotalCyclesV1);
        
        // Adaptive switching logic based on performance
        // If we have enough samples from both versions, use the better one
        Value* MinSamples = ConstantInt::get(Type::getInt32Ty(Ctx), 10);
        Value* HasEnoughV0 = Builder.CreateICmpUGE(CountV0, MinSamples);
        Value* HasEnoughV1 = Builder.CreateICmpUGE(CountV1, MinSamples);
        Value* HasEnoughBoth = Builder.CreateAnd(HasEnoughV0, HasEnoughV1);
        
        // Calculate average cycles for comparison
        // Cast 32-bit counters to 64-bit to match total cycle counters
        Value* CountV0_i64 = Builder.CreateZExt(CountV0, Type::getInt64Ty(Ctx));
        Value* CountV1_i64 = Builder.CreateZExt(CountV1, Type::getInt64Ty(Ctx));
        Value* AvgV0 = Builder.CreateUDiv(TotalV0, CountV0_i64);
        Value* AvgV1 = Builder.CreateUDiv(TotalV1, CountV1_i64);
        Value* V1IsBetter = Builder.CreateICmpULT(AvgV1, AvgV0);
        
        // Use adaptive logic if we have enough samples, otherwise alternate
        Value* TotalCalls = Builder.CreateAdd(CountV0, CountV1);
        Value* One = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
        Value* Parity = Builder.CreateAnd(TotalCalls, One);
        Value* AlternateUseV1 = Builder.CreateICmpEQ(Parity, One);
        
        // Choose: adaptive if enough samples, otherwise alternate
        Value* UseV1 = Builder.CreateSelect(HasEnoughBoth, V1IsBetter, AlternateUseV1);
        
        // Print best version information when we have enough samples
        BasicBlock* PrintBestVersion = BasicBlock::Create(Ctx, "print_best_version", Dispatcher);
        BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
        BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
        BasicBlock* EndBlock = BasicBlock::Create(Ctx, "end", Dispatcher);
        
        // Create a version selection block
        BasicBlock* VersionSelection = BasicBlock::Create(Ctx, "version_selection", Dispatcher);
        
        // Branch to print best version if we have enough samples, otherwise go to version selection
        Builder.CreateCondBr(HasEnoughBoth, PrintBestVersion, VersionSelection);
        
        // Print best version block
        Builder.SetInsertPoint(PrintBestVersion);
        Value* BestVersionMsg = Builder.CreateGlobalStringPtr("BEST VERSION: %s (Avg: %lld cycles)\n");
        Value* BestVersionName = Builder.CreateSelect(V1IsBetter, 
            Builder.CreateGlobalStringPtr("V1 (Optimized)"), 
            Builder.CreateGlobalStringPtr("V0 (Baseline)"));
        Value* BestAvgCycles = Builder.CreateSelect(V1IsBetter, AvgV1, AvgV0);
        Builder.CreateCall(Printf, {BestVersionMsg, BestVersionName, BestAvgCycles});
        // After printing, branch to the appropriate version based on the decision
        Builder.CreateCondBr(UseV1, CallV1, CallV0);
        
        // Version selection block (when we don't have enough samples yet)
        Builder.SetInsertPoint(VersionSelection);
        Builder.CreateCondBr(UseV1, CallV1, CallV0);
        
        // V0 execution block
        Builder.SetInsertPoint(CallV0);
        std::vector<Value*> Args;
        for (auto &Arg : Dispatcher->args()) {
            Args.push_back(&Arg);
        }
        
        // Start timing
        Value* StartTime = Builder.CreateCall(ChronoNow);
        
        // Log dispatch choice: V0
        Value* MsgV0 = Builder.CreateGlobalStringPtr("DISPATCH: V0 (Baseline) - ");
        Builder.CreateCall(Printf, {MsgV0});
        
        // Call V0
        Builder.CreateCall(V0, Args);
        
        // End timing and calculate cycles
        Value* EndTime = Builder.CreateCall(ChronoNow);
        Value* Cycles = Builder.CreateSub(EndTime, StartTime);
        
        // Update V0 metrics
        Value* NewCountV0 = Builder.CreateAdd(CountV0, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        Value* NewTotalV0 = Builder.CreateAdd(TotalV0, Cycles);
        Builder.CreateStore(NewCountV0, CallCountV0);
        Builder.CreateStore(NewTotalV0, TotalCyclesV0);
        
        // Log performance metrics
        Value* LogMsgV0 = Builder.CreateGlobalStringPtr("Cycles: %lld, Total: %lld, Count: %d\n");
        Builder.CreateCall(Printf, {LogMsgV0, Cycles, NewTotalV0, NewCountV0});
        
        Builder.CreateBr(EndBlock);
        
        // V1 execution block
        Builder.SetInsertPoint(CallV1);
        
        // Start timing
        StartTime = Builder.CreateCall(ChronoNow);
        
        // Log dispatch choice: V1
        Value* MsgV1 = Builder.CreateGlobalStringPtr("DISPATCH: V1 (Optimized) - ");
        Builder.CreateCall(Printf, {MsgV1});
        
        // Call V1
        Builder.CreateCall(V1, Args);
        
        // End timing and calculate cycles
        EndTime = Builder.CreateCall(ChronoNow);
        Cycles = Builder.CreateSub(EndTime, StartTime);
        
        // Update V1 metrics
        Value* NewCountV1 = Builder.CreateAdd(CountV1, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        Value* NewTotalV1 = Builder.CreateAdd(TotalV1, Cycles);
        Builder.CreateStore(NewCountV1, CallCountV1);
        Builder.CreateStore(NewTotalV1, TotalCyclesV1);
        
        // Log performance metrics
        Value* LogMsgV1 = Builder.CreateGlobalStringPtr("Cycles: %lld, Total: %lld, Count: %d\n");
        Builder.CreateCall(Printf, {LogMsgV1, Cycles, NewTotalV1, NewCountV1});
        
        Builder.CreateBr(EndBlock);
        
        // End block
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
    
    Function* createPerformanceSummary(Function* Original, Module& M) {
        LLVMContext &Ctx = M.getContext();
        
        // Create a function that prints performance summary
        FunctionType* SummaryType = FunctionType::get(
            Type::getVoidTy(Ctx),
            false
        );
        
        // Canonically get or insert the summary function declaration
        FunctionCallee Callee = M.getOrInsertFunction("print_performance_summary", SummaryType);
        Function* SummaryFunc = cast<Function>(Callee.getCallee());
        SummaryFunc->setLinkage(GlobalValue::ExternalLinkage);
        
        // Declare printf
        FunctionCallee Printf = M.getOrInsertFunction(
            "printf",
            FunctionType::get(
                Type::getInt32Ty(Ctx),
                PointerType::get(Type::getInt8Ty(Ctx), 0),
                /*isVarArg=*/true)
        );
        
        // Only define the body if it's not already defined
        if (SummaryFunc->empty()) {
            BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", SummaryFunc);
            IRBuilder<> Builder(Entry);
        
        // Print header
        Value* Header = Builder.CreateGlobalStringPtr("\n=== Performance Summary ===\n");
        Builder.CreateCall(Printf, {Header});
        
        // Load counters and totals from globals
        GlobalVariable* CallCountV0G = M.getGlobalVariable("call_count_v0", true);
        GlobalVariable* CallCountV1G = M.getGlobalVariable("call_count_v1", true);
        GlobalVariable* TotalCyclesV0G = M.getGlobalVariable("total_cycles_v0", true);
        GlobalVariable* TotalCyclesV1G = M.getGlobalVariable("total_cycles_v1", true);

        Value* CountV0 = Builder.CreateLoad(Type::getInt32Ty(Ctx), CallCountV0G);
        Value* CountV1 = Builder.CreateLoad(Type::getInt32Ty(Ctx), CallCountV1G);
        Value* TotalV0 = Builder.CreateLoad(Type::getInt64Ty(Ctx), TotalCyclesV0G);
        Value* TotalV1 = Builder.CreateLoad(Type::getInt64Ty(Ctx), TotalCyclesV1G);

        // Compute averages with zero-count guard
        Value* ZeroI32 = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
        Value* HasV0 = Builder.CreateICmpUGT(CountV0, ZeroI32);
        Value* HasV1 = Builder.CreateICmpUGT(CountV1, ZeroI32);
        Value* CountV0_i64 = Builder.CreateZExt(CountV0, Type::getInt64Ty(Ctx));
        Value* CountV1_i64 = Builder.CreateZExt(CountV1, Type::getInt64Ty(Ctx));
        Value* AvgV0Raw = Builder.CreateUDiv(TotalV0, CountV0_i64);
        Value* AvgV1Raw = Builder.CreateUDiv(TotalV1, CountV1_i64);
        Value* ZeroI64 = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
        Value* AvgV0 = Builder.CreateSelect(HasV0, AvgV0Raw, ZeroI64);
        Value* AvgV1 = Builder.CreateSelect(HasV1, AvgV1Raw, ZeroI64);

        // Print V0 stats with averages
        Value* V0Fmt = Builder.CreateGlobalStringPtr("V0 (Baseline): avg=%lld cycles (total=%lld, count=%d)\n");
        Builder.CreateCall(Printf, {V0Fmt, AvgV0, TotalV0, CountV0});

        // Print V1 stats with averages
        Value* V1Fmt = Builder.CreateGlobalStringPtr("V1 (Optimized): avg=%lld cycles (total=%lld, count=%d)\n");
        Builder.CreateCall(Printf, {V1Fmt, AvgV1, TotalV1, CountV1});

        // Determine best version string
        Value* HasBoth = Builder.CreateAnd(HasV0, HasV1);
        Value* V1Better = Builder.CreateICmpULT(AvgV1, AvgV0);
        Value* NameV1 = Builder.CreateGlobalStringPtr("V1 (Optimized)");
        Value* NameV0 = Builder.CreateGlobalStringPtr("V0 (Baseline)");
        Value* NameNA = Builder.CreateGlobalStringPtr("N/A");

        // If both available choose by avg, else prefer the one that exists, else N/A
        Value* BestWhenBoth = Builder.CreateSelect(V1Better, NameV1, NameV0);
        Value* BestWhenAny = Builder.CreateSelect(HasV1, NameV1, Builder.CreateSelect(HasV0, NameV0, NameNA));
        Value* BestName = Builder.CreateSelect(HasBoth, BestWhenBoth, BestWhenAny);

        Value* BestLine = Builder.CreateGlobalStringPtr("Best Version: %s\n");
        Builder.CreateCall(Printf, {BestLine, BestName});
        
        Value* Footer = Builder.CreateGlobalStringPtr("===========================\n");
        Builder.CreateCall(Printf, {Footer});
        
            Builder.CreateRetVoid();
            errs() << "  Created performance summary function\n";
        } else {
            errs() << "  Reused existing performance summary function\n";
        }
        return SummaryFunc;
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