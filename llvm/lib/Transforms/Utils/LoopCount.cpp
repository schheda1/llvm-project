#include "llvm/Transforms/Utils/LoopCount.h"
#include "llvm/ADT/PriorityWorklist.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/IR2Vec.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include <cmath>
#include <utility>

using namespace llvm;

int LoopCountPass::seenLoops = 0;
int LoopCountFunctionPass::seenLoopsFunction = 0;

// Dimensionality of the IR2Vec loop-content embedding appended to each CSV row.
// Must match seedEmbeddingVocab75D.json and IR2VEC_DIM in hecbench.py.
static constexpr unsigned IR2VecDim = 75;

// ---------------------------------------------------------------------------
// Flow-aware (FA) IR2Vec embedding — OFF by default.
//
// With -loopcount-emit-fa set, LoopCount appends a SECOND 75-wide embedding
// block ("femb0".."femb74") AFTER the symbolic "emb" columns. The symbolic
// block is left byte-identical, and with the flag OFF the output (and every
// existing consumer, including hecbench.py) is completely unchanged. FA is a
// pure-analysis/compile-time addition; it does not affect execute/measure.
// See followup_plan.md §1f.
// ---------------------------------------------------------------------------
static cl::opt<bool> EmitFAEmbedding(
    "loopcount-emit-fa", cl::init(false), cl::Hidden,
    cl::desc("Append flow-aware IR2Vec embedding columns (femb0..) after the "
             "symbolic emb columns. Off by default: output stays byte-identical."));

static cl::opt<unsigned> FAIterations(
    "loopcount-fa-iters", cl::init(5u), cl::Hidden,
    cl::desc("Flow-aware propagation iterations. A FIXED count (not an epsilon "
             "stop) so the result is deterministic across builds/ISAs; choose it "
             "from an offline epsilon-convergence sweep."));

static cl::opt<float> FAOpcWeight(
    "loopcount-fa-opc-weight", cl::init(1.0f), cl::Hidden,
    cl::desc("Flow-aware opcode weight Wo."));
static cl::opt<float> FATypeWeight(
    "loopcount-fa-type-weight", cl::init(0.5f), cl::Hidden,
    cl::desc("Flow-aware type weight Wt."));
static cl::opt<float> FAArgWeight(
    "loopcount-fa-arg-weight", cl::init(0.2f), cl::Hidden,
    cl::desc("Flow-aware operand weight Wa. Keep < 1 so the fixed point stays "
             "bounded (deeper def-use contributions decay geometrically)."));

// ---------------------------------------------------------------------------
// Kernel parent tracking
// ---------------------------------------------------------------------------

/// Return true if F is a PTX kernel entry point (__global__ function).
static bool isPTXKernel(const Function *F) {
  return F->getCallingConv() == CallingConv::PTX_Kernel;
}

/// For a given function F, return a '|'-separated string of the mangled names
/// of all PTX kernel entry points that (transitively) call F.
///
/// If F itself is a PTX kernel, returns F's own name.
/// If F is a __device__ function, BFS over the use-def call graph to find all
/// __global__ ancestors.  The | separator is chosen because ; is already used
/// as the CSV column delimiter in LoopCount output.
static std::string getKernelParents(Function *F) {
  if (isPTXKernel(F))
    return F->getName().str();

  SmallPtrSet<Function *, 8> Visited;
  SmallVector<Function *, 8> Worklist;
  SmallVector<std::string, 4> Parents;

  Visited.insert(F);
  Worklist.push_back(F);

  while (!Worklist.empty()) {
    Function *Curr = Worklist.pop_back_val();
    for (User *U : Curr->users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB)
        continue;
      Function *Caller = CB->getFunction();
      if (!Caller || !Visited.insert(Caller).second)
        continue;
      if (isPTXKernel(Caller))
        Parents.push_back(Caller->getName().str());
      else
        Worklist.push_back(Caller);
    }
  }

  std::string Result;
  for (size_t i = 0; i < Parents.size(); ++i) {
    if (i > 0)
      Result += "|";
    Result += Parents[i];
  }
  return Result;
}

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

void printContainsCall(Loop &L) {
  // True if the loop body contains at least one non-intrinsic call instruction.
  // Intrinsics (llvm.dbg.*, llvm.lifetime.*, etc.) are excluded because they
  // are typically lowered to nothing and do not inhibit unrolling.
  // Mirrors the check used by LLVM's loop unroller to detect calls that
  // prevent or limit unrolling due to code-size and ABI side effects.
  bool containsCall = false;
  for (BasicBlock *BB : L.blocks()) {
    if (containsCall)
      break;
    for (Instruction &I : *BB) {
      if (isa<CallInst>(I) && !isa<IntrinsicInst>(I)) {
        containsCall = true;
        break;
      }
    }
  }
  errs() << ";" << containsCall;
}

void printNumExits(Loop &L) {
  // Number of blocks inside the loop that have at least one edge leaving the
  // loop (exiting blocks).  Mirrors L.getExitBlock() == nullptr check used
  // by the LLVM unroller: a single exiting block (numExits == 1) means a
  // simple, predictable exit; multiple exiting blocks complicate unrolling
  // because each unrolled copy may exit early and requires separate handling.
  SmallVector<BasicBlock *, 8> ExitingBlocks;
  L.getExitingBlocks(ExitingBlocks);
  errs() << ";" << ExitingBlocks.size();
}

struct InstructionCounts {
  unsigned numBasicBlocks = 0;
  unsigned numMemoryInsts = 0;
  unsigned numComputeInsts = 0;
  unsigned numControlFlowInsts = 0;
};

static InstructionCounts getInstructionCounts(Loop &L) {
  InstructionCounts counts;
  for (BasicBlock *BB : L.blocks()) {
    counts.numBasicBlocks++;
    for (Instruction &I : *BB) {
      if (isa<LoadInst>(I) || isa<StoreInst>(I) ||
          isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        counts.numMemoryInsts++;
      } else if (isa<BinaryOperator>(I) || isa<UnaryOperator>(I) ||
                 isa<CmpInst>(I) || isa<CastInst>(I)) {
        counts.numComputeInsts++;
      } else if (isa<BranchInst>(I) || isa<SwitchInst>(I) || isa<CallInst>(I)) {
        counts.numControlFlowInsts++;
      }
    }
  }
  return counts;
}

void printInstructionCounts(Loop &L) {
  InstructionCounts counts = getInstructionCounts(L);
  errs() << counts.numBasicBlocks << ";";
  errs() << counts.numMemoryInsts << ";";
  errs() << counts.numComputeInsts << ";";
  errs() << counts.numControlFlowInsts;
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
           << "containsBranch;"
           << "tripCountKnown;"
           << "tripCount;"
           << "numBasicBlocks;"
           << "numMemoryInsts;"
           << "numComputeInsts;"
           << "numControlFlowInsts;"
           << "containsCall;"
           << "numExits;"
           << "isKernelFunction;"
           << "kernelParents";
    // IR2Vec loop-content embedding columns, appended at the END so the
    // structural feature positions (and the RL trip-count mask indices) never
    // move.  See design decision 2 in ir2vec_plan.md.
    for (unsigned I = 0; I < IR2VecDim; ++I)
      errs() << ";emb" << I;
    // Flow-aware columns, only when enabled, appended AFTER the symbolic block
    // so the symbolic emb positions (and the RL feature indices) never move.
    if (EmitFAEmbedding)
      for (unsigned I = 0; I < IR2VecDim; ++I)
        errs() << ";femb" << I;
    errs() << "\n";
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

/// Emit the IR2Vec loop-content embedding (75 ';'-prefixed columns).
///
/// The loop embedding is the *per-instruction mean* of IR2Vec's Symbolic
/// instruction vectors over the loop's blocks: a content signature (opcode /
/// type / operand mix) that is deliberately size-invariant, since loop size /
/// depth / shape are already carried by the structural features.
///
/// getBBVector(BB) returns the SUM of the block's instruction vectors, counting
/// only instructionsWithoutDebug() (see SymbolicEmbedder::computeEmbeddings).
/// The denominator MUST count the same non-debug instructions, or the mean is
/// skewed by debug/pseudo instructions the numerator never included.
///
/// L.blocks() includes nested child-loop blocks — consistent with how loopSize
/// and containsChildLoops already treat nesting.  Preheader/exit blocks are
/// excluded (loop body only).
///
/// Emb == nullptr is the defensive zero-fallback (vocabulary not cached): 75
/// zeros keep the CSV width invariant.  In the real pipeline the vocab is
/// always present — a missing --ir2vec-vocab-path fails the compile loudly at
/// IR2VecVocabAnalysis (and Python refuses to launch without it), so all-zero
/// embeddings never silently reach training.
static void printLoopEmbedding(Loop &L, const ir2vec::Embedder *Emb) {
  if (!Emb) {
    for (unsigned I = 0; I < IR2VecDim; ++I)
      errs() << ";0.000000";
    return;
  }
  ir2vec::Embedding Sum(IR2VecDim, 0.0);
  unsigned NumInsts = 0;
  for (BasicBlock *BB : L.blocks()) {
    Sum += Emb->getBBVector(*BB);
    for (const Instruction &I : BB->instructionsWithoutDebug()) {
      (void)I;
      ++NumInsts;
    }
  }
  double Scale = 1.0 / (NumInsts ? NumInsts : 1u);
  for (unsigned I = 0; I < IR2VecDim; ++I)
    errs() << ";" << format("%.6f", Sum[I] * Scale);
}

/// Compute flow-aware (FA) IR2Vec per-instruction embeddings for the whole
/// function F.
///
/// FA extends the symbolic per-instruction vector by replacing an operand's
/// KIND-seed with the (previous-round) embedding of the instruction that
/// DEFINES it, propagating data-flow along def-use chains. Non-instruction
/// operands (arguments, constants, globals, block labels) keep their kind-seed,
/// exactly as the symbolic embedder treats them — so FA follows register/SSA
/// flow only, never memory flow.
///
/// Cyclic def-use (PHIs / loop back-edges) is handled by a FIXED number of
/// Jacobi iterations (FAIterations): each round reads ONLY the previous round's
/// map, so the result is independent of map/traversal order and therefore
/// deterministic across builds and ISAs. Wa (< 1) decays the backward slice so
/// the values stay bounded. Mirrors the intended ir2vec::FlowAwareEmbedder, kept
/// local to this pass so the shared IR2Vec analysis (and its Symbolic output) is
/// left untouched.
static ir2vec::InstEmbeddingsMap
computeFlowAwareEmbeddings(const Function &F, const ir2vec::Vocabulary &Vocab) {
  const unsigned Dim = Vocab.getDimension();

  // The constant opcode+type part of an instruction's vector (never changes
  // across iterations).
  auto opTypeBase = [&](const Instruction &I) {
    ir2vec::Embedding B(Dim, 0.0);
    B.scaleAndAdd(Vocab[I.getOpcode()], FAOpcWeight);
    B.scaleAndAdd(Vocab[I.getType()->getTypeID()], FATypeWeight);
    return B;
  };

  // Round 0: the symbolic value — every operand contributes its kind-seed.
  ir2vec::InstEmbeddingsMap Cur;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB.instructionsWithoutDebug()) {
      ir2vec::Embedding Args(Dim, 0.0);
      for (const Use &U : I.operands())
        Args += Vocab[U.get()];
      ir2vec::Embedding E = opTypeBase(I);
      E.scaleAndAdd(Args, FAArgWeight);
      Cur[&I] = std::move(E);
    }

  // Jacobi fixed-point: an instruction operand now contributes the DEFINING
  // instruction's previous-round vector; every other operand keeps its seed.
  for (unsigned Round = 0; Round < FAIterations; ++Round) {
    ir2vec::InstEmbeddingsMap Nxt;
    Nxt.reserve(Cur.size());
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB.instructionsWithoutDebug()) {
        ir2vec::Embedding Args(Dim, 0.0);
        for (const Use &U : I.operands()) {
          const Value *V = U.get();
          if (const auto *DefI = dyn_cast<Instruction>(V)) {
            auto Found = Cur.find(DefI);
            if (Found != Cur.end()) {
              Args += Found->second;
              continue;
            }
          }
          Args += Vocab[V];
        }
        ir2vec::Embedding E = opTypeBase(I);
        E.scaleAndAdd(Args, FAArgWeight);
        Nxt[&I] = std::move(E);
      }
    Cur = std::move(Nxt);
  }

  return Cur;
}

/// Emit the flow-aware IR2Vec embedding (75 ';'-prefixed femb columns): the
/// per-instruction mean of FA vectors over the loop's blocks — exactly parallel
/// to printLoopEmbedding for the symbolic vectors, using the same non-debug
/// instruction set as its denominator. An absent/empty map yields 75 zeros,
/// mirroring the symbolic zero-fallback so the CSV width is invariant.
static void printLoopFAEmbedding(Loop &L,
                                 const ir2vec::InstEmbeddingsMap *FAMap) {
  if (!FAMap || FAMap->empty()) {
    for (unsigned I = 0; I < IR2VecDim; ++I)
      errs() << ";0.000000";
    return;
  }
  ir2vec::Embedding Sum(IR2VecDim, 0.0);
  unsigned NumInsts = 0;
  for (BasicBlock *BB : L.blocks()) {
    for (const Instruction &I : BB->instructionsWithoutDebug()) {
      auto It = FAMap->find(&I);
      if (It != FAMap->end())
        Sum += It->second;
      ++NumInsts;
    }
  }
  double Scale = 1.0 / (NumInsts ? NumInsts : 1u);
  for (unsigned I = 0; I < IR2VecDim; ++I)
    errs() << ";" << format("%.6f", Sum[I] * Scale);
}

static void printLoopData(Loop &L, Module *M, Function *F, AssumptionCache &AC,
                          TargetTransformInfo &TTI, int &seenLoops,
                          LoopInfo &LI, ScalarEvolution &SE,
                          const ir2vec::Embedder *Emb = nullptr,
                          const ir2vec::InstEmbeddingsMap *FAMap = nullptr) {
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
  unsigned tripCount = SE.getSmallConstantTripCount(&L);
  errs() << ";" << (tripCount > 0 ? 1 : 0);
  errs() << ";" << tripCount << ";";
  printInstructionCounts(L);
  printContainsCall(L);
  printNumExits(L);
  errs() << ";" << (isPTXKernel(F) ? 1 : 0);
  errs() << ";" << getKernelParents(F);
  printLoopEmbedding(L, Emb);
  if (EmitFAEmbedding)
    printLoopFAEmbedding(L, FAMap);
  errs() << "\n";
}

PreservedAnalyses LoopCountPass::run(Loop &L, LoopAnalysisManager &AM,
                                     LoopStandardAnalysisResults &AR,
                                     LPMUpdater &U) {
  Function *F = L.getHeader()->getParent();
  Module *M = F->getParent();
  // Legacy loop-pass variant — NOT part of the RL pipeline (which uses
  // LoopCountFunctionPass).  No embedder available here, so embeddings are
  // zeros.  Kept only for the standalone -loopcount loop-pass entry point.
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

  // Fetch the IR2Vec vocabulary cached at module scope by the
  // RequireAnalysisPass<IR2VecVocabAnalysis> added in PassBuilder::addLoopCount.
  // A function pass can only see *cached* module analyses via the proxy — it
  // cannot run one.  If the require pass is absent (e.g. a pipeline that does
  // not add it) the pointer is null and embeddings fall back to zeros.
  auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  const ir2vec::Vocabulary *Vocab =
      MAMProxy.getCachedResult<IR2VecVocabAnalysis>(*F.getParent());

  std::unique_ptr<ir2vec::Embedder> Emb;
  if (Vocab && Vocab->isValid() && Vocab->getDimension() == IR2VecDim) {
    Emb = ir2vec::Embedder::create(IR2VecKind::Symbolic, F, *Vocab);
  } else {
    // One warning per module, not per function.
    static SmallPtrSet<const Module *, 4> Warned;
    if (Warned.insert(F.getParent()).second)
      errs() << "LOOPCOUNT WARNING: ir2vec vocab unavailable "
                "(dim mismatch or not cached) — embeddings are zero\n";
  }

  bool changed = false;

  for (const auto &L : LI) {
    // need loop simplify and lcssa form for unroll and unmerge pass
    changed |=
        simplifyLoop(L, &DT, &LI, &SE, &AC, nullptr, false /* PreserveLCSSA */);
    changed |= formLCSSARecursively(*L, DT, &LI, &SE);
  }

  // Flow-aware embeddings: computed ONCE per function and pooled per loop.
  // MUST be built AFTER simplifyLoop/formLCSSA canonicalize the IR — the
  // symbolic embedder computes lazily (in the worklist loop below, i.e. also
  // post-canonicalization), so FA must see the same instructions, or its
  // instruction pointers predate the preheaders/LCSSA PHIs those passes insert
  // and the two embeddings describe different IR. Only when enabled and the
  // vocab is usable; otherwise femb falls back to zeros, like the symbolic path.
  ir2vec::InstEmbeddingsMap FAMap;
  if (EmitFAEmbedding && Vocab && Vocab->isValid() &&
      Vocab->getDimension() == IR2VecDim)
    FAMap = computeFlowAwareEmbeddings(F, *Vocab);

  SmallPriorityWorklist<Loop *, 4> Worklist;
  appendLoopsToWorklist(LI, Worklist);

  while (!Worklist.empty()) {
    Loop &L = *Worklist.pop_back_val();
    printLoopData(L, F.getParent(), &F, AC, TTI, seenLoopsFunction, LI, SE,
                  Emb.get(), EmitFAEmbedding ? &FAMap : nullptr);
  }

  if (changed) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
