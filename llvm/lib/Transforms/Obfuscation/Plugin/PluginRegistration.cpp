//===- PluginRegistration.cpp - Register obfuscation passes as plugin ----===//
//
// Unified registration for all OLLVM obfuscation passes
//
//===----------------------------------------------------------------------===//

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Obfuscation/BogusControlFlow.h"
#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Obfuscation/Split.h"
#include "llvm/Transforms/Obfuscation/Substitution.h"

using namespace llvm;

// Wrapper passes for new pass manager
namespace {

struct FlatteningPassWrapper : public PassInfoMixin<FlatteningPassWrapper> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    Pass *LegacyPass = createFlattening(true);
    FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
    bool Changed = FP->runOnFunction(F);
    delete LegacyPass;
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

struct SubstitutionPassWrapper : public PassInfoMixin<SubstitutionPassWrapper> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    Pass *LegacyPass = createSubstitution(true);
    FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
    bool Changed = FP->runOnFunction(F);
    delete LegacyPass;
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

struct SplitBasicBlockPassWrapper : public PassInfoMixin<SplitBasicBlockPassWrapper> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    Pass *LegacyPass = createSplitBasicBlock(true);
    FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
    bool Changed = FP->runOnFunction(F);
    delete LegacyPass;
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

struct BogusControlFlowPassWrapper : public PassInfoMixin<BogusControlFlowPassWrapper> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    Pass *LegacyPass = createBogus(true);
    FunctionPass *FP = static_cast<FunctionPass*>(LegacyPass);
    bool Changed = FP->runOnFunction(F);
    delete LegacyPass;
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

} // end anonymous namespace

// Plugin registration for opt
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "ObfuscationPasses", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "flattening") {
            FPM.addPass(FlatteningPassWrapper());
            return true;
          }
          if (Name == "substitution") {
            FPM.addPass(SubstitutionPassWrapper());
            return true;
          }
          if (Name == "split") {
            FPM.addPass(SplitBasicBlockPassWrapper());
            return true;
          }
          if (Name == "boguscf") {
            FPM.addPass(BogusControlFlowPassWrapper());
            return true;
          }
          return false;
        });
    }
  };
}
