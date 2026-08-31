//===- PDataStrip.cpp - Drop unwind-table fingerprints -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Win64 `.pdata`/`.xdata` sections carry the unwind and exception-handler
// records the backend emits for each function. Their presence and shape leak
// information about the compiler and the exception model in use.
//
// These sections are produced during code generation from a function's
// `uwtable` attribute and personality routine; they are not IR globals or
// metadata, so they cannot be "erased" directly. What an IR pass *can* do,
// without changing observable behaviour, is remove the inputs that force
// unnecessary records:
//
//   * `uwtable` forces an unwind entry even where one is not otherwise
//     required. LLVM only ever emits such an entry when it decides one is
//     needed, so dropping the attribute can only remove records the ABI does
//     not require - never a mandatory one. It is therefore always safe.
//
//   * A personality function pulls in a language-specific handler (recorded in
//     `.xdata`). It is only meaningful for functions that actually contain
//     exception-handling constructs, so on a function with none it is dead and
//     can be cleared.
//
// To stay conservative this pass only touches functions that contain no EH
// constructs at all (no invoke, landingpad, resume, or funclet pads). Fully
// removing every unwind record is not possible from IR alone - the OS ABI
// still requires entries for functions with real frames - and disabling
// exception handling wholesale (`/EHs-c-`, `/GS-`) is a front-end concern.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/PDataStrip.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "pdata-strip"

using namespace llvm;

namespace {

/// True if `F` contains any exception-handling construct, in which case its
/// unwind table and personality routine may be load-bearing and must be kept.
bool usesExceptionHandling(const Function &F) {
  for (const BasicBlock &BB : F) {
    if (BB.isEHPad())
      return true;
    const Instruction *T = BB.getTerminator();
    if (isa<InvokeInst>(T) || isa<ResumeInst>(T) || isa<CatchReturnInst>(T) ||
        isa<CleanupReturnInst>(T) || isa<CatchSwitchInst>(T))
      return true;
  }
  return false;
}

} // namespace

PreservedAnalyses PDataStripPass::run(Module &M, ModuleAnalysisManager &AM) {
  unsigned Stripped = 0;
  for (Function &F : M) {
    if (F.isDeclaration() || usesExceptionHandling(F))
      continue;

    bool Changed = false;
    if (F.hasUWTable()) {
      F.setUWTableKind(UWTableKind::None);
      Changed = true;
    }
    if (F.hasPersonalityFn()) {
      F.setPersonalityFn(nullptr);
      Changed = true;
    }
    if (Changed)
      ++Stripped;
  }

  if (!Stripped)
    return PreservedAnalyses::all();

  LLVM_DEBUG(dbgs() << "pdata-strip: cleared unwind/personality info on "
                    << Stripped << " function(s)\n");
  return PreservedAnalyses::none();
}
