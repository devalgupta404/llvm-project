//===- PDataStrip.h - Drop unwind-table fingerprints --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Removes the unwind-table (`uwtable`) attribute and dead exception
// personality references from functions that contain no exception-handling
// constructs, so the backend emits fewer `.pdata`/`.xdata` unwind records that
// otherwise act as a toolchain fingerprint. See PDataStrip.cpp for the exact
// safety conditions and the limits of what an IR pass can strip.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_PDATASTRIP_H
#define LLVM_TRANSFORMS_OBFUSCATION_PDATASTRIP_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

struct PDataStripPass : public PassInfoMixin<PDataStripPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_PDATASTRIP_H
