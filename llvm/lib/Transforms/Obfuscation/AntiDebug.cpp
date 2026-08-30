//===- AntiDebug.cpp - Anti-debugging instrumentation ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Emits a self-attach anti-debug check and runs it before main via a global
// constructor. On Linux a process may be ptrace-attached by only one tracer;
// the injected code calls ptrace(PTRACE_TRACEME) and, if that fails (a
// debugger already holds the slot), calls _exit(1). Nothing else changes.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/AntiDebug.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#define DEBUG_TYPE "anti-debug"

using namespace llvm;

// PTRACE_TRACEME request number in the Linux ptrace ABI.
static constexpr int PTRACE_TRACEME = 0;

PreservedAnalyses AntiDebugPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Injecting the check more than once would stack redundant constructors.
  if (M.getFunction("__obf_antidebug"))
    return PreservedAnalyses::all();

  LLVMContext &Ctx = M.getContext();
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *I8Ptr = PointerType::getUnqual(Ctx);
  Type *VoidTy = Type::getVoidTy(Ctx);

  // long ptrace(int request, int pid, void *addr, void *data);
  FunctionCallee Ptrace = M.getOrInsertFunction(
      "ptrace", FunctionType::get(I64, {I32, I32, I8Ptr, I8Ptr}, false));
  // void _exit(int status);
  FunctionCallee ExitFn =
      M.getOrInsertFunction("_exit", FunctionType::get(VoidTy, {I32}, false));

  auto *CheckTy = FunctionType::get(VoidTy, {}, false);
  auto *Check = Function::Create(CheckTy, GlobalValue::InternalLinkage,
                                 "__obf_antidebug", &M);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Check);
  BasicBlock *Detected = BasicBlock::Create(Ctx, "detected", Check);
  BasicBlock *Clean = BasicBlock::Create(Ctx, "clean", Check);

  IRBuilder<> B(Entry);
  Value *Null = ConstantPointerNull::get(cast<PointerType>(I8Ptr));
  Value *Res = B.CreateCall(
      Ptrace, {ConstantInt::get(I32, PTRACE_TRACEME), ConstantInt::get(I32, 0),
               Null, Null});
  // PTRACE_TRACEME returns -1 when the process is already being traced.
  Value *Traced = B.CreateICmpEQ(Res, ConstantInt::get(I64, -1));
  B.CreateCondBr(Traced, Detected, Clean);

  B.SetInsertPoint(Detected);
  CallInst *ExitCall = B.CreateCall(ExitFn, {ConstantInt::get(I32, 1)});
  ExitCall->setDoesNotReturn();
  B.CreateUnreachable();

  B.SetInsertPoint(Clean);
  B.CreateRetVoid();

  // Run before user constructors; priority 0 is the earliest conventional slot.
  appendToGlobalCtors(M, Check, /*Priority=*/0);

  LLVM_DEBUG(dbgs() << "anti-debug: injected ptrace self-attach check\n");
  return PreservedAnalyses::none();
}
