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
#include <chrono>

using namespace llvm;

namespace llvm
{

    // Structure to track function metadata
    struct FunctionMetadata
    {
        Function *original;
        Function *versions[2]; // v0 and v1
        Function *wrapper;
        Function *bestVersion; // Pointer to the best performing version
        uint32_t functionId;
        FunctionType *signature; // Store original signature
        uint64_t cpuCycles[2];   // CPU cycles for both versions
    };

    class SimpleAdaptivePassImpl
    {
    private:
        std::map<Function *, FunctionMetadata> functionMap;
        GlobalVariable *dispatchTable;
        GlobalVariable *functionCounterTable;
        GlobalVariable *functionCount;
        GlobalVariable *cpuCyclesTable; // Table to store CPU cycles for each version
        GlobalVariable *bestVersionTable; // Table to store best version for each function

        // Helper functions
        void addPrintToVersion(Function *F, int versionId, uint32_t funcId, Module &M);
        Function *createVersion(Function *F, int versionId, Module &M);
        void createGlobalDispatchInfrastructure(Module &M);
        void processFunctionWithDirectDispatcher(Function *F, uint32_t funcId, Module &M);
        void createInitializationFunction(Module &M);
        Function *createDirectDispatchWrapper(Function *Original, uint32_t funcId, Module &M);
        void addCPUCycleMeasurement(Function *F, int versionId, uint32_t funcId, Module &M);

    public:
        SimpleAdaptivePassImpl() : dispatchTable(nullptr), functionCounterTable(nullptr), 
                                 functionCount(nullptr), cpuCyclesTable(nullptr), bestVersionTable(nullptr) {}

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

        // Clear any previous state
        functionMap.clear();
        dispatchTable = nullptr;
        functionCounterTable = nullptr;
        functionCount = nullptr;
        cpuCyclesTable = nullptr;
        bestVersionTable = nullptr;

        // Step 1: Collect all adaptive functions
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

        // Step 2: Create global dispatch infrastructure
        createGlobalDispatchInfrastructure(M);

        // Step 3: Process each adaptive function with direct dispatch
        uint32_t funcId = 0;
        for (Function *F : adaptiveFunctions)
        {
            processFunctionWithDirectDispatcher(F, funcId++, M);
        }

        // Step 4: Create initialization function
        createInitializationFunction(M);

        errs() << "=== Transformation Complete ===\n";
        return PreservedAnalyses::none();
    }

    // === Create Global Infrastructure ===
    void SimpleAdaptivePassImpl::createGlobalDispatchInfrastructure(Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        // Check if globals already exist
        if (M.getGlobalVariable("__adaptive_dispatch_table"))
        {
            errs() << "  Dispatch infrastructure already exists, skipping creation\n";
            dispatchTable = M.getGlobalVariable("__adaptive_dispatch_table");
            functionCounterTable = M.getGlobalVariable("__adaptive_function_counters");
            functionCount = M.getGlobalVariable("__adaptive_function_count");
            cpuCyclesTable = M.getGlobalVariable("__adaptive_cpu_cycles");
            bestVersionTable = M.getGlobalVariable("__adaptive_best_versions");
            return;
        }

        // 1. Dispatch table: Array of function pointers for each version
        ArrayType *DispatchTableType = ArrayType::get(
            PointerType::get(Type::getInt8Ty(Ctx), 0),
            200 // Max 100 functions * 2 versions
        );

        dispatchTable = new GlobalVariable(
            M,
            DispatchTableType,
            false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(DispatchTableType),
            "__adaptive_dispatch_table");

        // 2. Counter table: Array of counters for each function
        ArrayType *CounterTableType = ArrayType::get(
            Type::getInt32Ty(Ctx),
            100 // Max 100 functions
        );

        functionCounterTable = new GlobalVariable(
            M,
            CounterTableType,
            false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(CounterTableType),
            "__adaptive_function_counters");

        // 3. Function count
        functionCount = new GlobalVariable(
            M,
            Type::getInt32Ty(Ctx),
            false,
            GlobalValue::InternalLinkage,
            ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            "__adaptive_function_count");

        // 4. CPU cycles table: Array of uint64 for each function version (2 per function)
        ArrayType *CPUCyclesTableType = ArrayType::get(
            Type::getInt64Ty(Ctx),
            200 // Max 100 functions * 2 versions
        );

        cpuCyclesTable = new GlobalVariable(
            M,
            CPUCyclesTableType,
            false,
            GlobalValue::InternalLinkage,
            ConstantAggregateZero::get(CPUCyclesTableType),
            "__adaptive_cpu_cycles");

        // 5. Best version table: Array of integers indicating best version for each function
        ArrayType *BestVersionTableType = ArrayType::get(
            Type::getInt32Ty(Ctx),
            100 // Max 100 functions
        );

        // Initialize with -1 (no best version determined yet)
        Constant *InitialBestVersions = ConstantAggregateZero::get(BestVersionTableType);
        bestVersionTable = new GlobalVariable(
            M,
            BestVersionTableType,
            false,
            GlobalValue::InternalLinkage,
            InitialBestVersions,
            "__adaptive_best_versions");

        errs() << "  Created global dispatch infrastructure\n";
    }

    // === Process Each Function with Direct Dispatch ===
    void SimpleAdaptivePassImpl::processFunctionWithDirectDispatcher(Function *F, uint32_t funcId, Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        // Create metadata entry
        FunctionMetadata metadata;
        metadata.original = F;
        metadata.functionId = funcId;
        metadata.signature = F->getFunctionType();
        metadata.cpuCycles[0] = 0;
        metadata.cpuCycles[1] = 0;
        metadata.bestVersion = nullptr; // Will be set after profiling
        StringRef name = F->getName();

        // 1. Create two versions
        metadata.versions[0] = createVersion(F, 0, M);
        metadata.versions[1] = createVersion(F, 1, M);

        // Add CPU cycle measurement and debug prints to versions
        addCPUCycleMeasurement(metadata.versions[0], 0, funcId, M);
        addCPUCycleMeasurement(metadata.versions[1], 1, funcId, M);
        addPrintToVersion(metadata.versions[0], 0, funcId, M);
        addPrintToVersion(metadata.versions[1], 1, funcId, M);

        // 2. Create wrapper with direct dispatch (no shared dispatcher)
        metadata.wrapper = createDirectDispatchWrapper(F, funcId, M);

        // 3. Replace original function with wrapper
        F->replaceAllUsesWith(metadata.wrapper);
        metadata.wrapper->takeName(F);

        F->setName(name.str() + "_original");
        F->setLinkage(GlobalValue::InternalLinkage);

        // 4. Store metadata
        functionMap[F] = metadata;

        errs() << "  Processed function " << funcId << ": " << F->getName() << "\n";
        errs() << "    Versions: " << metadata.versions[0]->getName() << ", " << metadata.versions[1]->getName() << "\n";
        errs() << "    Wrapper: " << metadata.wrapper->getName() << "\n";
    }

    // === Create Version ===
    Function *SimpleAdaptivePassImpl::createVersion(Function *F, int versionId, Module &M)
    {
        ValueToValueMapTy VMap;
        Function *NewF = CloneFunction(F, VMap);
        NewF->setName(F->getName() + "_v" + std::to_string(versionId));
        NewF->setLinkage(GlobalValue::InternalLinkage);

        if (versionId == 1)
        {
            // Add optimization hints for v1
            NewF->addFnAttr("prefer-vector-width", "256");
            NewF->addFnAttr("target-features", "+avx2");
        }

        M.getFunctionList().push_back(NewF);
        return NewF;
    }

    void SimpleAdaptivePassImpl::addCPUCycleMeasurement(Function *F, int versionId, uint32_t funcId, Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        // Get the first basic block and insert measurement at the beginning
        BasicBlock &EntryBB = F->getEntryBlock();
        IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());

        // Get current time at function entry
        FunctionType *HighResClockType = FunctionType::get(Type::getInt64Ty(Ctx), false);
        FunctionCallee HighResClock = M.getOrInsertFunction("llvm.readcyclecounter", HighResClockType);

        Value *StartTime = Builder.CreateCall(HighResClock);

        // Find all return instructions to insert end time measurement
        std::vector<ReturnInst*> ReturnInsts;
        for (BasicBlock &BB : *F)
        {
            if (ReturnInst *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
            {
                ReturnInsts.push_back(RI);
            }
        }

        // Insert end time measurement before each return
        for (ReturnInst *RI : ReturnInsts)
        {
            IRBuilder<> ReturnBuilder(RI);

            // Get end time
            Value *EndTime = ReturnBuilder.CreateCall(HighResClock);

            // Calculate elapsed cycles
            Value *ElapsedCycles = ReturnBuilder.CreateSub(EndTime, StartTime);

            // Store CPU cycles in global table
            Value *CPUCycleIdx = ReturnBuilder.CreateAdd(
                ReturnBuilder.CreateMul(
                    ConstantInt::get(Type::getInt32Ty(Ctx), funcId),
                    ConstantInt::get(Type::getInt32Ty(Ctx), 2)
                ),
                ConstantInt::get(Type::getInt32Ty(Ctx), versionId)
            );

            Value *CPUCyclePtr = ReturnBuilder.CreateGEP(
                cpuCyclesTable->getValueType(),
                cpuCyclesTable,
                {ConstantInt::get(Type::getInt32Ty(Ctx), 0), CPUCycleIdx});

            // Load current accumulated cycles and add new measurement
            Value *CurrentCycles = ReturnBuilder.CreateLoad(Type::getInt64Ty(Ctx), CPUCyclePtr);
            Value *NewCycles = ReturnBuilder.CreateAdd(CurrentCycles, ElapsedCycles);
            ReturnBuilder.CreateStore(NewCycles, CPUCyclePtr);
        }
    }

    void SimpleAdaptivePassImpl::addPrintToVersion(Function *F, int versionId, uint32_t funcId, Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        FunctionType *PrintfType = FunctionType::get(
            Type::getInt32Ty(Ctx),
            {PointerType::get(Type::getInt8Ty(Ctx), 0)},
            true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

        BasicBlock &EntryBB = F->getEntryBlock();
        IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());

        std::string FormatStr = "[EXECUTING] Function " + std::to_string(funcId) +
                                ", Version " + std::to_string(versionId) +
                                " (%s)\n";
        Value *FormatStrVal = Builder.CreateGlobalStringPtr(FormatStr);
        Value *FuncNameVal = Builder.CreateGlobalStringPtr(F->getName().str());

        Builder.CreateCall(Printf, {FormatStrVal, FuncNameVal});
    }

    // === Create Direct Dispatch Wrapper ===
    Function *SimpleAdaptivePassImpl::createDirectDispatchWrapper(Function *Original, uint32_t funcId, Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        // Create wrapper with same signature as original
        Function *Wrapper = Function::Create(
            Original->getFunctionType(),
            Original->getLinkage(),
            "temp_wrapper",
            &M);

        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
        IRBuilder<> Builder(Entry);

        // Get printf for debugging
        FunctionType *PrintfType = FunctionType::get(
            Type::getInt32Ty(Ctx),
            {PointerType::get(Type::getInt8Ty(Ctx), 0)},
            true);
        FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

        // === DIRECT DISPATCH LOGIC ===

        // 1. Load current counter
        Value *CounterPtr = Builder.CreateGEP(
            functionCounterTable->getValueType(),
            functionCounterTable,
            {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            ConstantInt::get(Type::getInt32Ty(Ctx), funcId)});
        Value *CurrentCount = Builder.CreateLoad(Type::getInt32Ty(Ctx), CounterPtr);
        
        // 2. Increment counter for next call
        Value *NewCount = Builder.CreateAdd(CurrentCount, ConstantInt::get(Type::getInt32Ty(Ctx), 1));
        Builder.CreateStore(NewCount, CounterPtr);

        // 3. Debug print
        Value *DebugMsg = Builder.CreateGlobalStringPtr("[DIRECT DISPATCH] Function %d, Call #%d: ");
        Builder.CreateCall(Printf, {DebugMsg, ConstantInt::get(Type::getInt32Ty(Ctx), funcId), NewCount});

        // 4. Check if we're in profiling phase (first 100 calls)
        // Use CurrentCount (not NewCount) because we want to decide based on current call number
        Value *ProfilingPhase = Builder.CreateICmpULT(CurrentCount, ConstantInt::get(Type::getInt32Ty(Ctx), 100));

        // Create basic blocks for different dispatch strategies
        BasicBlock *ProfilingBlock = BasicBlock::Create(Ctx, "profiling_dispatch", Wrapper);
        BasicBlock *BestVersionBlock = BasicBlock::Create(Ctx, "best_version_dispatch", Wrapper);
        BasicBlock *MergeBlock = BasicBlock::Create(Ctx, "merge", Wrapper);

        Builder.CreateCondBr(ProfilingPhase, ProfilingBlock, BestVersionBlock);

        // === Block 1: Profiling Phase (Alternate between versions 0 and 1 for calls 0-99) ===
        Builder.SetInsertPoint(ProfilingBlock);
        Value *ProfilingVersion = Builder.CreateURem(CurrentCount, ConstantInt::get(Type::getInt32Ty(Ctx), 2));
        Value *ProfilingTableIdx = Builder.CreateAdd(
        Builder.CreateMul(ConstantInt::get(Type::getInt32Ty(Ctx), funcId), 
                        ConstantInt::get(Type::getInt32Ty(Ctx), 2)),
        ProfilingVersion);
        Value *ProfilingFuncPtrPtr = Builder.CreateGEP(
        dispatchTable->getValueType(),
        dispatchTable,
        {ConstantInt::get(Type::getInt32Ty(Ctx), 0), ProfilingTableIdx});
        Value *ProfilingFuncPtrGeneric = Builder.CreateLoad(PointerType::get(Type::getInt8Ty(Ctx), 0), ProfilingFuncPtrPtr);
        Value *ProfilingFuncPtr = Builder.CreateBitCast(ProfilingFuncPtrGeneric, 
                                                PointerType::get(Original->getFunctionType(), 0));

        // === ADDED: Load and print CPU cycles for both versions ===
        // Get CPU cycles for version 0
        Value *CyclesV0Ptr = Builder.CreateGEP(
        cpuCyclesTable->getValueType(),
        cpuCyclesTable,
        {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            ConstantInt::get(Type::getInt32Ty(Ctx), funcId * 2)});
        Value *CyclesV0 = Builder.CreateLoad(Type::getInt64Ty(Ctx), CyclesV0Ptr);

        // Get CPU cycles for version 1  
        Value *CyclesV1Ptr = Builder.CreateGEP(
        cpuCyclesTable->getValueType(),
        cpuCyclesTable,
        {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            ConstantInt::get(Type::getInt32Ty(Ctx), funcId * 2 + 1)});
        Value *CyclesV1 = Builder.CreateLoad(Type::getInt64Ty(Ctx), CyclesV1Ptr);

        // Print profiling information with CPU cycles
        Value *ProfilingMsg = Builder.CreateGlobalStringPtr(
        "Profiling Phase - Function %d, Call #%d, Version %d\n"
        "  CPU Cycles: V0=%lu, V1=%lu, Current Total Calls=%d\n");
        Builder.CreateCall(Printf, {ProfilingMsg, 
                                ConstantInt::get(Type::getInt32Ty(Ctx), funcId),
                                CurrentCount,
                                ProfilingVersion,
                                CyclesV0, 
                                CyclesV1,
                                CurrentCount});

        Builder.CreateBr(MergeBlock);

        // === Block 2: Best Version Phase (Calls 100 and beyond) ===
        Builder.SetInsertPoint(BestVersionBlock);
        
        // Load best version for this function
        Value *BestVersionPtr = Builder.CreateGEP(
            bestVersionTable->getValueType(),
            bestVersionTable,
            {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
            ConstantInt::get(Type::getInt32Ty(Ctx), funcId)});
        Value *BestVersion = Builder.CreateLoad(Type::getInt32Ty(Ctx), BestVersionPtr);
        
        // Check if best version is determined (not -1)
        Value *BestVersionDetermined = Builder.CreateICmpSGE(BestVersion, ConstantInt::get(Type::getInt32Ty(Ctx), 0));
        
        BasicBlock *UseBestVersion = BasicBlock::Create(Ctx, "use_best_version", Wrapper);
        BasicBlock *UseDefault = BasicBlock::Create(Ctx, "use_default", Wrapper);
        
        Builder.CreateCondBr(BestVersionDetermined, UseBestVersion, UseDefault);
        
        // Use best version if determined
        Builder.SetInsertPoint(UseBestVersion);
        Value *BestTableIdx = Builder.CreateAdd(
            Builder.CreateMul(ConstantInt::get(Type::getInt32Ty(Ctx), funcId), 
                            ConstantInt::get(Type::getInt32Ty(Ctx), 2)),
            BestVersion);
        Value *BestFuncPtrPtr = Builder.CreateGEP(
            dispatchTable->getValueType(),
            dispatchTable,
            {ConstantInt::get(Type::getInt32Ty(Ctx), 0), BestTableIdx});
        Value *BestFuncPtrGeneric = Builder.CreateLoad(PointerType::get(Type::getInt8Ty(Ctx), 0), BestFuncPtrPtr);
        Value *BestFuncPtr = Builder.CreateBitCast(BestFuncPtrGeneric, 
                                                PointerType::get(Original->getFunctionType(), 0));
        
        Value *BestMsg = Builder.CreateGlobalStringPtr("Best Version Phase - Using Version %d\n");
        Builder.CreateCall(Printf, {BestMsg, BestVersion});
        Builder.CreateBr(MergeBlock);
        
        // Use default (version 0) if best version not determined
        Builder.SetInsertPoint(UseDefault);
        Value *DefaultTableIdx = Builder.CreateMul(ConstantInt::get(Type::getInt32Ty(Ctx), funcId), 
                                                ConstantInt::get(Type::getInt32Ty(Ctx), 2));
        Value *DefaultFuncPtrPtr = Builder.CreateGEP(
            dispatchTable->getValueType(),
            dispatchTable,
            {ConstantInt::get(Type::getInt32Ty(Ctx), 0), DefaultTableIdx});
        Value *DefaultFuncPtrGeneric = Builder.CreateLoad(PointerType::get(Type::getInt8Ty(Ctx), 0), DefaultFuncPtrPtr);
        Value *DefaultFuncPtr = Builder.CreateBitCast(DefaultFuncPtrGeneric, 
                                                    PointerType::get(Original->getFunctionType(), 0));
        
        Value *DefaultMsg = Builder.CreateGlobalStringPtr("Best Version Not Determined - Using Default Version 0\n");
        Builder.CreateCall(Printf, {DefaultMsg});
        Builder.CreateBr(MergeBlock);

        // === Merge Block: Call the selected function ===
        Builder.SetInsertPoint(MergeBlock);
        
        // PHI node to select the correct function pointer
        PHINode *FuncPtrPhi = Builder.CreatePHI(PointerType::get(Original->getFunctionType(), 0), 3);
        FuncPtrPhi->addIncoming(ProfilingFuncPtr, ProfilingBlock);
        FuncPtrPhi->addIncoming(BestFuncPtr, UseBestVersion);
        FuncPtrPhi->addIncoming(DefaultFuncPtr, UseDefault);

        // Collect arguments
        std::vector<Value *> Args;
        for (auto &Arg : Wrapper->args())
        {
            Args.push_back(&Arg);
        }

        // Call the selected function
        Value *Result = nullptr;
        if (Original->getReturnType()->isVoidTy())
        {
            Builder.CreateCall(Original->getFunctionType(), FuncPtrPhi, Args);
            Builder.CreateRetVoid();
        }
        else
        {
            Result = Builder.CreateCall(Original->getFunctionType(), FuncPtrPhi, Args);
            Builder.CreateRet(Result);
        }

        return Wrapper;
    }

    // === Create Initialization Function ===
    void SimpleAdaptivePassImpl::createInitializationFunction(Module &M)
    {
        LLVMContext &Ctx = M.getContext();

        // Check if init function already exists
        if (M.getFunction("__adaptive_init"))
        {
            errs() << "  Initialization function already exists, skipping\n";
            return;
        }

        // Create initialization function
        FunctionType *InitType = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
        Function *InitFunc = Function::Create(
            InitType,
            GlobalValue::InternalLinkage,
            "__adaptive_init",
            &M);

        BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", InitFunc);
        IRBuilder<> Builder(Entry);

        // Get printf for debugging
        FunctionCallee Printf = M.getOrInsertFunction("printf",
                                                      FunctionType::get(Type::getInt32Ty(Ctx), 
                                                                      {PointerType::get(Type::getInt8Ty(Ctx), 0)}, 
                                                                      true));

        // Populate dispatch table with function pointers
        uint32_t tableIdx = 0;
        for (auto &[OrigFunc, Metadata] : functionMap)
        {
            for (int v = 0; v < 2; v++)
            {
                Value *Slot = Builder.CreateGEP(
                    dispatchTable->getValueType(),
                    dispatchTable,
                    {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                     ConstantInt::get(Type::getInt32Ty(Ctx), tableIdx++)});

                // Cast function pointer to i8* for storage
                Value *FuncPtr = Builder.CreateBitCast(
                    Metadata.versions[v],
                    PointerType::get(Type::getInt8Ty(Ctx), 0));
                Builder.CreateStore(FuncPtr, Slot);
            }
        }

        // Store function count
        Builder.CreateStore(
            ConstantInt::get(Type::getInt32Ty(Ctx), functionMap.size()),
            functionCount);

        // Create a function to determine best versions (called after profiling)
        FunctionType *DetermineBestType = FunctionType::get(Type::getVoidTy(Ctx), false);
        Function *DetermineBestFunc = Function::Create(
            DetermineBestType,
            GlobalValue::InternalLinkage,
            "__adaptive_determine_best_versions",
            &M);

        BasicBlock *DetermineBB = BasicBlock::Create(Ctx, "entry", DetermineBestFunc);
        IRBuilder<> DetermineBuilder(DetermineBB);

        // For each function, compare CPU cycles and select best version
        for (auto &[OrigFunc, Metadata] : functionMap)
        {
            uint32_t funcId = Metadata.functionId;
            
            // Load CPU cycles for both versions
            Value *CyclesV0Ptr = DetermineBuilder.CreateGEP(
                cpuCyclesTable->getValueType(),
                cpuCyclesTable,
                {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                 ConstantInt::get(Type::getInt32Ty(Ctx), funcId * 2)});
            Value *CyclesV0 = DetermineBuilder.CreateLoad(Type::getInt64Ty(Ctx), CyclesV0Ptr);
            
            Value *CyclesV1Ptr = DetermineBuilder.CreateGEP(
                cpuCyclesTable->getValueType(),
                cpuCyclesTable,
                {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                 ConstantInt::get(Type::getInt32Ty(Ctx), funcId * 2 + 1)});
            Value *CyclesV1 = DetermineBuilder.CreateLoad(Type::getInt64Ty(Ctx), CyclesV1Ptr);
            
            // Compare cycles and select version with fewer cycles
            Value *V0IsBetter = DetermineBuilder.CreateICmpULT(CyclesV0, CyclesV1);
            Value *BestVersion = DetermineBuilder.CreateSelect(V0IsBetter, 
                                                             ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                                                             ConstantInt::get(Type::getInt32Ty(Ctx), 1));
            
            // Store best version
            Value *BestVersionPtr = DetermineBuilder.CreateGEP(
                bestVersionTable->getValueType(),
                bestVersionTable,
                {ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                 ConstantInt::get(Type::getInt32Ty(Ctx), funcId)});
            DetermineBuilder.CreateStore(BestVersion, BestVersionPtr);
            
            // Debug print
            Value *BestMsg = DetermineBuilder.CreateGlobalStringPtr(
                "Function %d: V0=%lu cycles, V1=%lu cycles, Best=%d\n");
            DetermineBuilder.CreateCall(Printf, {BestMsg, 
                                               ConstantInt::get(Type::getInt32Ty(Ctx), funcId),
                                               CyclesV0, CyclesV1, BestVersion});
        }
        
        DetermineBuilder.CreateRetVoid();

        // Call the determine best versions function in the init
        Builder.CreateCall(DetermineBestType, DetermineBestFunc);

        Value *InitMsg = Builder.CreateGlobalStringPtr(
            "[INIT] Adaptive dispatcher initialized with %d functions\n");
        Value *Count = Builder.CreateLoad(Type::getInt32Ty(Ctx), functionCount);
        Builder.CreateCall(Printf, {InitMsg, Count});

        Builder.CreateRetVoid();

        // Register to run before main
        appendToGlobalCtors(M, InitFunc, 65535);

        errs() << "  Created initialization function with best version selection\n";
    }

} // namespace llvm