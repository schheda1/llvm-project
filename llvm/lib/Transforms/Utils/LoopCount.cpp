#include "llvm/Transforms/Utils/LoopCount.h"
#include "llvm/ADT/PriorityWorklist.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include <cmath>

using namespace llvm;

int LoopCountPass::seenLoops = 0;
int LoopCountFunctionPass::seenLoopsFunction = 0;

LoopCountPass::LoopCountPass() {}

void printDebucLoc(const DebugLoc &debugLoc) {
  errs() << debugLoc.getLine() << ";";
  errs() << debugLoc.getCol() << ";";
  errs() << debugLoc.isImplicitCode();
  errs() << ";";
}

void printConvergent(Loop &L) { errs() << containsConvergent(L) << ";"; }

void printMetrics(Loop &L, AssumptionCache &AC, TargetTransformInfo &TTI) {
  CodeMetrics Metrics = getLoopMetrics(L, AC, TTI);
  errs() << !Metrics.notDuplicatable << ";";
  errs() << Metrics.NumInsts << ";";
  errs() << Metrics.NumInsts.isValid() << ";";
}

void printContainsPhi(Loop &L) {
  bool containsPHI = false;
  for (BasicBlock *BB : L.blocks()) {
    for (Instruction &I : *BB) {
      if (isa<PHINode>(I)) {
        containsPHI = true;
        break;
      }
    }
  }
  errs() << containsPHI << ";";
}

void printExitBlockContainsPhi(Loop &L) {
  bool exitBlocksContainPHI = false;
  SmallVector<BasicBlock *, 8> exitBlocks;
  L.getExitBlocks(exitBlocks);
  for (BasicBlock *BB : exitBlocks) {
    if (exitBlocksContainPHI) {
      break;
    }
    for (Instruction &I : *BB) {
      if (isa<PHINode>(I)) {
        exitBlocksContainPHI = true;
        break;
      }
    }
  }
  errs() << exitBlocksContainPHI << ";";
}

void printContainsUseOutsideLoop(Loop &L) {
  bool containsUseOutsideLoop = false;
  for (BasicBlock *BB : L.blocks()) {
    if (containsUseOutsideLoop) {
      break;
    }
    for (Instruction &I : *BB) {
      for (User *U : I.users()) {
        if (Instruction *UI = dyn_cast<Instruction>(U)) {
          if (!L.contains(UI)) {
            containsUseOutsideLoop = true;
            break;
          }
        }
      }
    }
  }

  errs() << containsUseOutsideLoop << ";";
}

void printContainsBarrier(Loop &L) {
  printConvergent(L);
}

void printContainsBranch(Loop &L) {
  errs() << loopContainsBranch(L);
}

void printContainsSubloops(Loop &L) {
  errs() << !L.isInnermost() << ";";
}

void printColumnHeader(int seenLoops, Module *M) {
  if (seenLoops == 0) {
    errs() << "LOOPCOUNT METADATA;" << M->getSourceFileName() << ";";
    errs() << M->getTargetTriple().str();
    errs() << "\n";
    errs() << "LOOPCOUNT::loopIdx;"
           << "loopDepth;"
           << "startLine;"
           << "startCol;"
           << "startIsImplicitCode;"
           << "endLine;"
           << "endCol;"
           << "endIsImplicitCode;"
           << "function;"
           << "numPaths;"
           << "duplicatable;"
           << "loopSize;"
           << "sizeIsValid;"
           << "containsPHI;"
           << "exitBlocksContainPHI;"
           << "containsUseOutsideLoop;"
           << "containsBarrier;"
           << "containsChildLoops;"
           << "containsBranch\n";
  }
}

void printLinesOfCode(Loop &L) {
  Loop::LocRange range = L.getLocRange();
  if (range) {
    DebugLoc startLoc = range.getStart();
    printDebucLoc(startLoc);
    DebugLoc endLoc = range.getEnd();
    printDebucLoc(endLoc);
  } else {
    errs() << "-1;-1;-1;-1;-1;-1;";
  }
}

void printNumberOfPaths(Loop &L, LoopInfo &LI) {
  errs() << getNumPaths(L, LI) << ";";
}

static void printLoopData(Loop &L, Module *M, Function *F, AssumptionCache &AC,
                          TargetTransformInfo &TTI, int &seenLoops,
                          LoopInfo &LI, ScalarEvolution &SE) {
  printColumnHeader(seenLoops, M);
  errs() << "LOOPCOUNT::";
  errs() << seenLoops++ << ";";
  errs() << L.getLoopDepth() << ";";
  printLinesOfCode(L);
  errs() << F->getName() << ";";
  printNumberOfPaths(L, LI);
  printMetrics(L, AC, TTI);
  printContainsPhi(L);
  printExitBlockContainsPhi(L);
  printContainsUseOutsideLoop(L);
  printContainsBarrier(L);
  printContainsSubloops(L);
  printContainsBranch(L);
  errs() << "\n";
}

PreservedAnalyses LoopCountPass::run(Loop &L, LoopAnalysisManager &AM,
                                     LoopStandardAnalysisResults &AR,
                                     LPMUpdater &U) {
  Function *F = L.getHeader()->getParent();
  Module *M = F->getParent();
  printLoopData(L, M, F, AR.AC, AR.TTI, seenLoops, AR.LI, AR.SE);
  return PreservedAnalyses::all();
}

PreservedAnalyses LoopCountFunctionPass::run(Function &F,
                                             FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &AC = FAM.getResult<AssumptionAnalysis>(F);
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);

  bool changed = false;

  for (const auto &L : LI) {
    // need loop simplify and lcssa form for unroll and unmerge pass
    changed |=
        simplifyLoop(L, &DT, &LI, &SE, &AC, nullptr, false /* PreserveLCSSA */);
    changed |= formLCSSARecursively(*L, DT, &LI, &SE);
  }

  SmallPriorityWorklist<Loop *, 4> Worklist;
  appendLoopsToWorklist(LI, Worklist);

  while (!Worklist.empty()) {
    Loop &L = *Worklist.pop_back_val();
    printLoopData(L, F.getParent(), &F, AC, TTI, seenLoopsFunction, LI, SE);
  }

  if (changed) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
