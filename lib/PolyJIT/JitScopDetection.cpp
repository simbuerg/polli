//===-- JitScopDetection.cpp ---------------------------- -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "polyjit"
#include <map>                             // for _Rb_tree_const_iterator, etc
#include <memory>                          // for unique_ptr
#include <set>                             // for set
#include <string>                          // for string
#include <utility>                         // for make_pair, pair
#include <vector>                          // for vector<>::iterator, vector
#include "llvm/ADT/Statistic.h"            // for STATISTIC, Statistic
#include "llvm/Analysis/RegionInfo.h"      // for Region, RegionInfo
#include "llvm/Analysis/RegionPass.h"      // for RGPassManager
#include "llvm/Analysis/ScalarEvolution.h" // for SCEV, ScalarEvolution
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/Dominators.h" // for DominatorTreeWrapperPass
#include "llvm/IR/LegacyPassManager.h" // for FunctionPassManager
#include "llvm/InitializePasses.h"
#include "llvm/PassAnalysisSupport.h"  // for AnalysisUsage, etc
#include "llvm/PassSupport.h"          // for INITIALIZE_PASS_DEPENDENCY, etc
#include "llvm/Support/CommandLine.h"  // for desc, opt
#include "llvm/Support/Debug.h"        // for dbgs, DEBUG
#include "llvm/Support/raw_ostream.h"  // for raw_ostream
#include "polli/JitScopDetection.h"    // for ParamList, etc
#include "polly/ScopDetection.h"       // for ScopDetection, etc
#include "polly/ScopDetectionDiagnostic.h" // for ReportNonAffBranch, etc
#include "polly/Support/SCEVValidator.h"   // for getParamsInNonAffineExpr, etc

#include "polly/LinkAllPasses.h"
#include "polly/Canonicalization.h"
#include "polli/ScopDetectionCheckers.h"
#include "polli/Utils.h"

#include "spdlog/spdlog.h"

namespace llvm {
class Function;
} // namespace llvm
namespace llvm {
class Module;
} // namespace llvm

using namespace llvm;
using namespace llvm::legacy;
using namespace polly;
using namespace polli;
using namespace spdlog::details;

STATISTIC(JitScopsFound, "Number of jitable SCoPs");

class DiagnosticJitScopFound : public DiagnosticInfo {
private:
  static int PluginDiagnosticKind;

  Function &F;
  std::string FileName;
  unsigned EntryLine, ExitLine;

public:
  DiagnosticJitScopFound(Function &F, std::string FileName, unsigned EntryLine,
                         unsigned ExitLine)
      : DiagnosticInfo(PluginDiagnosticKind, DS_Note), F(F), FileName(FileName),
        EntryLine(EntryLine), ExitLine(ExitLine) {}

  void print(DiagnosticPrinter &DP) const override;
  static bool classof(const DiagnosticInfo *DI) {
    return DI->getKind() == PluginDiagnosticKind;
  }
};

void JitScopDetection::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ScopDetection>();
  AU.addRequired<ScalarEvolution>();
  AU.addRequired<RegionInfoPass>();
  AU.setPreservesAll();
}

// Remove all direct and indirect children of region R from the region set Regs,
// but do not recurse further if the first child has been found.
//
// Return the number of regions erased from Regs.
static unsigned eraseAllChildren(ScopSet &Regs, const Region &R) {
  unsigned Count = 0;
  for (auto &SubRegion : R) {
    if (Regs.count(SubRegion.get())) {
      ++Count;
      Regs.remove(SubRegion.get());
    } else {
      Count += eraseAllChildren(Regs, *SubRegion);
    }
  }
  return Count;
}

static void getDebugLocations(const Region *R, DebugLoc &Begin, DebugLoc &End) {
  for (const BasicBlock *BB : R->blocks())
    for (const Instruction &Inst : *BB) {
      DebugLoc DL = Inst.getDebugLoc();
      if (!DL)
        continue;

      Begin = Begin ? std::min(Begin, DL) : DL;
      End = End ? std::max(End, DL) : DL;
    }
}

static void emitClassicalSCoPs(const Function &F, const ScopSet &Scops) {
  LLVMContext &Ctx = F.getContext();

  DebugLoc Begin, End;

  for (const Region *R : Scops) {
    getDebugLocations(R, Begin, End);

    emitOptimizationRemark(Ctx, DEBUG_TYPE, F, Begin,
                          "A classic SCoP begins here.");
    emitOptimizationRemark(Ctx, DEBUG_TYPE, F, End, "A classic SCoP ends here.");
  }
}

static void emitJitSCoPs(const Function &F, const ScopSet &Scops) {
  LLVMContext &Ctx = F.getContext();

  DebugLoc Begin, End;

  for (const Region *R : Scops) {
    getDebugLocations(R, Begin, End);

    emitOptimizationRemark(Ctx, DEBUG_TYPE, F, Begin,
                          "A JIT SCoP begins here.");
    emitOptimizationRemark(Ctx, DEBUG_TYPE, F, End, "A JIT SCoP ends here.");
  }
}

static bool isValidRec(const Region *CurR, const Region *R) {
  static auto Console = spdlog::stderr_logger_st("polli/jitsd");

  bool isValid = false;
  for (auto &Child : *CurR) {
    const Region *Sub = Child->getNodeAs<Region>();
    isValid = (Sub == R);
    if (isValid)
      break;
    else
      return isValidRec(Sub, R);
  }
  Console->error("IsValid: {} = {}", R->getNameStr(), isValid);
  return isValid;
}

bool JitScopDetection::isInvalidRegion(const Function &F,
                                       const Region *R) const {
  const RegionInfo &RInfo = RI->getRegionInfo();
  const Region *TopLevel = RInfo.getTopLevelRegion();

  // This would either be the TopLevel, or a dangling region pointer.
  // We want neither.
  const Region *Parent = R->getParent();
  if (Parent == nullptr)
    return true;

  while (Parent && !JitableScops.count(Parent))
    Parent = Parent->getParent();

  return !isValidRec(TopLevel, R);
}

bool JitScopDetection::runOnFunction(Function &F) {
  static auto Console = spdlog::stderr_logger_st("polli/jitsd");

  if (!Enabled)
    return false;

  if (F.isDeclaration())
    return false;

  if (F.hasFnAttribute("polyjit-jit-candidate"))
    return false;

  Console->error("Running on: {}", F.getName().str());

  SD = &getAnalysis<ScopDetection>();
  SE = &getAnalysis<ScalarEvolution>();
  RI = &getAnalysis<RegionInfoPass>();
  M = F.getParent();

  Console->warn("== Detect JIT SCoPs in function: {:>30}", F.getName().str());
  for (ScopDetection::const_reject_iterator Rej = SD->reject_begin(),
                                            RejE = SD->reject_end();
       Rej != RejE; ++Rej) {
    const Region *R = (*Rej).first;

    if (!R)
      continue;
    Console->warn("==== Next Region: {:>60s}", R->getNameStr());
    RejectLog Log = (*Rej).second;

    NonAffineAccessChecker NonAffAccessChk(R, SE);
    NonAffineBranchChecker NonAffBranchChk(R, SE);
    NonAffineLoopBoundChecker LoopBoundChk(R, SE);
    AliasingChecker AliasChk;

    ParamList Params;
    bool RegionIsValid = Log.size() > 0;

    for (auto &Reason : Log) {
      bool IsFixable = false;
      IsFixable |= isValid(NonAffAccessChk, *Reason);
      IsFixable |= isValid(NonAffBranchChk, *Reason);
      IsFixable |= isValid(LoopBoundChk, *Reason);
      IsFixable |= isValid(AliasChk, *Reason);

      RegionIsValid &= IsFixable;
    }

    if (RegionIsValid) {
      ParamList &L = RequiredParams[R];
      ParamList Tmp = NonAffAccessChk.params();
      L.insert(L.end(), Tmp.begin(), Tmp.end());

      Tmp = NonAffBranchChk.params();
      L.insert(L.end(), Tmp.begin(), Tmp.end());

      Tmp = LoopBoundChk.params();
      L.insert(L.end(), Tmp.begin(), Tmp.end());

      /* The SCoP can be fixed at run time. However, we want need to make
       * sure to fetch the largest parent region that is fixable.
       * We need to do two steps:
       *
       * 1) Eliminate all children from the set of jitable Scops. */
      unsigned deleted = eraseAllChildren(JitableScops, *R);
      Console->error("Deleted {} children.", deleted);

      /* 2) Search for one of our parents (up to the function entry) in the
       *    list of jitable Scops. If we find one in there, do not enter
       *    the set of jitable Scops. */
      if (isInvalidRegion(F, R)) {
        continue;
      }

      // We found one of our parent regions in the set of jitable Scops.
      }
      JitableScops.insert(R);
      ++JitScopsFound;
    }
  }

  ScopSet ClassicScops;

  ClassicScops.insert(SD->begin(), SD->end());
  AccumulatedScops.insert(SD->begin(), SD->end());
  AccumulatedScops.insert(JitableScops.begin(), JitableScops.end());

  emitClassicalSCoPs(F, ClassicScops);
  emitJitSCoPs(F, JitableScops);

  return false;
}

void JitScopDetection::print(raw_ostream &OS, const Module *) const {
  using reject_iterator = std::map<const Region *, RejectLog>::const_iterator;
  using reject_range = iterator_range<reject_iterator>;

  unsigned count = JitableScops.size();
  unsigned i = 0;

  OS << fmt::format("{:d} regions require runtime support:\n", count);
  for (const Region *R : JitableScops) {
    const ParamList &L = RequiredParams.at(R);
    OS.indent(2) << fmt::format("{:d} region {:s} requires {:d} params\n", i++,
                                R->getNameStr(), L.size());
    unsigned j = 0;
    for (const SCEV *S : L) {
      OS.indent(4) << fmt::format("{:d} - ", j);
      S->print(OS);
      OS << "\n";
    }

    const reject_range Rejects(SD->reject_begin(), SD->reject_end());
    for (auto &Reject : Rejects) {
      if (Reject.first == R) {
        unsigned k = 0;
        polly::RejectLog Log = Reject.second;
        OS.indent(4) << fmt::format("{:d} reasons can be fixed at run time:\n",
                                    Log.size());
        for (auto &Entry : Reject.second) {
          OS.indent(6) << fmt::format("{:d} - {:s}\n", k++,
                                      Entry->getMessage());
        }
      }
    }
  }
}

void JitScopDetection::releaseMemory() {
  JitableScops.clear();
  AccumulatedScops.clear();
  RequiredParams.clear();
}

char JitScopDetection::ID = 0;

static RegisterPass<JitScopDetection>
    X("polli-detect", "PolyJIT - Detect SCoPs that require runtime support.",
      false, false);
