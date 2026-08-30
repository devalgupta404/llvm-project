//===- Virtualization.cpp - VM-based code virtualization ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass virtualizes eligible functions: it compiles the function body into
// a compact register-based bytecode stored as a private global, emits a single
// in-module interpreter that executes that bytecode, and rewrites the original
// function to marshal its arguments and call the interpreter. The function's
// logic then exists only as data interpreted at runtime, defeating static
// recovery of the original instruction stream.
//
// Correctness is guaranteed by construction: a function is virtualized only if
// every instruction in it belongs to the interpreter's supported subset.
// Anything else is left byte-for-byte unchanged.
//
// Supported subset:
//   - uniform integer width W in {8,16,32,64} for all data values;
//   - arithmetic/logic (add/sub/mul, u/s div and rem, and/or/xor, shl/lshr/ashr);
//   - integer comparisons (icmp) producing an i1 used by branches;
//   - arbitrary control flow: conditional/unconditional branches, multiple
//     blocks, loops, and PHI nodes;
//   - integer returns.
// PHI nodes are taken out of SSA by emitting per-edge register copies (routed
// through temporaries so parallel copies with cycles stay correct).
//
// Bytecode (little-endian; register operands are one byte, so a function may use
// at most 256 virtual registers; jump targets are absolute 4-byte offsets):
//   LDIMM  dst:u8 imm:i64          reg[dst] = imm
//   RET    src:u8                  return reg[src]
//   MOV    dst:u8 src:u8           reg[dst] = reg[src]
//   JMP    target:u32              pc = target
//   JZ     cond:u8 target:u32      if reg[cond] == 0 then pc = target
//   SELECT dst:u8 c:u8 a:u8 b:u8   reg[dst] = reg[c] ? reg[a] : reg[b]
//   <binop> dst:u8 a:u8 b:u8       reg[dst] = reg[a] <op> reg[b]  (0x10..0x1C)
//   <cmp>   dst:u8 a:u8 b:u8       reg[dst] = (reg[a] <pred> reg[b]) ? 1 : 0
//
// Registers are kept masked to W; signed operations (sdiv/srem/ashr and the
// signed comparisons) sign-extend their operands from W bits first. The mask
// and the sign-extension shift (64 - W) are passed in per function, so one
// interpreter serves every supported width.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/Virtualization.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#include <cstdint>

#define DEBUG_TYPE "virtualize"

using namespace llvm;

namespace {

enum VMOpcode : uint8_t {
  OP_LDIMM = 0x01,
  OP_RET = 0x02,
  OP_MOV = 0x03,
  OP_JMP = 0x04,
  OP_JZ = 0x05,
  OP_SELECT = 0x06,

  OP_ADD = 0x10,
  OP_SUB = 0x11,
  OP_MUL = 0x12,
  OP_UDIV = 0x13,
  OP_SDIV = 0x14,
  OP_UREM = 0x15,
  OP_SREM = 0x16,
  OP_AND = 0x17,
  OP_OR = 0x18,
  OP_XOR = 0x19,
  OP_SHL = 0x1A,
  OP_LSHR = 0x1B,
  OP_ASHR = 0x1C,

  OP_CMP_EQ = 0x20,
  OP_CMP_NE = 0x21,
  OP_CMP_ULT = 0x22,
  OP_CMP_ULE = 0x23,
  OP_CMP_UGT = 0x24,
  OP_CMP_UGE = 0x25,
  OP_CMP_SLT = 0x26,
  OP_CMP_SLE = 0x27,
  OP_CMP_SGT = 0x28,
  OP_CMP_SGE = 0x29,
};

bool opcodeForBinOp(const BinaryOperator &BO, VMOpcode &Out) {
  switch (BO.getOpcode()) {
  case Instruction::Add:  Out = OP_ADD;  return true;
  case Instruction::Sub:  Out = OP_SUB;  return true;
  case Instruction::Mul:  Out = OP_MUL;  return true;
  case Instruction::UDiv: Out = OP_UDIV; return true;
  case Instruction::SDiv: Out = OP_SDIV; return true;
  case Instruction::URem: Out = OP_UREM; return true;
  case Instruction::SRem: Out = OP_SREM; return true;
  case Instruction::And:  Out = OP_AND;  return true;
  case Instruction::Or:   Out = OP_OR;   return true;
  case Instruction::Xor:  Out = OP_XOR;  return true;
  case Instruction::Shl:  Out = OP_SHL;  return true;
  case Instruction::LShr: Out = OP_LSHR; return true;
  case Instruction::AShr: Out = OP_ASHR; return true;
  default: return false;
  }
}

bool opcodeForCmp(ICmpInst::Predicate P, VMOpcode &Out) {
  switch (P) {
  case ICmpInst::ICMP_EQ:  Out = OP_CMP_EQ;  return true;
  case ICmpInst::ICMP_NE:  Out = OP_CMP_NE;  return true;
  case ICmpInst::ICMP_ULT: Out = OP_CMP_ULT; return true;
  case ICmpInst::ICMP_ULE: Out = OP_CMP_ULE; return true;
  case ICmpInst::ICMP_UGT: Out = OP_CMP_UGT; return true;
  case ICmpInst::ICMP_UGE: Out = OP_CMP_UGE; return true;
  case ICmpInst::ICMP_SLT: Out = OP_CMP_SLT; return true;
  case ICmpInst::ICMP_SLE: Out = OP_CMP_SLE; return true;
  case ICmpInst::ICMP_SGT: Out = OP_CMP_SGT; return true;
  case ICmpInst::ICMP_SGE: Out = OP_CMP_SGE; return true;
  default: return false;
  }
}

bool isSupportedWidth(unsigned W) {
  return W == 8 || W == 16 || W == 32 || W == 64;
}

/// Compiles one function to VM bytecode. Registers are assigned in a validation
/// pre-pass so a value defined in one block can be referenced from another;
/// blocks are then serialized with PHI nodes lowered to per-edge copies.
class VMFunctionEncoder {
public:
  bool encode(Function &F);

  ArrayRef<uint8_t> bytecode() const { return Bytecode; }
  unsigned numRegisters() const { return NextReg; }
  unsigned width() const { return Width; }

private:
  bool allocReg(unsigned &Out);
  bool getReg(Value *V, unsigned &Out) const;

  // Validation + register assignment. Returns false if F is out of subset.
  bool assignRegisters(Function &F);
  // Serialize a block's straight-line body and terminator.
  void emitBlock(BasicBlock &BB);
  // Emit the register copies that realize B's PHIs when entered from Pred.
  void emitEdgeCopies(BasicBlock *Pred, BasicBlock *Succ);

  void emitU8(unsigned V) { Bytecode.push_back(static_cast<uint8_t>(V)); }
  void emitU32(uint32_t V) {
    for (unsigned i = 0; i < 4; ++i)
      emitU8((V >> (8 * i)) & 0xFF);
  }
  // Emit a jump/branch target to a block, patched once all offsets are known.
  void emitBlockTarget(BasicBlock *BB) {
    Fixups.push_back({Bytecode.size(), BB});
    emitU32(0);
  }
  void patchU32(size_t Pos, uint32_t V) {
    for (unsigned i = 0; i < 4; ++i)
      Bytecode[Pos + i] = (V >> (8 * i)) & 0xFF;
  }

  DenseMap<Value *, unsigned> Regs;
  DenseMap<BasicBlock *, uint32_t> BlockOffset;
  SmallVector<std::pair<size_t, BasicBlock *>, 16> Fixups;
  SmallVector<ConstantInt *, 16> Constants;
  SmallVector<unsigned, 8> TempPool; // scratch registers for parallel copies
  SmallVector<uint8_t, 256> Bytecode;
  unsigned NextReg = 0;
  unsigned Width = 0;
};

bool VMFunctionEncoder::allocReg(unsigned &Out) {
  if (NextReg >= 256)
    return false;
  Out = NextReg++;
  return true;
}

bool VMFunctionEncoder::getReg(Value *V, unsigned &Out) const {
  auto It = Regs.find(V);
  if (It == Regs.end())
    return false;
  Out = It->second;
  return true;
}

bool VMFunctionEncoder::assignRegisters(Function &F) {
  for (Argument &A : F.args()) {
    auto *ArgTy = dyn_cast<IntegerType>(A.getType());
    if (!ArgTy || ArgTy->getBitWidth() != Width)
      return false;
    unsigned Reg;
    if (!allocReg(Reg))
      return false;
    Regs[&A] = Reg;
  }

  unsigned MaxPhis = 0;
  for (BasicBlock &BB : F) {
    unsigned PhiCount = 0;
    for (Instruction &I : BB) {
      if (auto *Phi = dyn_cast<PHINode>(&I)) {
        if (!Phi->getType()->isIntegerTy(Width))
          return false;
        ++PhiCount;
        unsigned Reg;
        if (!allocReg(Reg))
          return false;
        Regs[Phi] = Reg;
        continue;
      }
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        VMOpcode Op;
        if (!BO->getType()->isIntegerTy(Width) || !opcodeForBinOp(*BO, Op))
          return false;
        unsigned Reg;
        if (!allocReg(Reg))
          return false;
        Regs[BO] = Reg;
        continue;
      }
      if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
        VMOpcode Op;
        if (!Cmp->getOperand(0)->getType()->isIntegerTy(Width) ||
            !opcodeForCmp(Cmp->getPredicate(), Op))
          return false;
        unsigned Reg;
        if (!allocReg(Reg))
          return false;
        Regs[Cmp] = Reg;
        continue;
      }
      if (auto *Sel = dyn_cast<SelectInst>(&I)) {
        // Condition must be an icmp so it already occupies a register.
        if (!Sel->getType()->isIntegerTy(Width) ||
            !isa<ICmpInst>(Sel->getCondition()))
          return false;
        unsigned Reg;
        if (!allocReg(Reg))
          return false;
        Regs[Sel] = Reg;
        continue;
      }
      if (isa<BranchInst>(&I) || isa<ReturnInst>(&I))
        continue;
      // Any other instruction is outside the supported subset.
      return false;
    }
    MaxPhis = std::max(MaxPhis, PhiCount);
  }

  // Validate terminators up front so emitBlock can assume they are supported.
  for (BasicBlock &BB : F) {
    Instruction *T = BB.getTerminator();
    if (auto *RI = dyn_cast<ReturnInst>(T)) {
      Value *RV = RI->getReturnValue();
      if (!RV || !RV->getType()->isIntegerTy(Width))
        return false;
    } else if (auto *BI = dyn_cast<BranchInst>(T)) {
      if (BI->isConditional() && !isa<ICmpInst>(BI->getCondition()))
        return false;
    } else {
      return false;
    }
  }

  // Constants are loaded once in the entry prologue, where they dominate every
  // use, so a constant referenced only on a branch edge is still initialized.
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        if (auto *CI = dyn_cast<ConstantInt>(Op))
          if (CI->getType()->isIntegerTy(Width) && !Regs.count(CI)) {
            unsigned Reg;
            if (!allocReg(Reg))
              return false;
            Regs[CI] = Reg;
            Constants.push_back(CI);
          }

  for (unsigned i = 0; i < MaxPhis; ++i) {
    unsigned Reg;
    if (!allocReg(Reg))
      return false;
    TempPool.push_back(Reg);
  }

  return true;
}

void VMFunctionEncoder::emitEdgeCopies(BasicBlock *Pred, BasicBlock *Succ) {
  SmallVector<std::pair<unsigned, unsigned>, 8> Copies; // (dst phi reg, src reg)
  for (PHINode &Phi : Succ->phis()) {
    unsigned Dst = Regs[&Phi];
    unsigned Src;
    bool Ok = getReg(Phi.getIncomingValueForBlock(Pred), Src);
    assert(Ok && "phi incoming value must have a register");
    (void)Ok;
    if (Dst != Src)
      Copies.push_back({Dst, Src});
  }
  if (Copies.empty())
    return;

  // Route through temporaries so parallel copies (including cycles) are safe:
  // read all sources first, then write all destinations.
  for (size_t i = 0; i < Copies.size(); ++i) {
    emitU8(OP_MOV);
    emitU8(TempPool[i]);
    emitU8(Copies[i].second);
  }
  for (size_t i = 0; i < Copies.size(); ++i) {
    emitU8(OP_MOV);
    emitU8(Copies[i].first);
    emitU8(TempPool[i]);
  }
}

void VMFunctionEncoder::emitBlock(BasicBlock &BB) {
  BlockOffset[&BB] = Bytecode.size();

  for (Instruction &I : BB) {
    if (isa<PHINode>(&I) || I.isTerminator())
      continue;
    unsigned Dst = Regs[&I];
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      VMOpcode Op;
      opcodeForBinOp(*BO, Op);
      unsigned A, B;
      getReg(BO->getOperand(0), A);
      getReg(BO->getOperand(1), B);
      emitU8(Op);
      emitU8(Dst);
      emitU8(A);
      emitU8(B);
    } else if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
      VMOpcode Op;
      opcodeForCmp(Cmp->getPredicate(), Op);
      unsigned A, B;
      getReg(Cmp->getOperand(0), A);
      getReg(Cmp->getOperand(1), B);
      emitU8(Op);
      emitU8(Dst);
      emitU8(A);
      emitU8(B);
    } else if (auto *Sel = dyn_cast<SelectInst>(&I)) {
      unsigned C, TrueR, FalseR;
      getReg(Sel->getCondition(), C);
      getReg(Sel->getTrueValue(), TrueR);
      getReg(Sel->getFalseValue(), FalseR);
      emitU8(OP_SELECT);
      emitU8(Dst);
      emitU8(C);
      emitU8(TrueR);
      emitU8(FalseR);
    }
  }

  Instruction *T = BB.getTerminator();
  if (auto *RI = dyn_cast<ReturnInst>(T)) {
    unsigned Src;
    getReg(RI->getReturnValue(), Src);
    emitU8(OP_RET);
    emitU8(Src);
    return;
  }

  auto *BI = cast<BranchInst>(T);
  if (BI->isUnconditional()) {
    BasicBlock *Succ = BI->getSuccessor(0);
    emitEdgeCopies(&BB, Succ);
    emitU8(OP_JMP);
    emitBlockTarget(Succ);
    return;
  }

  // Conditional branch: JZ falls through to the taken-edge copies, so each
  // edge's PHI copies run only when that edge is taken (this also handles
  // critical edges without splitting them).
  unsigned Cond;
  getReg(BI->getCondition(), Cond);
  BasicBlock *T0 = BI->getSuccessor(0); // taken when cond != 0
  BasicBlock *F0 = BI->getSuccessor(1); // taken when cond == 0
  emitU8(OP_JZ);
  emitU8(Cond);
  size_t FalsePatch = Bytecode.size();
  emitU32(0);

  emitEdgeCopies(&BB, T0);
  emitU8(OP_JMP);
  emitBlockTarget(T0);

  patchU32(FalsePatch, static_cast<uint32_t>(Bytecode.size()));
  emitEdgeCopies(&BB, F0);
  emitU8(OP_JMP);
  emitBlockTarget(F0);
}

bool VMFunctionEncoder::encode(Function &F) {
  if (F.isDeclaration() || F.isVarArg() || F.empty())
    return false;

  auto *RetTy = dyn_cast<IntegerType>(F.getReturnType());
  if (!RetTy || !isSupportedWidth(RetTy->getBitWidth()))
    return false;
  Width = RetTy->getBitWidth();

  if (!assignRegisters(F))
    return false;

  for (ConstantInt *CI : Constants) {
    emitU8(OP_LDIMM);
    emitU8(Regs[CI]);
    uint64_t Imm = CI->getValue().getZExtValue();
    for (unsigned i = 0; i < 8; ++i)
      emitU8((Imm >> (8 * i)) & 0xFF);
  }

  for (BasicBlock &BB : F)
    emitBlock(BB);

  for (auto &Fix : Fixups)
    patchU32(Fix.first, BlockOffset[Fix.second]);

  return !Bytecode.empty();
}

/// Build (or return) the shared bytecode interpreter:
///   i64 @__obf_vm_run(ptr bc, ptr regs, i64 mask, i64 signShift)
Function *getOrCreateInterpreter(Module &M) {
  const char *Name = "__obf_vm_run";
  if (Function *Existing = M.getFunction(Name))
    return Existing;

  LLVMContext &Ctx = M.getContext();
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Ptr = PointerType::getUnqual(Ctx);

  auto *FnTy = FunctionType::get(I64, {Ptr, Ptr, I64, I64}, false);
  auto *F = Function::Create(FnTy, GlobalValue::InternalLinkage, Name, &M);
  Argument *BC = F->getArg(0);
  Argument *RegsArg = F->getArg(1);
  Argument *Mask = F->getArg(2);
  Argument *SignShift = F->getArg(3);
  BC->setName("bc");
  RegsArg->setName("regs");
  Mask->setName("mask");
  SignShift->setName("sh");

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *Loop = BasicBlock::Create(Ctx, "loop", F);
  BasicBlock *Bad = BasicBlock::Create(Ctx, "bad", F);

  IRBuilder<> B(Entry);
  AllocaInst *PC = B.CreateAlloca(I64, nullptr, "pc");
  B.CreateStore(ConstantInt::get(I64, 0), PC);
  B.CreateBr(Loop);

  B.SetInsertPoint(Loop);
  Value *P = B.CreateLoad(I64, PC, "p");
  Value *OpPtr = B.CreateGEP(I8, BC, P);
  Value *Op = B.CreateLoad(I8, OpPtr, "op");
  SwitchInst *Sw = B.CreateSwitch(Op, Bad, 26);

  B.SetInsertPoint(Bad);
  B.CreateRet(ConstantInt::get(I64, 0));

  auto byteAt = [&](IRBuilder<> &IB, uint64_t Off) -> Value * {
    Value *Idx = IB.CreateAdd(P, ConstantInt::get(I64, Off));
    Value *Bp = IB.CreateGEP(I8, BC, Idx);
    return IB.CreateZExt(IB.CreateLoad(I8, Bp), I64);
  };
  auto word32At = [&](IRBuilder<> &IB, uint64_t Off) -> Value * {
    Value *Idx = IB.CreateAdd(P, ConstantInt::get(I64, Off));
    Value *Wp = IB.CreateGEP(I8, BC, Idx);
    LoadInst *L = IB.CreateLoad(I32, Wp);
    L->setAlignment(Align(1));
    return IB.CreateZExt(L, I64);
  };
  auto regPtr = [&](IRBuilder<> &IB, Value *Idx) {
    return IB.CreateGEP(I64, RegsArg, Idx);
  };
  auto loadReg = [&](IRBuilder<> &IB, Value *Idx) {
    return IB.CreateLoad(I64, regPtr(IB, Idx));
  };
  auto advance = [&](IRBuilder<> &IB, uint64_t Size) {
    IB.CreateStore(IB.CreateAdd(P, ConstantInt::get(I64, Size)), PC);
    IB.CreateBr(Loop);
  };
  auto jumpTo = [&](IRBuilder<> &IB, Value *Target) {
    IB.CreateStore(Target, PC);
    IB.CreateBr(Loop);
  };
  auto signExtend = [&](IRBuilder<> &IB, Value *V) {
    return IB.CreateAShr(IB.CreateShl(V, SignShift), SignShift);
  };
  auto newCase = [&](VMOpcode Code, const char *Nm) {
    BasicBlock *Case = BasicBlock::Create(Ctx, Nm, F);
    Sw->addCase(ConstantInt::get(cast<IntegerType>(I8), Code), Case);
    return Case;
  };

  // LDIMM dst, imm
  {
    IRBuilder<> IB(newCase(OP_LDIMM, "ldimm"));
    Value *Dst = byteAt(IB, 1);
    Value *ImmPtr =
        IB.CreateGEP(I8, BC, IB.CreateAdd(P, ConstantInt::get(I64, 2)));
    LoadInst *Imm = IB.CreateLoad(I64, ImmPtr);
    Imm->setAlignment(Align(1));
    IB.CreateStore(Imm, regPtr(IB, Dst));
    advance(IB, 10);
  }
  // RET src
  {
    IRBuilder<> IB(newCase(OP_RET, "ret"));
    IB.CreateRet(loadReg(IB, byteAt(IB, 1)));
  }
  // MOV dst, src
  {
    IRBuilder<> IB(newCase(OP_MOV, "mov"));
    Value *Dst = byteAt(IB, 1);
    Value *V = loadReg(IB, byteAt(IB, 2));
    IB.CreateStore(V, regPtr(IB, Dst));
    advance(IB, 3);
  }
  // JMP target
  {
    IRBuilder<> IB(newCase(OP_JMP, "jmp"));
    jumpTo(IB, word32At(IB, 1));
  }
  // JZ cond, target
  {
    IRBuilder<> IB(newCase(OP_JZ, "jz"));
    Value *Cond = loadReg(IB, byteAt(IB, 1));
    Value *Target = word32At(IB, 2);
    Value *Zero = IB.CreateICmpEQ(Cond, ConstantInt::get(I64, 0));
    Value *Fall = IB.CreateAdd(P, ConstantInt::get(I64, 6));
    jumpTo(IB, IB.CreateSelect(Zero, Target, Fall));
  }
  // SELECT dst, cond, a, b : reg[dst] = reg[cond] ? reg[a] : reg[b]
  {
    IRBuilder<> IB(newCase(OP_SELECT, "select"));
    Value *Dst = byteAt(IB, 1);
    Value *Cond = loadReg(IB, byteAt(IB, 2));
    Value *Va = loadReg(IB, byteAt(IB, 3));
    Value *Vb = loadReg(IB, byteAt(IB, 4));
    Value *Taken = IB.CreateICmpNE(Cond, ConstantInt::get(I64, 0));
    IB.CreateStore(IB.CreateSelect(Taken, Va, Vb), regPtr(IB, Dst));
    advance(IB, 5);
  }

  auto binaryCase = [&](VMOpcode Code, const char *Nm,
                        function_ref<Value *(IRBuilder<> &, Value *, Value *)>
                            Compute) {
    IRBuilder<> IB(newCase(Code, Nm));
    Value *Dst = byteAt(IB, 1);
    Value *Va = loadReg(IB, byteAt(IB, 2));
    Value *Vb = loadReg(IB, byteAt(IB, 3));
    Value *R = IB.CreateAnd(Compute(IB, Va, Vb), Mask);
    IB.CreateStore(R, regPtr(IB, Dst));
    advance(IB, 4);
  };
  auto cmpCase = [&](VMOpcode Code, const char *Nm, ICmpInst::Predicate Pred,
                     bool Signed) {
    IRBuilder<> IB(newCase(Code, Nm));
    Value *Dst = byteAt(IB, 1);
    Value *Va = loadReg(IB, byteAt(IB, 2));
    Value *Vb = loadReg(IB, byteAt(IB, 3));
    if (Signed) {
      Va = signExtend(IB, Va);
      Vb = signExtend(IB, Vb);
    }
    Value *R = IB.CreateZExt(IB.CreateICmp(Pred, Va, Vb), I64);
    IB.CreateStore(R, regPtr(IB, Dst));
    advance(IB, 4);
  };

  binaryCase(OP_ADD, "add", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateAdd(A, V);
  });
  binaryCase(OP_SUB, "sub", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateSub(A, V);
  });
  binaryCase(OP_MUL, "mul", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateMul(A, V);
  });
  binaryCase(OP_UDIV, "udiv", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateUDiv(A, V);
  });
  binaryCase(OP_UREM, "urem", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateURem(A, V);
  });
  binaryCase(OP_AND, "and", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateAnd(A, V);
  });
  binaryCase(OP_OR, "or", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateOr(A, V);
  });
  binaryCase(OP_XOR, "xor", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateXor(A, V);
  });
  binaryCase(OP_SHL, "shl", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateShl(A, V);
  });
  binaryCase(OP_LSHR, "lshr", [](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateLShr(A, V);
  });
  binaryCase(OP_SDIV, "sdiv", [&](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateSDiv(signExtend(IB, A), signExtend(IB, V));
  });
  binaryCase(OP_SREM, "srem", [&](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateSRem(signExtend(IB, A), signExtend(IB, V));
  });
  binaryCase(OP_ASHR, "ashr", [&](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateAShr(signExtend(IB, A), V);
  });

  cmpCase(OP_CMP_EQ, "cmp.eq", ICmpInst::ICMP_EQ, false);
  cmpCase(OP_CMP_NE, "cmp.ne", ICmpInst::ICMP_NE, false);
  cmpCase(OP_CMP_ULT, "cmp.ult", ICmpInst::ICMP_ULT, false);
  cmpCase(OP_CMP_ULE, "cmp.ule", ICmpInst::ICMP_ULE, false);
  cmpCase(OP_CMP_UGT, "cmp.ugt", ICmpInst::ICMP_UGT, false);
  cmpCase(OP_CMP_UGE, "cmp.uge", ICmpInst::ICMP_UGE, false);
  cmpCase(OP_CMP_SLT, "cmp.slt", ICmpInst::ICMP_SLT, true);
  cmpCase(OP_CMP_SLE, "cmp.sle", ICmpInst::ICMP_SLE, true);
  cmpCase(OP_CMP_SGT, "cmp.sgt", ICmpInst::ICMP_SGT, true);
  cmpCase(OP_CMP_SGE, "cmp.sge", ICmpInst::ICMP_SGE, true);

  return F;
}

/// Replace F's body with argument marshalling and a call to the interpreter.
void rewriteFunction(Function &F, Function *Interp, GlobalVariable *BCGlobal,
                     unsigned NumRegs, unsigned Width) {
  LLVMContext &Ctx = F.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  auto *RetTy = cast<IntegerType>(F.getReturnType());

  F.deleteBody();
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", &F);
  IRBuilder<> B(Entry);

  auto *RegArrTy = ArrayType::get(I64, NumRegs);
  AllocaInst *Regs = B.CreateAlloca(RegArrTy, nullptr, "vm.regs");
  unsigned Idx = 0;
  for (Argument &A : F.args()) {
    Value *Slot = B.CreateConstInBoundsGEP2_64(RegArrTy, Regs, 0, Idx++);
    B.CreateStore(B.CreateZExt(&A, I64), Slot);
  }

  Value *RegsPtr = B.CreateConstInBoundsGEP2_64(RegArrTy, Regs, 0, 0);
  uint64_t Mask = Width == 64 ? ~0ULL : ((1ULL << Width) - 1);
  Value *Result =
      B.CreateCall(Interp, {BCGlobal, RegsPtr, ConstantInt::get(I64, Mask),
                            ConstantInt::get(I64, 64 - Width)});
  B.CreateRet(B.CreateTrunc(Result, RetTy));
}

} // namespace

PreservedAnalyses VirtualizationPass::run(Module &M, ModuleAnalysisManager &AM) {
  SmallVector<Function *, 8> Candidates;
  for (Function &F : M)
    if (!F.isDeclaration() && F.getName() != "__obf_vm_run")
      Candidates.push_back(&F);

  bool Changed = false;
  for (Function *F : Candidates) {
    VMFunctionEncoder Enc;
    if (!Enc.encode(*F))
      continue;

    auto *BCData = ConstantDataArray::get(M.getContext(), Enc.bytecode());
    auto *BCGlobal = new GlobalVariable(
        M, BCData->getType(), /*isConstant=*/true, GlobalValue::PrivateLinkage,
        BCData, "__obf_vm_bc_" + F->getName());
    BCGlobal->setAlignment(Align(1));

    Function *Interp = getOrCreateInterpreter(M);
    rewriteFunction(*F, Interp, BCGlobal, Enc.numRegisters(), Enc.width());
    Changed = true;
    LLVM_DEBUG(dbgs() << "virtualize: " << F->getName() << " -> "
                      << Enc.bytecode().size() << " bytes, "
                      << Enc.numRegisters() << " regs\n");
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
