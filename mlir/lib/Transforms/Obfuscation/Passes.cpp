#include "mlir/Transforms/Obfuscation/Passes.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"

#include <set>
#include <string>
#include <vector>

using namespace mlir;
using namespace mlir::obs;

namespace {

struct EncryptedGlobalInfo {
  std::string globalName;
  size_t length;
};

struct GlobalStringInfo {
  LLVM::GlobalOp global;
  std::vector<uint8_t> bytes;
  ShapedType denseType;
  bool useValueAttr;
  bool wasStringAttr;
};

LLVM::LLVMFuncOp ensureDecryptFunction(ModuleOp module, OpBuilder &builder) {
  auto *ctx = module.getContext();
  auto i8Ty = IntegerType::get(ctx, 8);
  auto i64Ty = builder.getI64Type();
  auto i8PtrTy = LLVM::LLVMPointerType::get(ctx);

  auto fnType = LLVM::LLVMFunctionType::get(i8PtrTy, {i8PtrTy, i64Ty}, false);

  if (auto fn = module.lookupSymbol<LLVM::LLVMFuncOp>("__llvm_decrypt_string")) {
    if (!fn.isDeclaration())
      return fn;
    OpBuilder::InsertionGuard guard(builder);
    auto *entry = fn.addEntryBlock(builder);
    builder.setInsertionPointToStart(entry);

    Value strPtr = entry->getArgument(0);
    Value len = entry->getArgument(1);

    Value zero64 = builder.create<LLVM::ConstantOp>(
        fn.getLoc(), i64Ty, builder.getI64IntegerAttr(0));
    Value one64 = builder.create<LLVM::ConstantOp>(
        fn.getLoc(), i64Ty, builder.getI64IntegerAttr(1));
    Value keyConst = builder.create<LLVM::ConstantOp>(
        fn.getLoc(), i8Ty, builder.getI8IntegerAttr(static_cast<int8_t>(0xAA)));

    Value iPtr = builder.create<LLVM::AllocaOp>(fn.getLoc(), i8PtrTy, i64Ty, one64);
    builder.create<LLVM::StoreOp>(fn.getLoc(), zero64, iPtr);

    Block *loopCond = fn.addBlock();
    Block *loopBody = fn.addBlock();
    Block *loopEnd = fn.addBlock();

    builder.create<LLVM::BrOp>(fn.getLoc(), loopCond);

    builder.setInsertionPointToStart(loopCond);
    Value i = builder.create<LLVM::LoadOp>(fn.getLoc(), i64Ty, iPtr);
    Value cond = builder.create<LLVM::ICmpOp>(fn.getLoc(), LLVM::ICmpPredicate::slt, i, len);
    builder.create<LLVM::CondBrOp>(fn.getLoc(), cond, loopBody, loopEnd);

    builder.setInsertionPointToStart(loopBody);
    Value iLoad = builder.create<LLVM::LoadOp>(fn.getLoc(), i64Ty, iPtr);
    Value strElemPtr = builder.create<LLVM::GEPOp>(fn.getLoc(), i8PtrTy, i8Ty, strPtr, ValueRange{iLoad});
    Value strChar = builder.create<LLVM::LoadOp>(fn.getLoc(), i8Ty, strElemPtr);

    Value iTrunc = builder.create<LLVM::TruncOp>(fn.getLoc(), i8Ty, iLoad);
    Value dec = builder.create<LLVM::SubOp>(fn.getLoc(), strChar, iTrunc);
    Value xored = builder.create<LLVM::XOrOp>(fn.getLoc(), dec, keyConst);
    builder.create<LLVM::StoreOp>(fn.getLoc(), xored, strElemPtr);

    Value iNext = builder.create<LLVM::AddOp>(fn.getLoc(), iLoad, one64);
    builder.create<LLVM::StoreOp>(fn.getLoc(), iNext, iPtr);
    builder.create<LLVM::BrOp>(fn.getLoc(), loopCond);

    builder.setInsertionPointToStart(loopEnd);
    builder.create<LLVM::ReturnOp>(fn.getLoc(), ValueRange{strPtr});

    return fn;
  }

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());

  auto fn = builder.create<LLVM::LLVMFuncOp>(
      module.getLoc(), "__llvm_decrypt_string", fnType);
  fn.setPrivate();

  auto *entry = fn.addEntryBlock(builder);
  builder.setInsertionPointToStart(entry);

  Value strPtr = entry->getArgument(0);
  Value len = entry->getArgument(1);

  Value zero64 = builder.create<LLVM::ConstantOp>(
      fn.getLoc(), i64Ty, builder.getI64IntegerAttr(0));
  Value one64 = builder.create<LLVM::ConstantOp>(
      fn.getLoc(), i64Ty, builder.getI64IntegerAttr(1));
  Value keyConst = builder.create<LLVM::ConstantOp>(
      fn.getLoc(), i8Ty, builder.getI8IntegerAttr(static_cast<int8_t>(0xAA)));

  Value iPtr = builder.create<LLVM::AllocaOp>(fn.getLoc(), i8PtrTy, i64Ty, one64);
  builder.create<LLVM::StoreOp>(fn.getLoc(), zero64, iPtr);

  Block *loopCond = fn.addBlock();
  Block *loopBody = fn.addBlock();
  Block *loopEnd = fn.addBlock();

  builder.create<LLVM::BrOp>(fn.getLoc(), loopCond);

  builder.setInsertionPointToStart(loopCond);
  Value i = builder.create<LLVM::LoadOp>(fn.getLoc(), i64Ty, iPtr);
  Value cond = builder.create<LLVM::ICmpOp>(fn.getLoc(), LLVM::ICmpPredicate::slt, i, len);
  builder.create<LLVM::CondBrOp>(fn.getLoc(), cond, loopBody, loopEnd);

  builder.setInsertionPointToStart(loopBody);
  Value iLoad = builder.create<LLVM::LoadOp>(fn.getLoc(), i64Ty, iPtr);
  Value strElemPtr = builder.create<LLVM::GEPOp>(fn.getLoc(), i8PtrTy, i8Ty, strPtr, ValueRange{iLoad});
  Value strChar = builder.create<LLVM::LoadOp>(fn.getLoc(), i8Ty, strElemPtr);

  Value iTrunc = builder.create<LLVM::TruncOp>(fn.getLoc(), i8Ty, iLoad);
  Value dec = builder.create<LLVM::SubOp>(fn.getLoc(), strChar, iTrunc);
  Value xored = builder.create<LLVM::XOrOp>(fn.getLoc(), dec, keyConst);
  builder.create<LLVM::StoreOp>(fn.getLoc(), xored, strElemPtr);

  Value iNext = builder.create<LLVM::AddOp>(fn.getLoc(), iLoad, one64);
  builder.create<LLVM::StoreOp>(fn.getLoc(), iNext, iPtr);
  builder.create<LLVM::BrOp>(fn.getLoc(), loopCond);

  builder.setInsertionPointToStart(loopEnd);
  builder.create<LLVM::ReturnOp>(fn.getLoc(), ValueRange{strPtr});

  return fn;
}

void rewriteUses(ModuleOp module,
                 LLVM::GlobalOp oldGlobal,
                 LLVM::GlobalOp newGlobal,
                 OpBuilder &builder) {
  SmallVector<LLVM::AddressOfOp> users;

  module.walk([&](LLVM::AddressOfOp addr) {
    if (addr.getGlobalName() == oldGlobal.getSymName())
      users.push_back(addr);
  });

  for (auto oldAddr : users) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(oldAddr);

    auto newAddr = builder.create<LLVM::AddressOfOp>(
      oldAddr.getLoc(), oldAddr.getType(), newGlobal.getSymName());

    oldAddr.replaceAllUsesWith(newAddr.getResult());
    oldAddr.erase();
  }
}

void obfuscateGlobal(ModuleOp module,
                     LLVM::GlobalOp global,
                     OpBuilder &builder,
                     const std::vector<uint8_t> &bytes,
                     ShapedType denseType,
                     bool useValueAttr,
                     bool wasStringAttr,
             SmallVectorImpl<EncryptedGlobalInfo> &encryptedGlobals) {
  SmallVector<llvm::APInt> encValues;
  for (size_t i = 0; i < bytes.size(); ++i) {
    uint8_t e = static_cast<uint8_t>((bytes[i] ^ 0xAA) + i);
    encValues.push_back(llvm::APInt(8, e));
  }

  std::string newName = (global.getSymName().str() + ".enc");

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointAfter(global);

  Attribute valueAttr;
  if (useValueAttr) {
    if (wasStringAttr) {
      std::string obfStr;
      obfStr.reserve(encValues.size());
      for (const auto &val : encValues)
        obfStr.push_back(static_cast<char>(val.getZExtValue()));
      valueAttr = builder.getStringAttr(obfStr);
    } else {
      valueAttr = DenseElementsAttr::get(denseType, encValues);
    }
  }

  auto newGlobal = builder.create<LLVM::GlobalOp>(
      global.getLoc(),
      global.getType(),
      /*isConstant=*/false,
      global.getLinkage(),
      newName,
      /*value=*/valueAttr,
      /*alignment=*/0,
      /*addrSpace=*/0);

  if (!useValueAttr) {
    auto encAttr = DenseElementsAttr::get(denseType, encValues);
    Region &region = newGlobal.getInitializerRegion();
    Block *block = new Block();
    region.push_back(block);

    builder.setInsertionPointToStart(block);

    auto newConst = builder.create<LLVM::ConstantOp>(
        global.getLoc(), denseType, encAttr);

    builder.create<LLVM::ReturnOp>(global.getLoc(), newConst.getResult());
  }

  rewriteUses(module, global, newGlobal, builder);

  encryptedGlobals.push_back({newGlobal.getSymName().str(), bytes.size()});

  global.erase();
}

}

void StringEncryptPass::runOnOperation() {
  ModuleOp module = getOperation();
  OpBuilder builder(module.getContext());
  auto *ctx = module.getContext();

  SmallVector<GlobalStringInfo> stringGlobals;

  auto extractBytes = [&](LLVM::GlobalOp global,
                          std::vector<uint8_t> &bytes,
                          ShapedType &denseType,
                          bool &useValueAttr,
                          bool &wasStringAttr) -> bool {
    auto arrayTy = dyn_cast<LLVM::LLVMArrayType>(global.getType());
    if (!arrayTy || !arrayTy.getElementType().isInteger(8))
      return false;

    if (auto valueAttr = global.getValueAttr()) {
      if (auto stringAttr = dyn_cast<StringAttr>(valueAttr)) {
        StringRef str = stringAttr.getValue();
        bytes.assign(str.bytes_begin(), str.bytes_end());
        size_t numElements = arrayTy.getNumElements();
        if (bytes.size() < numElements)
          bytes.resize(numElements, 0);
        denseType = VectorType::get({static_cast<int64_t>(numElements)},
                                    IntegerType::get(ctx, 8));
        useValueAttr = true;
        wasStringAttr = true;
        return true;
      }

      if (auto dense = dyn_cast<DenseElementsAttr>(valueAttr)) {
        for (auto v : dense.getValues<int8_t>())
          bytes.push_back(static_cast<uint8_t>(v));
        denseType = dense.getType();
        useValueAttr = true;
        return true;
      }
    }

    auto *initBlock = global.getInitializerBlock();
    if (!initBlock)
      return false;

    auto ret = dyn_cast<LLVM::ReturnOp>(initBlock->getTerminator());
    if (!ret)
      return false;

    auto cst = ret.getOperand(0).getDefiningOp<LLVM::ConstantOp>();
    if (!cst)
      return false;

    auto dense = dyn_cast<DenseElementsAttr>(cst.getValueAttr());
    if (!dense)
      return false;

    for (auto v : dense.getValues<int8_t>())
      bytes.push_back(static_cast<uint8_t>(v));
    denseType = dense.getType();
    return true;
  };

  module.walk([&](LLVM::GlobalOp global) {
    // FIX: Don't filter by constant flag - LLVM→MLIR translation may not preserve it
    // Instead, try to extract bytes from any global that looks like a string
    GlobalStringInfo info{global, {}, nullptr, false, false};
    if (!extractBytes(global, info.bytes, info.denseType, info.useValueAttr, info.wasStringAttr))
      return;

    if (info.bytes.empty())
      return;

    // Only obfuscate read-only globals (constant or unnamed_addr)
    // This prevents corrupting mutable data while catching all string constants
    if (!global.getConstant() && !global.getUnnamedAddr())
      return;

    stringGlobals.push_back(std::move(info));
  });

  if (stringGlobals.empty())
    return;

  SmallVector<EncryptedGlobalInfo> encryptedGlobals;
  for (auto &info : stringGlobals)
    obfuscateGlobal(module,
                    info.global,
                    builder,
                    info.bytes,
                    info.denseType,
                    info.useValueAttr,
                    info.wasStringAttr,
                    encryptedGlobals);

  auto decryptFn = ensureDecryptFunction(module, builder);

  if (encryptedGlobals.empty())
    return;

  auto voidType = LLVM::LLVMVoidType::get(ctx);
  auto i8PtrTy = LLVM::LLVMPointerType::get(ctx);
  auto i64Ty = builder.getI64Type();

  builder.setInsertionPointToEnd(module.getBody());
  auto initFuncType = LLVM::LLVMFunctionType::get(voidType, {}, false);

  if (!module.lookupSymbol<LLVM::LLVMFuncOp>("__llvm_string_obfs_init")) {
    auto initFunc = builder.create<LLVM::LLVMFuncOp>(
        module.getLoc(), "__llvm_string_obfs_init", initFuncType, LLVM::Linkage::External);
    initFunc.setNoInline(true);

    Block *entryBlock = initFunc.addEntryBlock(builder);
    builder.setInsertionPointToStart(entryBlock);

    for (const auto &info : encryptedGlobals) {
      Value globalAddr = builder.create<LLVM::AddressOfOp>(module.getLoc(), i8PtrTy, info.globalName);
      Value lenVal = builder.create<LLVM::ConstantOp>(
          module.getLoc(), i64Ty, builder.getI64IntegerAttr(info.length));
      builder.create<LLVM::CallOp>(module.getLoc(), decryptFn, ValueRange{globalAddr, lenVal});
    }

    builder.create<LLVM::ReturnOp>(module.getLoc(), ValueRange{});
  }

  builder.setInsertionPointToEnd(module.getBody());

  LLVM::GlobalCtorsOp existingCtors = nullptr;
  for (auto &op : module.getBody()->getOperations()) {
    if (auto ctorsOp = llvm::dyn_cast<LLVM::GlobalCtorsOp>(&op)) {
      existingCtors = ctorsOp;
      break;
    }
  }

  if (existingCtors) {
    SmallVector<Attribute> newCtors;
    SmallVector<Attribute> newPriorities;
    SmallVector<Attribute> newData;

    for (auto attr : existingCtors.getCtors())
      newCtors.push_back(attr);
    for (auto attr : existingCtors.getPriorities())
      newPriorities.push_back(attr);
    if (auto dataAttr = existingCtors.getData()) {
      for (auto attr : dataAttr)
        newData.push_back(attr);
    }

    newCtors.push_back(FlatSymbolRefAttr::get(ctx, "__llvm_string_obfs_init"));
    newPriorities.push_back(builder.getI32IntegerAttr(101));
    newData.push_back(LLVM::ZeroAttr::get(ctx));

    builder.setInsertionPoint(existingCtors);
    builder.create<LLVM::GlobalCtorsOp>(
        module.getLoc(),
        builder.getArrayAttr(newCtors),
        builder.getArrayAttr(newPriorities),
        builder.getArrayAttr(newData));
    existingCtors.erase();
  } else {
    SmallVector<Attribute> ctors;
    SmallVector<Attribute> priorities;
    SmallVector<Attribute> data;

    ctors.push_back(FlatSymbolRefAttr::get(ctx, "__llvm_string_obfs_init"));
    priorities.push_back(builder.getI32IntegerAttr(101));
    data.push_back(LLVM::ZeroAttr::get(ctx));

    builder.create<LLVM::GlobalCtorsOp>(
        module.getLoc(),
        builder.getArrayAttr(ctors),
        builder.getArrayAttr(priorities),
        builder.getArrayAttr(data));
  }
}

std::unique_ptr<Pass> mlir::obs::createStringEncryptPass(llvm::StringRef key) {
  return std::make_unique<StringEncryptPass>(key.str());
}
