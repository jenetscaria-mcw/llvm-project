#include "llvm/Transforms/IPO/SimpleAdaptive.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
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
#include "llvm/ADT/SmallVector.h"
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

  GlobalVariable *cpuCycles[4];
  GlobalVariable *cpuCyclesSquared[4];
  GlobalVariable *runCount[4];
  GlobalVariable *currentVersion;
  GlobalVariable *profilingComplete; // 0=profiling, 1=finalize, 2=done
  GlobalVariable *bestVersion;

  // Warmup: skip timing for first WARMUP_ROUNDS calls
  GlobalVariable *warmupCounter;

  // Paired t-tests: per-round measurements and difference accumulators
  GlobalVariable *lastMeasurement[4];  // Most recent elapsed for each version
  GlobalVariable *pairDiffSum[3];      // Accumulated (V_i - V0) for i=1,2,3
  GlobalVariable *pairDiffSqSum[3];    // Accumulated (V_i - V0)^2
  GlobalVariable *roundCount;          // Number of completed rounds
};

// Optimization strategies for variant generation
enum OptStrategy {
  BASELINE,           // No special optimizations
  VECTORIZE_AVX2,     // prefer-vector-width=256 + avx2
  VECTORIZE_AVX512,   // prefer-vector-width=512 + avx512f,avx512vl
  UNROLL_MODERATE,    // unroll-count=4
  UNROLL_AGGRESSIVE,  // unroll-count=16 + interleave-count=4
  FAST_MATH,          // unsafe-fp-math + no-nans + no-infs
  AGGRESSIVE_INLINE,  // AlwaysInline + high inline-threshold
  OPTIMIZE_SIZE,      // OptimizeForSize (better icache)
  LOOP_UNROLL_VECTOR, // Combined: unroll + avx2 vectorization
  PREFETCH_HEAVY,     // Software prefetch hints for memory-intensive
  VECTOR_FASTMATH,    // Combined: avx2 + fast-math (FP-heavy vectorizable loops)
  UNROLL_FASTMATH,    // Combined: unroll-count=4 + fast-math
  FULL_AGGRESSIVE,    // Combined: avx512 + unroll-count=8 + fast-math
};

static const char *strategyName(OptStrategy s) {
  switch (s) {
  case BASELINE:
    return "Baseline";
  case VECTORIZE_AVX2:
    return "Vectorize-AVX2";
  case VECTORIZE_AVX512:
    return "Vectorize-AVX512";
  case UNROLL_MODERATE:
    return "Unroll-Moderate";
  case UNROLL_AGGRESSIVE:
    return "Unroll-Aggressive";
  case FAST_MATH:
    return "Fast-Math";
  case AGGRESSIVE_INLINE:
    return "Aggressive-Inline";
  case OPTIMIZE_SIZE:
    return "Optimize-Size";
  case LOOP_UNROLL_VECTOR:
    return "Loop-Unroll+Vector";
  case PREFETCH_HEAVY:
    return "Prefetch-Heavy";
  case VECTOR_FASTMATH:
    return "Vector+FastMath";
  case UNROLL_FASTMATH:
    return "Unroll+FastMath";
  case FULL_AGGRESSIVE:
    return "Full-Aggressive";
  }
  return "Unknown";
}

struct FunctionCharacteristics {
  int instructionCount;
  int basicBlockCount;
  int loopCount;
  int loadStoreCount;
  int fpOpCount;
  int callCount;
  int branchCount;

  // Accurate analysis fields (from LoopInfo / ScalarEvolution)
  int loopDepth;          // Maximum loop nesting depth
  int estimatedTripCount; // Estimated trip count of hottest loop (0 = unknown)
  int reductionCount;     // Number of reduction patterns detected
  bool hasStrideAccess;   // Has loop-dependent memory access (vectorizable)

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

// Select 4 optimization strategies for a function based on its characteristics
static SmallVector<OptStrategy, 4>
selectStrategies(const FunctionCharacteristics &fc) {
  struct StrategyScore {
    OptStrategy strategy;
    int score;
  };
  SmallVector<StrategyScore, 10> candidates;

  // Score each applicable strategy
  if (fc.canBenefitFromVectorization) {
    candidates.push_back({VECTORIZE_AVX2, 80 + fc.loopDepth * 10});
    if (fc.estimatedTripCount == 0 || fc.estimatedTripCount > 16)
      candidates.push_back({VECTORIZE_AVX512, 85 + fc.loopDepth * 10});
  }

  if (fc.canBenefitFromUnrolling) {
    candidates.push_back(
        {UNROLL_MODERATE, 60 + std::min(fc.loopCount * 5, 20)});
    if (fc.isSmall || fc.estimatedTripCount > 8)
      candidates.push_back(
          {UNROLL_AGGRESSIVE, 70 + std::min(fc.loopCount * 5, 20)});
  }

  if (fc.canBenefitFromFastMath)
    candidates.push_back({FAST_MATH, 75 + std::min(fc.fpOpCount, 20)});

  if (fc.canBenefitFromInlining)
    candidates.push_back({AGGRESSIVE_INLINE, 50});

  if (fc.isLarge || fc.manyBranches)
    candidates.push_back({OPTIMIZE_SIZE, 40});

  if (fc.canBenefitFromVectorization && fc.canBenefitFromUnrolling)
    candidates.push_back({LOOP_UNROLL_VECTOR, 90 + fc.loopDepth * 10});

  // Combinatorial strategies — score highest when multiple traits are present
  if (fc.canBenefitFromVectorization && fc.canBenefitFromFastMath)
    candidates.push_back(
        {VECTOR_FASTMATH, 92 + fc.loopDepth * 10 + std::min(fc.fpOpCount, 10)});

  if (fc.canBenefitFromUnrolling && fc.canBenefitFromFastMath)
    candidates.push_back(
        {UNROLL_FASTMATH, 82 + std::min(fc.loopCount * 5, 20)});

  if (fc.canBenefitFromVectorization && fc.canBenefitFromUnrolling &&
      fc.canBenefitFromFastMath && fc.isLarge)
    candidates.push_back({FULL_AGGRESSIVE, 95 + fc.loopDepth * 10});

  if (fc.isMemoryIntensive && fc.hasLoops)
    candidates.push_back(
        {PREFETCH_HEAVY, 65 + std::min(fc.loadStoreCount, 30)});

  // Sort by score descending
  llvm::sort(candidates,
             [](const StrategyScore &a, const StrategyScore &b) {
               return a.score > b.score;
             });

  // Build result: baseline + top 3 unique strategies
  SmallVector<OptStrategy, 4> strategies;
  strategies.push_back(BASELINE);
  for (size_t i = 0; i < candidates.size() && strategies.size() < 4; i++) {
    bool dup = false;
    for (OptStrategy s : strategies)
      if (s == candidates[i].strategy) {
        dup = true;
        break;
      }
    if (!dup)
      strategies.push_back(candidates[i].strategy);
  }

  // Fill remaining slots with fallbacks
  OptStrategy fallbacks[] = {UNROLL_MODERATE, OPTIMIZE_SIZE, FAST_MATH};
  for (OptStrategy fb : fallbacks) {
    if (strategies.size() >= 4)
      break;
    bool dup = false;
    for (OptStrategy s : strategies)
      if (s == fb) {
        dup = true;
        break;
      }
    if (!dup)
      strategies.push_back(fb);
  }

  return strategies;
}

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

// Analyze function characteristics using LLVM analysis infrastructure
static FunctionCharacteristics
analyzeFunctionCharacteristics(Function *F, FunctionAnalysisManager &FAM) {
  FunctionCharacteristics fc = {};

  // Use LLVM's LoopInfo for accurate loop analysis
  auto &LI = FAM.getResult<LoopAnalysis>(*F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(*F);

  // Count all loops via depth-first traversal of the loop tree
  SmallVector<Loop *, 8> Worklist(LI.begin(), LI.end());
  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();
    fc.loopCount++;
    fc.loopDepth = std::max(fc.loopDepth, (int)L->getLoopDepth());
    // Estimate trip count via ScalarEvolution
    if (unsigned TC = SE.getSmallConstantTripCount(L))
      fc.estimatedTripCount = std::max(fc.estimatedTripCount, (int)TC);
    for (Loop *SubL : *L)
      Worklist.push_back(SubL);
  }

  // Analyze instructions
  for (BasicBlock &BB : *F) {
    fc.basicBlockCount++;
    Loop *BBLoop = LI.getLoopFor(&BB);
    bool isLoopHeader = BBLoop && BBLoop->getHeader() == &BB;

    for (Instruction &I : BB) {
      fc.instructionCount++;

      if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
        fc.loadStoreCount++;
        // Detect stride access: GEP with loop-dependent index
        if (BBLoop) {
          Value *PtrOp = isa<LoadInst>(I)
                             ? I.getOperand(0)
                             : cast<StoreInst>(I).getPointerOperand();
          if (auto *GEP =
                  dyn_cast<GetElementPtrInst>(PtrOp->stripPointerCasts())) {
            for (Use &Idx : GEP->indices()) {
              if (auto *IdxI = dyn_cast<Instruction>(Idx.get()))
                if (BBLoop->contains(IdxI))
                  fc.hasStrideAccess = true;
            }
          }
        }
      } else if (isa<CallInst>(I)) {
        fc.callCount++;
      } else if (isa<BranchInst>(I)) {
        fc.branchCount++;
      }

      Type *T = I.getType();
      if (T->isFloatingPointTy())
        fc.fpOpCount++;

      for (Use &U : I.operands())
        if (U.get()->getType()->isFloatingPointTy())
          fc.hasFPMath = true;

      // Detect reduction patterns: PHI in loop header with add/mul back-edge
      if (isLoopHeader) {
        if (auto *PN = dyn_cast<PHINode>(&I)) {
          for (unsigned i = 0; i < PN->getNumIncomingValues(); i++) {
            if (auto *BinOp =
                    dyn_cast<BinaryOperator>(PN->getIncomingValue(i))) {
              unsigned Op = BinOp->getOpcode();
              if (Op == Instruction::Add || Op == Instruction::FAdd ||
                  Op == Instruction::Mul || Op == Instruction::FMul)
                fc.reductionCount++;
            }
          }
        }
      }
    }
  }

  // Derived properties
  fc.isSmall = (fc.instructionCount < 50);
  fc.isMedium = (fc.instructionCount >= 50 && fc.instructionCount <= 200);
  fc.isLarge = (fc.instructionCount > 200);
  fc.hasLoops = (fc.loopCount > 0);
  fc.isMemoryIntensive = (fc.loadStoreCount > fc.instructionCount / 3);
  fc.hasFPMath = (fc.fpOpCount > 0);
  fc.manyBranches = (fc.branchCount > 10);

  // Improved benefit heuristics using accurate loop info
  fc.canBenefitFromVectorization =
      fc.hasLoops && (fc.hasStrideAccess || fc.reductionCount > 0) &&
      !fc.isMemoryIntensive;
  fc.canBenefitFromUnrolling =
      fc.hasLoops && (fc.isSmall || fc.isMedium) &&
      (fc.estimatedTripCount == 0 || fc.estimatedTripCount > 2);
  fc.canBenefitFromInlining = fc.isSmall && (fc.callCount < 3);
  fc.canBenefitFromFastMath = fc.hasFPMath && !fc.isMemoryIntensive;

  return fc;
}

// Pass Implementation

class SimpleAdaptivePassImpl {
private:
  std::map<Function *, FunctionMetadata> functionMap;

  Function *createVersion(Function *Orig, int versionIndex,
                          OptStrategy strategy, Module &M);
  void createWrapperStaticVars(FunctionMetadata &metadata, Module &M);
  Function *createProfilingWrapper(FunctionMetadata &metadata,
                                   Module &M); // Profiling wrapper
  Function *createProductionWrapper(FunctionMetadata &metadata,
                                    Module &M); // Production wrapper
  Function *createThinDispatcher(FunctionMetadata &metadata, Module &M,
                                 std::string targetName);
  void processAdaptiveFunction(Function *F, Module &M,
                               const SmallVector<OptStrategy, 4> &strategies);
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
  // Get FunctionAnalysisManager for LoopInfo / ScalarEvolution access
  auto &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // Phase 1: Identify adaptive functions and analyze characteristics upfront
  //          (before any transformations invalidate analysis results)
  struct AdaptiveFuncInfo {
    Function *F;
    SmallVector<OptStrategy, 4> strategies;
  };
  SmallVector<AdaptiveFuncInfo, 8> adaptiveFunctions;

  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;

    bool isAdaptive = false;
    if (F.hasFnAttribute("adaptive"))
      isAdaptive = true;

    if (!isAdaptive && F.hasMetadata()) {
      if (MDNode *MD = F.getMetadata("annotation")) {
        for (unsigned i = 0; i < MD->getNumOperands(); i++) {
          if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(i))) {
            if (MDS->getString() == "adaptive") {
              isAdaptive = true;
              break;
            }
          }
        }
      }
    }

    if (isAdaptive) {
      FunctionCharacteristics fc =
          analyzeFunctionCharacteristics(&F, FAM);
      auto strategies = selectStrategies(fc);

      errs() << "[ADAPTIVE] " << F.getName()
             << ": loops=" << fc.loopCount
             << " depth=" << fc.loopDepth
             << " trip=" << fc.estimatedTripCount
             << " instrs=" << fc.instructionCount
             << " reductions=" << fc.reductionCount
             << " stride=" << fc.hasStrideAccess
             << " | strategies:";
      for (size_t i = 0; i < strategies.size(); i++)
        errs() << " V" << i << "=" << strategyName(strategies[i]);
      errs() << "\n";

      adaptiveFunctions.push_back({&F, strategies});
    }
  }

  // Phase 2: Transform adaptive functions
  bool modified = false;
  for (auto &info : adaptiveFunctions) {
    processAdaptiveFunction(info.F, M, info.strategies);
    modified = true;
  }

  if (modified) {
    createInitializationFunction(M);
    M.addModuleFlag(
        Module::AppendUnique, "Linker Options",
        MDNode::get(M.getContext(),
                    {MDString::get(M.getContext(), "-lclang_rt.adaptive")}));
  }

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// Create Optimized Versions using strategy-based optimization
Function *SimpleAdaptivePassImpl::createVersion(Function *Orig,
                                                int versionIndex,
                                                OptStrategy strategy,
                                                Module &M) {
  ValueToValueMapTy VMap;
  Function *Clone = CloneFunction(Orig, VMap);
  Clone->setName(Orig->getName() + "_v" + std::to_string(versionIndex));
  Clone->setLinkage(GlobalValue::InternalLinkage);
  Clone->setCallingConv(Orig->getCallingConv());

  // Remove adaptive attribute to prevent recursive processing
  Clone->removeFnAttr("adaptive");
  if (Clone->hasMetadata()) {
    if (MDNode *MD = Clone->getMetadata("annotation")) {
      SmallVector<Metadata *, 4> NewAnnotations;
      for (unsigned i = 0; i < MD->getNumOperands(); i++) {
        if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(i)))
          if (MDS->getString() != "adaptive")
            NewAnnotations.push_back(MD->getOperand(i));
      }
      Clone->setMetadata("annotation",
                         NewAnnotations.empty()
                             ? nullptr
                             : MDNode::get(M.getContext(), NewAnnotations));
    }
  }

  // Rewrite recursive self-calls to call the clone directly
  for (BasicBlock &BB : *Clone) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Value *Callee = CB->getCalledOperand()->stripPointerCasts();
      if (Callee == Orig)
        CB->setCalledFunction(Clone);
    }
  }

  // Apply strategy-specific optimizations from the expanded pool
  switch (strategy) {
  case BASELINE:
    // No special attributes — standard optimization pipeline
    break;

  case VECTORIZE_AVX2:
    Clone->addFnAttr("target-features", "+avx2");
    Clone->addFnAttr("prefer-vector-width", "256");
    break;

  case VECTORIZE_AVX512:
    Clone->addFnAttr("target-features", "+avx512f,+avx512vl");
    Clone->addFnAttr("prefer-vector-width", "512");
    break;

  case UNROLL_MODERATE:
    Clone->addFnAttr("unroll-count", "4");
    break;

  case UNROLL_AGGRESSIVE:
    Clone->addFnAttr("unroll-count", "16");
    Clone->addFnAttr("interleave-count", "4");
    break;

  case FAST_MATH:
    Clone->addFnAttr("unsafe-fp-math", "true");
    Clone->addFnAttr("no-nans-fp-math", "true");
    Clone->addFnAttr("no-infs-fp-math", "true");
    break;

  case AGGRESSIVE_INLINE:
    Clone->addFnAttr(Attribute::AlwaysInline);
    Clone->addFnAttr("inline-threshold", "100000");
    break;

  case OPTIMIZE_SIZE:
    Clone->addFnAttr(Attribute::OptimizeForSize);
    break;

  case LOOP_UNROLL_VECTOR:
    // Combined: moderate unrolling + AVX2 vectorization
    Clone->addFnAttr("target-features", "+avx2");
    Clone->addFnAttr("prefer-vector-width", "256");
    Clone->addFnAttr("unroll-count", "4");
    break;

  case PREFETCH_HEAVY:
    Clone->addFnAttr("prefetch-distance", "128");
    break;

  case VECTOR_FASTMATH:
    // Combined: AVX2 vectorization + fast-math
    Clone->addFnAttr("target-features", "+avx2");
    Clone->addFnAttr("prefer-vector-width", "256");
    Clone->addFnAttr("unsafe-fp-math", "true");
    Clone->addFnAttr("no-nans-fp-math", "true");
    Clone->addFnAttr("no-infs-fp-math", "true");
    break;

  case UNROLL_FASTMATH:
    // Combined: moderate unrolling + fast-math
    Clone->addFnAttr("unroll-count", "4");
    Clone->addFnAttr("unsafe-fp-math", "true");
    Clone->addFnAttr("no-nans-fp-math", "true");
    Clone->addFnAttr("no-infs-fp-math", "true");
    break;

  case FULL_AGGRESSIVE:
    // Kitchen-sink: AVX512 + aggressive unrolling + fast-math
    Clone->addFnAttr("target-features", "+avx512f,+avx512vl");
    Clone->addFnAttr("prefer-vector-width", "512");
    Clone->addFnAttr("unroll-count", "8");
    Clone->addFnAttr("interleave-count", "2");
    Clone->addFnAttr("unsafe-fp-math", "true");
    Clone->addFnAttr("no-nans-fp-math", "true");
    Clone->addFnAttr("no-infs-fp-math", "true");
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

  // Warmup counter
  metadata.warmupCounter = new GlobalVariable(
      M, i32Ty, false, GlobalValue::InternalLinkage, ConstantInt::get(i32Ty, 0),
      "__adaptive_warmup_" + baseName);
  metadata.warmupCounter->setAlignment(Align(4));

  // Paired t-test globals
  for (int v = 0; v < 4; v++) {
    metadata.lastMeasurement[v] = new GlobalVariable(
        M, i64Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i64Ty, 0),
        "__adaptive_lastmeas_" + baseName + "_v" + std::to_string(v));
    metadata.lastMeasurement[v]->setAlignment(Align(8));
  }
  for (int i = 0; i < 3; i++) {
    metadata.pairDiffSum[i] = new GlobalVariable(
        M, i64Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i64Ty, 0),
        "__adaptive_pairdiff_" + baseName + "_" + std::to_string(i + 1));
    metadata.pairDiffSum[i]->setAlignment(Align(8));

    metadata.pairDiffSqSum[i] = new GlobalVariable(
        M, i64Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(i64Ty, 0),
        "__adaptive_pairdiffsq_" + baseName + "_" + std::to_string(i + 1));
    metadata.pairDiffSqSum[i]->setAlignment(Align(8));
  }
  metadata.roundCount = new GlobalVariable(
      M, i32Ty, false, GlobalValue::InternalLinkage, ConstantInt::get(i32Ty, 0),
      "__adaptive_rounds_" + baseName);
  metadata.roundCount->setAlignment(Align(4));
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
  // Declare printf for debugging
  FunctionType *PrintfType = FunctionType::get(
      i32Ty, {PointerType::get(Type::getInt8Ty(Ctx), 0)}, true);
  FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfType);
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

    // --- WARMUP CHECK ---
    // First 8 calls (2 full rounds) run without timing to stabilize icache
    constexpr int WARMUP_ROUNDS = 8;
    LoadInst *LoadWarmup = Builder.CreateLoad(i32Ty, metadata.warmupCounter);
    LoadWarmup->setAtomic(AtomicOrdering::Acquire);
    LoadWarmup->setAlignment(Align(4));
    Value *WarmupDone = Builder.CreateICmpUGE(
        LoadWarmup, ConstantInt::get(i32Ty, WARMUP_ROUNDS));

    BasicBlock *WarmupBB = BasicBlock::Create(Ctx, "warmup_call", Wrapper);
    BasicBlock *TimedBB = BasicBlock::Create(Ctx, "timed_profiling", Wrapper);
    Builder.CreateCondBr(WarmupDone, TimedBB, WarmupBB);

    // --- WARMUP PATH: call version without timing, rotate, return ---
    Builder.SetInsertPoint(WarmupBB);
    {
      Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.warmupCounter,
                              ConstantInt::get(i32Ty, 1), MaybeAlign(),
                              AtomicOrdering::Monotonic);

      // Dispatch to current version
      BasicBlock *WV0BB = BasicBlock::Create(Ctx, "warmup_v0", Wrapper);
      BasicBlock *WV1BB = BasicBlock::Create(Ctx, "warmup_v1", Wrapper);
      BasicBlock *WV2BB = BasicBlock::Create(Ctx, "warmup_v2", Wrapper);
      BasicBlock *WV3BB = BasicBlock::Create(Ctx, "warmup_v3", Wrapper);
      BasicBlock *WarmupRotBB =
          BasicBlock::Create(Ctx, "warmup_rotate", Wrapper);

      SwitchInst *WSwitch = Builder.CreateSwitch(Version, WV0BB, 4);
      WSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 0)), WV0BB);
      WSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 1)), WV1BB);
      WSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 2)), WV2BB);
      WSwitch->addCase(cast<ConstantInt>(ConstantInt::get(i32Ty, 3)), WV3BB);

      PHINode *WarmupResultPhi = nullptr;
      if (!Orig->getReturnType()->isVoidTy()) {
        Builder.SetInsertPoint(WarmupRotBB);
        WarmupResultPhi =
            Builder.CreatePHI(Orig->getReturnType(), 4, "warmup_result");
      }

      BasicBlock *WBBs[] = {WV0BB, WV1BB, WV2BB, WV3BB};
      for (int v = 0; v < 4; v++) {
        Builder.SetInsertPoint(WBBs[v]);
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        if (WarmupResultPhi)
          WarmupResultPhi->addIncoming(CI, WBBs[v]);
        Builder.CreateBr(WarmupRotBB);
      }

      Builder.SetInsertPoint(WarmupRotBB);
      Value *WNext =
          Builder.CreateAdd(Version, ConstantInt::get(i32Ty, 1));
      Value *WNextMod =
          Builder.CreateURem(WNext, ConstantInt::get(i32Ty, 4));
      StoreInst *WStore =
          Builder.CreateStore(WNextMod, metadata.currentVersion);
      WStore->setAtomic(AtomicOrdering::Release);
      WStore->setAlignment(Align(4));
      if (WarmupResultPhi)
        Builder.CreateRet(WarmupResultPhi);
      else
        Builder.CreateRetVoid();
    }

    // --- TIMED PROFILING PATH ---
    Builder.SetInsertPoint(TimedBB);

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
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        Builder.CreateBr(MeasureBB);
      } else {
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        ResultPhi->addIncoming(CI, VBB);
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

        // Store elapsed for paired t-test (per-round measurement)
        Builder.CreateStore(Elapsed, metadata.lastMeasurement[v]);

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

    // --- PAIRED T-TEST: on round completion (version was V3) ---
    Value *WasV3 =
        Builder.CreateICmpEQ(Version, ConstantInt::get(i32Ty, 3));
    BasicBlock *PairedUpdateBB =
        BasicBlock::Create(Ctx, "paired_update", Wrapper);
    BasicBlock *ProfilingRetBB =
        BasicBlock::Create(Ctx, "profiling_ret", Wrapper);
    Builder.CreateCondBr(WasV3, PairedUpdateBB, ProfilingRetBB);

    Builder.SetInsertPoint(PairedUpdateBB);
    {
      // Load all 4 per-round measurements
      Value *LastMeas[4];
      for (int v = 0; v < 4; v++)
        LastMeas[v] = Builder.CreateLoad(i64Ty, metadata.lastMeasurement[v]);

      // Compute paired differences: V_i - V0 for i=1,2,3
      for (int i = 0; i < 3; i++) {
        Value *Diff = Builder.CreateSub(LastMeas[i + 1], LastMeas[0]);
        Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.pairDiffSum[i],
                                Diff, MaybeAlign(), AtomicOrdering::Monotonic);
        Value *DiffSq = Builder.CreateMul(Diff, Diff);
        Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.pairDiffSqSum[i],
                                DiffSq, MaybeAlign(),
                                AtomicOrdering::Monotonic);
      }

      // Increment round count
      Builder.CreateAtomicRMW(AtomicRMWInst::Add, metadata.roundCount,
                              ConstantInt::get(i32Ty, 1), MaybeAlign(),
                              AtomicOrdering::Monotonic);
      Builder.CreateBr(ProfilingRetBB);
    }

    Builder.SetInsertPoint(ProfilingRetBB);
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

#if ADAPTIVE_DEBUG_PRINTS
        Value *FinalAvg = Mean[v];
        Value *FinalStdErr = StdErr_arr[v];
        Value *FinalCV = CV[v];
        Value *AvgForDisplay = Builder.CreateFPToUI(FinalAvg, i64Ty);
        Value *StdErrForDisplay = Builder.CreateFPToUI(FinalStdErr, i64Ty);
        Value *CVPercent = Builder.CreateFMul(
            FinalCV, ConstantFP::get(Builder.getDoubleTy(), 100.0));
        Value *CVForDisplay = Builder.CreateFPToUI(CVPercent, i32Ty);
        Value *StatsMsg =
            Builder.CreateGlobalStringPtr("STATS %s - V%d: total_cycles=%llu, "
                                          "runs=%u, avg=%llu, stderr=%llu, "
                                          "cv=%u%%\n");
        Builder.CreateCall(
            Printf, {StatsMsg, FuncName, ConstantInt::get(i32Ty, v), Cycles[v],
                     Runs[v], AvgForDisplay, StdErrForDisplay, CVForDisplay});
#else
        Value *AvgForDisplay = Builder.CreateFPToUI(Mean[v], i64Ty);
        Value *StatsMsg = Builder.CreateGlobalStringPtr(
            "STATS %s - V%d: total_cycles=%llu, runs=%u, avg=%llu\n");
        Builder.CreateCall(Printf,
                           {StatsMsg, FuncName, ConstantInt::get(i32Ty, v),
                            Cycles[v], Runs[v], AvgForDisplay});
#endif
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
            ConstantFP::get(Builder.getDoubleTy(), 0.10);
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
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        Builder.CreateBr(ReturnBB);
      } else {
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        FinalResult->addIncoming(CI, BB);
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
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        Builder.CreateBr(OptReturnBB);
      } else {
        CallInst *CI =
            Builder.CreateCall(metadata.versions[v]->getFunctionType(),
                               metadata.versions[v], Args);
        CI->setCallingConv(Orig->getCallingConv());
        CI->setAttributes(Orig->getAttributes());
        OptResult->addIncoming(CI, BB);
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
      CallInst *CI = Builder.CreateCall(WrapperType, FuncPtr, Args);
      CI->setCallingConv(Orig->getCallingConv());
      CI->setAttributes(Orig->getAttributes());
      Builder.CreateRetVoid();
    } else {
      CallInst *CI = Builder.CreateCall(WrapperType, FuncPtr, Args);
      CI->setCallingConv(Orig->getCallingConv());
      CI->setAttributes(Orig->getAttributes());
      Builder.CreateRet(CI);
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
    CallInst *CI = Builder.CreateCall(metadata.wrapper->getFunctionType(),
                                      metadata.wrapper, Args);
    CI->setCallingConv(Orig->getCallingConv());
    CI->setAttributes(Orig->getAttributes());
    Builder.CreateRetVoid();
  } else {
    CallInst *CI = Builder.CreateCall(metadata.wrapper->getFunctionType(),
                                      metadata.wrapper, Args);
    CI->setCallingConv(Orig->getCallingConv());
    CI->setAttributes(Orig->getAttributes());
    Builder.CreateRet(CI);
  }

  return Dispatcher;
}

// Process Adaptive Function
void SimpleAdaptivePassImpl::processAdaptiveFunction(
    Function *F, Module &M,
    const SmallVector<OptStrategy, 4> &strategies) {
  FunctionMetadata metadata;
  metadata.original = F;
  std::string originalName = F->getName().str();
  auto originalLinkage = F->getLinkage();
  auto originalVisibility = F->getVisibility();
  auto originalUnnamedAddr = F->getUnnamedAddr();
  bool originalDSOLocal = F->isDSOLocal();
  Comdat *originalComdat = F->hasComdat() ? F->getComdat() : nullptr;

  // Skip variadic functions as the current wrapper logic does not support them
  if (F->isVarArg()) {
    errs() << "[ADAPTIVE] Skipping variadic function: " << originalName << "\n";
    F->removeFnAttr("adaptive");
    return;
  }

  // RENAME ORIGINAL FIRST to avoid collision with dispatcher
  F->setName(originalName + "_orig");
  metadata.profileKey = buildProfileKey(M, F->getName());

  // Create 4 optimized versions using selected strategies
  for (int i = 0; i < 4; i++) {
    OptStrategy strat = (i < (int)strategies.size()) ? strategies[i] : BASELINE;
    metadata.versions[i] = createVersion(F, i, strat, M);
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

    // DCE: Erase unused variant clones and dead original to reduce binary size
    for (int i = 0; i < 4; i++) {
      if (i != selectedVersion && metadata.versions[i]) {
        metadata.versions[i]->replaceAllUsesWith(
            UndefValue::get(metadata.versions[i]->getType()));
        metadata.versions[i]->eraseFromParent();
      }
    }
    F->replaceAllUsesWith(UndefValue::get(F->getType()));
    F->eraseFromParent();
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
    // best_version, pair_diff[3], pair_diff_sq[3], round_count
    SmallVector<Type *, 24> RegParams;
    RegParams.push_back(i8PtrTy);  // name
    RegParams.push_back(i32PtrTy); // complete_flag
    for (int i = 0; i < 4; i++)
      RegParams.push_back(i64PtrTy); // cycles[4]
    for (int i = 0; i < 4; i++)
      RegParams.push_back(i64PtrTy); // cycles_sq[4]
    for (int i = 0; i < 4; i++)
      RegParams.push_back(i32PtrTy); // runs[4]
    RegParams.push_back(i32PtrTy);   // best_version
    for (int i = 0; i < 3; i++)
      RegParams.push_back(i64PtrTy); // pair_diff_sum[3]
    for (int i = 0; i < 3; i++)
      RegParams.push_back(i64PtrTy); // pair_diff_sq_sum[3]
    RegParams.push_back(i32PtrTy);   // round_count

    FunctionType *RegType =
        FunctionType::get(Type::getVoidTy(Ctx), RegParams, false);
    FunctionCallee RegFunc =
        M.getOrInsertFunction("__adaptive_register_function", RegType);

    for (auto &pair : functionMap) {
      FunctionMetadata &metadata = pair.second;
      if (metadata.profilingComplete) { // Only profiling mode has this
        Value *FuncName = Builder.CreateGlobalStringPtr(metadata.profileKey);

        SmallVector<Value *, 24> RegArgs;
        RegArgs.push_back(FuncName);
        RegArgs.push_back(metadata.profilingComplete);
        for (int i = 0; i < 4; i++)
          RegArgs.push_back(metadata.cpuCycles[i]);
        for (int i = 0; i < 4; i++)
          RegArgs.push_back(metadata.cpuCyclesSquared[i]);
        for (int i = 0; i < 4; i++)
          RegArgs.push_back(metadata.runCount[i]);
        RegArgs.push_back(metadata.bestVersion);
        for (int i = 0; i < 3; i++)
          RegArgs.push_back(metadata.pairDiffSum[i]);
        for (int i = 0; i < 3; i++)
          RegArgs.push_back(metadata.pairDiffSqSum[i]);
        RegArgs.push_back(metadata.roundCount);

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
