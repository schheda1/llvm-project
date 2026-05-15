#include "llvm/Transforms/Utils/UnrollAndUnmergeUtils.h"

using namespace llvm;

#define DEBUG_TYPE "uu"

std::string llvm::getSimpleNodeLabel(const BasicBlock *Node) {
  if (!Node->getName().empty())
    return Node->getName().str();

  std::string Str;
  raw_string_ostream OS(Str);

  Node->printAsOperand(OS, false);
  return OS.str();
}

long long llvm::getNumPaths(Loop &L, LoopInfo &LI) {
  BasicBlock *header = L.getHeader();
  BasicBlock *latch = L.getLoopLatch();
  long long paths = -1;
  if (header && latch) {
    std::unordered_map<BasicBlock *, long long> cache;
    paths = getNumberOfPaths(header, latch, cache, L, LI);
  }
  return paths;
}

long long
llvm::getNumberOfPaths(BasicBlock *start, BasicBlock *end,
                       std::unordered_map<BasicBlock *, long long> &cache,
                       Loop &L, LoopInfo &LI) {
  if (cache.find(start) != cache.end()) {
    return cache[start];
  }
  if (start == end) {
    return 1;
  }

  long long paths = 0;
  if (LI.getLoopFor(start)->getHeader() != L.getHeader()) {
    Loop *innerLoop = LI.getLoopFor(start);
    SmallVector<BasicBlock *, 8> exitBlocks;
    innerLoop->getExitBlocks(exitBlocks);
    for (BasicBlock *exitBlock : exitBlocks) {
      if (L.contains(exitBlock)) {
        paths += getNumberOfPaths(exitBlock, end, cache, L, LI);
      }
    }
  } else {
    for (BasicBlock *succ : successors(start)) {
      if (L.contains(succ)) {
        paths += getNumberOfPaths(succ, end, cache, L, LI);
      }
    }
  }
  cache[start] = paths;
  return paths;
}

bool llvm::containsConvergent(Function *F) {
  if (!F) {
    return true;
  }
  if (F->isDeclaration()) {
    return true;
  }
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (CI->isConvergent()) {
          Function *calledFunction = CI->getCalledFunction();
          if (calledFunction) {
            bool calledFunctionConvergent = containsConvergent(calledFunction);
            if (calledFunctionConvergent) {
              return true;
            }
          } else {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool llvm::containsConvergent(Loop &L) {
  for (BasicBlock *BB : L.blocks()) {
    for (Instruction &I : *BB) {
      if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (CI->isConvergent()) {
          bool calledFunctionConvergent =
              containsConvergent(CI->getCalledFunction());
          if (calledFunctionConvergent) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool llvm::loopContainsBranch(Loop &L) {
  bool containsBranch = false;
  for (BasicBlock *BB : L.blocks()) {
    // check if it has more than one predecessor
    unsigned numPredecessors = std::distance(pred_begin(BB), pred_end(BB));
    if (numPredecessors > 1 && L.getHeader() != BB) {
      containsBranch = true;
      break;
    }
  }
  return containsBranch;
}

CodeMetrics llvm::getLoopMetrics(Loop &L, AssumptionCache &AC,
                                 TargetTransformInfo &TTI) {
  SmallPtrSet<const Value *, 32> EphValues;
  CodeMetrics::collectEphemeralValues(&L, &AC, EphValues);
  CodeMetrics Metrics;
  for (BasicBlock *BB : L.blocks())
    Metrics.analyzeBasicBlock(BB, TTI, EphValues);
  return Metrics;
}

long long llvm::getEstimatedLoopSize(Loop &L, InstructionCost loopSize,
                                     int unrollFactor, long long numPaths) {
  if (numPaths == -1) {
    return -1;
  }
  long long estimatedLoopSize = 0;
  long long loopSizeLL = loopSize.getValue();
  // TODO: stop early if estimatedLoopSize > some threshold
  for (int i = 0; i < unrollFactor; i++) {
    estimatedLoopSize += pow(numPaths, i) * loopSizeLL;
  }
  return estimatedLoopSize;
}

int llvm::getHighestUnrollFactor(Loop &L, InstructionCost loopSize,
                                 long long numPaths, bool usePow) {
  if (usePow) {
    return getHighestUnrollFactorPow2(L, loopSize, numPaths);
  } else {
    return getHighestUnrollFactor(L, loopSize, numPaths);
  }
}

int llvm::getHighestUnrollFactor(Loop &L, InstructionCost loopSize,
                                 long long numPaths) {
  if (numPaths == -1)
    return 1;
  int maxUnrollFactor = 8;
  for (int i = 2; i <= maxUnrollFactor; i++) {
    long long estimatedLoopSize =
        getEstimatedLoopSize(L, loopSize, i, numPaths);
    if (estimatedLoopSize >= 1024) {
      return i - 1;
    }
  }
  return maxUnrollFactor;
}

int llvm::getHighestUnrollFactorPow2(Loop &L, InstructionCost loopSize,
                                     long long numPaths) {
  // only tries 2, 4 and 8 as unroll factors
  if (numPaths == -1)
    return 1;
  int maxUnrollFactor = 3;
  for (int i = 1; i <= maxUnrollFactor; i++) {
    int unrollFactor = pow(2, i) - 1;
    long long estimatedLoopSize =
        getEstimatedLoopSize(L, loopSize, unrollFactor, numPaths);
    if (estimatedLoopSize >= 1024) {
      return pow(2, i - 1);
    }
  }
  return pow(2, maxUnrollFactor);
}

int llvm::getUnrollFactor(Loop &L, LoopInfo &LI, TargetTransformInfo &TTI,
                          AssumptionCache &AC, bool usePow) {
  CodeMetrics metrics = getLoopMetrics(L, AC, TTI);
  InstructionCost loopSize = metrics.NumInsts;
  long long numPaths = getNumPaths(L, LI);
  int highestUnrollFactor =
      getHighestUnrollFactor(L, loopSize, numPaths, usePow);
  assert(highestUnrollFactor >= 1 && "highest unroll factor < 1");
  assert(highestUnrollFactor <= 8 && "highest unroll factor > 8");
  return highestUnrollFactor;
}
