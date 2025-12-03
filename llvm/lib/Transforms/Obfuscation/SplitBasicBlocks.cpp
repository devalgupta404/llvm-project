//===- SplitBasicBlock.cpp - SplitBasicBlokc Obfuscation pass--------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the split basic block pass with C++ exception handling
// support (Hikari-style).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/Split.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/CryptoUtils.h"

#define DEBUG_TYPE "split"

using namespace llvm;
using namespace std;

// Stats
STATISTIC(Split, "Basicblock splitted");

static cl::opt<int> SplitNum("split_num", cl::init(2),
                             cl::desc("Split <split_num> time each BB"));

namespace {
struct SplitBasicBlock : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  bool flag;

  SplitBasicBlock() : FunctionPass(ID) {}
  SplitBasicBlock(bool flag) : FunctionPass(ID) {
    
    this->flag = flag;
  }

  bool runOnFunction(Function &F) override;
  void split(Function *f);

  bool containsPHI(BasicBlock *b);
  bool containsExceptionHandling(BasicBlock *b);
  void shuffle(std::vector<int> &vec);
};
}

char SplitBasicBlock::ID = 0;
static RegisterPass<SplitBasicBlock> X("splitbbl", "BasicBlock splitting");

Pass *llvm::createSplitBasicBlock(bool flag) {
  return new SplitBasicBlock(flag);
}

bool SplitBasicBlock::runOnFunction(Function &F) {
  // Check if the number of applications is correct
  if (!((SplitNum > 1) && (SplitNum <= 10))) {
    errs()<<"Split application basic block percentage\
            -split_num=x must be 1 < x <= 10";
    return false;
  }

  Function *tmp = &F;

  // Do we obfuscate
  if (toObfuscate(flag, tmp, "split")) {
    split(tmp);
    ++Split;
  }

  return false;
}

void SplitBasicBlock::split(Function *f) {
  std::vector<BasicBlock *> origBB;
  int splitN = SplitNum;

  // Save all basic blocks
  for (Function::iterator I = f->begin(), IE = f->end(); I != IE; ++I) {
    origBB.push_back(&*I);
  }

  for (std::vector<BasicBlock *>::iterator I = origBB.begin(),
                                           IE = origBB.end();
       I != IE; ++I) {
    BasicBlock *curr = *I;

    // No need to split a 1 inst bb
    // Or ones containing a PHI node
    if (curr->size() < 2 || containsPHI(curr)) {
      continue;
    }

    // ===== EXCEPTION HANDLING: Skip landing pad blocks =====
    // Landing pad blocks must have the landingpad instruction as the first
    // non-PHI instruction. Splitting would break this invariant.
    if (curr->isLandingPad()) {
      DEBUG_WITH_TYPE("split", errs() << "split: Skipping landing pad block: " 
          << curr->getName() << "\n");
      continue;
    }

    // ===== EXCEPTION HANDLING: Skip blocks with exception-related instructions =====
    // We need to be careful not to split in the middle of exception handling code
    if (containsExceptionHandling(curr)) {
      DEBUG_WITH_TYPE("split", errs() << "split: Skipping block with exception handling: " 
          << curr->getName() << "\n");
      continue;
    }

    // Check splitN and current BB size
    if ((size_t)splitN > curr->size()) {
      splitN = curr->size() - 1;
    }

    // ===== EXCEPTION HANDLING: Don't split before invoke =====
    // If the block ends with invoke, we need to be careful.
    // The invoke terminator is fine, but we shouldn't create a situation
    // where splitting interferes with exception handling.
    Instruction *terminator = curr->getTerminator();
    bool endsWithInvoke = terminator && isa<InvokeInst>(terminator);
    
    // Calculate max split points (don't include the terminator position)
    size_t maxSplitPos = curr->size() - 1;
    if (endsWithInvoke) {
      // For invoke blocks, we can still split the non-terminator part
      // Just be more conservative
      if (maxSplitPos < 2) {
        DEBUG_WITH_TYPE("split", errs() << "split: Block with invoke too small to split: " 
            << curr->getName() << "\n");
        continue;
      }
    }

    // Generate splits point
    std::vector<int> test;
    for (unsigned i = 1; i < maxSplitPos; ++i) {
      test.push_back(i);
    }

    if (test.empty()) {
      continue;
    }

    // Shuffle
    if (test.size() != 1) {
      shuffle(test);
      std::sort(test.begin(), test.begin() + std::min((size_t)splitN, test.size()));
    }

    // Adjust splitN if necessary
    int effectiveSplitN = std::min((size_t)splitN, test.size());

    // Split
    BasicBlock::iterator it = curr->begin();
    BasicBlock *toSplit = curr;
    int last = 0;
    for (int i = 0; i < effectiveSplitN; ++i) {
      for (int j = 0; j < test[i] - last; ++j) {
        ++it;
        // Safety check: don't go past the end or to a terminator
        if (it == toSplit->end()) {
          break;
        }
      }
      last = test[i];
      
      if (it == toSplit->end()) {
        break;
      }
      
      // ===== EXCEPTION HANDLING: Don't split at exception-related instructions =====
      if (isa<LandingPadInst>(&*it) || isa<ResumeInst>(&*it) ||
          isa<CatchPadInst>(&*it) || isa<CatchReturnInst>(&*it) ||
          isa<CleanupPadInst>(&*it) || isa<CleanupReturnInst>(&*it) ||
          isa<InvokeInst>(&*it)) {
        DEBUG_WITH_TYPE("split", errs() << "split: Skipping split at exception instruction\n");
        continue;
      }
      
      if(toSplit->size() < 2)
        continue;
      toSplit = toSplit->splitBasicBlock(it, toSplit->getName() + ".split");
    }

    ++Split;
  }
}

bool SplitBasicBlock::containsPHI(BasicBlock *b) {
  for (BasicBlock::iterator I = b->begin(), IE = b->end(); I != IE; ++I) {
    if (isa<PHINode>(I)) {
      return true;
    }
  }
  return false;
}

// ===== EXCEPTION HANDLING: Check if block contains exception-related instructions =====
bool SplitBasicBlock::containsExceptionHandling(BasicBlock *b) {
  for (BasicBlock::iterator I = b->begin(), IE = b->end(); I != IE; ++I) {
    // Check for exception handling instructions that we shouldn't split around
    if (isa<LandingPadInst>(I) || isa<ResumeInst>(I) ||
        isa<CatchPadInst>(I) || isa<CatchReturnInst>(I) ||
        isa<CleanupPadInst>(I) || isa<CleanupReturnInst>(I) ||
        isa<CatchSwitchInst>(I)) {
      return true;
    }
  }
  return false;
}

void SplitBasicBlock::shuffle(std::vector<int> &vec) {
  int n = vec.size();
  for (int i = n - 1; i > 0; --i) {
    std::swap(vec[i], vec[cryptoutils->get_uint32_t() % (i + 1)]);
  }
}
