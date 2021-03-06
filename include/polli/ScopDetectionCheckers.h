//===-- ScopDetectionCheckers.h ----------------------------------*- C++
//-*-===//
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
#ifndef POLLI_SCOP_DETECTION_CHECKERS_H
#define POLLI_SCOP_DETECTION_CHECKERS_H

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "polly/ScopDetectionDiagnostic.h"
#include "polly/Support/SCEVValidator.h"

#include <memory>
#include <vector>

namespace polli {
using ParamList = std::vector<const llvm::SCEV *>;

class ScopDetectionExtension {
public:
  template <typename T>
  ScopDetectionExtension(const T &e)
      : Ext(new Extension<T>(e)) {}
  ScopDetectionExtension(const ScopDetectionExtension &E)
      : Ext(E.Ext->Copy()) {}
  ScopDetectionExtension(ScopDetectionExtension &&E) : Ext(std::move(E.Ext)) {}

  ScopDetectionExtension &operator=(ScopDetectionExtension E) {
    Ext = std::move(E.Ext);
    return *this;
  }

  friend bool isValid(ScopDetectionExtension &E, polly::RejectReason &R) {
    return E.Ext->isValid_(R);
  }

private:
  struct ValidatorConcept_t {
    virtual ~ValidatorConcept_t(){};
    virtual ValidatorConcept_t *Copy() = 0;
    virtual bool isValid_(polly::RejectReason &R) = 0;
  };

  template <typename T> struct Extension : ValidatorConcept_t {
    Extension(const T &E) : Ext(E) {}
    ValidatorConcept_t *Copy() { return new Extension(*this); }

    bool isValid_(polly::RejectReason &R) { return isValid(Ext, R); }

    T Ext;
  };

  std::unique_ptr<ValidatorConcept_t> Ext;
};

class NonAffineChecker {
public:
  NonAffineChecker(const Region *R, ScalarEvolution *SE) : R(R), SE(SE) {}

  const Region *region() const { return R; }

  ScalarEvolution *se() const { return SE; }

  ParamList params() const { return Params; }

  void setParams(ParamList &&NewParams) { Params = std::move(NewParams); }
  void append(ParamList &&L);

private:
  ParamList Params;
  const Region *R;
  ScalarEvolution *SE;
};

class NonAffineAccessChecker : public NonAffineChecker {
public:
  NonAffineAccessChecker(const Region *R, ScalarEvolution *SE)
      : NonAffineChecker(R, SE) {}
};

class NonAffineBranchChecker : public NonAffineChecker {
public:
  NonAffineBranchChecker(const Region *R, ScalarEvolution *SE)
      : NonAffineChecker(R, SE) {}
};

class NonAffineLoopBoundChecker : public NonAffineChecker {
public:
  NonAffineLoopBoundChecker(const Region *R, ScalarEvolution *SE)
      : NonAffineChecker(R, SE) {}
};

class AliasingChecker {};
class ProfitableChecker {};

template <typename T> bool isValid(T &Ext, polly::RejectReason &R) {
  return false;
}

bool isValid(NonAffineAccessChecker &Chk, polly::RejectReason &Reason);
bool isValid(NonAffineBranchChecker &Chk, polly::RejectReason &Reason);
bool isValid(NonAffineLoopBoundChecker &Chk, polly::RejectReason &Reason);
bool isValid(AliasingChecker &Chk, polly::RejectReason &Reason);
bool isValid(ProfitableChecker &Chk, polly::RejectReason &Reason);
}
#endif // POLLI_SCOP_DETECTION_CHECKERS_H
