//===- PluginRegistration.cpp - Register MLIR obfuscation passes ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Loadable pass-plugin entry point for the MLIR obfuscation passes. Load it
// into mlir-opt with `--load-pass-plugin=MLIRObfuscationPlugin.so`; the passes
// are then available by their text arguments (string-encrypt, symbol-obfuscate,
// constant-obfuscate, crypto-hash, scf-obfuscate, import-obfuscate).
//
//===----------------------------------------------------------------------===//

#include "mlir/Transforms/Obfuscation/Passes.h"

#include "mlir/Pass/PassRegistry.h"
#include "mlir/Tools/Plugins/PassPlugin.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Compiler.h"

using namespace mlir;

static void registerObfuscationPasses() {
  PassRegistration<obs::StringEncryptPass>();
  PassRegistration<obs::SymbolObfuscatePass>();
  PassRegistration<obs::ConstantObfuscationPass>();
  PassRegistration<obs::CryptoHashPass>();
  PassRegistration<obs::SCFObfuscatePass>();
  PassRegistration<obs::ImportObfuscationPass>();
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "MLIRObfuscation", LLVM_VERSION_STRING,
          registerObfuscationPasses};
}
