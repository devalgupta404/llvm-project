//===- StripSignature.h - Strip static toolchain fingerprints -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Removes identifying metadata a toolchain leaves in a module (compiler ident
// and command-line notes, module identifier, source file name) to reduce the
// static signature of the emitted object without altering program semantics.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_STRIPSIGNATURE_H
#define LLVM_TRANSFORMS_OBFUSCATION_STRIPSIGNATURE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

struct StripSignaturePass : public PassInfoMixin<StripSignaturePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_STRIPSIGNATURE_H
