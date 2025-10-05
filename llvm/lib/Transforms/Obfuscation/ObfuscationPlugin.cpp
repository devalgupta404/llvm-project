//===- ObfuscationPlugin.cpp - Unified plugin registration ---------------===//
//
// This file provides unified plugin registration for all obfuscation passes
//
//===----------------------------------------------------------------------===//

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Obfuscation/BogusControlFlow.h"
#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Obfuscation/Split.h"
#include "llvm/Transforms/Obfuscation/Substitution.h"

using namespace llvm;

// New Pass Manager wrappers
namespace {

struct FlatteningPass : public PassInfoMixin<FlatteningPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

struct SubstitutionPass : public PassInfoMixin<SubstitutionPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

struct SplitBasicBlockPass : public PassInfoMixin<SplitBasicBlockPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

struct BogusControlFlowPass : public PassInfoMixin<BogusControlFlowPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace

// Implementations
PreservedAnalyses FlatteningPass::run(Function &F, FunctionAnalysisManager &AM) {
  Pass *LegacyPass = createFlattening(true);
  FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
  bool Changed = FP->runOnFunction(F);
  delete LegacyPass;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses SubstitutionPass::run(Function &F, FunctionAnalysisManager &AM) {
  Pass *LegacyPass = createSubstitution(true);
  FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
  bool Changed = FP->runOnFunction(F);
  delete LegacyPass;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses SplitBasicBlockPass::run(Function &F, FunctionAnalysisManager &AM) {
  Pass *LegacyPass = createSplitBasicBlock(true);
  FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
  bool Changed = FP->runOnFunction(F);
  delete LegacyPass;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses BogusControlFlowPass::run(Function &F, FunctionAnalysisManager &AM) {
  Pass *LegacyPass = createBogus(true);
  FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
  bool Changed = FP->runOnFunction(F);
  delete LegacyPass;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// Unified plugin registration
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "ObfuscationPasses", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "flattening" || Name == "fla") {
            FPM.addPass(FlatteningPass());
            return true;
          }
          if (Name == "substitution" || Name == "sub") {
            FPM.addPass(SubstitutionPass());
            return true;
          }
          if (Name == "split" || Name == "splitbbl") {
            FPM.addPass(SplitBasicBlockPass());
            return true;
          }
          if (Name == "boguscf" || Name == "bcf") {
            FPM.addPass(BogusControlFlowPass());
            return true;
          }
          return false;
        });
    }
  };
}
