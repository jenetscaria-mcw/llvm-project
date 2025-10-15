#ifndef LLVM_TRANSFORMS_IPO_SIMPLEADAPTIVE_H
#define LLVM_TRANSFORMS_IPO_SIMPLEADAPTIVE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class SimpleAdaptivePass : public PassInfoMixin<SimpleAdaptivePass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif