//===- Virtualization.h - VM-based code virtualization ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Replaces the body of eligible functions with a call to an in-module bytecode
// interpreter that executes a virtualized encoding of the original code. Only
// functions the interpreter can execute with identical semantics are
// virtualized; every other function is left untouched, so the transform can
// never change program behaviour.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_VIRTUALIZATION_H
#define LLVM_TRANSFORMS_OBFUSCATION_VIRTUALIZATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

struct VirtualizationPass : public PassInfoMixin<VirtualizationPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_VIRTUALIZATION_H
