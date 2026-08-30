//===- Flattening.cpp - Flattening Obfuscation pass------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the flattening pass
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/CryptoUtils.h"
#include <cstring>

// Forward declarations (ProcessSwitchInst is in LowerSwitch.h)
namespace llvm {
  class LazyValueInfo;
  class AssumptionCache;
}

#define DEBUG_TYPE "flattening"

using namespace llvm;

// Stats
STATISTIC(Flattened, "Functions flattened");

namespace {
struct Flattening : public FunctionPass {
  static char ID;  // Pass identification, replacement for typeid
  bool flag;

  Flattening() : FunctionPass(ID) {}
  Flattening(bool flag) : FunctionPass(ID) { this->flag = flag; }

  bool runOnFunction(Function &F) override;
  bool flatten(Function *f);
};
}

char Flattening::ID = 0;
static RegisterPass<Flattening> X("flattening", "Call graph flattening");
Pass *llvm::createFlattening(bool flag) { return new Flattening(flag); }

bool Flattening::runOnFunction(Function &F) {
  Function *tmp = &F;
  // Do we obfuscate
  if (toObfuscate(flag, tmp, "fla")) {
    if (flatten(tmp)) {
      ++Flattened;
    }
  }

  return false;
}

bool Flattening::flatten(Function *f) {
  vector<BasicBlock *> origBB;
  BasicBlock *loopEntry;
  BasicBlock *loopEnd;
  LoadInst *load;
  SwitchInst *switchI;
  AllocaInst *switchVar;

  // SCRAMBLER
  char scrambling_key[16];
  bool use_scrambler = false;
  // Force initialization of cryptoutils if not already done
  if (cryptoutils.isConstructed()) {
    llvm::cryptoutils->get_bytes(scrambling_key, 16);
    use_scrambler = true;
  } else {
    // Fallback: use zeros if cryptoutils not available
    memset(scrambling_key, 0, 16);
  }

  // Helper lambda to scramble values or pass through
  auto scramble = [&](uint32_t val) -> uint32_t {
    return use_scrambler ? llvm::cryptoutils->scramble32(val, scrambling_key) : val;
  };
  // END OF SCRAMBLER

  // Lower switch - using modern LLVM utility
  SmallPtrSet<BasicBlock *, 16> DeleteList;
  for (BasicBlock &BB : *f) {
    if (SwitchInst *SI = dyn_cast<SwitchInst>(BB.getTerminator())) {
      ProcessSwitchInst(SI, DeleteList, nullptr, nullptr);
    }
  }
  for (BasicBlock *BB : DeleteList) {
    BB->eraseFromParent();
  }

  // Save all original BB
  for (Function::iterator i = f->begin(); i != f->end(); ++i) {
    BasicBlock *tmp = &*i;
    origBB.push_back(tmp);

    BasicBlock *bb = &*i;
    if (!bb->getTerminator()) {
      return false;
    }
    if (isa<InvokeInst>(bb->getTerminator())) {
      return false;
    }
  }

  // Nothing to flatten
  if (origBB.size() <= 1) {
    return false;
  }

  // Remove first BB
  origBB.erase(origBB.begin());

  // Get a pointer on the first BB
  Function::iterator tmp = f->begin();  //++tmp;
  BasicBlock *insert = &*tmp;

  // If main begin with an if
  BranchInst *br = NULL;
  if (isa<BranchInst>(insert->getTerminator())) {
    br = cast<BranchInst>(insert->getTerminator());
  }

  if ((br != NULL && br->isConditional()) ||
      insert->getTerminator()->getNumSuccessors() > 1) {

    // The goal: If the first BB has multiple instructions before a conditional branch,
    // split it so that there's a clean entry point.
    // In LLVM 22, splitBasicBlock(iterator, name, Before) has a Before parameter.
    // If Before=false (default): splits AT iterator, moving it to new block
    // If Before=true: splits BEFORE iterator, keeping it in original block

    Instruction *terminator = insert->getTerminator();

    // We want to split BEFORE the terminator if there are multiple instructions
    if (insert->size() > 1) {
      // With Before=false (default): Split AT terminator, moves it and everything after to new block
      // With Before=true: Split BEFORE terminator, keeps it in original, moves everything before to new block
      // We want the terminator to stay so we can erase it, so use Before=false (default)
      BasicBlock *tmpBB = insert->splitBasicBlock(terminator, "first");
      origBB.insert(origBB.begin(), tmpBB);
    }
  }

  // Remove jump - insert should still have its terminator
  if (insert->getTerminator()) {
    insert->getTerminator()->eraseFromParent();
  } else {
    return false;
  }

  // Create switch variable and set as it
  // NOTE: insert has no terminator now, so we need to insert at end()
  switchVar =
      new AllocaInst(Type::getInt32Ty(f->getContext()), 0, "switchVar", insert->end());
  new StoreInst(
      ConstantInt::get(Type::getInt32Ty(f->getContext()),
                       scramble(0)),
      switchVar, insert->end());

  // Create main loop
  loopEntry = BasicBlock::Create(f->getContext(), "loopEntry", f, insert);
  loopEnd = BasicBlock::Create(f->getContext(), "loopEnd", f, insert);

  // loopEntry has no terminator yet, use end()
  load = new LoadInst(Type::getInt32Ty(f->getContext()), switchVar, "switchVar", loopEntry->end());

  // Move first BB on top
  insert->moveBefore(loopEntry);
  // insert still has no terminator, use end()
  BranchInst::Create(loopEntry, insert->end());

  // loopEnd jump to loopEntry
  BranchInst::Create(loopEntry, loopEnd->end());

  BasicBlock *swDefault =
      BasicBlock::Create(f->getContext(), "switchDefault", f, loopEnd);
  BranchInst::Create(loopEnd, swDefault->end());

  // Create switch instruction itself and set condition
  switchI = SwitchInst::Create(&*f->begin(), swDefault, 0, loopEntry->end());
  switchI->setCondition(load);

  // Remove branch jump from 1st BB and make a jump to the while
  f->begin()->getTerminator()->eraseFromParent();

  BranchInst::Create(loopEntry, f->begin()->end());

  // Put all BB in the switch
  for (vector<BasicBlock *>::iterator b = origBB.begin(); b != origBB.end();
       ++b) {
    BasicBlock *i = *b;
    ConstantInt *numCase = NULL;

    // Move the BB inside the switch (only visual, no code logic)
    i->moveBefore(loopEnd);

    // Add case to switch
    numCase = cast<ConstantInt>(ConstantInt::get(
        switchI->getCondition()->getType(),
        scramble(switchI->getNumCases())));
    switchI->addCase(numCase, i);
  }

  // Recalculate switchVar
  for (vector<BasicBlock *>::iterator b = origBB.begin(); b != origBB.end();
       ++b) {
    BasicBlock *i = *b;
    ConstantInt *numCase = NULL;

    // Ret BB
    if (i->getTerminator()->getNumSuccessors() == 0) {
      continue;
    }

    // If it's a non-conditional jump
    if (i->getTerminator()->getNumSuccessors() == 1) {
      // Get successor and delete terminator
      BasicBlock *succ = i->getTerminator()->getSuccessor(0);
      i->getTerminator()->eraseFromParent();

      // Get next case
      numCase = switchI->findCaseDest(succ);

      // If next case == default case (switchDefault)
      if (numCase == NULL) {
        numCase = cast<ConstantInt>(
            ConstantInt::get(switchI->getCondition()->getType(),
                             scramble(switchI->getNumCases() - 1)));
      }

      // Update switchVar and jump to the end of loop
      // i has no terminator now (erased above), use end()
      new StoreInst(numCase, load->getPointerOperand(), i->end());
      BranchInst::Create(loopEnd, i->end());
      continue;
    }

    // If it's a conditional jump
    if (i->getTerminator()->getNumSuccessors() == 2) {
      // Get next cases
      ConstantInt *numCaseTrue =
          switchI->findCaseDest(i->getTerminator()->getSuccessor(0));
      ConstantInt *numCaseFalse =
          switchI->findCaseDest(i->getTerminator()->getSuccessor(1));

      // Check if next case == default case (switchDefault)
      if (numCaseTrue == NULL) {
        numCaseTrue = cast<ConstantInt>(
            ConstantInt::get(switchI->getCondition()->getType(),
                             scramble(switchI->getNumCases() - 1)));
      }

      if (numCaseFalse == NULL) {
        numCaseFalse = cast<ConstantInt>(
            ConstantInt::get(switchI->getCondition()->getType(),
                             scramble(switchI->getNumCases() - 1)));
      }

      // Create a SelectInst
      BranchInst *br = cast<BranchInst>(i->getTerminator());
      SelectInst *sel =
          SelectInst::Create(br->getCondition(), numCaseTrue, numCaseFalse, "",
                             i->getTerminator()->getIterator());

      // Erase terminator
      i->getTerminator()->eraseFromParent();

      // Update switchVar and jump to the end of loop
      // i has no terminator now (erased above), use end()
      new StoreInst(sel, load->getPointerOperand(), i->end());
      BranchInst::Create(loopEnd, i->end());
      continue;
    }
  }

  fixStack(f);

  return true;
}
