#include "llvm/Transforms/Utils/UnrollAndUnmerge.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "uu"

static int seenLoops = 0;
int UnrollAndUnmergeHeuristic::seenLoopsHeuristic = 0;

static cl::opt<unsigned>
    uuUnrollFactor("uu-unrollfactor", cl::init(2),
                         cl::desc("Set the unrolling count."));

static cl::opt<std::string> matchesTargetTriple(
    "uu-match-targettriple", cl::init("-"),
    cl::desc("If this is set, then this will only be applied if the Module's "
             "target triple is equal to matchesTargetTriple"),
    cl::CommaSeparated);

static cl::opt<std::string> matchesFilename(
    "uu-match-filename", cl::init("-"),
    cl::desc("If this is set, then this pass will only be applied if the "
             "current file is equal to matchesFilename."),
    cl::CommaSeparated);

static cl::list<int> optLoopIdx(
    "uu-opt-loop-idx",
    cl::desc(
        "If opt-loop-idx is set, only loops with index in optLoopIdx will be "
        "optimized and all other loops will remain unchanged."),
    cl::CommaSeparated);

struct UnmergeInfo {
  BasicBlock *BB;
  BasicBlock *startCutpoint;
  Function &F;
  PathInformation *pathInfo;
  NewLoopsMap &newLoopsMap;
  LoopInfo &LI;
  Loop &L;
  DominatorTree &DT;
  std::unordered_map<BasicBlock *, BasicBlock *> &clonedBB2BB;
  BasicBlock *firstClonedHeader;
  int unrollFactor;
};

PathInformation::PathInformation() {
  vmap = new ValueToValueMapTy();
  bb2cloned_bb = new std::unordered_map<BasicBlock *, BasicBlock *>();
  numHeaderSeen = 0;
}

// copy constructor
PathInformation::PathInformation(const PathInformation &pathInfo) {
  numHeaderSeen = pathInfo.numHeaderSeen;
  vmap = new ValueToValueMapTy();
  ValueToValueMapTy *pathInfoVmap = pathInfo.vmap;
  for (auto it = pathInfoVmap->begin(), it_end = pathInfoVmap->end();
       it != it_end; ++it) {
    auto pair = *it;
    if (pair.second) {
      (*vmap)[pair.first] = pair.second;
    }
  }
  bb2cloned_bb = new std::unordered_map<BasicBlock *, BasicBlock *>();
  for (auto it = pathInfo.bb2cloned_bb->begin(),
            it_end = pathInfo.bb2cloned_bb->end();
       it != it_end; ++it) {
    auto pair = *it;
    (*bb2cloned_bb)[pair.first] = pair.second;
  }
}

PathInformation::~PathInformation() {
  delete vmap;
  delete bb2cloned_bb;
}

std::unordered_map<BasicBlock *, BasicBlock *> *
PathInformation::getBB2ClonedBB() {
  return bb2cloned_bb;
}

void PathInformation::setClonedBB(BasicBlock *bb, BasicBlock *cloned_bb) {
  (*bb2cloned_bb)[bb] = cloned_bb;
}

BasicBlock *PathInformation::getClonedBB(BasicBlock *bb) {
  return (*bb2cloned_bb)[bb];
}

bool PathInformation::containsClonedBB(BasicBlock *bb) {
  return bb2cloned_bb->find(bb) != bb2cloned_bb->end();
}

ValueToValueMapTy *PathInformation::getVmap() { return vmap; }

static const Loop *addClonedBlockToLoopInfo(UnmergeInfo &unmergeInfo,
                                            BasicBlock *ClonedBB) {
  BasicBlock *OriginalBB = unmergeInfo.BB;
  LoopInfo *LI = &unmergeInfo.LI;
  NewLoopsMap &NewLoops = unmergeInfo.newLoopsMap;
  Loop *OldLoop = &unmergeInfo.L;

  // Figure out which loop New is in.
  // const Loop *OldLoop = LI->getLoopFor(OriginalBB);
  assert(OldLoop && "Should (at least) be in the loop being unrolled!");
  Loop *&NewLoop = NewLoops[OldLoop];
  if (!NewLoop) {
    // Found a new sub-loop.
    assert(OriginalBB == OldLoop->getHeader() &&
           "Header should be first in RPO");

    NewLoop = LI->AllocateLoop();
    Loop *NewLoopParent = NewLoops.lookup(OldLoop->getParentLoop());

    if (NewLoopParent) {
      NewLoopParent->addChildLoop(NewLoop);
    } else {
      LI->addTopLevelLoop(NewLoop);
    }

    NewLoop->addBasicBlockToLoop(ClonedBB, *LI);
    return OldLoop;
  } else {
    NewLoop->addBasicBlockToLoop(ClonedBB, *LI);
    return nullptr;
  }
}

/// Creates a copy of a BasicBlock.
BasicBlock *duplicateBB(UnmergeInfo &unmergeInfo) {
  PathInformation &pathInfo = *unmergeInfo.pathInfo;
  BasicBlock *clonedBB = CloneBasicBlock(unmergeInfo.BB, *pathInfo.getVmap(),
                                         ".unrolled", &unmergeInfo.F);

  // CloneBasicBlock does not remap instructions, so we do it by ourselves
  for (Instruction &instr : *clonedBB) {
    // remap if instr is not a PHINode, we take care of PHINodes later
    if (!isa<PHINode>(instr)) {
      RemapInstruction(&instr, *pathInfo.getVmap(),
                       RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
    }
  }

  pathInfo.setClonedBB(unmergeInfo.BB, clonedBB);
  unmergeInfo.clonedBB2BB[clonedBB] = unmergeInfo.BB;

  return clonedBB;
}

void static addIncomingValuesToPhi(const BasicBlock *BB,
                                   BasicBlock *newSuccessorBB,
                                   PathInformation &pathInfo,
                                   BasicBlock &copyBB) {
  // Add incoming values to PHINodes in newSuccessorBB
  for (PHINode &PHI : newSuccessorBB->phis()) {
    for (unsigned i = 0; i < PHI.getNumIncomingValues(); i++) {
      Value *incomingValue = PHI.getIncomingValue(i);
      BasicBlock *incomingBB = PHI.getIncomingBlock(i);
      if (incomingBB == BB) {
        ValueToValueMapTy::iterator It =
            pathInfo.getVmap()->find(incomingValue);

        // get duplicated value
        if (It != pathInfo.getVmap()->end()) {
          incomingValue = It->second;
        }

        // add incoming value with the duplicated value and duplicated BB
        PHI.addIncoming(incomingValue, &copyBB);
      }
    }
  }
}

static BasicBlock *dfsAndDuplicate(UnmergeInfo &unmergeInfo);

static void handleTerminator(UnmergeInfo &unmergeInfo, BasicBlock &copyBB) {
  Instruction *terminator = copyBB.getTerminator();
  unsigned numSuccessors = terminator->getNumSuccessors();
  PathInformation *pathInfo = unmergeInfo.pathInfo;
  BasicBlock *BB = unmergeInfo.BB;

  for (unsigned i = 0; i < numSuccessors; i++) {
    BasicBlock *succBB = terminator->getSuccessor(i);
    BasicBlock *newSuccessorBB = nullptr;

    // Only need to clone pathInfo if there is more than one successor
    if (numSuccessors == 1) {
      UnmergeInfo unmergeInfoCopy = unmergeInfo;
      unmergeInfoCopy.BB = succBB;
      newSuccessorBB = dfsAndDuplicate(unmergeInfoCopy);
    } else {
      // PathInformation contains a map that maps instructions to their cloned
      // instructions. We need to create a new PathInformation for each
      // successor BB because the instruction map is modified while visiting the
      // successors. I am pretty sure this is expensive and can be done better.

      PathInformation *newPathInfo = new PathInformation(*pathInfo);
      UnmergeInfo unmergeInfoCopy = unmergeInfo;
      unmergeInfoCopy.BB = succBB;
      unmergeInfoCopy.pathInfo = newPathInfo;
      newSuccessorBB = dfsAndDuplicate(unmergeInfoCopy);
    }

    terminator->setSuccessor(i, newSuccessorBB);
    addIncomingValuesToPhi(BB, newSuccessorBB, *pathInfo, copyBB);
  }
}

static BasicBlock *dfsAndDuplicate(UnmergeInfo &unmergeInfo) {
  BasicBlock *BB = unmergeInfo.BB;
  PathInformation &pathInfo = *unmergeInfo.pathInfo;

  // Check if it is a loop header of a child loop that we have already visited.
  // If so, return the corresponding cloned header. This is only relevant for
  // loops that contain child loops.
  Loop *LoopForBB = unmergeInfo.LI.getLoopFor(BB);
  if (LoopForBB) {
    if (LoopForBB != &unmergeInfo.L) {
      if (LoopForBB->getHeader() == BB) {
        if (pathInfo.containsClonedBB(BB)) {
          return pathInfo.getClonedBB(BB);
        }
      }
    }
  }

  bool isExitBlock = !unmergeInfo.L.contains(BB);
  if (isExitBlock) {
    return BB;
  }
  bool isHeader = unmergeInfo.L.getHeader() == BB;
  if (isHeader) {
    if (pathInfo.numHeaderSeen < unmergeInfo.unrollFactor) {
      pathInfo.numHeaderSeen += 1;

      // We are resetting it here because we do not want to reference cloned
      // basic blocks from the previous duplicated loop body.
      pathInfo.getBB2ClonedBB()->clear();
    } else {
      return unmergeInfo.firstClonedHeader;
    }
  }

  BasicBlock *clonedBB = duplicateBB(unmergeInfo);

  // The first cloned header is the header of the main loop
  if (unmergeInfo.firstClonedHeader == nullptr) {
    unmergeInfo.firstClonedHeader = clonedBB;
  }
  addClonedBlockToLoopInfo(unmergeInfo, clonedBB);
  handleTerminator(unmergeInfo, *clonedBB);

  if (isHeader) {
    pathInfo.numHeaderSeen -= 1;
  }
  return clonedBB;
}

static bool checkMatchesTargetTriple(Loop &L) {
  if (matchesTargetTriple != "-") {
    std::string targetTriple =
        L.getHeader()->getParent()->getParent()->getTargetTriple();
    if (matchesTargetTriple != targetTriple) {
      LLVM_DEBUG(dbgs() << "Skipped " << targetTriple
                        << " != " << matchesTargetTriple << " loop "
                        << seenLoops << "\n");
      seenLoops++;
      return false;
    }
  }
  return true;
}

static bool checkMatchesFilename(Loop &L) {
  if (matchesFilename != "-") {
    std::string filename =
        L.getHeader()->getParent()->getParent()->getSourceFileName();
    if (matchesFilename != filename) {
      LLVM_DEBUG(dbgs() << "Skipped " << filename << " != " << matchesFilename
                        << " loop " << seenLoops << "\n");
      seenLoops++;
      return false;
    }
  }
  return true;
}

static bool matchesLoopIdx() {
  if (!optLoopIdx.empty() && std::find(optLoopIdx.begin(), optLoopIdx.end(),
                                       seenLoops) == optLoopIdx.end()) {
    LLVM_DEBUG(dbgs() << "Skipped loop " << seenLoops << "\n");
    seenLoops++;
    return false;
  } else {
    LLVM_DEBUG(dbgs() << "Optimizing loop " << seenLoops << "\n");
    seenLoops++;
    return true;
  }
}

static void rewireEdges(BasicBlock *header, BasicBlock *duplicatedHeader) {
  // All edges that target the original loop header have to be redirected to the
  // duplicated header
  std::vector<BasicBlock *> originalBB_predecessors;
  for (BasicBlock *pred : predecessors(header)) {
    originalBB_predecessors.push_back(pred);
  }
  for (BasicBlock *pred : originalBB_predecessors) {
    Instruction *terminator = pred->getTerminator();
    unsigned numSuccessors = terminator->getNumSuccessors();
    for (unsigned i = 0; i < numSuccessors; i++) {
      if (header == terminator->getSuccessor(i)) {
        terminator->setSuccessor(i, duplicatedHeader);
        LLVM_DEBUG(dbgs() << "Rewired edge " << pred->getName() << " -> "
                          << header->getName() << " to "
                          << duplicatedHeader->getName() << "\n");
      }
    }
  }
}

static void removeDeadBlocksFromLoop(Function *F, Loop &L) {
  df_iterator_default_set<BasicBlock *> Reachable;
  // Mark all reachable blocks.
  for (BasicBlock *BB : depth_first_ext(F, Reachable))
    (void)BB /* Mark all reachable blocks */;

  // Collect all dead blocks.
  std::vector<BasicBlock *> DeadBlocks;
  for (BasicBlock &BB : *F)
    if (!Reachable.count(&BB))
      DeadBlocks.push_back(&BB);

  std::vector<BasicBlock *> DeadLoopBlocks;
  for (auto *BB : L.getBlocks()) {
    bool isDead =
        std::find(DeadBlocks.begin(), DeadBlocks.end(), BB) != DeadBlocks.end();
    if (isDead) {
      DeadLoopBlocks.push_back(BB);
      LLVM_DEBUG(dbgs() << "Loop BB is dead: " << getSimpleNodeLabel(BB)
                        << "\n");
    } else {
      LLVM_DEBUG(dbgs() << "Loop BB is not dead: " << getSimpleNodeLabel(BB)
                        << "\n");
    }
  }

  // Remove incoming values from PHINodes if they come from a dead block
  for (BasicBlock &BB : *F) {
    if (std::find(DeadLoopBlocks.begin(), DeadLoopBlocks.end(), &BB) !=
        DeadLoopBlocks.end()) {
      continue;
    }

    std::vector<BasicBlock *> preds;
    for (BasicBlock *pred : predecessors(&BB)) {
      preds.push_back(pred);
    }

    for (PHINode &PN : BB.phis()) {
      // remove incoming value if it does not come from a predecessor
      std::vector<BasicBlock *> phiBBs;
      for (unsigned i = 0; i < PN.getNumIncomingValues(); i++) {
        phiBBs.push_back(PN.getIncomingBlock(i));
      }
      for (BasicBlock *phiBB : phiBBs) {
        if (std::find(preds.begin(), preds.end(), phiBB) == preds.end()) {
          PN.removeIncomingValue(phiBB);
        }
      }
    }
  }
}

static void simplifySingleIncomingValuePHINodes(
    Function &F, Loop &L, const std::vector<BasicBlock *> &DeadBlocks) {
  for (auto &BB : F) {
    // BBs that are part of the old loop will be deleted, so we will not
    // simplify them
    if (std::find(DeadBlocks.begin(), DeadBlocks.end(), &BB) !=
        DeadBlocks.end()) {
      continue;
    }

    // Remove incomingValue from PHINode if incomingValue comes from a bb that
    // is part of the old loop, that will be deleted.
    for (auto &I : BB) {
      if (PHINode *phiNode = dyn_cast<PHINode>(&I)) {
        for (unsigned i = 0; i < phiNode->getNumIncomingValues(); i++) {
          if (std::find(DeadBlocks.begin(), DeadBlocks.end(),
                        phiNode->getIncomingBlock(i)) != DeadBlocks.end()) {
            LLVM_DEBUG(dbgs() << "Removing loop bb value from phi\n");
            LLVM_DEBUG(dbgs() << "Phi: " << *phiNode << "\n");
            LLVM_DEBUG(dbgs()
                       << "incoming bb: "
                       << phiNode->getIncomingBlock(i)->getName() << "\n");
            LLVM_DEBUG(dbgs() << "incoming value: "
                              << *phiNode->getIncomingValue(i) << "\n");
            phiNode->removeIncomingValue(i);
            i--;
          }
        }
      }
    }
  }
}

static bool skipLoop(Loop &L) {
  if (!checkMatchesTargetTriple(L)) {
    return true;
  }

  if (!checkMatchesFilename(L)) {
    return true;
  }

  if (!matchesLoopIdx()) {
    return true;
  }
  return false;
}

static void postProcessing(Loop &L, Loop *clonedLoop, Function *F, LoopInfo &LI,
                           DominatorTree &DT, ScalarEvolution &SE,
                           AssumptionCache &AC) {
  // Erase the instructions and the blocks without having to worry
  // about ordering because we already dropped the references.
  // NOTE: This iteration is safe because erasing the block does not remove
  // its entry from the loop's block list.  We do that in the next section.
  df_iterator_default_set<BasicBlock *> Reachable;
  // Mark all reachable blocks.
  for (BasicBlock *BB : depth_first_ext(F, Reachable))
    (void)BB /* Mark all reachable blocks */;

  // Collect all dead blocks.
  std::vector<BasicBlock *> DeadBlocks;
  for (BasicBlock &BB : *F)
    if (!Reachable.count(&BB))
      DeadBlocks.push_back(&BB);

  for (BasicBlock *BB : DeadBlocks) {
    LLVM_DEBUG(dbgs() << "Dead BB: drop all references: "
                      << getSimpleNodeLabel(BB) << "\n");
    BB->dropAllReferences();
  }
  simplifySingleIncomingValuePHINodes(*F, L, DeadBlocks);
  for (BasicBlock *BB : DeadBlocks) {
    LLVM_DEBUG(dbgs() << "Dead BB: Erase block from parent: "
                      << getSimpleNodeLabel(BB) << "\n");
    BB->eraseFromParent();
  }

  // Finally, the blocks from loopinfo.  This has to happen late because
  // otherwise our loop iterators won't work.

  for (BasicBlock *BB : DeadBlocks) {
    // removes BB from loop and all loops it is nested in
    LLVM_DEBUG(dbgs() << "Dead BB: Remove dead block from loopinfo: "
                      << BB->getName() << "\n");
    LI.removeBlock(BB);
  }

  std::vector<BasicBlock *> aliveLoopBBs;
  for (BasicBlock *BB : L.blocks()) {
    // we have already removed all dead BBs from L, so it should contain only
    // alive BBs
    assert(std::find(DeadBlocks.begin(), DeadBlocks.end(), BB) ==
               DeadBlocks.end() &&
           "Loop contains dead blocks");
    aliveLoopBBs.push_back(BB);
  }

  for (BasicBlock *aliveBB : aliveLoopBBs) {
    // remove alive BB from original loop
    LI.removeBlock(aliveBB);

    // add alive block to the cloned loop
    clonedLoop->addBasicBlockToLoop(aliveBB, LI);
  }

  // The last step is to update LoopInfo now that we've eliminated this loop.
  // Note: LoopInfo::erase remove the given loop and relink its subloops with
  // its parent. While removeLoop/removeChildLoop remove the given loop but
  // not relink its subloops, which is what we want.
  if (Loop *ParentLoop = L.getParentLoop()) {
    Loop::iterator I = find(*ParentLoop, &L);
    assert(I != ParentLoop->end() && "Couldn't find loop");
    ParentLoop->removeChildLoop(I);
  } else {
    Loop::iterator I = find(LI, &L);
    assert(I != LI.end() && "Couldn't find loop");
    LI.removeLoop(I);
  }

  LI.destroy(&L);
  DT.recalculate(*F);
}

static Loop *addClonedLoopToLoopInfo(Loop *L, Loop *PL, ValueToValueMapTy &VM,
                                     LoopInfo *LI) {
  Loop &New = *LI->AllocateLoop();
  if (PL)
    PL->addChildLoop(&New);
  else
    LI->addTopLevelLoop(&New);

  // if (LPM)
  //   LPM->addLoop(New);

  // Add all of the blocks in L to the new loop.
  for (BasicBlock *BB : L->blocks())
    if (LI->getLoopFor(BB) == L)
      New.addBasicBlockToLoop(cast<BasicBlock>(VM[BB]), *LI);

  // Add all of the subloops to the new loop.
  for (Loop *I : *L)
    addClonedLoopToLoopInfo(I, &New, VM, LI);

  return &New;
}

static std::pair<bool, Loop *>
unmerge(Loop &L, LoopInfo &LI, DominatorTree &DT, ScalarEvolution &SE,
        TargetTransformInfo &TTI, AssumptionCache &AC, LPMUpdater *U,
        bool checkSkip = true, int unrollFactor = -1) {
  if (unrollFactor == -1) {
    unrollFactor = uuUnrollFactor;
  }

  if (checkSkip && skipLoop(L)) {
    return std::make_pair(false, nullptr);
  }

  assert(L.isLoopSimplifyForm() && "Expected loop to already be in simplify "
                                   "form when starting unmerge pass.");
  assert(
      L.isLCSSAForm(DT) &&
      "Expected loop to already be in LCSSA form when starting unmerge pass.");

  std::unordered_map<BasicBlock *, BasicBlock *> clonedBB2BB;

  LLVM_DEBUG(dbgs() << "=== Start Unroll & Unmerge Pass ===\n");
  BasicBlock *header = L.getHeader();
  Function *F = header->getParent();

  std::string loopName = std::string(L.getName());
  if (U) {
    U->markLoopAsDeleted(L, loopName);
  }

  BasicBlock *startCutpoint = header;
  PathInformation *pathInfo = new PathInformation();

  Loop *ParentLoop = L.getParentLoop();
  NewLoopsMap newLoopsMap;
  if (ParentLoop) {
    newLoopsMap[ParentLoop] = ParentLoop;
  }

  UnmergeInfo unmergeInfo{
      header, startCutpoint, *F,      pathInfo,        newLoopsMap, LI, L,
      DT,     clonedBB2BB,   nullptr, unrollFactor};

  BasicBlock *duplicatedHeader = dfsAndDuplicate(unmergeInfo);

  Loop *uuLoop = newLoopsMap[&L];
  rewireEdges(header, duplicatedHeader);
  removeDeadBlocksFromLoop(F, L);
  postProcessing(L, uuLoop, F, LI, DT, SE, AC);

  return std::make_pair(true, uuLoop);
}

namespace {
class UnrollAndUnmerge : public LoopPass {
public:
  static char ID; // Pass ID, replacement for typeid

  UnrollAndUnmerge() : LoopPass(ID) {}

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto *SEWP = getAnalysisIfAvailable<ScalarEvolutionWrapperPass>();
    auto TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(
        *L->getHeader()->getParent());
    auto AC = &getAnalysis<AssumptionCacheTracker>().getAssumptionCache(
        *L->getHeader()->getParent());
    auto SE = SEWP ? &SEWP->getSE() : nullptr;
    return unmerge(*L, *LI, *DT, *SE, *TTI, *AC, nullptr).first;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    getLoopAnalysisUsage(AU);
  }
};
} // namespace

char UnrollAndUnmerge::ID = 0;

UnrollAndUnmergePass::UnrollAndUnmergePass() {}

PreservedAnalyses UnrollAndUnmergePass::run(Loop &L, LoopAnalysisManager &AM,
                                     LoopStandardAnalysisResults &AR,
                                     LPMUpdater &U) {
  bool changed = unmerge(L, AR.LI, AR.DT, AR.SE, AR.TTI, AR.AC, &U).first;
  if (!changed) {
    return PreservedAnalyses::all();
  }
  return PreservedAnalyses::none();
}

static RegisterPass<UnrollAndUnmerge> X("uu", "Unroll & Unmerge Pass");

Pass *llvm::createUnrollAndUnmergePass() { return new UnrollAndUnmerge(); }

UnrollAndUnmergeFunctionPass::UnrollAndUnmergeFunctionPass() {}

PreservedAnalyses UnrollAndUnmergeFunctionPass::run(Function &F,
                                             FunctionAnalysisManager &AM) {
  LLVM_DEBUG(dbgs() << "=== Start Unroll & Unmerge Function Pass ===\n");
  LLVM_DEBUG(dbgs() << "Function: " << F.getName() << "\n");
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
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
    std::pair<bool, Loop *> result = unmerge(L, LI, DT, SE, TTI, AC, nullptr);
    changed |= result.first;
  }

  if (!changed) {
    return PreservedAnalyses::all();
  }
  return PreservedAnalyses::none();
}

// Returns the loop hint metadata node with the given name (for example,
// "llvm.loop.unroll.count").  If no such metadata node exists, then nullptr is
// returned.
static MDNode *getUnrollMetadataForLoop(const Loop *L, StringRef Name) {
  if (MDNode *LoopID = L->getLoopID())
    return GetUnrollMetadata(LoopID, Name);
  return nullptr;
}

// Returns true if the loop has an unroll(full) pragma.
static bool hasUnrollFullPragma(const Loop *L) {
  return getUnrollMetadataForLoop(L, "llvm.loop.unroll.full");
}

// Returns true if the loop has an unroll(enable) pragma. This metadata is used
// for both "#pragma unroll" and "#pragma clang loop unroll(enable)" directives.
static bool hasUnrollEnablePragma(const Loop *L) {
  return getUnrollMetadataForLoop(L, "llvm.loop.unroll.enable");
}

bool canUnrollAndUnmerge(Loop &L, AssumptionCache &AC, TargetTransformInfo &TTI,
                         LoopInfo &LI) {
  bool unrollFullPragma = hasUnrollFullPragma(&L);
  bool unrollEnablePragma = hasUnrollEnablePragma(&L);
  if (unrollFullPragma || unrollEnablePragma) {
    LLVM_DEBUG(dbgs() << "skipping loop " << L.getHeader()->getName()
                      << " because it has unroll pragma\n");
    return false;
  }

  if (!loopContainsBranch(L)) {
    LLVM_DEBUG(dbgs() << "Skipping loop " << L.getHeader()->getName()
                      << " Loop does not contain branch\n");
    return false;
  }

  if (containsConvergent(L)) {
    LLVM_DEBUG(dbgs() << "Skipping loop " << L.getHeader()->getName()
                      << " Loop contains convergent instructions\n");
    return false;
  }

  CodeMetrics metrics = getLoopMetrics(L, AC, TTI);
  if (metrics.notDuplicatable) {
    LLVM_DEBUG(dbgs() << "Skipping loop " << L.getHeader()->getName()
                      << " Loop does contains not duplicatable instructions\n");
    return false;
  }

  InstructionCost loopSize = metrics.NumInsts;
  if (loopSize > 512) {
    LLVM_DEBUG(dbgs() << "Skipping loop " << L.getHeader()->getName()
                      << " Loop size: " << loopSize << " > 512\n");
    return false;
  }

  long long numPaths = getNumPaths(L, LI);
  if (numPaths > 16) {
    LLVM_DEBUG(dbgs() << "Skipping loop " << L.getHeader()->getName()
                      << " num paths: " << numPaths << " > 16\n");
    return false;
  }

  int unrollFactor = 2;
  long long estimatedLoopSize =
      getEstimatedLoopSize(L, loopSize, unrollFactor, numPaths);
  if (estimatedLoopSize > 1024) {
    LLVM_DEBUG(dbgs() << "Skipping loop " << L.getHeader()->getName()
                      << " estimated loop size: " << estimatedLoopSize
                      << " > 1024\n");
    return false;
  }
  return true;
}

bool containsLoopThatWillBeUU(
    Loop &L, std::unordered_map<BasicBlock *, bool> &willBeUnrolled) {
  for (Loop *subLoop : L.getSubLoops()) {
    assert(willBeUnrolled.find(subLoop->getHeader()) != willBeUnrolled.end());
    if (willBeUnrolled[subLoop->getHeader()]) {
      return true;
    }
    if (containsLoopThatWillBeUU(*subLoop, willBeUnrolled)) {
      return true;
    }
  }
  return false;
}

void getLoopsToUnrollAndUnmerge(SmallPriorityWorklist<Loop *, 4> &Worklist,
                                LoopInfo &LI,
                                SmallVector<Loop *, 8> &loopsToUnrollAndUnmerge,
                                AssumptionCache &AC, TargetTransformInfo &TTI,
                                std::vector<int> &loopIndices,
                                int &seenLoopsHeuristic) {
  std::unordered_map<BasicBlock *, bool> willBeUnrolled;
  while (!Worklist.empty()) {
    Loop &L = *Worklist.pop_back_val();
    bool containsSubLoopThatWillBeUnrolled =
        containsLoopThatWillBeUU(L, willBeUnrolled);
    if (containsSubLoopThatWillBeUnrolled) {
      LLVM_DEBUG(
          dbgs() << "Skipping loop " << L.getHeader()->getName()
                 << " outer loop contains child loops that will be u&u\n");
      willBeUnrolled[L.getHeader()] = false;
      seenLoopsHeuristic++;
      continue;
    }

    bool uu = canUnrollAndUnmerge(L, AC, TTI, LI);
    willBeUnrolled[L.getHeader()] = uu;
    if (uu) {
      LLVM_DEBUG(dbgs() << "Will unroll " << L.getHeader()->getName() << "\n");
      loopsToUnrollAndUnmerge.push_back(&L);
      loopIndices.push_back(seenLoopsHeuristic);
    }
    seenLoopsHeuristic++;
  }
}

PreservedAnalyses UnrollAndUnmergeHeuristic::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  bool targetIsNVPTX = Triple(F.getParent()->getTargetTriple()).isNVPTX();
  if (!targetIsNVPTX) {
    return PreservedAnalyses::all();
  }
  LLVM_DEBUG(dbgs() << "=== Start Unroll & Unmerge Heuristic Pass ===\n");
  LLVM_DEBUG(dbgs() << "Function: " << F.getName() << "\n");

  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);

  bool changed = false;

  for (const auto &L : LI) {
    // need loop simplify and lcssa form for unroll and unmerge pass
    changed |=
        simplifyLoop(L, &DT, &LI, &SE, &AC, nullptr, false /* PreserveLCSSA */);
    changed |= formLCSSARecursively(*L, DT, &LI, &SE);
  }

  SmallVector<Loop *, 8> loopsToUnrollAndUnmerge;
  std::vector<int> loopIndices;
  SmallPriorityWorklist<Loop *, 4> Worklist;
  appendLoopsToWorklist(LI, Worklist);
  getLoopsToUnrollAndUnmerge(Worklist, LI, loopsToUnrollAndUnmerge, AC, TTI,
                             loopIndices, seenLoopsHeuristic);

  if (loopsToUnrollAndUnmerge.empty()) {
    if (changed) {
      return PreservedAnalyses::none();
    } else {
      return PreservedAnalyses::all();
    }
  }

  int idx = 0;
  for (auto *L : loopsToUnrollAndUnmerge) {
    int unrollFactor = getUnrollFactor(*L, LI, TTI, AC, false);
    errs() << "UnrollAndUnmergeHeuristic::" << loopIndices[idx] << ";" << unrollFactor
           << "\n";
    LLVM_DEBUG(dbgs() << "U&U loop: " << L->getHeader()->getName()
                      << " unroll factor: " << unrollFactor << " loop idx "
                      << loopIndices[idx] << "\n");
    std::pair<bool, Loop *> result =
        unmerge(*L, LI, DT, SE, TTI, AC, nullptr, false, unrollFactor);
    changed |= result.first;
    idx++;
  }

  if (!changed) {
    return PreservedAnalyses::all();
  }
  return PreservedAnalyses::none();
}