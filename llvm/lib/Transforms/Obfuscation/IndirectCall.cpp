//===- IndirectCall.cpp - Route direct calls through a pointer table ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// For every eligible direct call `call foo(args)` this pass loads the address
// of `foo` from a module-level table of function pointers and calls through
// that pointer instead:
//
//   @__indirect_call_table = internal global [N x ptr] [ ptr @a, ptr @b, ... ]
//   ...
//   %slot = getelementptr [N x ptr], ptr @__indirect_call_table, i32 0, i32 K
//   %fp   = load volatile ptr, ptr %slot
//   %r    = call <ty> %fp(args)
//
// The load is volatile so the optimizer cannot fold the table entry back into a
// direct call, which is what keeps the direct caller/callee edge out of the
// emitted call graph. The table slot for each callee is assigned from a
// seeded PRNG, so the layout is randomized yet reproducible for a fixed seed.
//
// The transform is semantics-preserving: the pointer loaded from slot K is
// exactly the original callee, and the new call copies the calling convention,
// parameter/return attributes, tail-call kind and debug location of the call it
// replaces.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/IndirectCall.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <random>

#define DEBUG_TYPE "indirect-call"

using namespace llvm;

namespace {

/// A call site paired with the table index assigned to its callee.
struct Site {
  CallInst *Call;
  unsigned Index;
};

/// True if `CI` is a direct call we can safely reroute through a pointer.
bool isEligible(const CallInst *CI) {
  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false; // Already indirect, or an inline-asm / ifunc callee.
  if (Callee->isIntrinsic())
    return false; // Intrinsics must be called by name, not by pointer.
  if (CI->isInlineAsm())
    return false;
  // musttail requires the call to stay in tail position with a matching
  // prototype; leave those alone rather than risk violating the constraint.
  if (CI->isMustTailCall())
    return false;
  // Operand bundles (e.g. "funclet", "deopt") carry semantics we would drop
  // when rebuilding the call, so skip any call that has them.
  if (CI->hasOperandBundles())
    return false;
  return true;
}

} // namespace

PreservedAnalyses IndirectCallPass::run(Module &M, ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();

  // Collect eligible call sites and the set of callees they reference, keeping
  // first-seen order so the result is deterministic before shuffling.
  SmallVector<CallInst *, 32> Calls;
  MapVector<Function *, unsigned> CalleeIndex;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *CI = dyn_cast<CallInst>(&I))
          if (isEligible(CI)) {
            Calls.push_back(CI);
            CalleeIndex.insert({CI->getCalledFunction(), 0});
          }

  if (Calls.empty())
    return PreservedAnalyses::all();

  // Order the unique callees, then shuffle to randomize their table slots.
  SmallVector<Function *, 16> Callees;
  Callees.reserve(CalleeIndex.size());
  for (auto &Entry : CalleeIndex)
    Callees.push_back(Entry.first);

  std::mt19937_64 Rng(Seed);
  std::shuffle(Callees.begin(), Callees.end(), Rng);

  for (unsigned I = 0, E = Callees.size(); I != E; ++I)
    CalleeIndex[Callees[I]] = I;

  // Build the function-pointer table: element I holds the address of Callees[I].
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  ArrayType *TableTy = ArrayType::get(PtrTy, Callees.size());
  SmallVector<Constant *, 16> Entries(Callees.begin(), Callees.end());
  auto *Table = new GlobalVariable(
      M, TableTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
      ConstantArray::get(TableTy, Entries), "__indirect_call_table");

  // Rewrite each call site to load its callee from the table and call through
  // the loaded pointer.
  unsigned Rewritten = 0;
  for (CallInst *CI : Calls) {
    Function *Callee = CI->getCalledFunction();
    unsigned Index = CalleeIndex[Callee];

    IRBuilder<> B(CI);
    Value *Slot =
        B.CreateConstInBoundsGEP2_32(TableTy, Table, 0, Index, "ind.slot");
    Value *FnPtr =
        B.CreateLoad(PtrTy, Slot, /*isVolatile=*/true, "ind.fp");

    SmallVector<Value *, 8> Args(CI->arg_begin(), CI->arg_end());
    CallInst *NewCI = B.CreateCall(Callee->getFunctionType(), FnPtr, Args);
    NewCI->setCallingConv(CI->getCallingConv());
    NewCI->setAttributes(CI->getAttributes());
    NewCI->setTailCallKind(CI->getTailCallKind());
    NewCI->setDebugLoc(CI->getDebugLoc());
    NewCI->takeName(CI);

    CI->replaceAllUsesWith(NewCI);
    CI->eraseFromParent();
    ++Rewritten;
  }

  LLVM_DEBUG(dbgs() << "indirect-call: rerouted " << Rewritten
                    << " call(s) through a table of " << Callees.size()
                    << " callee(s)\n");
  return PreservedAnalyses::none();
}
