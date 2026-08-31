//===- IndirectCall.h - Route direct calls through a pointer table -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Replaces direct calls with indirect calls that load the callee address from
// a module-level function-pointer table, so the static call graph no longer
// records a direct edge between caller and callee.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_INDIRECTCALL_H
#define LLVM_TRANSFORMS_OBFUSCATION_INDIRECTCALL_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

struct IndirectCallPass : public PassInfoMixin<IndirectCallPass> {
  uint64_t Seed;

  explicit IndirectCallPass(uint64_t Seed = 0xC0FFEE) : Seed(Seed) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_INDIRECTCALL_H
