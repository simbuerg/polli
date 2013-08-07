//===-- PolyJIT.cpp - LLVM Just in Time Compiler --------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This tool implements a just-in-time compiler for LLVM, allowing direct
// execution of LLVM bitcode in an efficient manner.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "polyjit"
#include "llvm/Support/Debug.h"

#include "polli/PolyJIT.h"

#include "polly/RegisterPasses.h"
#include "polly/LinkAllPasses.h"

#include "polly/PapiProfiling.h"

#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CodeGen/JITCodeEmitter.h"
#include "llvm/CodeGen/MachineCodeInfo.h"
#include "llvm/Config/config.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetJITInfo.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"

#include "llvm/Linker.h"

#include "polli/FunctionDispatcher.h"
#include "polli/NonAffineScopDetection.h"
#include "polli/ScopMapper.h"
#include "polli/Utils.h"

#include <set>
#include <map>

using namespace polli;
using namespace polly;
using namespace llvm;
using namespace llvm::sys::fs;

namespace fs = llvm::sys::fs;
namespace p  = llvm::sys::path;

namespace {
class StaticInitializer {
public:
    StaticInitializer() {
      PassRegistry &Registry = *PassRegistry::getPassRegistry();
      initializePollyPasses(Registry);
      initializeOutputDir();
    }
};
}
static StaticInitializer InitializeEverything;
static FunctionDispatcher *Disp = new FunctionDispatcher();

extern "C" {
static void pjit_callback(const char *fName, unsigned paramc,
                   void** params) {
  /* Let's hope that we have called it before ;-)
   * Otherwise it will blow up. FIXME: Don't blow up. */
  PolyJIT *JIT = PolyJIT::Get();

  /* Be very careful here, we want to exit this callback asap to cut down on
   * overhead. Think about triggering any modifications to the underlying IR
   * in a concurrent thread instead of blocking everything here. */
  Module& M = JIT->getExecutedModule();
  Function *F = M.getFunction(fName);

  if (!F)
    llvm_unreachable("Function not in this module. It has to be there!");

  RTParams RuntimeParams = getRuntimeParameters(F, paramc, params);  
  ParamVector<RuntimeParam> PArr = RuntimeParams;

  // FIXME: Do it properly
  std::vector<GenericValue> ArgValues(paramc);
  for (unsigned i  = 0; i < paramc; ++i)
    ArgValues[i] = PTOGV(params[i]);

  Function *NewF = Disp->getFunctionForValues(F, PArr);

  DEBUG(dbgs() << "Dispatching to: " << NewF->getName());
  //ExecutionEngine *EE = JIT->GetEngine();
  //GenericValue Ret = EE->runFunction(NewF, ArgValues);
};
}

class ScopDetectionResultsViewer : public FunctionPass {
  //===--------------------------------------------------------------------===//
  // DO NOT IMPLEMENT
  ScopDetectionResultsViewer(const ScopDetectionResultsViewer &);
  // DO NOT IMPLEMENT
  const ScopDetectionResultsViewer &operator=(const ScopDetectionResultsViewer &);

  ScopDetection *SD;

public:
  static char ID;
  explicit ScopDetectionResultsViewer() : FunctionPass(ID) {}

  /// @name FunctionPass interface
  //@{
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<ScopDetection>();
    AU.setPreservesAll();
  };

  virtual void releaseMemory() {

  };

  virtual bool runOnFunction(Function &F) {
    SD = &getAnalysis<ScopDetection>();

    polly::RejectedLog rl = SD->getRejectedLog();
    for (polly::RejectedLog::iterator
         i = rl.begin(), ie = rl.end(); i != ie; ++i) {
      const Region *R              = (*i).first;
      std::vector<RejectInfo> rlog = (*i).second;

      if (R) {
        outs() << "[polli] rejected region: " <<  R->getNameStr() << "\n";

        for (unsigned n = 0; n < rlog.size(); ++n) {
          outs() << "        reason:  " << rlog[n].getRejectReason() << "\n";
          if (rlog[n].Failed_LHS) {
            outs() << "        details: ";
            rlog[n].Failed_LHS->print(outs());
            outs() << "\n";
          }

          if (rlog[n].Failed_RHS) {
            outs() << "                 ";
            rlog[n].Failed_RHS->print(outs());
            outs() << "\n";
          }
        }
      }
    }

    return false;
  };

  virtual void print(raw_ostream &OS, const Module *) const {

  };
  //@}
};

char ScopDetectionResultsViewer::ID = 0;

namespace llvm {
void PolyJIT::instrumentScops(Module &M, ManagedModules &Mods) {
  outs() << "[polli] Phase III: Injecting call to JIT\n";
  LLVMContext &Ctx = M.getContext();
  IRBuilder<> Builder(Ctx);

  PointerType *PtoArr = PointerType::get(Type::getInt8PtrTy(Ctx), 0);

  StringRef cbName = StringRef("polli.enter.runtime");

  /* Insert callback declaration & call into each extracted module */
  for (ManagedModules::iterator
       i = Mods.begin(), ie = Mods.end(); i != ie; ++i) {
    Module *ScopM = (*i);
    Function *PJITCB = cast<Function>(
      ScopM->getOrInsertFunction(cbName, Type::getVoidTy(Ctx),
                                         Type::getInt8PtrTy(Ctx),
                                         Type::getInt32Ty(Ctx),
                                         PtoArr,
                                         NULL));
    PJITCB->setLinkage(GlobalValue::ExternalLinkage);
    EE.addGlobalMapping(PJITCB, (void *)&pjit_callback);

    std::vector<Value *> Args(3);

    /* Inject call to callback declaration into every function */
    for (Module::iterator
         F = ScopM->begin(), FE = ScopM->end(); F != FE; ++F) {
      if (F->isDeclaration())
        continue;
      BasicBlock *BB = F->begin();
      Builder.SetInsertPoint(BB->getFirstInsertionPt());
     
      /* Create a generic IR sequence of this example C-code:
       * 
       * void foo(int n, int A[42]) {
       *  void *params[2];
       *  params[0] = &n;
       *  params[1] = &A;
       *  
       *  pjit_callback("foo", 2, params);
       * }
       */

      /* Prepare a stack array for the parameters. We will pass a pointer to
       * this array into our callback function. */
      int argc = F->arg_size();
      Value *ParamC = ConstantInt::get(Type::getInt32Ty(Ctx), argc, true);
      Value *Params = Builder.CreateAlloca(Type::getInt8PtrTy(Ctx),
                                           ParamC, "params");

      /* Store each parameter as pointer in the params array */
      int i = 0;
      Value *One    = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
      for (Function::arg_iterator Arg = F->arg_begin(), ArgE = F->arg_end();
           Arg != ArgE; ++Arg) {

        /* Allocate a slot on the stack for the i'th argument and store it */
        Value *Slot   = Builder.CreateAlloca(Arg->getType(), One,
                                             "params." + Twine(i));
        Builder.CreateAlignedStore(Arg, Slot, 4);
       
        /* Bitcast the allocated stack slot to i8* */
        Value *Slot8 = Builder.CreateBitCast(Slot, Type::getInt8PtrTy(Ctx),
                                             "ps.i8ptr." + Twine(i)); 
          
        /* Get the appropriate slot in the parameters array and store
         * the stack slot in form of a i8*. */
        Value *ArrIdx = ConstantInt::get(Type::getInt32Ty(Ctx), i);
        Value *Dest   = Builder.CreateGEP(Params, ArrIdx, "p." + Twine(i));
        Builder.CreateAlignedStore(Slot8, Dest, 8); 

        i++;
      }

      Args[0] = Builder.CreateGlobalStringPtr(F->getName());
      Args[1] = ParamC;
      Args[2] = Params;

      Builder.CreateCall(PJITCB, Args);
    }
  }
};

void PolyJIT::linkJitableScops(ManagedModules &Mods, Module &M) {
  Linker L(&M);

  /* We need to link the functions back in for execution */
  std::string ErrorMsg;
  for (ManagedModules::iterator
       src = Mods.begin(), se = Mods.end(); src != se; ++src)
    if(L.linkInModule(*src, &ErrorMsg))
      errs().indent(2) << "ERROR: " << ErrorMsg << "\n";

  StringRef cbName = StringRef("polli.enter.runtime");
  /* Register our callback with the system linker, so the MCJIT can find it
   * during object compilation */
  sys::DynamicLibrary::AddSymbol(cbName, (void *)&pjit_callback);
};

void PolyJIT::extractJitableScops(Module &M) {
  ScopDetection *SD = (ScopDetection *)polly::createScopDetectionPass();
  NonAffineScopDetection *NaSD = new NonAffineScopDetection();
  ScopMapper *SM = new ScopMapper();

  FPM = new FunctionPassManager(&M);

  /* Add ScopDetection, ResultsViewer and NonAffineScopDetection */
  FPM->add(SD);
  FPM->add(NaSD);
  FPM->add(SM);

  FPM->doInitialization();

  outs() << "[polli] Phase II: Extracting NonAffine Scops\n";
  for (Module::iterator f = M.begin(), fe = M.end(); f != fe ; ++f) {
    if (f->isDeclaration())
      continue;
    outs().indent(2) << "Extract: " << (*f).getName() << "\n";
    FPM->run(*f);
  }

  /* Copy the set of modules generated by the ScopMapper */
  for (ScopMapper::module_iterator
       m = SM->modules_begin(), me = SM->modules_end(); m != me; ++m)
    Mods.insert(*m);

  /* Remove bodies of cloned functions, we will link in an instrumented
   * version of it. */
  for (ScopMapper::iterator
       F = SM->begin(), FE = SM->end(); F != FE; ++F) {
   (*F)->deleteBody();
  }

  FPM->doFinalization();
  delete FPM;

  StoreModule(M, M.getModuleIdentifier() + ".extr");
};

int PolyJIT::runMain(const std::vector<std::string> &inputArgs,
                     const char * const *envp) {
  Function *Main = M.getFunction(EntryFn);

  if (!Main) {
    errs() << '\'' << EntryFn << "\' function not found in module.\n";
    return -1;
  }

  // Run static constructors.
  EE.runStaticConstructorsDestructors(false);
  // Trigger compilation separately so code regions that need to be
  // invalidated will be known.
  //(void)EE.getPointerToFunction(Main);

  /* Preoptimize our module for polly */
  runPollyPreoptimizationPasses(M);

  /* Extract suitable Scops */
  extractJitableScops(M);

  /* Instrument extracted Scops with a callback */
//  instrumentScops(M, Mods);

  /* Store temporary files */
  StoreModules(Mods);

  /* Get the Scops back */
  linkJitableScops(Mods, M);

  /* Store module before execution */
  StoreModule(M, M.getModuleIdentifier() + ".final");

  /* Add a mapping to our JIT callback function. */
  return EE.runFunctionAsMain(Main, inputArgs, envp);
}

void PolyJIT::runPollyPreoptimizationPasses(Module &M) {
  registerCanonicalicationPasses(*FPM);

  FPM->doInitialization();

  outs() << "[polli] Phase I: Applying Preoptimization:\n";
  for (Module::iterator f = M.begin(), fe = M.end(); f != fe ; ++f) {
    if (f->isDeclaration())
      continue;

    outs().indent(2) << "PreOpt: " << (*f).getName() << "\n";
    FPM->run(*f);
  }
  FPM->doFinalization();
}

int PolyJIT::shutdown(int result) {
  LLVMContext &Context = M.getContext();

  // Run static destructors.
  EE.runStaticConstructorsDestructors(true);

  // If the program doesn't explicitly call exit, we will need the Exit
  // function later on to make an explicit call, so get the function now.
  Constant *Exit = M.getOrInsertFunction("exit", Type::getVoidTy(Context),
                                                 Type::getInt32Ty(Context),
                                                 NULL);

  // If the program didn't call exit explicitly, we should call it now.
  // This ensures that any atexit handlers get called correctly.
  if (Function *ExitF = dyn_cast<Function>(Exit)) {
    std::vector<GenericValue> Args;
    GenericValue ResultGV;
    ResultGV.IntVal = APInt(32, result);
    Args.push_back(ResultGV);
    EE.runFunction(ExitF, Args);
    errs() << "ERROR: exit(" << result << ") returned!\n";
    abort();
  } else {
    errs() << "ERROR: exit defined with wrong prototype!\n";
    abort();
  }
};

PolyJIT* PolyJIT::Instance = NULL;
PolyJIT* PolyJIT::Get(ExecutionEngine *EE, Module *M) {
  if (!Instance) {
    Instance = new PolyJIT(EE, M);
  }
  return Instance; 
};
} // end of llvm namespace