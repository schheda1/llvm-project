#ifndef LLVM_TRANSFORMS_UTILS_LOOPCOUNT_H
#define LLVM_TRANSFORMS_UTILS_LOOPCOUNT_H

#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/UnrollAndUnmergeUtils.h"

namespace llvm {

class Loop;
class LPMUpdater;

class LoopCountPass : public PassInfoMixin<LoopCountPass> {
private:
  static int seenLoops;

public:
  LoopCountPass();
  PreservedAnalyses run(Loop &L, LoopAnalysisManager &AM,
                        LoopStandardAnalysisResults &AR, LPMUpdater &U);

  static bool isRequired() { return true; }
};

class LoopCountFunctionPass : public PassInfoMixin<LoopCountFunctionPass> {
private:
  static int seenLoopsFunction;

public:
  LoopCountFunctionPass() {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif /* LLVM_TRANSFORMS_UTILS_LOOPCOUNT_H */