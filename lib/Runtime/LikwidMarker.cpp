//===-- LikwidMarker.cpp - LikwidMarker pass --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Place likwidMarker* calls in parallel regions.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "polyjit"

#include "polli/LikwidMarker.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Debug.h"
#include "llvm/Pass.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/PassSupport.h"

#include "cppformat/format.h"

namespace polli {
class LikwidMarker : public llvm::ModulePass {
public:
  static char ID;
  explicit LikwidMarker() : llvm::ModulePass(ID) {}

  /// @name ModulePass interface
  //@{
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  void releaseMemory() override;
  bool runOnModule(llvm::Module &M) override;
  void print(llvm::raw_ostream &OS, const llvm::Module *) const override;
  //@}
private:
  //===--------------------------------------------------------------------===//
  // DO NOT IMPLEMENT
  LikwidMarker(const LikwidMarker &);
  // DO NOT IMPLEMENT
  const LikwidMarker &operator=(const LikwidMarker &);
};

/**
 * @brief Mark generated functions with calls to PolyJIT's instrumentation.
 *
 * This instrumentation is based on libPAPI and supports only timing
 * information.
 *
 * For this to actually do anything, it is necessary to provide an
 * environment variable: POLLI_ENABLE_PAPI.
 *
 * @see polli/Options.h
 * @see polli::havePapi()
 */
class TraceMarker : public llvm::ModulePass {
public:
  static char ID;
  explicit TraceMarker() : llvm::ModulePass(ID) {}

  /// @name ModulePass interface
  //@{
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  void releaseMemory() override;
  bool runOnModule(llvm::Module &M) override;
  void print(llvm::raw_ostream &OS, const llvm::Module *) const override;
  //@}
private:
  //===--------------------------------------------------------------------===//
  // DO NOT IMPLEMENT
  TraceMarker(const TraceMarker &);
  // DO NOT IMPLEMENT
  const TraceMarker &operator=(const TraceMarker &);
};
} // namespace polli

using namespace polli;
using namespace llvm;

char LikwidMarker::ID = 0;
char TraceMarker::ID = 0;

namespace polli {
void LikwidMarker::getAnalysisUsage(llvm::AnalysisUsage &AU) const {}

void LikwidMarker::releaseMemory() {}

void LikwidMarker::print(llvm::raw_ostream &OS, const llvm::Module *) const {}

bool LikwidMarker::runOnModule(llvm::Module &M) {
  LLVMContext &Ctx = getGlobalContext();
  Function *OmpStartFn = M.getFunction("GOMP_loop_runtime_next");
  Function *ThreadInit = static_cast<Function *>(M.getOrInsertFunction(
      "likwid_markerThreadInit", Type::getVoidTy(Ctx), nullptr));
  Function *Start = static_cast<Function *>(
      M.getOrInsertFunction("likwid_markerStartRegion", Type::getVoidTy(Ctx),
                            Type::getInt8PtrTy(Ctx, 0), nullptr));
  Function *Stop = static_cast<Function *>(
      M.getOrInsertFunction("likwid_markerStopRegion", Type::getVoidTy(Ctx),
                            Type::getInt8PtrTy(Ctx, 0), nullptr));

  // Find the OpenMP sub function
  SetVector<Function *> SubFunctions;
  if (OmpStartFn) {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      for (auto &I : instructions(F)) {
        if (CallInst *Call = dyn_cast<CallInst>(&I)) {
          if (Call->getCalledFunction() == OmpStartFn) {
            SubFunctions.insert(&F);
          }
        }
      }
    }
  }

  if (SubFunctions.size() == 0) {
    DEBUG(dbgs() << fmt::format("No OpenMP SubFunction generated by polly."));
    // In the case polly does not generate a OpenMP sub function, instrument
    // all functions in the module with sequential likwid markers.
    IRBuilder<> Builder(Ctx);
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      BasicBlock &Entry = F.getEntryBlock();
      Builder.SetInsertPoint(&*(Entry.getFirstInsertionPt()));
      Builder.CreateCall(Start, Builder.CreateGlobalStringPtr(F.getName()));

      for (auto &I : instructions(F)) {
        if (isa<ReturnInst>(&I)) {
          Builder.SetInsertPoint(&I);
          Builder.CreateCall(Stop, Builder.CreateGlobalStringPtr(F.getName()));
        }
      }
    }
  }

  for (auto SubFn : SubFunctions) {
    DEBUG(dbgs() << fmt::format("OpenMP subfn found: {}",
                                SubFn->getName().str()));
    BasicBlock &Entry = SubFn->getEntryBlock();
    IRBuilder<> Builder(Ctx);

    Builder.SetInsertPoint(&*(Entry.getFirstInsertionPt()));
    Builder.Insert(CallInst::Create(ThreadInit));
    Builder.CreateCall(Start, Builder.CreateGlobalStringPtr(SubFn->getName()));

    for (auto &I : instructions(*SubFn)) {
      if (isa<ReturnInst>(&I)) {
        Builder.SetInsertPoint(&I);
        Builder.CreateCall(Stop,
                           Builder.CreateGlobalStringPtr(SubFn->getName()));
      }
    }
  }
  return true;
}

///--------------------------------------------------------------------------///
//
// TraceMarker
//
///--------------------------------------------------------------------------///

void TraceMarker::getAnalysisUsage(llvm::AnalysisUsage &AU) const {}

void TraceMarker::releaseMemory() {}

void TraceMarker::print(llvm::raw_ostream &OS, const llvm::Module *) const {}

bool TraceMarker::runOnModule(llvm::Module &M) {
  LLVMContext &Ctx = getGlobalContext();
  Function *OmpStartFn = M.getFunction("GOMP_loop_runtime_next");
  Function *Start = static_cast<Function *>(M.getOrInsertFunction(
      "polliTracingScopStart", Type::getVoidTy(Ctx), Type::getInt64Ty(Ctx),
      Type::getInt8PtrTy(Ctx, 0), nullptr));
  Function *Stop = static_cast<Function *>(M.getOrInsertFunction(
      "polliTracingScopStop", Type::getVoidTy(Ctx), Type::getInt64Ty(Ctx),
      Type::getInt8PtrTy(Ctx, 0), nullptr));

  // Find the OpenMP sub function
  SetVector<Function *> SubFunctions;
  if (OmpStartFn) {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      for (auto &I : instructions(F)) {
        if (CallInst *Call = dyn_cast<CallInst>(&I)) {
          if (Call->getCalledFunction() == OmpStartFn) {
            SubFunctions.insert(&F);
          }
        }
      }
    }
  }

  if (SubFunctions.size() == 0) {
    DEBUG(dbgs() << fmt::format("No OpenMP SubFunction generated by polly."));
    // In the case polly does not generate a OpenMP sub function, instrument
    // all functions in the module with sequential likwid markers.
    IRBuilder<> Builder(Ctx);
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      ConstantInt *ID = ConstantInt::get(Type::getInt64Ty(Ctx),
                                         reinterpret_cast<uint64_t>(&F), false);

      BasicBlock &Entry = F.getEntryBlock();
      Builder.SetInsertPoint(&*(Entry.getFirstInsertionPt()));
      Builder.CreateCall(Start,
                         {ID, Builder.CreateGlobalStringPtr(F.getName())});

      for (auto &I : instructions(F)) {
        if (isa<ReturnInst>(&I)) {
          Builder.SetInsertPoint(&I);
          Builder.CreateCall(Stop,
                             {ID, Builder.CreateGlobalStringPtr(F.getName())});
        }
      }
    }
  }

  for (auto SubFn : SubFunctions) {
    DEBUG(dbgs() << fmt::format("OpenMP subfn found: {}",
                                SubFn->getName().str()));
    BasicBlock &Entry = SubFn->getEntryBlock();
    IRBuilder<> Builder(Ctx);

    ConstantInt *ID = ConstantInt::get(
        Type::getInt64Ty(Ctx), reinterpret_cast<uint64_t>(&SubFn), false);

    Builder.SetInsertPoint(&*(Entry.getFirstInsertionPt()));
    Builder.CreateCall(Start,
                       {ID, Builder.CreateGlobalStringPtr(SubFn->getName())});

    for (auto &I : instructions(*SubFn)) {
      if (isa<ReturnInst>(&I)) {
        Builder.SetInsertPoint(&I);
        Builder.CreateCall(
            Stop, {ID, Builder.CreateGlobalStringPtr(SubFn->getName())});
      }
    }
  }
  return true;
};

ModulePass *createLikwidMarkerPass() { return new LikwidMarker(); }
ModulePass *createTraceMarkerPass() { return new TraceMarker(); }
} // namespace polli

static RegisterPass<LikwidMarker>
    X("polli-likwid", "PolyJIT - Mark parallel regions with likwid calls.",
      false, false);

static RegisterPass<TraceMarker>
    Y("polli-trace", "PolyJIT - Mark parallel regions with trace calls.",
      false, false);
