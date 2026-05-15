#ifndef LLVM_TRANSFORMS_UTILS_UNROLLANDUNMERGE_H
#define LLVM_TRANSFORMS_UTILS_UNROLLANDUNMERGE_H

#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/UnrollAndUnmergeUtils.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {

class Loop;
class LPMUpdater;

using NewLoopsMap = SmallDenseMap<const Loop *, Loop *, 4>;

class PathInformation {
private:
  // Maps basic blocks to their cloned basic blocks. We actually only need this
  // for nested loops.
  std::unordered_map<BasicBlock *, BasicBlock *> *bb2cloned_bb;
  ValueToValueMapTy *vmap;

public:
  int numHeaderSeen;
  
  ~PathInformation();
  PathInformation();
  PathInformation(const PathInformation &pathInfo);
  int getCount(BasicBlock *cutpoint);
  void increaseCount(BasicBlock *cutpoint);
  void decreaseCount(BasicBlock *cutpoint);
  bool isCutpoint(BasicBlock *bb);
  ValueToValueMapTy *getVmap();
  std::unordered_map<BasicBlock *, BasicBlock *> *getBB2ClonedBB();
  void setClonedBB(BasicBlock *bb, BasicBlock *cloned_bb);
  BasicBlock *getClonedBB(BasicBlock *bb);
  bool containsClonedBB(BasicBlock *bb);
};

class UnrollAndUnmergePass : public PassInfoMixin<UnrollAndUnmergePass> {
public:
  UnrollAndUnmergePass();
  PreservedAnalyses run(Loop &L, LoopAnalysisManager &AM,
                        LoopStandardAnalysisResults &AR, LPMUpdater &U);

  static bool isRequired() { return false; }
};

class UnrollAndUnmergeFunctionPass : public PassInfoMixin<UnrollAndUnmergeFunctionPass> {
public:
  UnrollAndUnmergeFunctionPass();
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return false; }
};

class UnrollAndUnmergeHeuristic
    : public PassInfoMixin<UnrollAndUnmergeHeuristic> {
private:
  static int seenLoopsHeuristic;

public:
  UnrollAndUnmergeHeuristic() {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return false; }
};

Pass *createUnrollAndUnmergePass();

} // namespace llvm

#endif /* LLVM_TRANSFORMS_UTILS_UNROLLANDUNMERGE_H */