//===- OpaquePredicates.cpp - Opaque predicate insertion ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// For every conditional branch `br cond, T, F` this pass rewrites the
// condition to `cond & P`, where P is an opaque predicate that is provably
// always true (x * (x + 1) is even for every integer x) but is computed from
// a value the optimizer cannot know, so it survives constant folding. The
// transform is semantics-preserving: P is always 1, so control flow is
// unchanged while the true condition is buried in arithmetic.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/OpaquePredicates.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "opaque-pred"

using namespace llvm;

namespace {

/// Return (creating on first use) the module-global integer whose runtime value
/// seeds the opaque predicate. A volatile load of it prevents the optimizer
/// from assuming a concrete value and folding the predicate away.
GlobalVariable *getOpaqueGlobal(Module &M) {
  const char *Name = "__obf_opaque_seed";
  if (auto *GV = M.getGlobalVariable(Name, /*AllowInternal=*/true))
    return GV;

  Type *I32 = Type::getInt32Ty(M.getContext());
  auto *GV = new GlobalVariable(M, I32, /*isConstant=*/false,
                                GlobalValue::InternalLinkage,
                                ConstantInt::get(I32, 1), Name);
  return GV;
}

/// Emit `((x * (x + 1)) & 1) == 0`, an i1 that is always true.
Value *emitOpaqueTrue(IRBuilder<> &B, GlobalVariable *Seed) {
  Type *I32 = B.getInt32Ty();
  Value *X = B.CreateLoad(I32, Seed, /*isVolatile=*/true, "opq.x");
  Value *XPlus1 = B.CreateAdd(X, ConstantInt::get(I32, 1));
  Value *Prod = B.CreateMul(X, XPlus1);
  Value *Low = B.CreateAnd(Prod, ConstantInt::get(I32, 1));
  return B.CreateICmpEQ(Low, ConstantInt::get(I32, 0), "opq.pred");
}

} // namespace

PreservedAnalyses OpaquePredicatesPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  SmallVector<BranchInst *, 16> Targets;
  for (BasicBlock &BB : F)
    if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator()))
      if (BI->isConditional())
        Targets.push_back(BI);

  if (Targets.empty())
    return PreservedAnalyses::all();

  GlobalVariable *Seed = getOpaqueGlobal(*F.getParent());
  for (BranchInst *BI : Targets) {
    IRBuilder<> B(BI);
    Value *Opaque = emitOpaqueTrue(B, Seed);
    Value *Guarded = B.CreateAnd(BI->getCondition(), Opaque, "opq.cond");
    BI->setCondition(Guarded);
  }

  LLVM_DEBUG(dbgs() << "opaque-pred: guarded " << Targets.size()
                    << " branches in " << F.getName() << "\n");
  return PreservedAnalyses::none();
}
