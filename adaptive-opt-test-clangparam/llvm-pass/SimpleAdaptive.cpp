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
        
        // Create dispatcher with debug output
        Function* Dispatcher = createDispatcherWithDebug(AdaptiveFunc, V0, V1, M);
        
        
        //Function* Dispatcher = createDispatcher(AdaptiveFunc, V0, V1, M);
        
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
        
        // Create counter
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
        
        // Load and increment counter
        Value* Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), Counter);
        Value* NewCount = Builder.CreateAdd(Count, 
            ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        Builder.CreateStore(NewCount, Counter);
        
        // Print current call number
        Value* CallNumStr = Builder.CreateGlobalStringPtr(
            "[DISPATCHER] Call #%d: ");
        Builder.CreateCall(Printf, {CallNumStr, NewCount});
        
        // Decide which version
        //Value* UseV1 = Builder.CreateICmpUGE(Count, 
        //    ConstantInt::get(Type::getInt32Ty(Ctx), 100));
        
        BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
        BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
        
        // ROUND-ROBIN: Alternate between versions
        Value* Two = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
        Value* Version = Builder.CreateURem(Count, Two);  // Count % 2

        // Version will be: 0, 1, 0, 1, 0, 1...
        Value* Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
        Value* UseV0 = Builder.CreateICmpEQ(Version, Zero);

        // Branch based on even/odd
        Builder.CreateCondBr(UseV0, CallV0, CallV1);
        //Builder.CreateCondBr(UseV1, CallV1, CallV0);
        
        // Call V0 path
        Builder.SetInsertPoint(CallV0);
        Value* V0Msg = Builder.CreateGlobalStringPtr("Selecting V0 (baseline)\n");
        Builder.CreateCall(Printf, {V0Msg});
        
        std::vector<Value*> Args;
        for (auto &Arg : Dispatcher->args()) {
            Args.push_back(&Arg);
        }
        Builder.CreateCall(V0, Args);
        Builder.CreateRetVoid();
        
        // Call V1 path
        Builder.SetInsertPoint(CallV1);
        Value* V1Msg = Builder.CreateGlobalStringPtr("Selecting V1 (optimized)\n");
        Builder.CreateCall(Printf, {V1Msg});
        Builder.CreateCall(V1, Args);
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
        
        Value* UseV1 = Builder.CreateICmpUGE(Count, 
            ConstantInt::get(Type::getInt32Ty(Ctx), 100));
        
        BasicBlock* CallV0 = BasicBlock::Create(Ctx, "call_v0", Dispatcher);
        BasicBlock* CallV1 = BasicBlock::Create(Ctx, "call_v1", Dispatcher);
        
        Builder.CreateCondBr(UseV1, CallV1, CallV0);
        
        Builder.SetInsertPoint(CallV0);
        std::vector<Value*> Args;
        for (auto &Arg : Dispatcher->args()) {
            Args.push_back(&Arg);
        }
        Builder.CreateCall(V0, Args);
        Builder.CreateRetVoid();
        
        Builder.SetInsertPoint(CallV1);
        Builder.CreateCall(V1, Args);
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