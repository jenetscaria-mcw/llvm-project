#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define ADAPTIVE_DEBUG_PRINTS 0

using namespace llvm;

namespace llvm {

struct FunctionMetadata {
  Function *original;
  Function *versions[4]; // V0, V1, V2, V3
  Function *wrapper;
  Function *dispatcher;
  std::string profileKey;

  //  6 Global total
  GlobalVariable *cpuCycles[4];
  GlobalVariable *cpuCyclesSquared[4];
  GlobalVariable *runCount[4];
  GlobalVariable *currentVersion;
  GlobalVariable *profilingComplete; // 0=profiling, 1=finalize, 2=done
  GlobalVariable *bestVersion;
};

struct FunctionCharacteristics {
  int instructionCount;
  int basicBlockCount;
  int loopCount;
  int loadStoreCount;
  int fpOpCount;
  int callCount;
  int branchCount;

  bool isSmall;
  bool isMedium;
  bool isLarge;
  bool hasLoops;
  bool isMemoryIntensive;
  bool hasFPMath;
  bool manyBranches;

  bool canBenefitFromVectorization;
  bool canBenefitFromUnrolling;
  bool canBenefitFromInlining;
  bool canBenefitFromFastMath;
};

// Helper Functions
// Mode detection - controls which wrapper to generate
static bool isProfilingMode() {
  const char *mode = std::getenv("ADAPTIVE_MODE");
  return mode && std::string(mode) == "PROFILE";
}

static bool isProductionMode() {
  const char *mode = std::getenv("ADAPTIVE_MODE");
  return mode && std::string(mode) == "PRODUCTION";
}

static std::string getProfileFilePath() {
  const char *path = std::getenv("ADAPTIVE_PROFILE_PATH");
  return path ? std::string(path) : "/tmp/adaptive_profiles.txt";
}

static int readBestVersionFromProfile(const std::string &Key) {
  std::ifstream profile(getProfileFilePath());
  if (!profile.is_open())
    return -1;

  int BestVersion = -1;
  std::string line;
  while (std::getline(profile, line)) {
    size_t sep = line.rfind(':');
    if (sep == std::string::npos)
      continue;

    if (line.substr(0, sep) != Key)
      continue;

    const std::string versionStr = line.substr(sep + 1);
    char *end = nullptr;
    long parsed = std::strtol(versionStr.c_str(), &end, 10);
    if (end == versionStr.c_str() || *end != '\0')
      continue;
    if (parsed < 0 || parsed > 3)
      continue;

    // Keep the latest valid entry for this key.
    BestVersion = static_cast<int>(parsed);
  }

  return BestVersion;
}

static std::string buildProfileKey(const Module &M, StringRef FuncName) {
  return (M.getModuleIdentifier() + "::" + FuncName.str());
}

// Analyze function characteristics
static FunctionCharacteristics analyzeFunctionCharacteristics(Function *F) {
  FunctionCharacteristics fc = {};

  for (BasicBlock &BB : *F) {
    fc.basicBlockCount++;

    for (Instruction &I : BB) {
      fc.instructionCount++;

      if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
        fc.loadStoreCount++;
      } else if (isa<CallInst>(I)) {
        fc.callCount++;
      } else if (isa<BranchInst>(I)) {
        fc.branchCount++;
        BranchInst *BI = cast<BranchInst>(&I);
        if (BI->isConditional()) {
          fc.loopCount++;
        }
      }

      Type *T = I.getType();
      if (T->isFloatingPointTy()) {
        fc.fpOpCount++;
      }

      for (Use &U : I.operands()) {
        if (U.get()->getType()->isFloatingPointTy()) {
          fc.hasFPMath = true;
        }
      }
    }
  }

  fc.isSmall = (fc.instructionCount < 50);
  fc.isMedium = (fc.instructionCount >= 50 && fc.instructionCount <= 200);
  fc.isLarge = (fc.instructionCount > 200);
  fc.hasLoops = (fc.loopCount > 0);
  fc.isMemoryIntensive = (fc.loadStoreCount > fc.instructionCount / 3);
  fc.hasFPMath = (fc.fpOpCount > 0);
  fc.manyBranches = (fc.branchCount > 10);

  fc.canBenefitFromVectorization =
      fc.hasLoops && !fc.isLarge && !fc.isMemoryIntensive;
  fc.canBenefitFromUnrolling = fc.hasLoops && (fc.isSmall || fc.isMedium);
  fc.canBenefitFromInlining = fc.isSmall && (fc.callCount < 3);
  fc.canBenefitFromFastMath = fc.hasFPMath && !fc.isMemoryIntensive;

  return fc;
}

// Pass Implementation

class SimpleAdaptivePassImpl {
private:
  std::map<Function *, FunctionMetadata> functionMap;

  Function *createVersion(Function *Orig, int versionIndex, Module &M);
  void createWrapperStaticVars(FunctionMetadata &metadata, Module &M);
  Function *createProfilingWrapper(FunctionMetadata &metadata,
                                   Module &M); // Profiling wrapper
  Function *createProductionWrapper(FunctionMetadata &metadata,
                                    Module &M); // Production wrapper
  Function *createThinDispatcher(FunctionMetadata &metadata, Module &M,
                                 std::string targetName);
  void processAdaptiveFunction(Function *F, Module &M);
  void createInitializationFunction(Module &M);

public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

PreservedAnalyses SimpleAdaptivePass::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
  SimpleAdaptivePassImpl Impl;
  return Impl.run(M, MAM);
}

PreservedAnalyses SimpleAdaptivePassImpl::run(Module &M,
                                              ModuleAnalysisManager &MAM) {
  bool modified = false;

  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;

    // Check for adaptive attribute (no callcount - just "adaptive")
    bool isAdaptive = false;

    if (F.hasFnAttribute("adaptive")) {
      isAdaptive = true;
    }

    if (!isAdaptive && F.hasMetadata()) {
      if (MDNode *MD = F.getMetadata("annotation")) {
        for (unsigned i = 0; i < MD->getNumOperands(); i++) {
          if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(i))) {
            StringRef anno = MDS->getString();
            if (anno == "adaptive") {
              isAdaptive = true;
              break;
            }
          }
        }
      }
    }

    if (isAdaptive) {
      processAdaptiveFunction(&F, M);
      modified = true;
    }
  }

  if (modified) {
    createInitializationFunction(M);

    // Tell linker to automatically link the adaptive runtime library
    M.addModuleFlag(
        Module::AppendUnique, "Linker Options",
        MDNode::get(M.getContext(),
                    {MDString::get(M.getContext(), "-lclang_rt.adaptive")}));
  }

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// Create Optimized Versions
Function *SimpleAdaptivePassImpl::createVersion(Function *Orig,
                                                int versionIndex, Module &M) {
  ValueToValueMapTy VMap;
  Function *Clone = CloneFunction(Orig, VMap);
  Clone->setName(Orig->getName() + "_v" + std::to_string(versionIndex));
  Clone->setLinkage(GlobalValue::InternalLinkage);
  Clone->setCallingConv(Orig->getCallingConv());

  // CRITICAL FIX: Remove adaptive attribute to prevent recursive processing
  // Without this, cloned versions inherit the attribute and trigger infinite
  // recursion
  Clone->removeFnAttr("adaptive");

  // Also remove from metadata annotations if present
  if (Clone->hasMetadata()) {
    if (MDNode *MD = Clone->getMetadata("annotation")) {
      // Create a new annotation list without "adaptive"
      SmallVector<Metadata *, 4> NewAnnotations;
      for (unsigned i = 0; i < MD->getNumOperands(); i++) {
        if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(i))) {
          if (MDS->getString() != "adaptive") {
            NewAnnotations.push_back(MD->getOperand(i));
          }
        }
      }
      if (!NewAnnotations.empty()) {
        Clone->setMetadata("annotation",
                           MDNode::get(M.getContext(), NewAnnotations));
      } else {
        Clone->setMetadata("annotation", nullptr);
      }
    }
  }

  // Keep recursive calls inside each clone direct, so adaptive wrappers are not
  // re-entered from hot recursive paths.
  for (BasicBlock &BB : *Clone) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Value *Callee = CB->getCalledOperand()->stripPointerCasts();
      if (Callee == Orig) {
        CB->setCalledFunction(Clone);
      }
    }
  }

  FunctionCharacteristics fc = analyzeFunctionCharacteristics(Orig);

  // Apply version-specific optimizations
  switch (versionIndex) {
  case 0: // Baseline
    break;

  case 1: // Vectorization or prefetching
    if (fc.canBenefitFromVectorization) {
      Clone->addFnAttr("target-features", "+avx512f,+avx512vl");
      Clone->addFnAttr("prefer-vector-width", "512");
    } else if (fc.isMemoryIntensive) {
      Clone->addFnAttr("prefetch-distance", "128");
    } else {
      Clone->addFnAttr("unroll-count", "4");
    }
    break;

  case 2: // Aggressive unrolling
    if (fc.canBenefitFromUnrolling && fc.isSmall) {
      Clone->addFnAttr("unroll-count", "16");
      Clone->addFnAttr("interleave-count", "4");
    } else if (fc.canBenefitFromUnrolling && fc.isMedium) {
      Clone->addFnAttr("unroll-count", "8");
      Clone->addFnAttr("interleave-count", "2");
    } else if (fc.canBenefitFromUnrolling) {
      Clone->addFnAttr("unroll-count", "4");
    } else {
      Clone->addFnAttr("unroll-count", "2");
    }
    break;

  case 3: // Specialty optimizations
    if (fc.canBenefitFromInlining) {
      Clone->addFnAttr(Attribute::AlwaysInline);
      Clone->addFnAttr("inline-threshold", "100000");
      errs() << "[ADAPTIVE] " << Orig->getName()
             << "_v3: canBenefitFromInlining - Applied AlwaysInline + "
                "inline-threshold=100000\n";
    } else if (fc.canBenefitFromFastMath) {
      Clone->addFnAttr("unsafe-fp-math", "true");
      Clone->addFnAttr("no-nans-fp-math", "true");
      Clone->addFnAttr("no-infs-fp-math", "true");
      errs() << "[ADAPTIVE] " << Orig->getName()
             << "_v3: canBenefitFromFastMath - Applied unsafe-fp-math, "
                "no-nans-fp-math, no-infs-fp-math\n";
    } else if (fc.manyBranches) {
      Clone->addFnAttr(Attribute::OptimizeForSize);
      errs() << "[ADAPTIVE] " << Orig->getName()
             << "_v3: manyBranches - Applied OptimizeForSize\n";
    } else {
      Clone->addFnAttr("unroll-count", "6");
      errs() << "[ADAPTIVE] " << Orig->getName()
             << "_v3: default - Applied unroll-count=6\n";
    }
    break;
  }

  return Clone;
}

// Create Global Variables for only Profiling
void SimpleAdaptivePassImpl::createWrapperStaticVars(FunctionMetadata &metadata,
                                                     Module &M) {
  LLVMContext &Ctx = M.getContext();
  const std::string baseName = metadata.original->getName().str();

  IntegerType *i32Ty = Type::getInt32Ty(Ctx);
  IntegerType *i64Ty = Type::getInt64Ty(Ctx);

  // Create 6 globals per function
  for (int v = 0; v < 4; v++) {
    metadata.cpuCycles[v] = new GlobalVariable(
        M, i64Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i64Ty, 0),
        "__adaptive_cycles_" + baseName + "_v" + std::to_string(v));
    metadata.cpuCycles[v]->setAlignment(Align(8));

    metadata.cpuCyclesSquared[v] = new GlobalVariable(
        M, i64Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i64Ty, 0),
        "__adaptive_cycles_sq_" + baseName + "_v" + std::to_string(v));
    metadata.cpuCyclesSquared[v]->setAlignment(Align(8));

    metadata.runCount[v] = new GlobalVariable(
        M, i32Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i32Ty, 0),
        "__adaptive_runs_" + baseName + "_v" + std::to_string(v));
    metadata.runCount[v]->setAlignment(Align(4));
  }

  metadata.currentVersion = new GlobalVariable(
      M, i32Ty, false, GlobalValue::InternalLinkage, ConstantInt::get(i32Ty, 0),
      "__adaptive_current_" + baseName);
  metadata.currentVersion->setAlignment(Align(4));

  metadata.profilingComplete = new GlobalVariable(
      M, i32Ty, false, GlobalValue::InternalLinkage, ConstantInt::get(i32Ty, 0),
      "__adaptive_complete_" + baseName);
  metadata.profilingComplete->setAlignment(Align(4));

  metadata.bestVersion = new GlobalVariable(
      M, i32Ty, false, GlobalValue::InternalLinkage, ConstantInt::get(i32Ty, 0),
      "__adaptive_best_" + baseName);
  metadata.bestVersion->setAlignment(Align(4));
}

// Create Profiling Wrapper with CAS
Function *
SimpleAdaptivePassImpl::createProfilingWrapper(FunctionMetadata &metadata,
                                               Module &M) {
  LLVMContext &Ctx = M.getContext();
  Function *Orig = metadata.original;

  FunctionType *WrapperType = Orig->getFunctionType();
  Function *Wrapper =
      Function::Create(WrapperType, GlobalValue::ExternalLinkage,
                       Orig->getName() + "_wrapper", &M);
  Wrapper->setAttributes(Orig->getAttributes());
  Wrapper->setCallingConv(Orig->getCallingConv());
  Wrapper->setVisibility(Orig->getVisibility());
  Wrapper->setUnnamedAddr(Orig->getUnnamedAddr());
  Wrapper->setDSOLocal(Orig->isDSOLocal());
  if (Orig->hasComdat())
    Wrapper->setComdat(Orig->getComdat());

  // CRITICAL FIX: Remove adaptive attribute to prevent recursive processing
  Wrapper->removeFnAttr("adaptive");
  if (Wrapper->hasMetadata()) {
    Wrapper->setMetadata("annotation", nullptr);
  }

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
  IRBuilder<> Builder(Entry);

  Type *i32Ty = Type::getInt32Ty(Ctx);
  Type *i64Ty = Type::getInt64Ty(Ctx);

  std::vector<Value *> Args;
  for (auto &Arg : Wrapper->args())
    Args.push_back(&Arg);

  Value *FuncName = Builder.CreateGlobalStringPtr(metadata.profileKey);

  // Load profilingComplete with ATOMIC ACQUIRE
  LoadInst *LoadComplete =
      Builder.CreateLoad(i32Ty, metadata.profilingComplete);
  LoadComplete->setAtomic(AtomicOrdering::Acquire);
  LoadComplete->setAlignment(Align(4));
  Value *Complete = LoadComplete;

  // Create basic blocks for 3 states
  BasicBlock *ProfilingBB = BasicBlock::Create(Ctx, "profiling", Wrapper);
  BasicBlock *FinalizeBB = BasicBlock::Create(Ctx, "finalize", Wrapper);
  BasicBlock *OptimalBB = BasicBlock::Create(Ctx, "optimal", Wrapper);

  // Switch on profilingComplete value
  SwitchInst *Switch = Builder.CreateSwitch(Complete, ProfilingBB, 3);
  Switch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)), ProfilingBB);
  Switch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)), FinalizeBB);
  Switch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)), OptimalBB);

  //--- STATE 0: PROFILING ---
  Builder.SetInsertPoint(ProfilingBB);
  {
    // Load current version with ATOMIC ACQUIRE
    LoadInst *LoadVersion = Builder.CreateLoad(i32Ty, metadata.currentVersion);
    LoadVersion->setAtomic(AtomicOrdering::Acquire);
    LoadVersion->setAlignment(Align(4));
    Value *Version = LoadVersion;

    // RDTSC before
    Function *RdtscFn =
        Intrinsic::getDeclaration(&M, Intrinsic::readcyclecounter);
    Value *StartCycles = Builder.CreateCall(RdtscFn, {});

    // Create version dispatch blocks
    BasicBlock *V0BB = BasicBlock::Create(Ctx, "call_v0", Wrapper);
    BasicBlock *V1BB = BasicBlock::Create(Ctx, "call_v1", Wrapper);
    BasicBlock *V2BB = BasicBlock::Create(Ctx, "call_v2", Wrapper);
    BasicBlock *V3BB = BasicBlock::Create(Ctx, "call_v3", Wrapper);
    BasicBlock *MeasureBB = BasicBlock::Create(Ctx, "measure", Wrapper);

    SwitchInst *VersionSwitch = Builder.CreateSwitch(Version, V0BB, 4);
    VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)), V0BB);
    VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)), V1BB);
    VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)), V2BB);
    VersionSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 3)), V3BB);

    // Create PHI for results if non-void
    PHINode *ResultPhi = nullptr;
    if (!Orig->getReturnType()->isVoidTy()) {
      Builder.SetInsertPoint(MeasureBB);
      ResultPhi = Builder.CreatePHI(Orig->getReturnType(), 4, "result");
    }

    // Call each version
    for (int v = 0; v < 4; v++) {
      BasicBlock *VBB = (v == 0 ? V0BB : v == 1 ? V1BB : v == 2 ? V2BB : V3BB);
      Builder.SetInsertPoint(VBB);

      if (Orig->getReturnType()->isVoidTy()) {
        Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                           metadata.versions[v], Args);
        Builder.CreateBr(MeasureBB);
      } else {
        Value *VResult =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        ResultPhi->addIncoming(VResult, VBB);
        Builder.CreateBr(MeasureBB);
      }
    }

    Builder.SetInsertPoint(MeasureBB);

    // RDTSC after
    Value *EndCycles = Builder.CreateCall(RdtscFn, {});
    Value *Elapsed = Builder.CreateSub(EndCycles, StartCycles);

    // Update statistics with ATOMIC ADD
    BasicBlock *UpdateBB[4], *NextBB[4];
    for (int v = 0; v < 4; v++) {
      UpdateBB[v] =
          BasicBlock::Create(Ctx, "update_v" + std::to_string(v), Wrapper);
      NextBB[v] =
          BasicBlock::Create(Ctx, "next_v" + std::to_string(v), Wrapper);
    }

    BasicBlock *RotateBB = BasicBlock::Create(Ctx, "rotate", Wrapper);

    for (int v = 0; v < 4; v++) {
      Value *IsThisVersion =
          Builder.CreateICmpEQ(Version, ConstantInt::get(i32Ty, v));
      Builder.CreateCondBr(IsThisVersion, UpdateBB[v], NextBB[v]);

      Builder.SetInsertPoint(UpdateBB[v]);
      {
        // Atomic add to cpuCycles
        Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.cpuCycles[v],
                                Elapsed, MaybeAlign(),
                                AtomicOrdering::Monotonic);

        // Atomic add to cpuCyclesSquared
        Value *ElapsedSquared = Builder.CreateMul(Elapsed, Elapsed);
        Builder.CreateAtomicRMW(AtomicRMWInst::Add,
                                metadata.cpuCyclesSquared[v], ElapsedSquared,
                                MaybeAlign(), AtomicOrdering::Monotonic);

        // Atomic add to runCount
        Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.runCount[v],
                                ConstantInt::get(i32Ty, 1), MaybeAlign(),
                                AtomicOrdering::Monotonic);

        Builder.CreateBr(RotateBB);
      }

      Builder.SetInsertPoint(NextBB[v]);
      if (v < 3) {
        // Continue checking
      } else {
        Builder.CreateBr(RotateBB);
      }
    }

    Builder.SetInsertPoint(RotateBB);

    // Rotate to next version with ATOMIC
    Value *NextVersion = Builder.CreateAdd(Version, ConstantInt::get(i32Ty, 1));
    Value *NextVersionMod =
        Builder.CreateURem(NextVersion, ConstantInt::get(i32Ty, 4));
    StoreInst *StoreNext =
        Builder.CreateStore(NextVersionMod, metadata.currentVersion);
    StoreNext->setAtomic(AtomicOrdering::Release);
    StoreNext->setAlignment(Align(4));

    if (ResultPhi) {
      Builder.CreateRet(ResultPhi);
    } else {
      Builder.CreateRetVoid();
    }
  }

  //--- STATE 1: FINALIZE WITH CAS ---
  Builder.SetInsertPoint(FinalizeBB);
  {
    // CAS: Try to change profilingComplete from 1 to 2
    // Only ONE thread succeeds, others skip
    Value *Expected = ConstantInt::get(i32Ty, 1);
    Value *Desired = ConstantInt::get(i32Ty, 2);

    AtomicCmpXchgInst *CAS = Builder.CreateAtomicCmpXchg(
        metadata.profilingComplete, Expected, Desired, MaybeAlign(),
        AtomicOrdering::Release, AtomicOrdering::Acquire);
    CAS->setAlignment(Align(4));

    Value *Success = Builder.CreateExtractValue(CAS, 1);

    BasicBlock *DoFinalizeBB = BasicBlock::Create(Ctx, "do_finalize", Wrapper);
    BasicBlock *SkipFinalizeBB =
        BasicBlock::Create(Ctx, "skip_finalize", Wrapper);

    Builder.CreateCondBr(Success, DoFinalizeBB, SkipFinalizeBB);

    // Only ONE thread enters DoFinalizeBB
    Builder.SetInsertPoint(DoFinalizeBB);
    {
      // Load all statistics
      Value *Cycles[4], *CyclesSq[4], *Runs[4];
      for (int v = 0; v < 4; v++) {
        LoadInst *LC = Builder.CreateLoad(i64Ty, metadata.cpuCycles[v]);
        LC->setAtomic(AtomicOrdering::Acquire);
        Cycles[v] = LC;

        LoadInst *LCS = Builder.CreateLoad(i64Ty, metadata.cpuCyclesSquared[v]);
        LCS->setAtomic(AtomicOrdering::Acquire);
        CyclesSq[v] = LCS;

        LoadInst *LR = Builder.CreateLoad(i32Ty, metadata.runCount[v]);
        LR->setAtomic(AtomicOrdering::Acquire);
        Runs[v] = LR;
      }

      // Calculate statistics
      Value *Mean[4], *CV[4], *StdErr_arr[4];
      Function *SqrtFn = Intrinsic::getDeclaration(&M, Intrinsic::sqrt,
                                                   {Builder.getDoubleTy()});
      for (int v = 0; v < 4; v++) {
        Value *RunsF64 = Builder.CreateUIToFP(Runs[v], Builder.getDoubleTy());
        Value *CyclesF64 =
            Builder.CreateUIToFP(Cycles[v], Builder.getDoubleTy());
        Value *CyclesSqF64 =
            Builder.CreateUIToFP(CyclesSq[v], Builder.getDoubleTy());

        Value *HasRuns =
            Builder.CreateICmpSGT(Runs[v], ConstantInt::get(i32Ty, 0));
        Value *SafeRuns = Builder.CreateSelect(
            HasRuns, RunsF64, ConstantFP::get(Builder.getDoubleTy(), 1.0));

        Mean[v] = Builder.CreateFDiv(CyclesF64, SafeRuns);

        Value *MeanOfSquares = Builder.CreateFDiv(CyclesSqF64, SafeRuns);
        Value *MeanSquared = Builder.CreateFMul(Mean[v], Mean[v]);
        Value *ZeroF64 = ConstantFP::get(Builder.getDoubleTy(), 0.0);
        Value *Variance = Builder.CreateFSub(MeanOfSquares, MeanSquared);
        Value *SafeVariance = Builder.CreateSelect(
            Builder.CreateFCmpOGT(Variance, ZeroF64), Variance, ZeroF64);

        Value *StdDev = Builder.CreateCall(SqrtFn, {SafeVariance});
        Value *SqrtN = Builder.CreateCall(SqrtFn, {SafeRuns});
        Value *StdErr = Builder.CreateFDiv(StdDev, SqrtN);
        Value *SafeMean = Builder.CreateSelect(
            Builder.CreateFCmpOGT(Mean[v], ZeroF64), Mean[v],
            ConstantFP::get(Builder.getDoubleTy(), 1.0));
        CV[v] = Builder.CreateFDiv(StdErr, SafeMean);

        Value *MaxVal = ConstantFP::get(Builder.getDoubleTy(),
                                        std::numeric_limits<double>::max());
        Value *ZeroStdErr = ZeroF64;
        Mean[v] = Builder.CreateSelect(HasRuns, Mean[v], MaxVal);
        CV[v] = Builder.CreateSelect(HasRuns, CV[v], MaxVal);
        StdErr_arr[v] = Builder.CreateSelect(HasRuns, StdErr, ZeroStdErr);
      }

      // Select best version using Welch's t-test + 10% improvement threshold
      Value *BestVersion = ConstantInt::get(i32Ty, 0);
      Value *BestMean = Mean[0];
      Value *BestCV = CV[0];
      Value *BestStdErr = StdErr_arr[0];
      Value *CVThreshold = ConstantFP::get(Builder.getDoubleTy(), 0.50);

      for (int v = 1; v < 4; v++) {
        Value *CurrentReliable = Builder.CreateFCmpOLT(CV[v], CVThreshold);
        Value *BestReliable = Builder.CreateFCmpOLT(BestCV, CVThreshold);
        Value *IsFaster = Builder.CreateFCmpOLT(Mean[v], BestMean);

        // MINIMUM IMPROVEMENT: Require >10% improvement to avoid noise
        Value *ImprovementAbs = Builder.CreateFSub(BestMean, Mean[v]);
        Value *IsBestMeanNonZero = Builder.CreateFCmpONE(
            BestMean, ConstantFP::get(Builder.getDoubleTy(), 0.0));
        Value *ImprovementRatio = Builder.CreateSelect(
            IsBestMeanNonZero, Builder.CreateFDiv(ImprovementAbs, BestMean),
            ConstantFP::get(Builder.getDoubleTy(), 0.0));
        Value *MinImprovement =
            ConstantFP::get(Builder.getDoubleTy(), 0.10); // 10%
        Value *SignificantImprovement =
            Builder.CreateFCmpOGT(ImprovementRatio, MinImprovement);

        // WELCH'S T-TEST: Statistical significance test
        // t = (mean_best - mean_candidate) / sqrt(stderr_candidate^2 +
        // stderr_best^2)
        // For 95% confidence with large samples, |t| > 1.96 is significant
        Value *StdErr1Sq = Builder.CreateFMul(StdErr_arr[v], StdErr_arr[v]);
        Value *StdErr2Sq = Builder.CreateFMul(BestStdErr, BestStdErr);
        Value *PooledVar = Builder.CreateFAdd(StdErr1Sq, StdErr2Sq);
        Value *PooledStdErr = Builder.CreateCall(SqrtFn, {PooledVar});

        Value *MeanDiff = Builder.CreateFSub(BestMean, Mean[v]);
        Value *HasValidStdErr = Builder.CreateFCmpOGT(
            PooledStdErr, ConstantFP::get(Builder.getDoubleTy(), 0.0));
        Value *TStatistic = Builder.CreateSelect(
            HasValidStdErr, Builder.CreateFDiv(MeanDiff, PooledStdErr),
            ConstantFP::get(Builder.getDoubleTy(), 0.0));

        // |t| > 1.96 for 95% confidence
        Value *TThreshold = ConstantFP::get(Builder.getDoubleTy(), 1.96);
        Value *WelchSignificant = Builder.CreateAnd(
            HasValidStdErr, Builder.CreateFCmpOGT(TStatistic, TThreshold));

        // Decision logic:
        // 1. Both reliable: pick faster AND (improvement > 10% OR Welch
        // significant)
        // 2. Only current reliable: pick current
        // 3. Only best reliable: keep best
        Value *BothReliable = Builder.CreateAnd(CurrentReliable, BestReliable);
        Value *OnlyCurrentReliable =
            Builder.CreateAnd(CurrentReliable, Builder.CreateNot(BestReliable));

        // Statistically significant: either 10% improvement OR Welch's
        // t-test passes
        Value *StatisticallySignificant =
            Builder.CreateOr(SignificantImprovement, WelchSignificant);

        // Switch if: (both reliable AND faster AND statistically
        // significant) OR (only current reliable)
        Value *BothReliableAndSignificant =
            Builder.CreateAnd(Builder.CreateAnd(BothReliable, IsFaster),
                              StatisticallySignificant);

        Value *ShouldUpdate =
            Builder.CreateOr(BothReliableAndSignificant, OnlyCurrentReliable);

        BestVersion = Builder.CreateSelect(
            ShouldUpdate, ConstantInt::get(i32Ty, v), BestVersion);
        BestMean = Builder.CreateSelect(ShouldUpdate, Mean[v], BestMean);
        BestCV = Builder.CreateSelect(ShouldUpdate, CV[v], BestCV);
        BestStdErr =
            Builder.CreateSelect(ShouldUpdate, StdErr_arr[v], BestStdErr);
      }

      // Store best version with ATOMIC
      StoreInst *StoreBest =
          Builder.CreateStore(BestVersion, metadata.bestVersion);
      StoreBest->setAtomic(AtomicOrdering::Release);
      StoreBest->setAlignment(Align(4));

      // Call __adaptive_write_profile
      FunctionType *WriteProfileType = FunctionType::get(
          Type::getVoidTy(Ctx),
          {PointerType::get(Type::getInt8Ty(Ctx), 0), i32Ty}, false);
      FunctionCallee WriteProfile =
          M.getOrInsertFunction("__adaptive_write_profile", WriteProfileType);
      Builder.CreateCall(WriteProfile, {FuncName, BestVersion});

      Builder.CreateBr(SkipFinalizeBB);
    }

    Builder.SetInsertPoint(SkipFinalizeBB);

    // Load best version and use it
    LoadInst *LoadBest = Builder.CreateLoad(i32Ty, metadata.bestVersion);
    LoadBest->setAtomic(AtomicOrdering::Acquire);
    LoadBest->setAlignment(Align(4));
    Value *Best = LoadBest;

    // Call best version
    BasicBlock *BestV0BB = BasicBlock::Create(Ctx, "best_v0", Wrapper);
    BasicBlock *BestV1BB = BasicBlock::Create(Ctx, "best_v1", Wrapper);
    BasicBlock *BestV2BB = BasicBlock::Create(Ctx, "best_v2", Wrapper);
    BasicBlock *BestV3BB = BasicBlock::Create(Ctx, "best_v3", Wrapper);
    BasicBlock *ReturnBB = BasicBlock::Create(Ctx, "return_finalize", Wrapper);

    SwitchInst *BestSwitch = Builder.CreateSwitch(Best, BestV0BB, 4);
    BestSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)),
                        BestV0BB);
    BestSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)),
                        BestV1BB);
    BestSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)),
                        BestV2BB);
    BestSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 3)),
                        BestV3BB);

    PHINode *FinalResult = nullptr;
    if (!Orig->getReturnType()->isVoidTy()) {
      Builder.SetInsertPoint(ReturnBB);
      FinalResult = Builder.CreatePHI(Orig->getReturnType(), 4, "final_result");
    }

    for (int v = 0; v < 4; v++) {
      BasicBlock *BB = (v == 0   ? BestV0BB
                        : v == 1 ? BestV1BB
                        : v == 2 ? BestV2BB
                                 : BestV3BB);
      Builder.SetInsertPoint(BB);

      if (Orig->getReturnType()->isVoidTy()) {
        Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                           metadata.versions[v], Args);
        Builder.CreateBr(ReturnBB);
      } else {
        Value *VResult =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        FinalResult->addIncoming(VResult, BB);
        Builder.CreateBr(ReturnBB);
      }
    }

    Builder.SetInsertPoint(ReturnBB);
    if (FinalResult) {
      Builder.CreateRet(FinalResult);
    } else {
      Builder.CreateRetVoid();
    }
  }

  //--- STATE 2: OPTIMAL ---
  Builder.SetInsertPoint(OptimalBB);
  {
    // Load best version with ATOMIC
    LoadInst *LoadBest = Builder.CreateLoad(i32Ty, metadata.bestVersion);
    LoadBest->setAtomic(AtomicOrdering::Acquire);
    LoadBest->setAlignment(Align(4));
    Value *Best = LoadBest;

    // Call best version
    BasicBlock *OptV0BB = BasicBlock::Create(Ctx, "opt_v0", Wrapper);
    BasicBlock *OptV1BB = BasicBlock::Create(Ctx, "opt_v1", Wrapper);
    BasicBlock *OptV2BB = BasicBlock::Create(Ctx, "opt_v2", Wrapper);
    BasicBlock *OptV3BB = BasicBlock::Create(Ctx, "opt_v3", Wrapper);
    BasicBlock *OptReturnBB =
        BasicBlock::Create(Ctx, "return_optimal", Wrapper);

    SwitchInst *OptSwitch = Builder.CreateSwitch(Best, OptV0BB, 4);
    OptSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)), OptV0BB);
    OptSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)), OptV1BB);
    OptSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)), OptV2BB);
    OptSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 3)), OptV3BB);

    PHINode *OptResult = nullptr;
    if (!Orig->getReturnType()->isVoidTy()) {
      Builder.SetInsertPoint(OptReturnBB);
      OptResult = Builder.CreatePHI(Orig->getReturnType(), 4, "opt_result");
    }

    for (int v = 0; v < 4; v++) {
      BasicBlock *BB = (v == 0   ? OptV0BB
                        : v == 1 ? OptV1BB
                        : v == 2 ? OptV2BB
                                 : OptV3BB);
      Builder.SetInsertPoint(BB);

      if (Orig->getReturnType()->isVoidTy()) {
        Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                           metadata.versions[v], Args);
        Builder.CreateBr(OptReturnBB);
      } else {
        Value *VResult =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        OptResult->addIncoming(VResult, BB);
        Builder.CreateBr(OptReturnBB);
      }
    }

    Builder.SetInsertPoint(OptReturnBB);
    if (OptResult) {
      Builder.CreateRet(OptResult);
    } else {
      Builder.CreateRetVoid();
    }
  }

  return Wrapper;
}

// Create Production Wrapper (Zero-Overhead Direct Call via Function Pointer)
Function *
SimpleAdaptivePassImpl::createProductionWrapper(FunctionMetadata &metadata,
                                                Module &M) {
  LLVMContext &Ctx = M.getContext();
  Function *Orig = metadata.original;

  FunctionType *WrapperType = Orig->getFunctionType();
  Function *Wrapper =
      Function::Create(WrapperType, GlobalValue::ExternalLinkage,
                       Orig->getName() + "_wrapper", &M);
  Wrapper->setAttributes(Orig->getAttributes());
  Wrapper->setCallingConv(Orig->getCallingConv());
  Wrapper->setVisibility(Orig->getVisibility());
  Wrapper->setUnnamedAddr(Orig->getUnnamedAddr());
  Wrapper->setDSOLocal(Orig->isDSOLocal());
  if (Orig->hasComdat())
    Wrapper->setComdat(Orig->getComdat());

  // CRITICAL FIX: Remove adaptive attribute to prevent recursive processing
  Wrapper->removeFnAttr("adaptive");
  if (Wrapper->hasMetadata()) {
    Wrapper->setMetadata("annotation", nullptr);
  }

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Wrapper);
  BasicBlock *InitBB = BasicBlock::Create(Ctx, "init", Wrapper);
  BasicBlock *CallBB = BasicBlock::Create(Ctx, "call", Wrapper);
  IRBuilder<> Builder(Entry);

  Type *i32Ty = Type::getInt32Ty(Ctx);
  Type *i8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
  PointerType *FuncPtrTy = PointerType::get(WrapperType, 0);

  std::vector<Value *> Args;
  for (auto &Arg : Wrapper->args())
    Args.push_back(&Arg);

  // Create a global variable to store the function pointer (initialized to
  // null) This replaces the bestVersion integer with a direct function pointer
  GlobalVariable *BestFuncPtr =
      new GlobalVariable(M, FuncPtrTy, false, GlobalValue::InternalLinkage,
                         ConstantPointerNull::get(FuncPtrTy),
                         "__adaptive_best_func_" + Orig->getName().str());
  BestFuncPtr->setAlignment(Align(8));

  // Check if function pointer is null (one-time initialization check)
  LoadInst *LoadPtr = Builder.CreateLoad(FuncPtrTy, BestFuncPtr);
  Value *IsNull =
      Builder.CreateICmpEQ(LoadPtr, ConstantPointerNull::get(FuncPtrTy));

  Builder.CreateCondBr(IsNull, InitBB, CallBB);

  // Initialize: Read profile file once and store function pointer
  Builder.SetInsertPoint(InitBB);
  {
    Value *FuncName = Builder.CreateGlobalStringPtr(metadata.profileKey);

    // Call __adaptive_read_profile
    FunctionType *ReadProfileType = FunctionType::get(i32Ty, {i8PtrTy}, false);
    FunctionCallee ReadProfile =
        M.getOrInsertFunction("__adaptive_read_profile", ReadProfileType);

    Value *ProfiledBest = Builder.CreateCall(ReadProfile, {FuncName});

    // If not found (-1), default to V0
    Value *NotFound =
        Builder.CreateICmpEQ(ProfiledBest, ConstantInt::get(i32Ty, -1));
    Value *ActualBest = Builder.CreateSelect(
        NotFound, ConstantInt::get(i32Ty, 0), ProfiledBest);

#if ADAPTIVE_DEBUG_PRINTS
    // Debug output to confirm which version is selected
    FunctionType *PrintfType = FunctionType::get(i32Ty, {i8PtrTy}, true);
    FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);

    Value *DebugMsg = Builder.CreateGlobalStringPtr(
        "[ADAPTIVE PRODUCTION] %s using version %d\n");
    Builder.CreateCall(Printf, {DebugMsg, FuncName, ActualBest});
#endif

    // Create switch to select the correct function pointer based on best
    // version
    BasicBlock *SetV0 = BasicBlock::Create(Ctx, "set_v0", Wrapper);
    BasicBlock *SetV1 = BasicBlock::Create(Ctx, "set_v1", Wrapper);
    BasicBlock *SetV2 = BasicBlock::Create(Ctx, "set_v2", Wrapper);
    BasicBlock *SetV3 = BasicBlock::Create(Ctx, "set_v3", Wrapper);
    BasicBlock *StorePtr = BasicBlock::Create(Ctx, "store_ptr", Wrapper);

    SwitchInst *InitSwitch = Builder.CreateSwitch(ActualBest, SetV0, 4);
    InitSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)), SetV0);
    InitSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)), SetV1);
    InitSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)), SetV2);
    InitSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 3)), SetV3);

    // Create PHI node to collect the selected function pointer
    Builder.SetInsertPoint(StorePtr);
    PHINode *SelectedFunc = Builder.CreatePHI(FuncPtrTy, 4, "selected_func");

    // Set each version's function pointer
    for (int v = 0; v < 4; v++) {
      BasicBlock *SetBB = (v == 0   ? SetV0
                           : v == 1 ? SetV1
                           : v == 2 ? SetV2
                                    : SetV3);
      Builder.SetInsertPoint(SetBB);
      SelectedFunc->addIncoming(metadata.versions[v], SetBB);
      Builder.CreateBr(StorePtr);
    }

    // Store the selected function pointer (one-time write)
    Builder.SetInsertPoint(StorePtr);
    Builder.CreateStore(SelectedFunc, BestFuncPtr);

    // Also store to bestVersion for compatibility with profiling output
    Builder.CreateStore(ActualBest, metadata.bestVersion);

    Builder.CreateBr(CallBB);
  }

  // Call: Direct call through function pointer (ZERO OVERHEAD after init)
  Builder.SetInsertPoint(CallBB);
  {
    // Load function pointer - this is the ONLY operation on the hot path
    LoadInst *FuncPtr = Builder.CreateLoad(FuncPtrTy, BestFuncPtr);

    // Direct call through function pointer - no atomics, no branches, no switch
    if (Orig->getReturnType()->isVoidTy()) {
      Builder.CreateCall(WrapperType, FuncPtr, Args);
      Builder.CreateRetVoid();
    } else {
      Value *Result = Builder.CreateCall(WrapperType, FuncPtr, Args);
      Builder.CreateRet(Result);
    }
  }

  return Wrapper;
}

// Create Thin Dispatcher
Function *SimpleAdaptivePassImpl::createThinDispatcher(
    FunctionMetadata &metadata, Module &M, std::string targetName) {
  LLVMContext &Ctx = M.getContext();
  Function *Orig = metadata.original;

  FunctionType *DispatcherType = Orig->getFunctionType();
  Function *Dispatcher =
      Function::Create(DispatcherType, Orig->getLinkage(), targetName, &M);
  Dispatcher->setAttributes(Orig->getAttributes());
  Dispatcher->setCallingConv(Orig->getCallingConv());

  // CRITICAL FIX: Remove adaptive attribute to prevent recursive processing
  Dispatcher->removeFnAttr("adaptive");
  if (Dispatcher->hasMetadata()) {
    Dispatcher->setMetadata("annotation", nullptr);
  }

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Dispatcher);
  IRBuilder<> Builder(Entry);

  std::vector<Value *> Args;
  for (auto &Arg : Dispatcher->args())
    Args.push_back(&Arg);

  if (Orig->getReturnType()->isVoidTy()) {
    Builder.CreateCall(metadata.wrapper->getFunctionType(), metadata.wrapper,
                       Args);
    Builder.CreateRetVoid();
  } else {
    Value *Result = Builder.CreateCall(metadata.wrapper->getFunctionType(),
                                       metadata.wrapper, Args);
    Builder.CreateRet(Result);
  }

  return Dispatcher;
}

// Process Adaptive Function
void SimpleAdaptivePassImpl::processAdaptiveFunction(Function *F, Module &M) {
  FunctionMetadata metadata;
  metadata.original = F;
  std::string originalName = F->getName().str();
  auto originalLinkage = F->getLinkage();
  auto originalVisibility = F->getVisibility();
  auto originalUnnamedAddr = F->getUnnamedAddr();
  bool originalDSOLocal = F->isDSOLocal();
  Comdat *originalComdat = F->hasComdat() ? F->getComdat() : nullptr;

  // CRITICAL FIX: Skip variadic functions as the current wrapper logic does not
  // support them properly
  if (F->isVarArg()) {
    errs() << "[ADAPTIVE] Skipping variadic function: " << originalName << "\n";
    F->removeFnAttr("adaptive");
    return;
  }

  // RENAME ORIGINAL FIRST to avoid collision with dispatcher
  F->setName(originalName + "_orig");
  metadata.profileKey = buildProfileKey(M, F->getName());

  // Always create 4 optimized versions
  for (int i = 0; i < 4; i++) {
    metadata.versions[i] = createVersion(F, i, M);
  }

  if (isProductionMode()) {
    int selectedVersion = readBestVersionFromProfile(metadata.profileKey);
    if (selectedVersion < 0 || selectedVersion > 3)
      selectedVersion = 0;

    Function *Selected = metadata.versions[selectedVersion];
    Selected->setName(originalName);
    Selected->setLinkage(originalLinkage);
    Selected->setVisibility(originalVisibility);
    Selected->setUnnamedAddr(originalUnnamedAddr);
    Selected->setDSOLocal(originalDSOLocal);
    if (originalComdat)
      Selected->setComdat(originalComdat);

    SmallVector<Use *, 32> UsesToRewrite;
    for (Use &U : F->uses())
      UsesToRewrite.push_back(&U);
    for (Use *U : UsesToRewrite)
      U->set(Selected);

    // Keep the old original internal and dead; normal DCE can clean it up.
    F->setLinkage(GlobalValue::InternalLinkage);
    return;
  }

  // MODE-SPECIFIC WRAPPER GENERATION
  createWrapperStaticVars(metadata, M);
  metadata.wrapper = createProfilingWrapper(metadata, M);

  // Create dispatcher with the ORIGINAL NAME
  metadata.dispatcher = createThinDispatcher(metadata, M, originalName);

  // Replace original uses in non-adaptive code paths only. Keeping adaptive
  // internals direct avoids recursive dispatch overhead.
  SmallVector<Use *, 32> UsesToRewrite;
  for (Use &U : F->uses()) {
    auto *I = dyn_cast<Instruction>(U.getUser());
    if (!I) {
      UsesToRewrite.push_back(&U);
      continue;
    }

    Function *Caller = I->getFunction();
    if (!Caller) {
      UsesToRewrite.push_back(&U);
      continue;
    }

    StringRef CallerName = Caller->getName();
    bool IsAdaptiveInternal =
        Caller == F || Caller == metadata.wrapper ||
        Caller == metadata.dispatcher || Caller->hasFnAttribute("adaptive") ||
        CallerName.contains("_orig_v") ||
        CallerName.ends_with("_orig_wrapper") || CallerName.ends_with("_orig");

    if (!IsAdaptiveInternal)
      UsesToRewrite.push_back(&U);
  }

  for (Use *U : UsesToRewrite)
    U->set(metadata.dispatcher);

  F->setLinkage(GlobalValue::InternalLinkage);

  functionMap[F] = metadata;
}

// Initialization Function
void SimpleAdaptivePassImpl::createInitializationFunction(Module &M) {
  LLVMContext &Ctx = M.getContext();

  if (M.getFunction("__adaptive_init"))
    return;

  FunctionType *InitType = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
  Function *InitFunc = Function::Create(InitType, GlobalValue::InternalLinkage,
                                        "__adaptive_init", &M);
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", InitFunc);
  IRBuilder<> Builder(Entry);

  // PROFILING MODE: Initialize runtime and register functions
  if (isProfilingMode()) {
    FunctionType *InitProfType =
        FunctionType::get(Type::getVoidTy(Ctx), {}, false);
    FunctionCallee InitProf =
        M.getOrInsertFunction("__adaptive_init_profiling", InitProfType);
    Builder.CreateCall(InitProf);

    Type *i8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
    Type *i32PtrTy = PointerType::get(Type::getInt32Ty(Ctx), 0);
    Type *i64PtrTy = PointerType::get(Type::getInt64Ty(Ctx), 0);

    // New signature: name, complete_flag, cycles[4], cycles_sq[4], runs[4],
    // best_version
    SmallVector<Type *, 16> RegParams;
    RegParams.push_back(i8PtrTy);  // name
    RegParams.push_back(i32PtrTy); // complete_flag
    for (int i = 0; i < 4; i++)
      RegParams.push_back(i64PtrTy); // cycles[4]
    for (int i = 0; i < 4; i++)
      RegParams.push_back(i64PtrTy); // cycles_sq[4]
    for (int i = 0; i < 4; i++)
      RegParams.push_back(i32PtrTy); // runs[4]
    RegParams.push_back(i32PtrTy);   // best_version

    FunctionType *RegType =
        FunctionType::get(Type::getVoidTy(Ctx), RegParams, false);
    FunctionCallee RegFunc =
        M.getOrInsertFunction("__adaptive_register_function", RegType);

    for (auto &pair : functionMap) {
      FunctionMetadata &metadata = pair.second;
      if (metadata.profilingComplete) { // Only profiling mode has this
        Value *FuncName = Builder.CreateGlobalStringPtr(metadata.profileKey);

        SmallVector<Value *, 16> RegArgs;
        RegArgs.push_back(FuncName);
        RegArgs.push_back(metadata.profilingComplete);
        for (int i = 0; i < 4; i++)
          RegArgs.push_back(metadata.cpuCycles[i]);
        for (int i = 0; i < 4; i++)
          RegArgs.push_back(metadata.cpuCyclesSquared[i]);
        for (int i = 0; i < 4; i++)
          RegArgs.push_back(metadata.runCount[i]);
        RegArgs.push_back(metadata.bestVersion);

        Builder.CreateCall(RegFunc, RegArgs);
      }
    }
  } else {
    // PRODUCTION MODE: Just print info
    // errs() << "[ADAPTIVE] Production mode initialization for "
    // << functionMap.size() << " function(s)\n";
  }

  Builder.CreateRetVoid();
  appendToGlobalCtors(M, InitFunc, 65535);
}

} // namespace llvm
