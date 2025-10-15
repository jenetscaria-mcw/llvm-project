#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// Remove the anonymous namespace since it's now in the llvm namespace
namespace llvm {

  static Function* createBaselineVersion(Function* F, int versionId, Module& M);
  static Function* createOptimizedVersion(Function* F, int versionId, Module& M);
  static void addPrintToVersion(Function* F, int versionId, Module& M);
  static Function* createDispatcherWithDebug(Function* Original, Function* V0, 
                                              Function* V1, Module& M);
  static Function* createWrapper(Function* Original, Function* Dispatcher, Module& M);

PreservedAnalyses SimpleAdaptivePass::run(Module &M, ModuleAnalysisManager &) {
    errs() << "=== SimpleAdaptive Pass Running ===\n";
    
    // Find the target function
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
    StringRef name = AdaptiveFunc->getName();
    std::string s = name.str();

    Function* V0 = createBaselineVersion(AdaptiveFunc, 0, M);
    Function* V1 = createOptimizedVersion(AdaptiveFunc, 1, M);

    addPrintToVersion(V0, 0, M);
    addPrintToVersion(V1, 1, M);
    
    Function* Dispatcher = createDispatcherWithDebug(AdaptiveFunc, V0, V1, M);
    Function* Wrapper = createWrapper(AdaptiveFunc, Dispatcher, M);
    
    AdaptiveFunc->replaceAllUsesWith(Wrapper);
    Wrapper->takeName(AdaptiveFunc);
    AdaptiveFunc->setName(s + "_original");
    AdaptiveFunc->setLinkage(GlobalValue::InternalLinkage);
    
    errs() << "=== Transformation Complete ===\n";
    return PreservedAnalyses::none();
}

// Keep all your helper functions (make them static or private methods)
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
        {Type::getInt32Ty(Ctx)->getPointerTo()},
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

static Function* createDispatcherWithDebug(Function* Original, Function* V0, 
                                    Function* V1, Module& M) {
    LLVMContext &Ctx = M.getContext();
    
    Function* Dispatcher = Function::Create(
        Original->getFunctionType(),
        GlobalValue::InternalLinkage,
        Original->getName() + "_dispatcher",
        &M
    );
    
    FunctionType* PrintfType = FunctionType::get(
        Type::getInt32Ty(Ctx),
        {Type::getInt32Ty(Ctx)->getPointerTo()},
        true
    );
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
    
    GlobalVariable* Counter = new GlobalVariable(
        M,
        Type::getInt32Ty(Ctx),
        false,
        GlobalValue::InternalLinkage,
        ConstantInt::get(Type::getInt32Ty(Ctx), 0),
        "call_counter"
    );
    
    BasicBlock* Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
    IRBuilder<> Builder(Entry);
    
    Value* Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), Counter);
    Value* NewCount = Builder.CreateAdd(Count, 
        ConstantInt::get(Type::getInt32Ty(Ctx), 1));
    Builder.CreateStore(NewCount, Counter);
    
    Value* CallNumStr = Builder.CreateGlobalStringPtr(
        "[DISPATCHER] Call #%d: ");
    Builder.CreateCall(Printf, {CallNumStr, NewCount});
    
    BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
    BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
    
    Value* Two = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
    Value* Version = Builder.CreateURem(Count, Two);
    Value* Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
    Value* UseV0 = Builder.CreateICmpEQ(Version, Zero);

    Builder.CreateCondBr(UseV0, CallV0, CallV1);
    
    Builder.SetInsertPoint(CallV0);
    Value* V0Msg = Builder.CreateGlobalStringPtr("Selecting V0 (baseline)\n");
    Builder.CreateCall(Printf, {V0Msg});
    
    std::vector<Value*> Args;
    for (auto &Arg : Dispatcher->args()) {
        Args.push_back(&Arg);
    }
    Builder.CreateCall(V0, Args);
    Builder.CreateRetVoid();
    
    Builder.SetInsertPoint(CallV1);
    Value* V1Msg = Builder.CreateGlobalStringPtr("Selecting V1 (optimized)\n");
    Builder.CreateCall(Printf, {V1Msg});
    Builder.CreateCall(V1, Args);
    Builder.CreateRetVoid();
    
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
    
    Builder.CreateCall(Dispatcher, Args);
    Builder.CreateRetVoid();
    
    errs() << "  Created wrapper\n";
    return Wrapper;
}

} // namespace llvm
