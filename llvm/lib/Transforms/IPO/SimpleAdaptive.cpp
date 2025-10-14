#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/raw_ostream.h"
#include <atomic>

using namespace llvm;

namespace llvm {

  static Function* createBaselineVersion(Function* F, int versionId, Module& M);
  static Function* createOptimizedVersion(Function* F, int versionId, Module& M);
  static void addPrintToVersion(Function* F, int versionId, Module& M);
  static Function* createDispatcherWithDebug(Function* Original, Function* V0, 
                                              Function* V1, Module& M);
  static Function* createWrapper(Function* Original, Function* Dispatcher, Module& M);
  static void createRuntimeFunctions(Module& M);
  static StructType* createMetricsStructType(LLVMContext &Ctx);

  // Performance Metrics Structure to store all adaptation data
  struct PerformanceMetrics {
    // Raw counters
    std::atomic<int> call_count_v0;
    std::atomic<int> call_count_v1;
    std::atomic<long long> total_cycles_v0;
    std::atomic<long long> total_cycles_v1;
    
    // Function pointers
    Function* func_v0;
    Function* func_v1;
    std::atomic<Function*> best_version;
    
    // Computed metrics
    std::atomic<double> avg_cycles_v0;
    std::atomic<double> avg_cycles_v1;
    
    // Configuration
    const int min_samples = 10;
    
    PerformanceMetrics(Function* v0, Function* v1) 
        : call_count_v0(0), call_count_v1(0),
          total_cycles_v0(0), total_cycles_v1(0),
          func_v0(v0), func_v1(v1),
          best_version(v0), avg_cycles_v0(0.0), avg_cycles_v1(0.0) {}
    
    // Update metrics for a specific version
    void updateMetrics(int version, long long cycles) {
        if (version == 0) {
            int new_count = ++call_count_v0;
            long long new_total = total_cycles_v0.fetch_add(cycles) + cycles;
            avg_cycles_v0.store(static_cast<double>(new_total) / new_count);
        } else {
            int new_count = ++call_count_v1;
            long long new_total = total_cycles_v1.fetch_add(cycles) + cycles;
            avg_cycles_v1.store(static_cast<double>(new_total) / new_count);
        }
        updateBestVersion();
    }
    
    // Determine which version to execute
    int selectVersion() {
        int count_v0 = call_count_v0.load();
        int count_v1 = call_count_v1.load();
        int total_calls = count_v0 + count_v1;
        
        // Exploration phase: alternate between versions until we have enough samples
        if (!hasEnoughSamples()) {
            return total_calls % 2; // Alternate between 0 and 1
        }
        
        // Exploitation phase: use the better version
        return getBestVersionIndex();
    }
    
    // Get the best version function pointer
    Function* getBestVersion() {
        return best_version.load();
    }
    
    int getBestVersionIndex() {
        Function* best = best_version.load();
        return (best == func_v1) ? 1 : 0;
    }
    
    // Print current metrics for debugging
    void printMetrics(raw_ostream &OS) {
        OS << "[METRICS] V0: calls=" << call_count_v0.load() 
           << ", total_cycles=" << total_cycles_v0.load() 
           << ", avg=" << avg_cycles_v0.load() << "\n";
        OS << "[METRICS] V1: calls=" << call_count_v1.load() 
           << ", total_cycles=" << total_cycles_v1.load() 
           << ", avg=" << avg_cycles_v1.load() << "\n";
        OS << "[METRICS] Best: " << (getBestVersionIndex() == 1 ? "V1" : "V0") << "\n";
    }
    
private:
    void updateBestVersion() {
        if (hasEnoughSamples()) {
            int count_v0 = call_count_v0.load();
            int count_v1 = call_count_v1.load();
            double avg_v0 = (count_v0 > 0) ? avg_cycles_v0.load() : 1e9;
            double avg_v1 = (count_v1 > 0) ? avg_cycles_v1.load() : 1e9;
            Function* new_best = (avg_v1 < avg_v0) ? func_v1 : func_v0;
            best_version.store(new_best);
        }
    }
    
    bool hasEnoughSamples() {
        return call_count_v0.load() >= min_samples && call_count_v1.load() >= min_samples;
    }
};

// Global performance metrics instance
static PerformanceMetrics* GlobalPerfMetrics = nullptr;

// Runtime function declarations
static Function* UpdateMetricsFunc = nullptr;
static Function* SelectVersionFunc = nullptr;

PreservedAnalyses SimpleAdaptivePass::run(Module &M, ModuleAnalysisManager &) {
    errs() << "=== SimpleAdaptive Pass Running ===\n";
    
    Function* AdaptiveFunc = nullptr;
    
    for (Function &F : M) {
        if (!F.isDeclaration() && F.hasFnAttribute("adaptive")) {
            errs() << "[ADAPTIVE] Found annotated function: "
                   << F.getName() << "\n";
            AdaptiveFunc = &F;
            break;
        }
    }
    
    if (!AdaptiveFunc) {
        errs() << "Target function not found!\n";
        return PreservedAnalyses::all();
    }
    
    errs() << "Found function: " << AdaptiveFunc->getName() << "\n";

    Function* V0 = createBaselineVersion(AdaptiveFunc, 0, M);
    Function* V1 = createOptimizedVersion(AdaptiveFunc, 1, M);
    
    // Create global performance metrics structure
    GlobalPerfMetrics = new PerformanceMetrics(V0, V1);
    
    // Create runtime functions that will use the PerformanceMetrics structure
    createRuntimeFunctions(M);

    addPrintToVersion(V0, 0, M);
    addPrintToVersion(V1, 1, M);
    
    Function* Dispatcher = createDispatcherWithDebug(AdaptiveFunc, V0, V1, M);
    Function* Wrapper = createWrapper(AdaptiveFunc, Dispatcher, M);
    
    AdaptiveFunc->replaceAllUsesWith(Wrapper);
    Wrapper->takeName(AdaptiveFunc);
    AdaptiveFunc->setName(AdaptiveFunc->getName() + "_original");
    AdaptiveFunc->setLinkage(GlobalValue::InternalLinkage);
    
    errs() << "=== Transformation Complete ===\n";
    
    // Print initial metrics
    GlobalPerfMetrics->printMetrics(errs());
    
    return PreservedAnalyses::none();
}

// Create runtime functions that interact with the PerformanceMetrics structure
static void createRuntimeFunctions(Module& M) {
    LLVMContext &Ctx = M.getContext();
    
    // update_performance_metrics(int version, long long cycles)
    FunctionType* UpdateMetricsType = FunctionType::get(
        Type::getVoidTy(Ctx),
        {Type::getInt32Ty(Ctx), Type::getInt64Ty(Ctx)},
        false
    );
    
    UpdateMetricsFunc = Function::Create(
        UpdateMetricsType,
        GlobalValue::ExternalLinkage,
        "update_performance_metrics",
        &M
    );
    
    // select_function_version() -> int
    FunctionType* SelectVersionType = FunctionType::get(
        Type::getInt32Ty(Ctx),
        false
    );
    
    SelectVersionFunc = Function::Create(
        SelectVersionType,
        GlobalValue::ExternalLinkage,
        "select_function_version",
        &M
    );
    
    // print_metrics()
    FunctionType* PrintMetricsType = FunctionType::get(
        Type::getVoidTy(Ctx),
        false
    );
    
    Function::Create(
        PrintMetricsType,
        GlobalValue::ExternalLinkage,
        "print_metrics",
        &M
    );
}

static Function* createBaselineVersion(Function* F, int versionId, Module& M) {
    ValueToValueMapTy VMap;
    Function* NewF = CloneFunction(F, VMap);
    NewF->setName(F->getName() + "_v" + std::to_string(versionId));
    M.getFunctionList().push_back(NewF);
    return NewF;
}

static Function* createOptimizedVersion(Function* F, int versionId, Module& M) {
    ValueToValueMapTy VMap;
    Function* NewF = CloneFunction(F, VMap);
    NewF->setName(F->getName() + "_v" + std::to_string(versionId));
    
    NewF->addFnAttr("prefer-vector-width", "256");
    NewF->addFnAttr("no-unroll-loops", "false");
    
    M.getFunctionList().push_back(NewF);
    return NewF;
}

static void addPrintToVersion(Function* F, int versionId, Module& M) {
    LLVMContext &Ctx = M.getContext();
    
    FunctionType* PrintfType = FunctionType::get(
        Type::getInt32Ty(Ctx),
        {PointerType::getUnqual(Ctx)},
        true
    );
    
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
    
    BasicBlock &EntryBB = F->getEntryBlock();
    IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());
    
    std::string FormatStr = "[DISPATCHER] Calling Version " + 
                           std::to_string(versionId) + " of %s\n";
    Value* FormatStrVal = Builder.CreateGlobalStringPtr(FormatStr);
    Value* FuncNameVal = Builder.CreateGlobalStringPtr(F->getName().str());
    
    Builder.CreateCall(Printf, {FormatStrVal, FuncNameVal});
}

StructType* createMetricsStructType(LLVMContext &Ctx) {
    // This is kept for compatibility but not used for the main logic
    std::vector<Type*> Elements = {
        Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx),
        PointerType::getUnqual(Ctx), PointerType::getUnqual(Ctx),
        Type::getInt64Ty(Ctx), Type::getInt64Ty(Ctx),
        PointerType::getUnqual(Ctx), Type::getDoubleTy(Ctx), Type::getDoubleTy(Ctx)
    };
    return StructType::create(Ctx, Elements, "PerformanceMetrics");
}

static Function* createDispatcherWithDebug(Function* Original, Function* V0, Function* V1, Module& M) {
    LLVMContext &Ctx = M.getContext();
    
    Function* Dispatcher = Function::Create(
        Original->getFunctionType(),
        GlobalValue::InternalLinkage,
        Original->getName() + "_dispatcher",
        &M
    );
    
    FunctionType* PrintfType = FunctionType::get(
        Type::getInt32Ty(Ctx),
        {PointerType::getUnqual(Ctx)},
        true
    );
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    // RDTSC intrinsic for timing
    FunctionCallee RDTSC = M.getOrInsertFunction(
        "llvm.x86.rdtsc",
        FunctionType::get(Type::getInt64Ty(Ctx), false)
    );

    BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
    IRBuilder<> Builder(Entry);

    // Use the runtime function to select version based on PerformanceMetrics
    Value* SelectedVersion = Builder.CreateCall(SelectVersionFunc);
    
    BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
    BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
    BasicBlock* Merge = BasicBlock::Create(Ctx, "merge", Dispatcher);
    
    Builder.CreateCondBr(
        Builder.CreateICmpEQ(SelectedVersion, ConstantInt::get(Type::getInt32Ty(Ctx), 1)),
        CallV1, 
        CallV0
    );

    // === V0 execution path ===
    Builder.SetInsertPoint(CallV0);
    Value* StartTimeV0 = Builder.CreateCall(RDTSC);
    
    std::vector<Value*> ArgsV0;
    for (auto &Arg : Dispatcher->args()) {
        ArgsV0.push_back(&Arg);
    }
    Value* ResultV0 = Builder.CreateCall(V0, ArgsV0);
    
    Value* EndTimeV0 = Builder.CreateCall(RDTSC);
    Value* CyclesV0 = Builder.CreateSub(EndTimeV0, StartTimeV0);
    
    // Update metrics using PerformanceMetrics structure
    Builder.CreateCall(UpdateMetricsFunc, {
        ConstantInt::get(Type::getInt32Ty(Ctx), 0),
        CyclesV0
    });
    
    Builder.CreateBr(Merge);

    // === V1 execution path ===
    Builder.SetInsertPoint(CallV1);
    Value* StartTimeV1 = Builder.CreateCall(RDTSC);
    
    std::vector<Value*> ArgsV1;
    for (auto &Arg : Dispatcher->args()) {
        ArgsV1.push_back(&Arg);
    }
    Value* ResultV1 = Builder.CreateCall(V1, ArgsV1);
    
    Value* EndTimeV1 = Builder.CreateCall(RDTSC);
    Value* CyclesV1 = Builder.CreateSub(EndTimeV1, StartTimeV1);
    
    // Update metrics using PerformanceMetrics structure
    Builder.CreateCall(UpdateMetricsFunc, {
        ConstantInt::get(Type::getInt32Ty(Ctx), 1),
        CyclesV1
    });
    
    Builder.CreateBr(Merge);

    // === Merge point ===
    Builder.SetInsertPoint(Merge);
    PHINode* Result = Builder.CreatePHI(Original->getReturnType(), 2);
    Result->addIncoming(ResultV0, CallV0);
    Result->addIncoming(ResultV1, CallV1);
    
    Builder.CreateRet(Result);
    
    errs() << "  Created dispatcher: " << Dispatcher->getName() << "\n";
    return Dispatcher;
}

static Function* createWrapper(Function* Original, Function* Dispatcher, Module& M) {
    Function* Wrapper = Function::Create(
        Original->getFunctionType(),
        Original->getLinkage(),
        "temp_wrapper",
        &M
    );
    
    BasicBlock* Entry = BasicBlock::Create(M.getContext(), "entry", Wrapper);
    IRBuilder<> Builder(Entry);
    
    std::vector<Value*> Args;
    for (auto &Arg : Wrapper->args()) {
        Args.push_back(&Arg);
    }
    
    Value* Result = Builder.CreateCall(Dispatcher, Args);
    
    if (Original->getReturnType()->isVoidTy()) {
        Builder.CreateRetVoid();
    } else {
        Builder.CreateRet(Result);
    }
    
    errs() << "  Created wrapper\n";
    return Wrapper;
}

// Runtime function implementations
extern "C" {
    void update_performance_metrics(int version, long long cycles) {
        if (GlobalPerfMetrics) {
            GlobalPerfMetrics->updateMetrics(version, cycles);
        }
    }
    
    int select_function_version() {
        if (GlobalPerfMetrics) {
            return GlobalPerfMetrics->selectVersion();
        }
        return 0; // Fallback to V0
    }
    
    void print_metrics() {
        if (GlobalPerfMetrics) {
            GlobalPerfMetrics->printMetrics(llvm::errs());
        }
    }
}

} // namespace llvm