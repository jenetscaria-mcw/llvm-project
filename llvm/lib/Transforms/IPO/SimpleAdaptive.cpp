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
#include <map>
#include <vector>
#include <algorithm>

#define ADAPTIVE_DEBUG_PRINTS 0

using namespace llvm;

namespace llvm
{

    struct FunctionGroupConfig
    {
        int warmupRuns;
        int sampleRate;
        int profilingThreshold;
        std::string description;
    };

    // Updated for dynamic thresholds
    struct FunctionMetadata
    {
        Function *original;
        Function *versions[4]; // v0 to v3
        Function *wrapper;
        Function *dispatcher;       // Dispatcher function
        FunctionGroupConfig config; // Store per-function config

        // Per-function static globals for adaptive runtime tracking
        GlobalVariable *callCounter;    // total call count
        GlobalVariable *warmupCounter;  // warmup call count
        GlobalVariable *profileCounter; // profiled call count (sampled)
        GlobalVariable *cpuCycles[4];        // accumulated cpu cycles per version
        GlobalVariable *cpuCyclesSquared[4]; // PHASE 2: accumulated squared cycles for variance
        GlobalVariable *runCount[4];         // actual profiled runs per version
        GlobalVariable *bestVersion;    // best version index after profiling
        GlobalVariable *currentPhase;   // 0=warmup, 1=profiling, 2=optimal
        GlobalVariable *currentVersion; // current version being tested
        GlobalVariable *sampleCounter;  // counter for sampling
        GlobalVariable *functionPtr;    // Function pointer for direct dispatch either wrapper or best function
    };


    struct FunctionCharacteristics
    {
        int instructionCount;
        int basicBlockCount;
        int loopCount;              // Approximate loop count
        int loadStoreCount;         // Memory operations
        int fpOpCount;              // Floating-point operations
        int callCount;              // Function calls
        int branchCount;            // Branches
        
        // Derived characteristics
        bool isSmall;               // < 50 instructions
        bool isMedium;              // 50-200 instructions
        bool isLarge;               // > 200 instructions
        bool hasLoops;              // Has backward branches (approximation)
        bool isMemoryIntensive;     // High load/store ratio
        bool hasFPMath;             // Has floating-point operations
        bool manyBranches;          // Branch-heavy
        
        // Optimization recommendations
        bool canBenefitFromVectorization;
        bool canBenefitFromUnrolling;
        bool canBenefitFromInlining;
        bool canBenefitFromFastMath;
    };
    
    static FunctionCharacteristics analyzeFunctionCharacteristics(Function *F)
    {
        FunctionCharacteristics fc = {};
        
        // Count instructions and categorize
        for (BasicBlock &BB : *F)
        {
            fc.basicBlockCount++;
            
            for (Instruction &I : BB)
            {
                fc.instructionCount++;
                
                // Categorize instruction types
                if (isa<LoadInst>(I) || isa<StoreInst>(I))
                {
                    fc.loadStoreCount++;
                }
                else if (isa<CallInst>(I))
                {
                    fc.callCount++;
                }
                else if (isa<BranchInst>(I))
                {
                    fc.branchCount++;
                    
                    // Simple loop detection: backward branches
                    BranchInst *BI = cast<BranchInst>(&I);
                    if (BI->isConditional())
                    {
                        // Check if branch target is before current instruction
                        // This is a simple heuristic for loop detection
                        fc.loopCount++;
                    }
                }
                
                // Check for floating-point operations
                Type *T = I.getType();
                if (T->isFloatingPointTy())
                {
                    fc.fpOpCount++;
                }
                
                // Also check operands for FP types
                for (Use &U : I.operands())
                {
                    if (U.get()->getType()->isFloatingPointTy())
                    {
                        fc.hasFPMath = true;
                    }
                }
            }
        }
        
        // Derive characteristics
        fc.isSmall = (fc.instructionCount < 50);
        fc.isMedium = (fc.instructionCount >= 50 && fc.instructionCount <= 200);
        fc.isLarge = (fc.instructionCount > 200);
        fc.hasLoops = (fc.loopCount > 0);
        fc.isMemoryIntensive = (fc.loadStoreCount > fc.instructionCount / 3);
        fc.hasFPMath = (fc.fpOpCount > 0);
        fc.manyBranches = (fc.branchCount > 10);
        
        // Optimization recommendations
        // Vectorization: beneficial for loops with reasonable size
        fc.canBenefitFromVectorization = fc.hasLoops && !fc.isLarge && !fc.isMemoryIntensive;
        
        // Unrolling: beneficial for small to medium loops
        fc.canBenefitFromUnrolling = fc.hasLoops && (fc.isSmall || fc.isMedium);
        
        // Inlining: only for very small functions without many calls
        fc.canBenefitFromInlining = fc.isSmall && (fc.callCount < 3);
        
        // Fast math: beneficial for FP-heavy, compute-bound code
        fc.canBenefitFromFastMath = fc.hasFPMath && !fc.isMemoryIntensive;
        
        return fc;
    }

    // Extract expected call count from function attributes / metadata
    static uint64_t getExpectedCallCount(Function *F)
    {
       
        if (F->hasFnAttribute("adaptive"))
        {
            Attribute Attr = F->getFnAttribute("adaptive");
            StringRef Val = Attr.getValueAsString(); // e.g. "388"
            uint64_t Count = 0;
            if (!Val.empty() && !Val.getAsInteger(10, Count))
                return Count;
        }


        if (F->hasMetadata())
        {
            if (MDNode *MD = F->getMetadata("annotation"))
            {
                for (unsigned i = 0; i < MD->getNumOperands(); i++)
                {
                    if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(i)))
                    {
                        StringRef anno = MDS->getString();
                        if (anno.starts_with("adaptive:"))
                        {
                            uint64_t count;
                            if (!anno.substr(9).getAsInteger(10, count))
                            {
                                return count;
                            }
                        }
                    }
                }
            }
        }

        if (auto EntryCount = F->getEntryCount())
        {
            return EntryCount->getCount();
        }

        // Method 4: Estimate from function complexity (fallback)
        size_t numInstructions = 0;
        for (auto &BB : *F)
        {
            numInstructions += BB.size();
        }

        if (numInstructions < 30)
            return 100000;
        else if (numInstructions < 100)
            return 50000;
        else if (numInstructions < 300)
            return 10000;
        else
            return 1000;
    }

    // Calculate dynamic thresholds based on expected call count
    static FunctionGroupConfig calculateDynamicThresholds(uint64_t expectedCalls, const std::string &source)
    {
        FunctionGroupConfig config;

        // Configuration parameters
        const uint64_t MIN_PROFILE_SAMPLES = 40;      // Was 200 (5× reduction)
        const uint64_t MAX_PROFILE_SAMPLES = 400;     // Was 4000 (10× reduction)
        const double PROFILE_PERCENTAGE = 0.001;      // Was 0.01 (10× reduction = 0.1%)
        const double PROFILING_PHASE_PERCENTAGE = 0.20; // Use 20% of calls for profiling, 80% for optimal

        // Calculate target profiling samples (3% of total calls)
        uint64_t targetSamples = (uint64_t)(expectedCalls * PROFILE_PERCENTAGE);

        // Clamp to reasonable range
        targetSamples = std::max(targetSamples, MIN_PROFILE_SAMPLES);
        targetSamples = std::min(targetSamples, MAX_PROFILE_SAMPLES);

        // Ensure divisible by 4 (we have 4 versions)
        targetSamples = (targetSamples / 4) * 4;

        config.profilingThreshold = (int)targetSamples;


        int warmupRounds = 0;  
        warmupRounds = std::min(warmupRounds, 50); // Max 50 rounds (200 total warmup calls)
        config.warmupRuns = warmupRounds; // Each round = 4 calls (one per version)

        uint64_t targetProfilingCalls = (uint64_t)(expectedCalls * PROFILING_PHASE_PERCENTAGE);
        
        if (targetProfilingCalls > targetSamples)
        {
            config.sampleRate = (int)(targetProfilingCalls / targetSamples);
        }
        else
        {
            // For very cold functions, sample every call
            config.sampleRate = 1;
        }

        // Ensure sample rate is reasonable
        config.sampleRate = std::max(1, std::min(config.sampleRate, 500));

        // Set description
        config.description = source;

        return config;
    }

    class SimpleAdaptivePassImpl
    {
    private:
        std::map<Function *, FunctionMetadata> functionMap;

        Function *createVersion(Function *F, int versionId, Module &M);
        void createWrapperStaticVars(FunctionMetadata &metadata, Module &M);
        void processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M);
        Function *createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M);
        Function *createThinDispatcher(FunctionMetadata &metadata, Module &M);
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
        functionMap.clear();

        // Collect adaptive functions
        std::vector<Function *> adaptiveFunctions;
        for (Function &F : M)
        {
            if (!F.isDeclaration() && F.hasFnAttribute("adaptive"))
            {
                adaptiveFunctions.push_back(&F);
            }
        }
        
        std::vector<Function *> filteredFunctions;
        for (Function *F : adaptiveFunctions)
        {
            // Count instructions - skip tiny functions
            size_t instructionCount = 0;
            for (BasicBlock &BB : *F) {
                instructionCount += BB.size();
            }
            if (instructionCount < 100) {  
                errs() << "SKIP (tiny): " << F->getName() 
                       << " (" << instructionCount << " instructions)\n";
                continue;
            }
            
            //  Skip cold functions
            uint64_t expectedCalls = getExpectedCallCount(F);
            if (expectedCalls < 50000) { 
                errs() << "SKIP (cold): " << F->getName() 
                       << " (" << expectedCalls << " expected calls)\n";
                continue;
            }
            
            // Must have loops
            bool hasLoops = false;
            int branchCount = 0;
            for (BasicBlock &BB : *F) {
                for (Instruction &I : BB) {
                    if (isa<BranchInst>(&I)) {
                        branchCount++;
                        if (branchCount >= 2) {
                            hasLoops = true;
                            break;
                        }
                    }
                }
                if (hasLoops) break;
            }
            
            if (!hasLoops) {
                errs() << "SKIP (no loops): " << F->getName() << "\n";
                continue;
            }
            
            // Must have optimization potential
            FunctionCharacteristics fc = analyzeFunctionCharacteristics(F);
            if (!fc.canBenefitFromVectorization && 
                !fc.canBenefitFromUnrolling &&
                !fc.canBenefitFromFastMath) {
                errs() << "SKIP (no opt potential): " << F->getName() << "\n";
                continue;
            }
            
            // Skip if too memory-bound
            if (fc.isMemoryIntensive && fc.loadStoreCount > instructionCount * 0.7) {
                errs() << "SKIP (memory-bound): " << F->getName() 
                       << " (" << fc.loadStoreCount << " mem ops)\n";
                continue;
            }
            
            // Passed all filters
            errs() << "ACCEPT: " << F->getName() 
                   << " (inst=" << instructionCount 
                   << ", calls=" << expectedCalls << ")\n";
            filteredFunctions.push_back(F);
        }
        
        // Use filtered list
        adaptiveFunctions = filteredFunctions;
        if (adaptiveFunctions.empty())
        {
            return PreservedAnalyses::all();
        }

        uint32_t funcId = 0;
        for (Function *F : adaptiveFunctions)
        {
            // Calculate dynamic thresholds per function
            uint64_t expectedCalls = getExpectedCallCount(F);

            // Determine source
            std::string source;
            if (F->hasMetadata() && F->getMetadata("annotation"))
            {
                source = "attribute";
            }
            else if (F->getEntryCount())
            {
                source = "PGO";
            }
            else
            {
                source = "estimated";
            }

            FunctionGroupConfig config = calculateDynamicThresholds(expectedCalls, source);

            // Store config in metadata
            FunctionMetadata &metadata = functionMap[F];
            metadata.original = F;
            metadata.config = config;

            // Print configuration
            errs() << "ADAPTIVE Function: " << F->getName()
                   << " | Expected calls: " << expectedCalls << " (" << source << ")"
                   << " | Warmup: " << config.warmupRuns
                   << " | Sample rate: 1/" << config.sampleRate
                   << " | Profile threshold: " << config.profilingThreshold
                   << " (" << (config.profilingThreshold / 4) << " per version)\n";

            processFunctionWithDirectDispatch(F, funcId++, M);
        }

        createInitializationFunction(M);

        return PreservedAnalyses::none();
    }

    void SimpleAdaptivePassImpl::createWrapperStaticVars(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        const std::string baseName = metadata.original->getName().str();

        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        Type *funcPtrTy = metadata.original->getFunctionType()->getPointerTo();

        const std::string counterName = "__static_counter_" + baseName;
        const std::string warmupName = "__static_warmup_" + baseName;
        const std::string profileName = "__static_profile_" + baseName;
        const std::string bestName = "__static_best_" + baseName;
        const std::string phaseName = "__static_phase_" + baseName;
        const std::string currentVersionName = "__static_current_version_" + baseName;
        const std::string sampleCounterName = "__static_sample_counter_" + baseName;
        const std::string funcPtrName = "__static_funcptr_" + baseName;
        const std::string cyclesBase = "__static_cycles_" + baseName + "_v";
        const std::string runsBase = "__static_runs_" + baseName + "_v";
        const std::string cyclesSquaredBase = "__static_cycles_sq_" + baseName + "_v"; // PHASE 2

        // Create variables with atomic alignment
        metadata.callCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), counterName);
        metadata.callCounter->setAlignment(Align(4));

        metadata.warmupCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), warmupName);
        metadata.warmupCounter->setAlignment(Align(4));

        metadata.profileCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), profileName);
        metadata.profileCounter->setAlignment(Align(4));

        metadata.sampleCounter = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), sampleCounterName);
        metadata.sampleCounter->setAlignment(Align(4));

        auto *ptrToFuncTy = PointerType::get(funcPtrTy, 0); // i.e. T*
        auto *nullFuncPtr = ConstantPointerNull::get(ptrToFuncTy);

        metadata.functionPtr = new GlobalVariable(
            M,
            ptrToFuncTy, // The type of the global var
            false,
            GlobalValue::InternalLinkage,
            nullFuncPtr, // initializer
            funcPtrName);

        metadata.functionPtr->setAlignment(Align(8));

        const std::array<std::string, 4> versionStrs = {
            "0", "1", "2", "3"};

        for (int v = 0; v < 4; v++)
        {
            metadata.cpuCycles[v] = new GlobalVariable(
                M, i64Ty, false, GlobalValue::InternalLinkage,
                ConstantInt::get(i64Ty, 0), cyclesBase + versionStrs[v]);
            metadata.cpuCycles[v]->setAlignment(Align(8));
            
            // PHASE 2: Create squared cycles global for variance tracking
            metadata.cpuCyclesSquared[v] = new GlobalVariable(
                M, i64Ty, false, GlobalValue::InternalLinkage,
                ConstantInt::get(i64Ty, 0), cyclesSquaredBase + versionStrs[v]);
            metadata.cpuCyclesSquared[v]->setAlignment(Align(8));

            metadata.runCount[v] = new GlobalVariable(
                M, i32Ty, false, GlobalValue::InternalLinkage,
                ConstantInt::get(i32Ty, 0), runsBase + versionStrs[v]);
            metadata.runCount[v]->setAlignment(Align(4));
        }

        metadata.bestVersion = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, -1), bestName);
        metadata.bestVersion->setAlignment(Align(4));

        metadata.currentPhase = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), phaseName); // 0=warmup, 1=profiling, 2=optimal
        metadata.currentPhase->setAlignment(Align(4));

        metadata.currentVersion = new GlobalVariable(
            M, i32Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(i32Ty, 0), currentVersionName);
        metadata.currentVersion->setAlignment(Align(4));
    }

    // ============================================================================
    // PHASE 1: MODIFIED createVersion() WITH INTELLIGENT VERSION GENERATION
    // ============================================================================
    
    Function *SimpleAdaptivePassImpl::createVersion(Function *F, int versionId, Module &M)
    {
        ValueToValueMapTy VMap;
        Function *NewF = CloneFunction(F, VMap);
        NewF->setName(F->getName() + "_v" + std::to_string(versionId));
        NewF->setLinkage(GlobalValue::InternalLinkage);

        // PHASE 1 ADDITION: Analyze function characteristics
        FunctionCharacteristics fc = analyzeFunctionCharacteristics(F);
        
        // Print characteristics for debugging
        #if ADAPTIVE_DEBUG_PRINTS
        errs() << "  Function characteristics for " << F->getName() << ":\n";
        errs() << "    Instructions: " << fc.instructionCount << "\n";
        errs() << "    Loops: " << fc.loopCount << "\n";
        errs() << "    Memory ops: " << fc.loadStoreCount << "\n";
        errs() << "    FP ops: " << fc.fpOpCount << "\n";
        errs() << "    Can vectorize: " << fc.canBenefitFromVectorization << "\n";
        errs() << "    Can unroll: " << fc.canBenefitFromUnrolling << "\n";
        errs() << "    Can inline: " << fc.canBenefitFromInlining << "\n";
        #endif

        switch (versionId)
        {
        case 0: 
            // Baseline - no optimizations
            break;
            
        case 1:
            // V1: VECTORIZATION-FOCUSED 
            NewF->addFnAttr(Attribute::NoInline);
            
            if (fc.canBenefitFromVectorization)
            {
                // Best case: aggressive vectorization
                NewF->addFnAttr("prefer-vector-width", "512");
                NewF->addFnAttr("min-legal-vector-width", "512");
                NewF->addFnAttr("target-features", "+avx512f,+avx512vl");
            }
            else if (fc.isMemoryIntensive)
            {
                // Memory-bound: prefetching
                NewF->addFnAttr("prefetch-distance", "128");
            }
            else
            {
                // Fallback: light unrolling (always different from V0)
                NewF->addFnAttr("unroll-count", "4");
            }
            break;

        case 2: 
            // V2: UNROLLING-FOCUSED 
            NewF->addFnAttr(Attribute::NoInline);
            
            if (fc.canBenefitFromUnrolling && fc.isSmall)
            {
                // Best case: aggressive unrolling for small loops
                NewF->addFnAttr("unroll-count", "16");
                NewF->addFnAttr("unroll-full-unroll-max", "1024");
                NewF->addFnAttr("interleave-count", "4");
            }
            else if (fc.canBenefitFromUnrolling && fc.isMedium)
            {
                // Medium: moderate unrolling
                NewF->addFnAttr("unroll-count", "8");
                NewF->addFnAttr("interleave-count", "2");
            }
            else if (fc.canBenefitFromUnrolling)
            {
                // Large functions: conservative unrolling
                NewF->addFnAttr("unroll-count", "4");
            }
            else
            {
                // Fallback: always apply at least minimal unrolling
                NewF->addFnAttr("unroll-count", "2");
            }
            break;

        case 3: 
            // V3: SPECIALTY 
            if (fc.canBenefitFromInlining)
            {
                // Best case: aggressive inlining for small functions
                NewF->addFnAttr(Attribute::AlwaysInline);
                NewF->addFnAttr("inline-threshold", "100000");
            }
            else if (fc.canBenefitFromFastMath)
            {
                // FP-heavy: fast math
                NewF->addFnAttr(Attribute::NoInline);
                NewF->addFnAttr("unsafe-fp-math", "true");
                NewF->addFnAttr("no-nans-fp-math", "true");
                NewF->addFnAttr("no-infs-fp-math", "true");
            }
            else if (fc.manyBranches)
            {
                // Branch-heavy: optimize for size (better branch prediction)
                NewF->addFnAttr(Attribute::OptimizeForSize);
                NewF->addFnAttr(Attribute::NoInline);
            }
            else
            {
                // Fallback: moderate unrolling (different from V1/V2)
                NewF->addFnAttr(Attribute::NoInline);
                NewF->addFnAttr("unroll-count", "6");
            }
            break;
        }

        return NewF;
    }

    Function *SimpleAdaptivePassImpl::createThinDispatcher(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;

        // Create dispatcher function with same signature as original
        Function *Dispatcher = Function::Create(
            Orig->getFunctionType(),
            Orig->getLinkage(),
            Orig->getName() + "_dispatcher",
            &M);
        Dispatcher->copyAttributesFrom(Orig);

        // Mark as always inline for minimal overhead
        Dispatcher->addFnAttr(Attribute::AlwaysInline);

        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
        IRBuilder<> Builder(Entry);

        // Load the function pointer
        LoadInst *FuncPtrLoad = Builder.CreateLoad(
            metadata.original->getFunctionType()->getPointerTo(),
            metadata.functionPtr);
        FuncPtrLoad->setAtomic(AtomicOrdering::Acquire);
        FuncPtrLoad->setAlignment(Align(8));

        // Collect arguments
        std::vector<Value *> Args;
        for (auto &Arg : Dispatcher->args())
            Args.push_back(&Arg);

        // Call through the function pointer
        if (Orig->getReturnType()->isVoidTy())
        {
            Builder.CreateCall(Orig->getFunctionType(), FuncPtrLoad, Args);
            Builder.CreateRetVoid();
        }
        else
        {
            Value *Result = Builder.CreateCall(Orig->getFunctionType(), FuncPtrLoad, Args);
            Builder.CreateRet(Result);
        }

        return Dispatcher;
    }

    void SimpleAdaptivePassImpl::processFunctionWithDirectDispatch(Function *F, uint32_t funcId, Module &M)
    {
        FunctionMetadata metadata;
        metadata.original = F;
        auto funcName = F->getName().str();
        // Config already stored in functionMap, retrieve it
        metadata.config = functionMap[F].config;

        // Create versions
        for (int i = 0; i < 4; i++)
        {
            metadata.versions[i] = createVersion(F, i, M);
        }

        // Create static vars for wrapper
        createWrapperStaticVars(metadata, M);

        // Create wrapper (profiling logic)
        metadata.wrapper = createOptimizedDispatchWrapper(metadata, M);

        // Create thin dispatcher
        metadata.dispatcher = createThinDispatcher(metadata, M);

        // Initialize function pointer to point to wrapper
        metadata.functionPtr->setInitializer(
            ConstantExpr::getPointerCast(metadata.wrapper,
                                         metadata.original->getFunctionType()->getPointerTo()));

        // Replace all uses with the dispatcher
        F->replaceAllUsesWith(metadata.dispatcher);
        metadata.dispatcher->takeName(F);

        F->setName(funcName + "_original");
        F->setLinkage(GlobalValue::InternalLinkage);

        errs() << F->getName().str() << "\n";
        errs() << metadata.wrapper->getName().str() << "\n";
        errs() << metadata.dispatcher->getName().str() << "\n";
        errs() << metadata.dispatcher->getName().str() << "\n";

        functionMap[F] = metadata;
    }

    Function *SimpleAdaptivePassImpl::createOptimizedDispatchWrapper(FunctionMetadata &metadata, Module &M)
    {
        LLVMContext &Ctx = M.getContext();
        Function *Orig = metadata.original;

        // NEW: Get configuration for this function's group
        // Get configuration for this function
        const FunctionGroupConfig &config = metadata.config;

        // Create wrapper function
        Function *Wrapper = Function::Create(
            Orig->getFunctionType(),
            GlobalValue::InternalLinkage, // Internal linkage
            Orig->getName() + "_wrapper",
            &M);
        Wrapper->copyAttributesFrom(Orig);

        // Set up basic blocks
        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
        BasicBlock *WarmupPhaseBB = BasicBlock::Create(Ctx, "warmup_phase", Wrapper);
        BasicBlock *ProfilingPhaseBB = BasicBlock::Create(Ctx, "profiling_phase", Wrapper);
        BasicBlock *OptimalPhaseBB = BasicBlock::Create(Ctx, "optimal_phase", Wrapper);
        BasicBlock *UpdateStatsBB = BasicBlock::Create(Ctx, "update_stats", Wrapper);
        BasicBlock *TransitionToProfilingBB = BasicBlock::Create(Ctx, "transition_to_profiling", Wrapper);
        BasicBlock *TransitionToOptimalBB = BasicBlock::Create(Ctx, "transition_to_optimal", Wrapper);

        IRBuilder<> Builder(Entry);

        // Types and constants
        Type *i32Ty = Type::getInt32Ty(Ctx);
        Type *i64Ty = Type::getInt64Ty(Ctx);
        // Constants
        ConstantInt *zero32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 0));
        ConstantInt *one32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 1));
        ConstantInt *two32 = cast<ConstantInt>(ConstantInt::get(i32Ty, 2));
        ConstantInt *warmupThreshold = cast<ConstantInt>(ConstantInt::get(i32Ty, config.warmupRuns * 4)); // Total warmup runs
        ConstantInt *sampleRate = cast<ConstantInt>(ConstantInt::get(i32Ty, config.sampleRate));

        // Get printf for debugging
        FunctionType *PrintfType = FunctionType::get(i32Ty,
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        #if ADAPTIVE_DEBUG_PRINTS
        Value *FuncName = Builder.CreateGlobalStringPtr(Orig->getName().str());
        #endif

        // Entry: Check current phase
        LoadInst *PhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
        PhaseLoad->setAtomic(AtomicOrdering::Acquire);
        PhaseLoad->setAlignment(Align(4));

        // Branch based on phase
        SwitchInst *PhaseSwitch = Builder.CreateSwitch(PhaseLoad, OptimalPhaseBB, 3);
        PhaseSwitch->addCase(zero32, WarmupPhaseBB);
        PhaseSwitch->addCase(one32, ProfilingPhaseBB);
        PhaseSwitch->addCase(two32, OptimalPhaseBB);

        const int NUM_VERSIONS = 4;

        // Warmpup phase
        Builder.SetInsertPoint(WarmupPhaseBB);
        {
            // Increment warmup counter
            Value *WarmupCount = Builder.CreateAtomicRMW(
                AtomicRMWInst::Add, metadata.warmupCounter, one32,
                MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

            // Calculate which version to run (round-robin during warmup)
            Value *VersionToRun = Builder.CreateURem(WarmupCount, ConstantInt::get(i32Ty, NUM_VERSIONS));

            // Store current version
            StoreInst *VersionStore = Builder.CreateStore(VersionToRun, metadata.currentVersion);
            VersionStore->setAtomic(AtomicOrdering::Release);
            VersionStore->setAlignment(Align(4));

            // Check if warmup is complete
            Value *IsWarmupComplete = Builder.CreateICmpUGE(WarmupCount, warmupThreshold);
            Builder.CreateCondBr(IsWarmupComplete, TransitionToProfilingBB, UpdateStatsBB);
        }

        // Transition to Profiling
        Builder.SetInsertPoint(TransitionToProfilingBB);
        {
            // Use atomic CAS to ensure only ONE thread performs transition
            BasicBlock *DoWarmupTransitionBB = BasicBlock::Create(Ctx, "do_warmup_transition", Wrapper);
            BasicBlock *AlreadyInProfilingBB = BasicBlock::Create(Ctx, "already_in_profiling", Wrapper);

            // Attempt atomic compare-exchange: if currentPhase == 0, set it to 1
            AtomicCmpXchgInst *CASWarmup = Builder.CreateAtomicCmpXchg(
                metadata.currentPhase, zero32, one32,
                MaybeAlign(4),
                AtomicOrdering::SequentiallyConsistent,
                AtomicOrdering::SequentiallyConsistent);
            CASWarmup->setWeak(false); // Strong CAS

            // Extract the success flag from the result
            Value *CASWarmupResult = Builder.CreateExtractValue(CASWarmup, 1);
            Builder.CreateCondBr(CASWarmupResult, DoWarmupTransitionBB, AlreadyInProfilingBB);

            // If CAS failed, another thread already transitioned - go to profiling
            Builder.SetInsertPoint(AlreadyInProfilingBB);
            Builder.CreateBr(ProfilingPhaseBB);

            // Only the winning thread performs the transition
            Builder.SetInsertPoint(DoWarmupTransitionBB);
            #if ADAPTIVE_DEBUG_PRINTS
            // Print transition message
            Value *TransMsg = Builder.CreateGlobalStringPtr(
                "WARMUP COMPLETE %s - Transitioning to profiling phase after %d warmup runs\n");
            Builder.CreateCall(Printf, {TransMsg, FuncName, warmupThreshold});
            #endif

            // Phase already updated by CAS above - no need to update again

            // Reset counters for profiling phase
            StoreInst *ResetProfile = Builder.CreateStore(zero32, metadata.profileCounter);
            ResetProfile->setAtomic(AtomicOrdering::Release);
            ResetProfile->setAlignment(Align(4));

            Builder.CreateBr(ProfilingPhaseBB);
        }

        // Profiling Phase with Sampling
        Builder.SetInsertPoint(ProfilingPhaseBB);
        {
            // Increment sample counter for sampling decision
            Value *SampleCount = Builder.CreateAtomicRMW(
                AtomicRMWInst::Add, metadata.sampleCounter, one32,
                MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

            // Check if this call should be sampled (every Nth call)
            Value *ShouldSample = Builder.CreateICmpEQ(
                Builder.CreateURem(SampleCount, sampleRate), zero32);

            // Create blocks for sampling decision
            BasicBlock *DoSampleBB = BasicBlock::Create(Ctx, "do_sample", Wrapper);
            BasicBlock *SkipSampleBB = BasicBlock::Create(Ctx, "skip_sample", Wrapper);
            Builder.CreateCondBr(ShouldSample, DoSampleBB, SkipSampleBB);

            // Do Sample Block - measure performance with round-robin version selection
            Builder.SetInsertPoint(DoSampleBB);
            {
                // ROUND-ROBIN VERSION SELECTION with atomic operations to prevent race conditions
                // Atomically get next version counter value and increment it
                // Using fetch_add ensures each thread gets a unique counter value
                Value *VersionCounter = Builder.CreateAtomicRMW(
                    AtomicRMWInst::Add, metadata.currentVersion, one32,
                    MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

                // Apply modulo 4 to wrap around: v0->v1->v2->v3->v4->v5->v0->v1...
                Value *SelectedVersion = Builder.CreateURem(VersionCounter,
                                                            ConstantInt::get(i32Ty, NUM_VERSIONS));

                // Create blocks for round-robin checking
                BasicBlock *RoundRobinStartBB = BasicBlock::Create(Ctx, "roundrobin_start", Wrapper);
                Builder.CreateBr(RoundRobinStartBB);

                Builder.SetInsertPoint(RoundRobinStartBB);

                // Try up to NUM_VERSIONS times in round-robin fashion to find a version under 200 runs
                BasicBlock *CheckVersionBBs[NUM_VERSIONS];
                for (int i = 0; i < NUM_VERSIONS; i++)
                {
                    CheckVersionBBs[i] = BasicBlock::Create(Ctx, "roundrobin_check" + std::to_string(i), Wrapper);
                }

                Builder.CreateBr(CheckVersionBBs[0]);

                // Check each version in round-robin order
                for (int attempt = 0; attempt < NUM_VERSIONS; attempt++)
                {
                    Builder.SetInsertPoint(CheckVersionBBs[attempt]);

                    // Calculate which version to try: (SelectedVersion + attempt) % NUM_VERSIONS
                    Value *VersionToTry = Builder.CreateURem(
                        Builder.CreateAdd(SelectedVersion, ConstantInt::get(i32Ty, attempt)),
                        ConstantInt::get(i32Ty, NUM_VERSIONS));

                    // Create switch statement to dispatch to correct version check
                    BasicBlock *VersionCheckBBs[NUM_VERSIONS];
                    for (int v = 0; v < NUM_VERSIONS; v++)
                    {
                        VersionCheckBBs[v] = BasicBlock::Create(Ctx,
                                                                "check_v" + std::to_string(v) + "_attempt" + std::to_string(attempt), Wrapper);
                    }

                    SwitchInst *VersionSwitch = Builder.CreateSwitch(VersionToTry, VersionCheckBBs[0], NUM_VERSIONS);

                    for (int v = 0; v < NUM_VERSIONS; v++)
                    {
                        VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, v)), VersionCheckBBs[v]);

                        Builder.SetInsertPoint(VersionCheckBBs[v]);

                        // Atomically try to claim a run for this version
                        Value *OldRunCount = Builder.CreateAtomicRMW(
                            AtomicRMWInst::Add, metadata.runCount[v], one32,
                            MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

                        // Check if this version still needs runs (old count < 200)
                        Value *NeedsRun = Builder.CreateICmpULT(OldRunCount, ConstantInt::get(i32Ty, (config.profilingThreshold / 4)));

                        BasicBlock *UseVersionBB = BasicBlock::Create(Ctx,
                                                                      "use_v" + std::to_string(v) + "_attempt" + std::to_string(attempt), Wrapper);

                        // Create unique decrement block for this specific version
                        BasicBlock *DecrementVersionBB = BasicBlock::Create(Ctx,
                                                                            "decrement_v" + std::to_string(v) + "_attempt" + std::to_string(attempt), Wrapper);

                        Builder.CreateCondBr(NeedsRun, UseVersionBB, DecrementVersionBB);

                        // Use this version - store it and profile
                        Builder.SetInsertPoint(UseVersionBB);
                        StoreInst *VersionStore = Builder.CreateStore(ConstantInt::get(i32Ty, v), metadata.currentVersion);
                        VersionStore->setAtomic(AtomicOrdering::Release);
                        VersionStore->setAlignment(Align(4));
                        Builder.CreateBr(UpdateStatsBB);

                        // Didn't use this version - decrement back in version-specific block
                        Builder.SetInsertPoint(DecrementVersionBB);
                        Builder.CreateAtomicRMW(
                            AtomicRMWInst::Sub, metadata.runCount[v], one32,
                            MaybeAlign(4), AtomicOrdering::SequentiallyConsistent);

                        // After decrement, either try next attempt or transition to optimal
                        if (attempt < NUM_VERSIONS - 1)
                        {
                            Builder.CreateBr(CheckVersionBBs[attempt + 1]);
                        }
                        else
                        {
                            // All versions are full - transition to optimal phase
                            Builder.CreateBr(TransitionToOptimalBB);
                        }
                    }
                }
            }

            // Skip Sample Block - run current best or default version without measurement
            Builder.SetInsertPoint(SkipSampleBB);
            {
                // Load current version (last profiled version or default)
                LoadInst *CurrentVersionLoad = Builder.CreateLoad(i32Ty, metadata.currentVersion);
                CurrentVersionLoad->setAtomic(AtomicOrdering::Acquire);
                CurrentVersionLoad->setAlignment(Align(4));

                // Dispatch to current version without timing
                std::vector<Value *> VersionFuncs;
                for (int i = 0; i < NUM_VERSIONS; i++)
                {
                    VersionFuncs.push_back(Builder.CreatePointerCast(
                        metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
                }

                Value *FuncPtr = VersionFuncs[0];
                for (int i = 1; i < NUM_VERSIONS; i++)
                {
                    Value *IsVersionI = Builder.CreateICmpEQ(CurrentVersionLoad, ConstantInt::get(i32Ty, i));
                    FuncPtr = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtr);
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
        }

        // Update Stats Block (for sampled profiling calls and warmup)
        Builder.SetInsertPoint(UpdateStatsBB);
        {
            // Load the current version
            LoadInst *CurrentVersionLoad = Builder.CreateLoad(i32Ty, metadata.currentVersion);
            CurrentVersionLoad->setAtomic(AtomicOrdering::Acquire);
            CurrentVersionLoad->setAlignment(Align(4));

            // Load current phase to determine if we should measure
            LoadInst *CurrentPhaseLoad = Builder.CreateLoad(i32Ty, metadata.currentPhase);
            CurrentPhaseLoad->setAtomic(AtomicOrdering::Acquire);
            CurrentPhaseLoad->setAlignment(Align(4));

            // Only measure during profiling phase (not warmup)
            Value *IsProfilingPhase = Builder.CreateICmpEQ(CurrentPhaseLoad, one32);

            // Create dispatch logic with optional timing
            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(
                    metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Function *ReadCycleCounter = Intrinsic::getDeclaration(&M, Intrinsic::readcyclecounter);

            // Conditional timing based on phase
            BasicBlock *MeasureBB = BasicBlock::Create(Ctx, "measure", Wrapper);
            BasicBlock *NoMeasureBB = BasicBlock::Create(Ctx, "no_measure", Wrapper);
            BasicBlock *UpdateDispatchBB = BasicBlock::Create(Ctx, "update_dispatch", Wrapper);
            BasicBlock *ContinueBB = BasicBlock::Create(Ctx, "continue", Wrapper);

            std::vector<Value *> WrapperArgs;
            for (auto &Arg : Wrapper->args())
                WrapperArgs.push_back(&Arg);

            Value *MeasuredResult = nullptr;
            Value *NoMeasureResult = nullptr;
            std::vector<BasicBlock *> UpdateBlocks;
            UpdateBlocks.reserve(NUM_VERSIONS + 1);

            Builder.CreateCondBr(IsProfilingPhase, MeasureBB, NoMeasureBB);

            // Measure block - with timing
            Builder.SetInsertPoint(MeasureBB);
            Value *StartCycles = Builder.CreateCall(ReadCycleCounter);

            Value *FuncPtrMeasure = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsVersionI = Builder.CreateICmpEQ(CurrentVersionLoad, ConstantInt::get(i32Ty, i));
                FuncPtrMeasure = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtrMeasure);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtrMeasure, WrapperArgs);
            }
            else
            {
                MeasuredResult = Builder.CreateCall(Orig->getFunctionType(), FuncPtrMeasure, WrapperArgs);
            }

            Value *EndCycles = Builder.CreateCall(ReadCycleCounter);
            Value *ElapsedCycles = Builder.CreateSub(EndCycles, StartCycles);

            Builder.CreateBr(UpdateDispatchBB);

            // Update statistics dispatch
            Builder.SetInsertPoint(UpdateDispatchBB);
            BasicBlock *DefaultUpdateBB = BasicBlock::Create(Ctx, "update_default", Wrapper);
            SwitchInst *VersionSwitch = Builder.CreateSwitch(CurrentVersionLoad, DefaultUpdateBB, NUM_VERSIONS);

            Builder.SetInsertPoint(DefaultUpdateBB);
            UpdateBlocks.push_back(DefaultUpdateBB);
            Builder.CreateBr(ContinueBB);

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                BasicBlock *UpdateVersionBB = BasicBlock::Create(Ctx, "update_v" + std::to_string(i), Wrapper);
                VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), UpdateVersionBB);
                Builder.SetInsertPoint(UpdateVersionBB);
                // Atomic add for cycles (runCount already incremented in DoSampleBB)
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.cpuCycles[i],
                                        ElapsedCycles, MaybeAlign(8), AtomicOrdering::SequentiallyConsistent);
                
                // PHASE 2: Accumulate squared cycles for variance calculation
                Value *CyclesSquared = Builder.CreateMul(ElapsedCycles, ElapsedCycles);
                Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.cpuCyclesSquared[i],
                                        CyclesSquared, MaybeAlign(8), AtomicOrdering::SequentiallyConsistent);
                
                Builder.CreateBr(ContinueBB);
                UpdateBlocks.push_back(UpdateVersionBB);
            }

            // No measure block - warmup execution without timing
            Builder.SetInsertPoint(NoMeasureBB);
            Value *FuncPtrNoMeasure = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
            {
                Value *IsVersionI = Builder.CreateICmpEQ(CurrentVersionLoad, ConstantInt::get(i32Ty, i));
                FuncPtrNoMeasure = Builder.CreateSelect(IsVersionI, VersionFuncs[i], FuncPtrNoMeasure);
            }

            if (Orig->getReturnType()->isVoidTy())
            {
                Builder.CreateCall(Orig->getFunctionType(), FuncPtrNoMeasure, WrapperArgs);
            }
            else
            {
                NoMeasureResult = Builder.CreateCall(Orig->getFunctionType(), FuncPtrNoMeasure, WrapperArgs);
            }
            Builder.CreateBr(ContinueBB);

            // Continue block
            Builder.SetInsertPoint(ContinueBB);
            if (!Orig->getReturnType()->isVoidTy())
            {
                PHINode *ResultPhi = Builder.CreatePHI(Orig->getReturnType(), UpdateBlocks.size() + 1);
                for (BasicBlock *Pred : UpdateBlocks)
                {
                    ResultPhi->addIncoming(MeasuredResult, Pred);
                }
                ResultPhi->addIncoming(NoMeasureResult, NoMeasureBB);
                Builder.CreateRet(ResultPhi);
            }
            else
            {
                Builder.CreateRetVoid();
            }
        }

        // Key change: Transition to Optimal
        Builder.SetInsertPoint(TransitionToOptimalBB);
        {
            // Use atomic CAS to ensure only ONE thread performs transition
            BasicBlock *DoTransitionBB = BasicBlock::Create(Ctx, "do_transition", Wrapper);
            BasicBlock *AlreadyTransitionedBB = BasicBlock::Create(Ctx, "already_transitioned", Wrapper);

            // Attempt atomic compare-exchange: if currentPhase == 1, set it to 2
            AtomicCmpXchgInst *CAS = Builder.CreateAtomicCmpXchg(
                metadata.currentPhase, one32, two32,
                MaybeAlign(4),
                AtomicOrdering::SequentiallyConsistent,
                AtomicOrdering::SequentiallyConsistent);
            CAS->setWeak(false); // Strong CAS

            // Extract the success flag from the result
            Value *CASResult = Builder.CreateExtractValue(CAS, 1);
            Builder.CreateCondBr(CASResult, DoTransitionBB, AlreadyTransitionedBB);

            // If CAS failed, another thread already transitioned - go to optimal
            Builder.SetInsertPoint(AlreadyTransitionedBB);
            Builder.CreateBr(OptimalPhaseBB);

            // Only the winning thread performs the transition
            Builder.SetInsertPoint(DoTransitionBB);

            // Load all version data
            std::vector<Value *> CurrentCycles;
            std::vector<Value *> CurrentRuns;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                LoadInst *CyclesLoad = Builder.CreateLoad(i64Ty, metadata.cpuCycles[i]);
                CyclesLoad->setAtomic(AtomicOrdering::Acquire);
                CyclesLoad->setAlignment(Align(8));
                CurrentCycles.push_back(CyclesLoad);

                LoadInst *RunsLoad = Builder.CreateLoad(i32Ty, metadata.runCount[i]);
                RunsLoad->setAtomic(AtomicOrdering::Acquire);
                RunsLoad->setAlignment(Align(4));
                CurrentRuns.push_back(RunsLoad);
            }

            // Load squared cycles for variance calculation
            std::vector<Value *> CurrentCyclesSquared;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                LoadInst *CyclesSquaredLoad = Builder.CreateLoad(i64Ty, metadata.cpuCyclesSquared[i]);
                CyclesSquaredLoad->setAtomic(AtomicOrdering::Acquire);
                CyclesSquaredLoad->setAlignment(Align(8));
                CurrentCyclesSquared.push_back(CyclesSquaredLoad);
            }

            // CV-BASED SELECTION: Filter by reliability (CV < 50%) + pick minimum mean
            Value *BestVersion = zero32;
            Value *BestAvg = nullptr;
            Value *BestCV = nullptr;
            
            // Track all versions for fallback
            std::vector<Value *> AllMeans;
            std::vector<Value *> AllCVs;
            std::vector<Value *> AllStdErrs;

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                Value *HasRuns = Builder.CreateICmpUGT(CurrentRuns[i], zero32);
                
                // Calculate mean
                Value *CyclesFloat = Builder.CreateUIToFP(CurrentCycles[i], Builder.getDoubleTy());
                Value *RunsFloat = Builder.CreateUIToFP(CurrentRuns[i], Builder.getDoubleTy());
                Value *SafeRunsFloat = Builder.CreateSelect(
                    HasRuns, RunsFloat,
                    ConstantFP::get(Builder.getDoubleTy(), 1.0));
                Value *MeanFloat = Builder.CreateFDiv(CyclesFloat, SafeRunsFloat);
                
                // Calculate variance
                Value *CyclesSquaredFloat = Builder.CreateUIToFP(CurrentCyclesSquared[i], Builder.getDoubleTy());
                Value *MeanOfSquares = Builder.CreateFDiv(CyclesSquaredFloat, SafeRunsFloat);
                Value *MeanSquared = Builder.CreateFMul(MeanFloat, MeanFloat);
                Value *Variance = Builder.CreateFSub(MeanOfSquares, MeanSquared);
                
                // Standard deviation = sqrt(variance)
                Function *SqrtFn = Intrinsic::getDeclaration(&M, Intrinsic::sqrt, {Builder.getDoubleTy()});
                Value *StdDev = Builder.CreateCall(SqrtFn, {Variance});
                
                // Standard error = stddev / sqrt(n)
                Value *SqrtN = Builder.CreateCall(SqrtFn, {RunsFloat});
                Value *StdErr = Builder.CreateFDiv(StdDev, SqrtN);
                
                // CV-FILTER: Calculate coefficient of variation (CV = stderr / mean)
                Value *CV = Builder.CreateFDiv(StdErr, MeanFloat);
                
                Value *MaxValFloat = ConstantFP::get(Builder.getDoubleTy(),
                                                     std::numeric_limits<double>::max());
                Value *FinalAvg = Builder.CreateSelect(HasRuns, MeanFloat, MaxValFloat);
                Value *FinalStdErr = Builder.CreateSelect(HasRuns, StdErr, MaxValFloat);
                Value *FinalCV = Builder.CreateSelect(HasRuns, CV, MaxValFloat);
                
                // Store for later (needed for fallback)
                AllMeans.push_back(FinalAvg);
                AllCVs.push_back(FinalCV);
                AllStdErrs.push_back(FinalStdErr);

                if (i == 0)
                {
                    BestAvg = FinalAvg;
                    BestCV = FinalCV;
                    BestVersion = ConstantInt::get(i32Ty, 0);
                }
                else
                {
                    // CV-FILTER: Only consider versions with CV < 50%
                    Value *CVThreshold = ConstantFP::get(Builder.getDoubleTy(), 0.50); // 50%
                    
                    // Check if current version is reliable
                    Value *CurrentReliable = Builder.CreateFCmpOLT(FinalCV, CVThreshold);
                    
                    // Check if best version is reliable
                    Value *BestReliable = Builder.CreateFCmpOLT(BestCV, CVThreshold);
                    
                    // Check if current is better than best
                    Value *IsFaster = Builder.CreateFCmpOLT(FinalAvg, BestAvg);
                    
                    // MINIMUM IMPROVEMENT: Require >10% improvement to avoid noise
                    // This prevents switching on 1-2 cycle differences (e.g., 46 vs 48)
                    Value *ImprovementAbs = Builder.CreateFSub(BestAvg, FinalAvg);
                    Value *ImprovementRatio = Builder.CreateFDiv(ImprovementAbs, BestAvg);
                    Value *MinImprovement = ConstantFP::get(Builder.getDoubleTy(), 0.10); // 10%
                    Value *SignificantImprovement = Builder.CreateFCmpOGT(ImprovementRatio, MinImprovement);
                    
                    // Decision logic:
                    // 1. If both reliable: pick faster AND improvement > 10%
                    // 2. If only current reliable: pick current
                    // 3. If only best reliable: keep best
                    // 4. If both unreliable: handle in fallback
                    
                    Value *BothReliable = Builder.CreateAnd(CurrentReliable, BestReliable);
                    Value *OnlyCurrentReliable = Builder.CreateAnd(CurrentReliable, 
                        Builder.CreateNot(BestReliable));
                    
                    // Switch if: (both reliable AND faster AND >10% improvement) OR (only current reliable)
                    Value *BothReliableAndSignificant = Builder.CreateAnd(
                        Builder.CreateAnd(BothReliable, IsFaster),
                        SignificantImprovement);
                    
                    Value *ShouldSwitch = Builder.CreateOr(
                        BothReliableAndSignificant,
                        OnlyCurrentReliable);
                    
                    BestVersion = Builder.CreateSelect(ShouldSwitch,
                                                       ConstantInt::get(i32Ty, i), BestVersion);
                    BestAvg = Builder.CreateSelect(ShouldSwitch, FinalAvg, BestAvg);
                    BestCV = Builder.CreateSelect(ShouldSwitch, FinalCV, BestCV);
                }

                // Print stats with CV
                Value *AvgForDisplay = Builder.CreateFPToUI(FinalAvg, i64Ty);
                #if ADAPTIVE_DEBUG_PRINTS
                Value *StdErrForDisplay = Builder.CreateFPToUI(FinalStdErr, i64Ty);
                Value *CVPercent = Builder.CreateFMul(FinalCV, ConstantFP::get(Builder.getDoubleTy(), 100.0));
                Value *CVForDisplay = Builder.CreateFPToUI(CVPercent, i32Ty);
                Value *StatsMsg = Builder.CreateGlobalStringPtr(
                    "STATS %s - V%d: total_cycles=%llu, runs=%u, avg=%llu, stderr=%llu, cv=%u%%\n");
                Builder.CreateCall(Printf, {StatsMsg, FuncName,
                                            ConstantInt::get(i32Ty, i),
                                            CurrentCycles[i], CurrentRuns[i], 
                                            AvgForDisplay, StdErrForDisplay, CVForDisplay});
                // #else
                // Value *StatsMsg = Builder.CreateGlobalStringPtr(
                //     "STATS %s - V%d: total_cycles=%llu, runs=%u, avg=%llu\n");
                // Builder.CreateCall(Printf, {StatsMsg, FuncName,
                //                             ConstantInt::get(i32Ty, i),
                //                             CurrentCycles[i], CurrentRuns[i], AvgForDisplay});
                #endif
            }
            
            // FALLBACK: If best version has CV >= 50% (all unreliable), use weighted score
            // Save current block before creating conditional branch
            BasicBlock *BeforeFallbackBB = Builder.GetInsertBlock();
            
            Value *CVThreshold = ConstantFP::get(Builder.getDoubleTy(), 0.50);
            Value *BestIsUnreliable = Builder.CreateFCmpOGE(BestCV, CVThreshold);
            
            BasicBlock *UseFallbackBB = BasicBlock::Create(Ctx, "use_fallback", Wrapper);
            BasicBlock *SkipFallbackBB = BasicBlock::Create(Ctx, "skip_fallback", Wrapper);
            Builder.CreateCondBr(BestIsUnreliable, UseFallbackBB, SkipFallbackBB);
            
            // Fallback block: All versions unreliable, use weighted score (mean × (1 + CV))
            Builder.SetInsertPoint(UseFallbackBB);
            {
                #if ADAPTIVE_DEBUG_PRINTS
                Value *FallbackMsg = Builder.CreateGlobalStringPtr(
                    "WARNING %s: All versions unreliable (CV>=50%%), using weighted score fallback\n");
                Builder.CreateCall(Printf, {FallbackMsg, FuncName});
                #endif
                
                // Calculate weighted scores for all versions
                Value *FallbackBest = zero32;
                Value *BestScore = nullptr;
                
                for (int i = 0; i < NUM_VERSIONS; i++)
                {
                    // Weighted score = mean × (1 + CV)
                    Value *OnePlusCV = Builder.CreateFAdd(
                        ConstantFP::get(Builder.getDoubleTy(), 1.0), AllCVs[i]);
                    Value *Score = Builder.CreateFMul(AllMeans[i], OnePlusCV);
                    
                    if (i == 0)
                    {
                        BestScore = Score;
                        FallbackBest = ConstantInt::get(i32Ty, 0);
                    }
                    else
                    {
                        Value *IsBetter = Builder.CreateFCmpOLT(Score, BestScore);
                        FallbackBest = Builder.CreateSelect(IsBetter,
                                                            ConstantInt::get(i32Ty, i), FallbackBest);
                        BestScore = Builder.CreateSelect(IsBetter, Score, BestScore);
                    }
                }
                
                // Branch to skip fallback with fallback result
                Builder.CreateBr(SkipFallbackBB);
                
                // Skip fallback - use original BestVersion
                Builder.SetInsertPoint(SkipFallbackBB);
                
                // PHI node to merge results (FIXED: use correct predecessor blocks)
                PHINode *FinalBestVersion = Builder.CreatePHI(i32Ty, 2);
                FinalBestVersion->addIncoming(FallbackBest, UseFallbackBB);
                FinalBestVersion->addIncoming(BestVersion, BeforeFallbackBB);
                
                BestVersion = FinalBestVersion;
            }

            // Store best version
            StoreInst *BestStore = Builder.CreateStore(BestVersion, metadata.bestVersion);
            BestStore->setAtomic(AtomicOrdering::Release);
            BestStore->setAlignment(Align(4));

            // Create blocks for each version
            BasicBlock *PatchDoneBB = BasicBlock::Create(Ctx, "patch_done", Wrapper);
            BasicBlock *DefaultPatchBB = BasicBlock::Create(Ctx, "default_patch", Wrapper);

            SwitchInst *PatchSwitch = Builder.CreateSwitch(BestVersion, DefaultPatchBB, 4);

            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                BasicBlock *PatchBB = BasicBlock::Create(Ctx,
                                                         "patch_v" + std::to_string(i), Wrapper);
                PatchSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, i)), PatchBB);

                Builder.SetInsertPoint(PatchBB);

                // Patch the function pointer to point directly to the best version
                Value *VersionPtr = Builder.CreatePointerCast(
                    metadata.versions[i],
                    metadata.original->getFunctionType()->getPointerTo());

                StoreInst *PatchStore = Builder.CreateStore(VersionPtr, metadata.functionPtr);
                PatchStore->setAtomic(AtomicOrdering::Release);
                PatchStore->setAlignment(Align(8));
                #if ADAPTIVE_DEBUG_PRINTS
                // Print patch message
                Value *PatchMsg = Builder.CreateGlobalStringPtr(
                    "PATCH %s - Function pointer patched to V%d, wrapper bypassed!\n");
                Builder.CreateCall(Printf, {PatchMsg, FuncName, ConstantInt::get(i32Ty, i)});
                #endif
                Builder.CreateBr(PatchDoneBB);
            }

            Builder.SetInsertPoint(DefaultPatchBB);
            Builder.CreateBr(PatchDoneBB);

            Builder.SetInsertPoint(PatchDoneBB);
            Builder.CreateBr(OptimalPhaseBB);
        }

        // Optimal Phase: Call best version (only used once before patching)
        Builder.SetInsertPoint(OptimalPhaseBB);
        {
            LoadInst *BestLoad = Builder.CreateLoad(i32Ty, metadata.bestVersion);
            BestLoad->setAtomic(AtomicOrdering::Acquire);
            BestLoad->setAlignment(Align(4));

            std::vector<Value *> VersionFuncs;
            for (int i = 0; i < NUM_VERSIONS; i++)
            {
                VersionFuncs.push_back(Builder.CreatePointerCast(
                    metadata.versions[i], Orig->getFunctionType()->getPointerTo()));
            }

            Value *FuncPtr = VersionFuncs[0];
            for (int i = 1; i < NUM_VERSIONS; i++)
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

        // Print initialization message
        FunctionType *PrintfType = FunctionType::get(Type::getInt32Ty(Ctx),
                                                     {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
        // NEW: Print group configurations
#if ADAPTIVE_DEBUG_PRINTS
        Value *InitMsg = Builder.CreateGlobalStringPtr(
            "INIT Adaptive dispatcher - PERFORMANCE \n");
        Builder.CreateCall(Printf, {InitMsg});
#endif
        Builder.CreateRetVoid();

        appendToGlobalCtors(M, InitFunc, 65535);
    }

} // end namespace llvm