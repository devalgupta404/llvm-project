//===- StripSignature.cpp - Strip static toolchain fingerprints -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Removes the toolchain fingerprints a module carries into the emitted object:
// the `llvm.ident` and `llvm.commandline` named metadata, the module
// identifier, and the source file name. None of these affect semantics; they
// only make a binary easier to attribute to a compiler/version.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/StripSignature.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "strip-signature"

using namespace llvm;

PreservedAnalyses StripSignaturePass::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;

  for (const char *Name : {"llvm.ident", "llvm.commandline"}) {
    if (NamedMDNode *NMD = M.getNamedMetadata(Name)) {
      M.eraseNamedMetadata(NMD);
      Changed = true;
    }
  }

  if (!M.getModuleIdentifier().empty()) {
    M.setModuleIdentifier("");
    Changed = true;
  }
  if (!M.getSourceFileName().empty()) {
    M.setSourceFileName("");
    Changed = true;
  }

  LLVM_DEBUG(dbgs() << "strip-signature: fingerprints "
                    << (Changed ? "removed\n" : "already absent\n"));
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
