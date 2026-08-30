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
// every instruction in it belongs to the interpreter's supported subset
// (uniform-width integer arithmetic/logic straight-line code). Anything else
// is left byte-for-byte unchanged.
//
// Bytecode encoding (little-endian, all register operands are one byte, so a
// function may use at most 256 virtual registers):
//   LDIMM dst:u8 imm:i64   (0x01)  reg[dst] = imm
//   RET   src:u8           (0x02)  return reg[src]
//   <binop> dst:u8 a:u8 b:u8       reg[dst] = reg[a] <op> reg[b]  (0x10..0x1C)
//
// The interpreter keeps every register masked to the function's integer width
// W; signed operations sign-extend their operands from W bits first. Both the
// mask and the sign-extension shift (64 - W) are passed in per function, so one
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
};

/// Map a supported integer binary operator to its VM opcode; return false for
/// anything the interpreter cannot execute.
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

bool isSupportedWidth(unsigned W) {
  return W == 8 || W == 16 || W == 32 || W == 64;
}

/// Compiles one function to VM bytecode, assigning a virtual register to every
/// argument, constant, and instruction result. Encoding only succeeds when the
/// whole function lies inside the supported subset.
class VMFunctionEncoder {
public:
  bool encode(Function &F);

  ArrayRef<uint8_t> bytecode() const { return Bytecode; }
  unsigned numRegisters() const { return NextReg; }
  unsigned width() const { return Width; }

private:
  // Returns the register holding V, allocating one (and emitting LDIMM for
  // constants) on first use. Fails for unsupported operands.
  bool regFor(Value *V, unsigned &Out);
  bool allocReg(unsigned &Out);

  DenseMap<Value *, unsigned> Regs;
  SmallVector<uint8_t, 128> Bytecode;
  unsigned NextReg = 0;
  unsigned Width = 0;
};

bool VMFunctionEncoder::allocReg(unsigned &Out) {
  if (NextReg >= 256)
    return false;
  Out = NextReg++;
  return true;
}

bool VMFunctionEncoder::regFor(Value *V, unsigned &Out) {
  auto It = Regs.find(V);
  if (It != Regs.end()) {
    Out = It->second;
    return true;
  }

  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    unsigned Reg;
    if (!allocReg(Reg))
      return false;
    Regs[V] = Reg;
    Bytecode.push_back(OP_LDIMM);
    Bytecode.push_back(static_cast<uint8_t>(Reg));
    uint64_t Imm = CI->getValue().getZExtValue();
    for (unsigned i = 0; i < 8; ++i)
      Bytecode.push_back(static_cast<uint8_t>(Imm >> (8 * i)));
    Out = Reg;
    return true;
  }

  // Arguments and instruction results must already have registers assigned
  // before their first use; anything else (globals, undef, ...) is unsupported.
  return false;
}

bool VMFunctionEncoder::encode(Function &F) {
  if (F.isDeclaration() || F.isVarArg())
    return false;
  if (F.size() != 1)
    return false;

  auto *RetTy = dyn_cast<IntegerType>(F.getReturnType());
  if (!RetTy || !isSupportedWidth(RetTy->getBitWidth()))
    return false;
  Width = RetTy->getBitWidth();

  // Every argument shares the working width and gets a register up front.
  for (Argument &A : F.args()) {
    auto *ArgTy = dyn_cast<IntegerType>(A.getType());
    if (!ArgTy || ArgTy->getBitWidth() != Width)
      return false;
    unsigned Reg;
    if (!allocReg(Reg))
      return false;
    Regs[&A] = Reg;
  }

  BasicBlock &BB = F.front();
  for (Instruction &I : BB) {
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      VMOpcode Op;
      if (!BO->getType()->isIntegerTy(Width) || !opcodeForBinOp(*BO, Op))
        return false;
      unsigned A, B;
      if (!regFor(BO->getOperand(0), A) || !regFor(BO->getOperand(1), B))
        return false;
      unsigned Dst;
      if (!allocReg(Dst))
        return false;
      Regs[BO] = Dst;
      Bytecode.push_back(Op);
      Bytecode.push_back(static_cast<uint8_t>(Dst));
      Bytecode.push_back(static_cast<uint8_t>(A));
      Bytecode.push_back(static_cast<uint8_t>(B));
      continue;
    }
    if (auto *RI = dyn_cast<ReturnInst>(&I)) {
      Value *RV = RI->getReturnValue();
      if (!RV || !RV->getType()->isIntegerTy(Width))
        return false;
      unsigned Reg;
      if (!regFor(RV, Reg))
        return false;
      Bytecode.push_back(OP_RET);
      Bytecode.push_back(static_cast<uint8_t>(Reg));
      continue;
    }
    // Any other instruction is outside the supported subset.
    return false;
  }

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

  // Loop header: fetch the opcode at bc[pc] and dispatch.
  B.SetInsertPoint(Loop);
  Value *P = B.CreateLoad(I64, PC, "p");
  Value *OpPtr = B.CreateGEP(I8, BC, P);
  Value *Op = B.CreateLoad(I8, OpPtr, "op");
  SwitchInst *Sw = B.CreateSwitch(Op, Bad, 15);

  // Malformed bytecode never reaches here for well-formed input.
  B.SetInsertPoint(Bad);
  B.CreateRet(ConstantInt::get(I64, 0));

  // Helpers shared by the case blocks.
  auto byteAt = [&](IRBuilder<> &IB, uint64_t Off) -> Value * {
    Value *Idx = IB.CreateAdd(P, ConstantInt::get(I64, Off));
    Value *Bp = IB.CreateGEP(I8, BC, Idx);
    return IB.CreateZExt(IB.CreateLoad(I8, Bp), I64);
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
  // Sign-extend a masked W-bit value held in an i64 to a full i64.
  auto signExtend = [&](IRBuilder<> &IB, Value *V) {
    return IB.CreateAShr(IB.CreateShl(V, SignShift), SignShift);
  };

  auto newCase = [&](VMOpcode Code, const char *Nm) {
    BasicBlock *Case = BasicBlock::Create(Ctx, Nm, F);
    Sw->addCase(ConstantInt::get(cast<IntegerType>(I8), Code), Case);
    return Case;
  };

  // LDIMM dst, imm : reg[dst] = imm  (already the full 64-bit immediate)
  {
    IRBuilder<> IB(newCase(OP_LDIMM, "ldimm"));
    Value *Dst = byteAt(IB, 1);
    Value *ImmPtr = IB.CreateGEP(I8, BC, IB.CreateAdd(P, ConstantInt::get(I64, 2)));
    LoadInst *Imm = IB.CreateLoad(I64, ImmPtr);
    Imm->setAlignment(Align(1));
    IB.CreateStore(Imm, regPtr(IB, Dst));
    advance(IB, 10);
  }

  // RET src : return reg[src]
  {
    IRBuilder<> IB(newCase(OP_RET, "ret"));
    Value *Src = byteAt(IB, 1);
    IB.CreateRet(loadReg(IB, Src));
  }

  // Binary op cases share the same shape: load a,b, compute, mask, store.
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
  // Signed operations reconstruct the sign from the W-bit value first.
  binaryCase(OP_SDIV, "sdiv", [&](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateSDiv(signExtend(IB, A), signExtend(IB, V));
  });
  binaryCase(OP_SREM, "srem", [&](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateSRem(signExtend(IB, A), signExtend(IB, V));
  });
  binaryCase(OP_ASHR, "ashr", [&](IRBuilder<> &IB, Value *A, Value *V) {
    return IB.CreateAShr(signExtend(IB, A), V);
  });

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

  // Register file: [NumRegs x i64] on the stack, arguments stored first.
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
