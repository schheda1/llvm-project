#ifndef LLVM_TRANSFORMS_UTILS_UNROLLANDUNMERGEUTILS_H
#define LLVM_TRANSFORMS_UTILS_UNROLLANDUNMERGEUTILS_H

#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace llvm {

std::string getSimpleNodeLabel(const BasicBlock *Node);

long long getNumberOfPaths(BasicBlock *start, BasicBlock *end,
                           std::unordered_map<BasicBlock *, long long> &cache,
                           Loop &L, LoopInfo &LI);

long long getNumPaths(Loop &L, LoopInfo &LI);

long long getNumberOfPaths(BasicBlock *start, BasicBlock *end,
                           std::unordered_map<BasicBlock *, long long> &cache,
                           Loop &L, LoopInfo &LI);

bool containsConvergent(Function *F);

bool containsConvergent(Loop &L);

bool loopContainsBranch(Loop &L);

CodeMetrics getLoopMetrics(Loop &L, AssumptionCache &AC,
                           TargetTransformInfo &TTI);

long long getEstimatedLoopSize(Loop &L, InstructionCost loopSize,
                               int unrollFactor, long long numPaths);

int getHighestUnrollFactor(Loop &L, InstructionCost loopSize,
                           long long numPaths, bool usePow);

int getHighestUnrollFactor(Loop &L, InstructionCost loopSize,
                           long long numPaths);

int getHighestUnrollFactorPow2(Loop &L, InstructionCost loopSize,
                               long long numPaths);

int getUnrollFactor(Loop &L, LoopInfo &LI, TargetTransformInfo &TTI,
                    AssumptionCache &AC, bool usePow);

void getTripCount(unsigned &TripCount, unsigned &TripMultiple, Loop *L,
                  ScalarEvolution &SE);

} // namespace llvm

#endif /* LLVM_TRANSFORMS_UTILS_UNROLLANDUNMERGEUTILS_H */