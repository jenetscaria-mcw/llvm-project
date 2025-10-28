#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <vector>

using namespace llvm;

// Remove the anonymous namespace since it's now in the llvm namespace
namespace llvm {

  static void addPrintToVersion(Function* F, int versionId, uint32_t funcId, Module& M);
  static Function* createVersion(Function* F, int versionId, Module& M);
  static Function* createWrapper(Function* Original, Function* Dispatcher, Module& M);
  static void createGlobalDispatchInfrastructure(Module &M);
  static Function* createSharedDispatcher(Module &M);
  static void processFunctionWithSharedDispatcher(Function* F, Function* SharedDispatcher,
                                        uint32_t funcId, Module &M);
  static void createInitializationFunction(Module &M);
  static Function* createWrapperForSharedDispatcher(Function* Original,
                                            Function* SharedDispatcher,
                                            uint32_t funcId, Module& M);

PreservedAnalyses SimpleAdaptivePass::run(Module &M, ModuleAnalysisManager &) {
    errs() << "=== SimpleAdaptive Pass Running ===\n";
    
    // Step 1: Collect all adaptive functions
    std::vector<Function*> adaptiveFunctions;
    for (Function &F : M) {
        if (!F.isDeclaration() && F.hasFnAttribute("adaptive")) {
            errs() << "[ADAPTIVE] Found: " << F.getName() << "\n";
            adaptiveFunctions.push_back(&F);
        }
    }

    if (adaptiveFunctions.empty()) {
        errs() << "Target function not found!\n";
        return PreservedAnalyses::all();
    }

    // Step 2: Create global dispatch infrastructure
    createGlobalDispatchInfrastructure(M);

    // Step 3: Create shared dispatcher
    Function* SharedDispatcher = createSharedDispatcher(M);

    // Step 4: Process each adaptive function
    uint32_t funcId = 0;
    for (Function* F : adaptiveFunctions) {
        processFunctionWithSharedDispatcher(F, SharedDispatcher, funcId++, M);
    }

    // Step 5: Create initialization function
    createInitializationFunction(M);


    errs() << "=== Transformation Complete ===\n";
    return PreservedAnalyses::none();
}

// Structure to track function metadata
struct FunctionMetadata {
    Function* original;
    Function* versions[2];  // v0 and v1 for simplicity
    Function* wrapper;
    uint32_t functionId;
};

std::map<Function*, FunctionMetadata> functionMap;
GlobalVariable* dispatchTable = nullptr;
GlobalVariable* functionCounterTable = nullptr;
GlobalVariable* functionCount = nullptr;

// === STEP 1: Create Global Infrastructure ===
static void createGlobalDispatchInfrastructure(Module &M) {
    LLVMContext &Ctx = M.getContext();

    // 1. Dispatch table: Array of function pointers [func0_v0, func0_v1, func1_v0, func1_v1, ...]
    ArrayType* DispatchTableType = ArrayType::get(
        PointerType::get(Type::getInt8Ty(Ctx), 0), 
        200  // Max 100 functions * 2 versions
    );

    dispatchTable = new GlobalVariable(
        M,
        DispatchTableType,
        false,
        GlobalValue::InternalLinkage,
        ConstantAggregateZero::get(DispatchTableType),
        "__shared_dispatch_table"
    );

    // 2. Counter table: Array of counters for each function
    ArrayType* CounterTableType = ArrayType::get(
        Type::getInt32Ty(Ctx),
        100  // Max 100 functions
    );

    functionCounterTable = new GlobalVariable(
        M,
        CounterTableType,
        false,
        GlobalValue::InternalLinkage,
        ConstantAggregateZero::get(CounterTableType),
        "__function_counters"
    );

    // 3. Function count
    functionCount = new GlobalVariable(
        M,
        Type::getInt32Ty(Ctx),
        false,
        GlobalValue::InternalLinkage,
        ConstantInt::get(Type::getInt32Ty(Ctx), 0),
        "__adaptive_function_count"
    );

    errs() << "  Created global dispatch infrastructure\n";
}

// === STEP 2: Create Shared Dispatcher ===
static Function* createSharedDispatcher(Module &M) {
    LLVMContext &Ctx = M.getContext();

    // Create generic dispatcher that takes function ID and arguments
    // Signature: void* __shared_dispatcher(i32 funcId, i8** args)
    FunctionType* DispatcherType = FunctionType::get(
        PointerType::get(Type::getInt8Ty(Ctx), 0),  // Returns void* (generic)
        {
            Type::getInt32Ty(Ctx),                                            // Function ID
            PointerType::get(PointerType::get(Type::getInt8Ty(Ctx), 0), 0)    // Generic args array (i8**)
        },
        false
    );

    Function* Dispatcher = Function::Create(
        DispatcherType,
        GlobalValue::ExternalLinkage,
        "__shared_dispatcher",
        &M
    );

    BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
    IRBuilder<> Builder(Entry);

    // Get arguments
    Function::arg_iterator Args = Dispatcher->arg_begin();
    Value* FuncId = Args++;
    FuncId->setName("funcId");
    Value* ArgsArray = Args++;
    ArgsArray->setName("args");

    // Get printf for debugging
    FunctionType* PrintfType = FunctionType::get(
        Type::getInt32Ty(Ctx),
        {PointerType::get(Type::getInt8Ty(Ctx), 0)},
        true
    );
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    // === DISPATCHER LOGIC ===

    // 1. Load counter for this function
    Value* CounterPtr = Builder.CreateGEP(
        functionCounterTable->getValueType(),
        functionCounterTable,
        {ConstantInt::get(Type::getInt32Ty(Ctx), 0), FuncId}
    );
    Value* Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), CounterPtr);

    // 2. Increment counter
    Value* NewCount = Builder.CreateAdd(Count, 
        ConstantInt::get(Type::getInt32Ty(Ctx), 1));
    Builder.CreateStore(NewCount, CounterPtr);

    // 3. Print debug info
    Value* DebugMsg = Builder.CreateGlobalStringPtr(
        "[SHARED DISPATCHER] Function %d, Call #%d: ");
    Builder.CreateCall(Printf, {DebugMsg, FuncId, NewCount});

    // 4. Decide version using round-robin
    Value* Two = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
    Value* Version = Builder.CreateURem(Count, Two);  // 0 or 1

    // 5. Calculate dispatch table index: funcId * 2 + version
    Value* BaseIdx = Builder.CreateMul(FuncId, Two);
    Value* TableIdx = Builder.CreateAdd(BaseIdx, Version);

    // 6. Load function pointer from dispatch table
    Value* FuncPtrPtr = Builder.CreateGEP(
        dispatchTable->getValueType(),
        dispatchTable,
        {ConstantInt::get(Type::getInt32Ty(Ctx), 0), TableIdx}
    );
    Value* FuncPtr = Builder.CreateLoad(PointerType::get(Type::getInt8Ty(Ctx), 0), FuncPtrPtr);

    // 7. Print which version
    Value* VersionMsg = Builder.CreateGlobalStringPtr("Version %d\n");
    Builder.CreateCall(Printf, {VersionMsg, Version});

    // 8. Call the function (simplified - assumes void return for demo)
    // In real implementation, would need to handle different signatures
    Builder.CreateCall(
        FunctionType::get(Type::getVoidTy(Ctx), {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true),
        FuncPtr,
        {ArgsArray}
    );

    // 9. Return null for now (would return actual result in real impl)
    Builder.CreateRet(ConstantPointerNull::get(PointerType::get(Type::getInt8Ty(Ctx), 0)));

    errs() << "  Created shared dispatcher\n";
    return Dispatcher;
}

// === STEP 3: Process Each Function ===
static void processFunctionWithSharedDispatcher(Function* F, Function* SharedDispatcher, 
                                        uint32_t funcId, Module &M) {
    LLVMContext &Ctx = M.getContext();

    // Create metadata entry
    FunctionMetadata metadata;
    metadata.original = F;
    metadata.functionId = funcId;
    StringRef name = F->getName();

    // 1. Create two versions
    metadata.versions[0] = createVersion(F, 0, M);
    metadata.versions[1] = createVersion(F, 1, M);

    // Add debug prints to versions
    addPrintToVersion(metadata.versions[0], 0, funcId, M);
    addPrintToVersion(metadata.versions[1], 1, funcId, M);

    // 2. Create wrapper that calls shared dispatcher
    metadata.wrapper = createWrapperForSharedDispatcher(F, SharedDispatcher, funcId, M);

    // 3. Replace original function with wrapper
    F->replaceAllUsesWith(metadata.wrapper);
    metadata.wrapper->takeName(F);
    F->setName(name.str() + "_original");
    F->setLinkage(GlobalValue::InternalLinkage);

    // 4. Store metadata
    functionMap[F] = metadata;

    errs() << "  Processed function " << funcId << ": " << F->getName() << "\n";
}

// === Create Version ===
static Function* createVersion(Function* F, int versionId, Module& M) {
    ValueToValueMapTy VMap;
    Function* NewF = CloneFunction(F, VMap);
    NewF->setName(F->getName() + "_v" + std::to_string(versionId));
    NewF->setLinkage(GlobalValue::InternalLinkage);

    if (versionId == 1) {
        // Add optimization hints for v1
        NewF->addFnAttr("prefer-vector-width", "256");
    }

    M.getFunctionList().push_back(NewF);
    return NewF;
}

static void addPrintToVersion(Function* F, int versionId, uint32_t funcId, Module& M) {
    LLVMContext &Ctx = M.getContext();
    
    FunctionType* PrintfType = FunctionType::get(
        Type::getInt32Ty(Ctx),
        {PointerType::get(Type::getInt8Ty(Ctx), 0)},
        true
    );
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    BasicBlock &EntryBB = F->getEntryBlock();
    IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());

    std::string FormatStr = "[EXECUTING] Function " + std::to_string(funcId) + 
                            ", Version " + std::to_string(versionId) + 
                            " (%s)\n";
    Value* FormatStrVal = Builder.CreateGlobalStringPtr(FormatStr);
    Value* FuncNameVal = Builder.CreateGlobalStringPtr(F->getName().str());

    Builder.CreateCall(Printf, {FormatStrVal, FuncNameVal});
}

// === Create Wrapper for Shared Dispatcher ===
static Function* createWrapperForSharedDispatcher(Function* Original, 
                                            Function* SharedDispatcher,
                                            uint32_t funcId, Module& M) {
    LLVMContext &Ctx = M.getContext();

    // Create wrapper with same signature as original
    Function* Wrapper = Function::Create(
        Original->getFunctionType(),
        Original->getLinkage(),
        "temp_wrapper",
        &M
    );

    BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
    IRBuilder<> Builder(Entry);

    // Package arguments into generic array
    std::vector<Value*> ArgPtrs;
    for (auto &Arg : Wrapper->args()) {
        // Allocate space for argument
        Value* ArgAlloc = Builder.CreateAlloca(Arg.getType());
        Builder.CreateStore(&Arg, ArgAlloc);

        // Cast to i8*
        Value* ArgAsI8Ptr = Builder.CreateBitCast(ArgAlloc, PointerType::get(Type::getInt8Ty(Ctx), 0));
        ArgPtrs.push_back(ArgAsI8Ptr);
    }
    
    // Create array of argument pointers
    Value* ArgsArray = nullptr;
    if (!ArgPtrs.empty()) {
        ArgsArray = Builder.CreateAlloca(
            PointerType::get(Type::getInt8Ty(Ctx), 0),
            ConstantInt::get(Type::getInt32Ty(Ctx), ArgPtrs.size())
        );

        for (size_t i = 0; i < ArgPtrs.size(); i++) {
            Value* Slot = Builder.CreateGEP(
                PointerType::get(Type::getInt8Ty(Ctx), 0),
                ArgsArray,
                ConstantInt::get(Type::getInt32Ty(Ctx), i)
            );
            Builder.CreateStore(ArgPtrs[i], Slot);
        }
    } else {
        ArgsArray = ConstantPointerNull::get(PointerType::get(PointerType::get(Type::getInt8Ty(Ctx), 0), 0));
    }

    // Call shared dispatcher with function ID
    Builder.CreateCall(SharedDispatcher, {
        ConstantInt::get(Type::getInt32Ty(Ctx), funcId),
        ArgsArray
    });

    // Return (simplified - assumes void return)
    if (Original->getReturnType()->isVoidTy()) {
        Builder.CreateRetVoid();
    } else {
        // In real implementation, would handle return value
        Builder.CreateRet(UndefValue::get(Original->getReturnType()));
    }

    return Wrapper;
}

// === STEP 4: Create Initialization Function ===
static void createInitializationFunction(Module &M) {
    LLVMContext &Ctx = M.getContext();

    // Create __adaptive_init function
    FunctionType* InitType = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
    Function* InitFunc = Function::Create(
        InitType,
        GlobalValue::InternalLinkage,
        "__adaptive_init",
        &M
    );

    BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", InitFunc);
    IRBuilder<> Builder(Entry);

    // Populate dispatch table with function pointers
    uint32_t tableIdx = 0;
    for (auto &[OrigFunc, Metadata] : functionMap) {
        for (int v = 0; v < 2; v++) {
            Value* Slot = Builder.CreateGEP(
                dispatchTable->getValueType(),
                dispatchTable,
                {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                    ConstantInt::get(Type::getInt32Ty(Ctx), tableIdx++)}
            );
            
            Value* FuncPtr = Builder.CreateBitCast(
                Metadata.versions[v],
                PointerType::get(Type::getInt8Ty(Ctx), 0)
            );
            Builder.CreateStore(FuncPtr, Slot);
        }
    }

    // Store function count
    Builder.CreateStore(
        ConstantInt::get(Type::getInt32Ty(Ctx), functionMap.size()),
        functionCount
    );

    // Debug print
    FunctionCallee Printf = M.getOrInsertFunction("printf",
        FunctionType::get(Type::getInt32Ty(Ctx), {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true));

    Value* InitMsg = Builder.CreateGlobalStringPtr(
        "[INIT] Shared dispatcher initialized with %d functions\n");
    Value* Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), functionCount);
    Builder.CreateCall(Printf, {InitMsg, Count});

    Builder.CreateRetVoid();
    
    // Register to run before main
    // appendToGlobalCtors(M, InitFunc, 65535); -------- Fix issue here

    errs() << "  Created initialization function\n";
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

    Builder.CreateCall(Dispatcher, Args);
    Builder.CreateRetVoid();

    errs() << "  Created wrapper\n";
    return Wrapper;
}

} // namespace llvm
