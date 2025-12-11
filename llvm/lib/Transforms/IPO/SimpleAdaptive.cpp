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
#include "llvm/Support/CommandLine.h"
#include <map>
#include <vector>
#include <fstream>
#include <sstream>

using namespace llvm;

static cl::opt<bool> ProductionMode(
    "adaptive-production-mode",
    cl::desc("Enable production mode with pre-determined best versions"),
    cl::init(true));

static cl::opt<std::string> BestVersionsConfig(
    "adaptive-best-versions",
    cl::desc("Path to configuration file with best version mappings"),
    cl::value_desc("filename"),
    cl::init("/mnt/Data/kamal/new_clone/llama.cpp/adaptive_build/BestVersions.conf"));

namespace llvm
{
    struct BestVersionMap
    {
        std::map<std::string, int> functionToVersion;

        bool loadFromFile(const std::string &filename)
        {
            std::ifstream file(filename);
            if (!file.is_open())
            {
                errs() << "[PRODUCTION] Warning: Could not open config: "
                       << filename << "\n";
                return false;
            }

            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;

                size_t dashPos = line.find('-');
                if (dashPos == std::string::npos)
                    continue;

                std::string funcName = line.substr(0, dashPos);
                std::string versionStr = line.substr(dashPos + 1);

                // Trim whitespace
                funcName.erase(0, funcName.find_first_not_of(" \t"));
                funcName.erase(funcName.find_last_not_of(" \t") + 1);
                versionStr.erase(0, versionStr.find_first_not_of(" \t"));
                versionStr.erase(versionStr.find_last_not_of(" \t") + 1);

                if (versionStr[0] == 'v')
                {
                    int version = std::stoi(versionStr.substr(1));
                    functionToVersion[funcName] = version;
                    errs() << "[PRODUCTION] Loaded: " << funcName
                           << " -> v" << version << "\n";
                }
            }

            file.close();
            return !functionToVersion.empty();
        }

        int getBestVersion(const std::string &funcName) const
        {
            auto it = functionToVersion.find(funcName);
            return (it != functionToVersion.end()) ? it->second : -1;
        }
    };

    struct FunctionGroupConfig
    {
        int warmupRuns;
        int sampleRate;
        int profilingThreshold;
        std::string description;
    };

    static std::map<std::string, FunctionGroupConfig> FUNCTION_GROUPS = {
        {"HIGH_FREQUENCY", {10, 10, 49998, "Functions with millions of calls"}},
        {"MEDIUM_FREQUENCY", {10, 5, 5010, "Functions with 10k-1M calls"}},
        {"LOW_FREQUENCY", {10, 2, 510, "Functions with less than 10k calls"}}};

    static const FunctionGroupConfig DEFAULT_CONFIG = {10, 2, 1200, "Default configuration"};

    struct FunctionMetadata
    {
        Function *original;
        Function *versions[4]; // v0 to v3
        Function *wrapper;
        Function *dispatcher;
        std::string groupName;

        Function *bestVersionOnly; // Only best version for production

        GlobalVariable *callCounter;
        GlobalVariable *warmupCounter;
        GlobalVariable *profileCounter;
        GlobalVariable *cpuCycles[4];
        GlobalVariable *runCount[4];
        GlobalVariable *bestVersion;
        GlobalVariable *currentPhase;
        GlobalVariable *currentVersion;
        GlobalVariable *sampleCounter;
        GlobalVariable *functionPtr;
    };

    class SimpleAdaptivePassImpl
    {
    private:
        std::map<Function *, FunctionMetadata> functionMap;
        BestVersionMap bestVersions;

        Function *createVersion(Function *F, int versionId, Module &M);
        void createWrapperStaticVars(FunctionMetadata &metadata, Module &M);
        void processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M);
        Function *createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M);
        Function *createThinDispatcher(FunctionMetadata &metadata, Module &M);
        void createInitializationFunction(Module &M);

        void processProductionMode(Function *F, Module &M);

        std::string determineFunctionGroup(Function *F);
        const FunctionGroupConfig &getGroupConfig(const std::string &groupName);
        void printFunctionGroupInfo(Function *F, const std::string &groupName,
                                    const FunctionGroupConfig &config, raw_ostream &OS);

    public:
        SimpleAdaptivePassImpl() {}
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
    };

    PreservedAnalyses SimpleAdaptivePass::run(Module &M, ModuleAnalysisManager &AM)
    {
        SimpleAdaptivePassImpl impl;
        return impl.run(M, AM);
    }

    std::string SimpleAdaptivePassImpl::determineFunctionGroup(Function *F)
    {
        std::string funcName = F->getName().str();

        if (funcName.find("_ZN12_GLOBAL__N_115tinyBLAS_Q0_AVXI10block_q4_010block_q8_0fE7gemm4xNILi4EEEvllll") != std::string::npos)
        {
            return "LOW_FREQUENCY";
        }

        if (funcName.find("_ZN12_GLOBAL__N_18tinyBLASILi16EDv16_fS1_tffE6mnpackEllll") != std::string::npos ||
            funcName.find("quantize_row_q8_0") != std::string::npos ||
            funcName.find("ggml_compute_forward_dup") != std::string::npos ||
            funcName.find("ggml_compute_forward_rope_f32") != std::string::npos)
        {
            return "MEDIUM_FREQUENCY";
        }

        if (funcName.find("ggml_fp32_to_fp16_row") != std::string::npos ||
            funcName.find("ggml_vec_dot_q4_0_q8_0") != std::string::npos ||
            funcName.find("ggml_vec_dot_q6_K_q8_K") != std::string::npos ||
            funcName.find("ggml_vec_dot_f16") != std::string::npos)
        {
            return "HIGH_FREQUENCY";
        }

        if (F->hasFnAttribute("adaptive-group"))
        {
            Attribute groupAttr = F->getFnAttribute("adaptive-group");
            std::string groupName = groupAttr.getValueAsString().str();
            if (FUNCTION_GROUPS.find(groupName) != FUNCTION_GROUPS.end())
            {
                return groupName;
            }
        }

        return "MEDIUM_FREQUENCY";
    }

    const FunctionGroupConfig &SimpleAdaptivePassImpl::getGroupConfig(const std::string &groupName)
    {
        auto it = FUNCTION_GROUPS.find(groupName);
        if (it != FUNCTION_GROUPS.end())
        {
            return it->second;
        }
        return DEFAULT_CONFIG;
    }

    void SimpleAdaptivePassImpl::printFunctionGroupInfo(Function *F, const std::string &groupName,
                                                        const FunctionGroupConfig &config, raw_ostream &OS)
    {
        OS << "[ADAPTIVE] Function: " << F->getName()
           << " | Group: " << groupName
           << " | Warmup: " << config.warmupRuns
           << " | Sample Rate: 1/" << config.sampleRate
           << " | Threshold: " << config.profilingThreshold
           << " | Desc: " << config.description << "\n";
    }

    // ========================================================================
    // CHANGE 6: Modified run() to support production mode
    // ========================================================================
    PreservedAnalyses SimpleAdaptivePassImpl::run(Module &M, ModuleAnalysisManager &)
    {
        functionMap.clear();

        // Check if production mode is enabled
        if (ProductionMode)
        {
            errs() << "========================================\n";
            errs() << "[PRODUCTION MODE] Loading best versions\n";
            errs() << "========================================\n";

            if (!bestVersions.loadFromFile(BestVersionsConfig))
            {
                errs() << "[PRODUCTION] ERROR: Could not load config file\n";
                errs() << "[PRODUCTION] Falling back to profiling mode\n";
            }
            else
            {
                errs() << "[PRODUCTION] Configuration loaded successfully\n";
            }
        }

        std::vector<Function *> adaptiveFunctions;
        for (Function &F : M)
        {
            if (!F.isDeclaration() && F.hasFnAttribute("adaptive"))
            {
                adaptiveFunctions.push_back(&F);
            }
        }

        if (adaptiveFunctions.empty())
        {
            return PreservedAnalyses::all();
        }

        if (ProductionMode && !bestVersions.functionToVersion.empty())
        {
            // PRODUCTION MODE: Generate only best versions
            errs() << "[PRODUCTION] Processing " << adaptiveFunctions.size()
                   << " functions in production mode\n";

            for (Function *F : adaptiveFunctions)
            {
                processProductionMode(F, M);
            }
        }
        else
        {
            // PROFILING MODE: Original behavior
            uint32_t funcId = 0;
            for (Function *F : adaptiveFunctions)
            {
                std::string groupName = determineFunctionGroup(F);
                const FunctionGroupConfig &config = getGroupConfig(groupName);
                printFunctionGroupInfo(F, groupName, config, errs());
                processFunctionWithDirectDispatch(F, funcId++, M);
            }
            createInitializationFunction(M);
        }

        return PreservedAnalyses::none();
    }

    void SimpleAdaptivePassImpl::processProductionMode(Function *F, Module &M)
    {
        std::string funcName = F->getName().str();
        errs() << "\n[PRODUCTION] Processing function: " << funcName << "\n";

        // Look up best version from config
        int bestVersionIdx = bestVersions.getBestVersion(funcName);

        if (bestVersionIdx == -1)
        {
            errs() << "[PRODUCTION] No best version configured, skipping\n";
            return;
        }

        errs() << "[PRODUCTION] Best version from config: v" << bestVersionIdx << "\n";

        errs() << "[PRODUCTION] Creating best version v" << bestVersionIdx
               << " directly...\n";

        FunctionMetadata metadata;
        metadata.original = F;
        metadata.bestVersionOnly = createVersion(F, bestVersionIdx, M);

        if (metadata.bestVersionOnly)
        {
            errs() << "[PRODUCTION]  Created: "
                   << metadata.bestVersionOnly->getName() << "\n";

            errs() << "[PRODUCTION] Replacing original function with v"
                   << bestVersionIdx << "...\n";

            // Replace all uses of original with best version
            F->replaceAllUsesWith(metadata.bestVersionOnly);
            metadata.bestVersionOnly->takeName(F);

            F->setName(funcName + "_original");
            F->setLinkage(GlobalValue::InternalLinkage);


            errs() << "[PRODUCTION]  Successfully replaced " << F->getName().str()
                   << " with " << metadata.bestVersionOnly->getName().str() << "\n";
        }
        else
        {
            errs() << "[PRODUCTION] ✗ Failed to create best version v"
                   << bestVersionIdx << "\n";
        }

        functionMap[F] = metadata;
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
        case 1: // V1: VECTORIZED - Maximum SIMD (Explicit AVX512)
            NewF->addFnAttr(Attribute::NoInline);
            NewF->addFnAttr("prefer-vector-width", "512");
            NewF->addFnAttr("min-legal-vector-width", "512");
            NewF->addFnAttr("target-features", "+avx512f,+avx512vl");
            NewF->addFnAttr("vectorize-predicate", "enable");
            break;

        case 2: // V2: LOOP OPTIMIZED - Aggressive Unroll/Scalar (Forcing non-AVX)
            // Explicitly disable wider vectorization to force differentiation
            NewF->addFnAttr(Attribute::NoInline);
            //NewF->addFnAttr("target-features", "-avx512f");
            //NewF->addFnAttr("prefer-vector-width", "128");
            NewF->addFnAttr("unroll-count", "16");
            NewF->addFnAttr("unroll-full-unroll-max", "1024");
            NewF->addFnAttr("interleave-count", "4");
            break;

        case 3: // V3: FAST-MATH - Optimization for Floats/General Aggressiveness
            // These attributes enable optimizations based on relaxed math rules
            NewF->addFnAttr(Attribute::AlwaysInline); // Still useful for optimization but needs careful testing
            NewF->addFnAttr("unsafe-fp-math", "true");
            NewF->addFnAttr("no-nans-fp-math", "true");
            NewF->addFnAttr("no-infs-fp-math", "true");
            NewF->addFnAttr("inline-threshold", "100000");
            NewF->addFnAttr(Attribute::NoUnwind); // Attribute::NoUnwind is set using Attribute::get [2][3]
            break;
        }

        return NewF;
    }

    // Keep the rest of the original implementation for profiling mode
    void SimpleAdaptivePassImpl::createWrapperStaticVars(FunctionMetadata &metadata, Module &M)
    {
        // Original implementation unchanged
        LLVMContext &Ctx = M.getContext();
        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        PointerType *ptrTy = PointerType::get(metadata.original->getFunctionType(), 0);

        std::string baseName = metadata.original->getName().str();

        auto createGlobal = [&](const std::string &name, Type *type, uint64_t initVal)
        {
            Constant *InitVal = (type->isIntegerTy(32))
                                    ? cast<Constant>(ConstantInt::get(i32Ty, initVal))
                                    : cast<Constant>(ConstantInt::get(i64Ty, initVal));
            return new GlobalVariable(M, type, false, GlobalValue::InternalLinkage,
                                      InitVal, baseName + name);
        };

        metadata.callCounter = createGlobal("_call_counter", i32Ty, 0);
        metadata.warmupCounter = createGlobal("_warmup_counter", i32Ty, 0);
        metadata.profileCounter = createGlobal("_profile_counter", i32Ty, 0);
        metadata.bestVersion = createGlobal("_best_version", i32Ty, 0);
        metadata.currentPhase = createGlobal("_current_phase", i32Ty, 0);
        metadata.currentVersion = createGlobal("_current_version", i32Ty, 0);
        metadata.sampleCounter = createGlobal("_sample_counter", i32Ty, 0);

        for (int i = 0; i < 4; i++)
        {
            metadata.cpuCycles[i] = createGlobal(
                "_cpu_cycles_v" + std::to_string(i), i64Ty, 0);
            metadata.runCount[i] = createGlobal(
                "_run_count_v" + std::to_string(i), i32Ty, 0);
        }

        metadata.functionPtr = new GlobalVariable(
            M, ptrTy, false, GlobalValue::InternalLinkage,
            ConstantPointerNull::get(ptrTy),
            baseName + "_function_ptr");
    }

    void SimpleAdaptivePassImpl::processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M)
    {
        // Original profiling mode implementation
        FunctionMetadata metadata;
        metadata.original = F;

        for (int i = 0; i < 4; i++)
        {
            metadata.versions[i] = createVersion(F, i, M);
        }

        createWrapperStaticVars(metadata, M);
        metadata.dispatcher = createThinDispatcher(metadata, M);
        metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

        LLVMContext &Ctx = M.getContext();
        IRBuilder<> Builder(Ctx);
        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", metadata.dispatcher);
        Builder.SetInsertPoint(Entry);

        LoadInst *FuncLoad = Builder.CreateLoad(
            metadata.original->getFunctionType()->getPointerTo(),
            metadata.functionPtr);
        FuncLoad->setAtomic(AtomicOrdering::Acquire);
        FuncLoad->setAlignment(Align(8));

        Value *IsNull = Builder.CreateIsNull(FuncLoad);
        BasicBlock *InitBB = BasicBlock::Create(Ctx, "init", metadata.dispatcher);
        BasicBlock *CallBB = BasicBlock::Create(Ctx, "call", metadata.dispatcher);
        Builder.CreateCondBr(IsNull, InitBB, CallBB);

        Builder.SetInsertPoint(InitBB);
        Value *WrapperPtr = Builder.CreatePointerCast(
            metadata.wrapper,
            metadata.original->getFunctionType()->getPointerTo());
        StoreInst *InitStore = Builder.CreateStore(WrapperPtr, metadata.functionPtr);
        InitStore->setAtomic(AtomicOrdering::Release);
        InitStore->setAlignment(Align(8));
        Builder.CreateBr(CallBB);

        Builder.SetInsertPoint(CallBB);
        PHINode *Phi = Builder.CreatePHI(
            metadata.original->getFunctionType()->getPointerTo(), 2);
        Phi->addIncoming(FuncLoad, Entry);
        Phi->addIncoming(WrapperPtr, InitBB);

        std::vector<Value *> Args;
        for (auto &Arg : metadata.dispatcher->args())
            Args.push_back(&Arg);

        if (F->getReturnType()->isVoidTy())
        {
            Builder.CreateCall(F->getFunctionType(), Phi, Args);
            Builder.CreateRetVoid();
        }
        else
        {
            Value *Result = Builder.CreateCall(F->getFunctionType(), Phi, Args);
            Builder.CreateRet(Result);
        }

        F->replaceAllUsesWith(metadata.dispatcher);
        functionMap[F] = metadata;
    }

    Function *SimpleAdaptivePassImpl::createThinDispatcher(FunctionMetadata &metadata, Module &M)
    {
        Function *Orig = metadata.original;
        FunctionType *FT = Orig->getFunctionType();
        std::string dispatcherName = Orig->getName().str() + "_dispatch";

        Function *Dispatcher = Function::Create(
            FT, GlobalValue::ExternalLinkage, dispatcherName, &M);

        size_t argIdx = 0;
        for (auto &Arg : Dispatcher->args())
        {
            if (argIdx < Orig->arg_size())
            {
                Arg.setName(Orig->getArg(argIdx)->getName());
            }
            argIdx++;
        }

        return Dispatcher;
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
    {
        // Original wrapper implementation - keeping for profiling mode
        Function *Orig = metadata.original;
        std::string wrapperName = Orig->getName().str() + "_wrapper";

        if (M.getFunction(wrapperName))
        {
            return M.getFunction(wrapperName);
        }

        Function *Wrapper = Function::Create(
            Orig->getFunctionType(),
            GlobalValue::InternalLinkage,
            wrapperName, &M);

        size_t argIdx = 0;
        for (auto &Arg : Wrapper->args())
        {
            if (argIdx < Orig->arg_size())
            {
                Arg.setName(Orig->getArg(argIdx)->getName());
            }
            argIdx++;
        }

        LLVMContext &Ctx = M.getContext();
        IRBuilder<> Builder(Ctx);

        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
        BasicBlock *WarmupBB = BasicBlock::Create(Ctx, "warmup", Wrapper);
        BasicBlock *ProfilingBB = BasicBlock::Create(Ctx, "profiling", Wrapper);
        BasicBlock *OptimalPhaseBB = BasicBlock::Create(Ctx, "optimal_phase", Wrapper);

        Builder.SetInsertPoint(Entry);

        Type *i32Ty = Type::getInt32Ty(Ctx);
        LoadInst *PhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
        PhaseLoad->setAtomic(AtomicOrdering::Acquire);
        PhaseLoad->setAlignment(Align(4));

        SwitchInst *PhaseSwitch = Builder.CreateSwitch(PhaseLoad, WarmupBB, 3);
        PhaseSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)), WarmupBB);
        PhaseSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)), ProfilingBB);
        PhaseSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)), OptimalPhaseBB);

        Builder.SetInsertPoint(WarmupBB);
        {
            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(metadata.versions[0]->getFunctionType(),
                                   metadata.versions[0], Args);
                Builder.CreateRetVoid();
            }
            else
            {
                Value *Result = Builder.CreateCall(metadata.versions[0]->getFunctionType(),
                                                   metadata.versions[0], Args);
                Builder.CreateRet(Result);
            }
        }

        Builder.SetInsertPoint(ProfilingBB);
        {
            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(metadata.versions[0]->getFunctionType(),
                                   metadata.versions[0], Args);
                Builder.CreateRetVoid();
            }
            else
            {
                Value *Result = Builder.CreateCall(metadata.versions[0]->getFunctionType(),
                                                   metadata.versions[0], Args);
                Builder.CreateRet(Result);
            }
        }

        Builder.SetInsertPoint(OptimalPhaseBB);
        {
            LoadInst *BestLoad = Builder.CreateLoad(i32Ty, metadata.bestVersion);
            BestLoad->setAtomic(AtomicOrdering::Acquire);
            BestLoad->setAlignment(Align(4));

            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < 4; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(
                    metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < 4; i++)
            {
                Value *IsBestVersionI = Builder.CreateICmpEQ(BestLoad,
                                                             ConstantInt::get(i32Ty, i));
                FuncPtr = Builder.CreateSelect(IsBestVersionI, VersionFuncs[i], FuncPtr);
            }

            std::vector<Value *> Args;
            for (auto &Arg : Wrapper->args())
                Args.push_back(&Arg);

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
                Builder.CreateRetVoid();
            }
            else
            {
                Value *Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtr, Args);
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
        Function *InitFunc = Function::Create(InitType, GlobalValue::InternalLinkage,
                                              "__adaptive_init", &M);
        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", InitFunc);
        IRBuilder<> Builder(Entry);

        FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

        Value *GroupHeaderMsg = Builder.CreateGlobalStringPtr(
            "[INIT] Adaptive dispatcher with MANUAL GROUP CONFIGURATIONS initialized:\n");
        Builder.CreateCall(Printf, {GroupHeaderMsg});

        for (const auto &[groupName, config] : FUNCTION_GROUPS)
        {
            Value *GroupMsg = Builder.CreateGlobalStringPtr(
                "  - Group %s: Warmup=%d, Sample Rate=1/%d, Threshold=%d (%s)\n");
            Value *GroupNameStr = Builder.CreateGlobalStringPtr(groupName);
            Value *DescStr = Builder.CreateGlobalStringPtr(config.description);
            Builder.CreateCall(Printf, {GroupMsg, GroupNameStr,
                                        ConstantInt::get(Type::getInt32Ty(Ctx), config.warmupRuns),
                                        ConstantInt::get(Type::getInt32Ty(Ctx), config.sampleRate),
                                        ConstantInt::get(Type::getInt32Ty(Ctx), config.profilingThreshold),
                                        DescStr});
        }
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm