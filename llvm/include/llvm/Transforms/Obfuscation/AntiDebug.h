//===- AntiDebug.h - Anti-debugging instrumentation ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Injects a Linux ptrace(PTRACE_TRACEME) self-attach check that runs before
// main via a global constructor. If a debugger already traces the process the
// attach fails and the program terminates, hindering dynamic analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_ANTIDEBUG_H
#define LLVM_TRANSFORMS_OBFUSCATION_ANTIDEBUG_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

struct AntiDebugPass : public PassInfoMixin<AntiDebugPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_ANTIDEBUG_H
