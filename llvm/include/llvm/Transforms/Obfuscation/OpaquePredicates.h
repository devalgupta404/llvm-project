//===- OpaquePredicates.h - Opaque predicate insertion ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Guards every conditional branch with an opaque predicate that always
// evaluates to true but cannot be folded away statically, so the original
// branch condition is hidden behind arithmetic the optimizer must preserve.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_OPAQUEPREDICATES_H
#define LLVM_TRANSFORMS_OBFUSCATION_OPAQUEPREDICATES_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

struct OpaquePredicatesPass : public PassInfoMixin<OpaquePredicatesPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_OPAQUEPREDICATES_H
