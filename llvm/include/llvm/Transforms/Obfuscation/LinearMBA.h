//===- LinearMBA.h - Linear MBA Obfuscation Pass -------------------------===//
//
// Linear Mixed Boolean-Arithmetic (MBA) obfuscation pass
// Replaces bitwise operations with complex per-bit reconstructions
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_OBFUSCATION_LINEARMBA_H
#define LLVM_TRANSFORMS_OBFUSCATION_LINEARMBA_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include <random>

namespace llvm {

struct LinearMBAPass : public PassInfoMixin<LinearMBAPass> {
  unsigned Cycles;
  uint64_t Seed;

  LinearMBAPass(unsigned Cycles = 1, uint64_t Seed = 0xC0FFEE);

  Value* replaceBitwiseWithMBA(BinaryOperator *BO, unsigned bitWidth,
                                IRBuilder<> &B, std::mt19937_64 &R);

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_OBFUSCATION_LINEARMBA_H
