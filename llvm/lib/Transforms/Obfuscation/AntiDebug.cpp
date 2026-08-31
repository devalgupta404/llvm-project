//===- AntiDebug.cpp - Anti-debugging instrumentation ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Emits a self-attach anti-debug check and runs it before main via a global
// constructor. The check is target aware:
//   - On Windows it calls IsDebuggerPresent() and, if a debugger is attached,
//     calls ExitProcess(1).
//   - Elsewhere (Linux and other ptrace systems) it calls
//     ptrace(PTRACE_TRACEME); a process may be traced by only one tracer, so if
//     that fails a debugger already holds the slot and the program _exit(1)s.
// Nothing else in the module changes.
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

namespace {

/// IsDebuggerPresent() != 0  ->  ExitProcess(1).
void emitWindowsCheck(Module &M, Function *Check) {
  LLVMContext &Ctx = M.getContext();
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *VoidTy = Type::getVoidTy(Ctx);

  // BOOL IsDebuggerPresent(void);  void ExitProcess(UINT);  (both kernel32)
  FunctionCallee IsDbg =
      M.getOrInsertFunction("IsDebuggerPresent", FunctionType::get(I32, {}, false));
  FunctionCallee ExitProc =
      M.getOrInsertFunction("ExitProcess", FunctionType::get(VoidTy, {I32}, false));

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Check);
  BasicBlock *Detected = BasicBlock::Create(Ctx, "detected", Check);
  BasicBlock *Clean = BasicBlock::Create(Ctx, "clean", Check);

  IRBuilder<> B(Entry);
  Value *Res = B.CreateCall(IsDbg, {});
  Value *Traced = B.CreateICmpNE(Res, ConstantInt::get(I32, 0));
  B.CreateCondBr(Traced, Detected, Clean);

  B.SetInsertPoint(Detected);
  CallInst *ExitCall = B.CreateCall(ExitProc, {ConstantInt::get(I32, 1)});
  ExitCall->setDoesNotReturn();
  B.CreateUnreachable();

  B.SetInsertPoint(Clean);
  B.CreateRetVoid();
}

/// ptrace(PTRACE_TRACEME) == -1  ->  _exit(1).
void emitPtraceCheck(Module &M, Function *Check) {
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
}

} // namespace

PreservedAnalyses AntiDebugPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Injecting the check more than once would stack redundant constructors.
  if (M.getFunction("__obf_antidebug"))
    return PreservedAnalyses::all();

  auto *CheckTy = FunctionType::get(Type::getVoidTy(M.getContext()), {}, false);
  auto *Check = Function::Create(CheckTy, GlobalValue::InternalLinkage,
                                 "__obf_antidebug", &M);

  if (M.getTargetTriple().isOSWindows())
    emitWindowsCheck(M, Check);
  else
    emitPtraceCheck(M, Check);

  // Run before user constructors; priority 0 is the earliest conventional slot.
  appendToGlobalCtors(M, Check, /*Priority=*/0);

  LLVM_DEBUG(dbgs() << "anti-debug: injected "
                    << (M.getTargetTriple().isOSWindows() ? "IsDebuggerPresent"
                                                          : "ptrace")
                    << " check\n");
  return PreservedAnalyses::none();
}
