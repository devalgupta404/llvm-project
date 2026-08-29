#include "mlir/Transforms/Obfuscation/Passes.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "mlir/Pass/Pass.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"

#include "llvm/ADT/APInt.h"

#include <string>

using namespace mlir;
using namespace mlir::obs;

namespace {

static std::string xorEncrypt(const std::string &input, const std::string &key) {
  std::string out = input;
  for (size_t i = 0; i < input.size(); i++) {
    out[i] = input[i] ^ key[i % key.size()];
  }
  return out;
}

struct EncryptedGlobalInfo {
  std::string globalName;
  size_t originalLength;
  unsigned elementBitWidth;
};

}

void ConstantObfuscationPass::runOnOperation() {
  ModuleOp module = getOperation();
  MLIRContext *ctx = module.getContext();
  OpBuilder moduleBuilder(ctx);

  std::vector<EncryptedGlobalInfo> encryptedGlobals;
  bool needsDecrypt16 = false;

  module.walk([&](LLVM::GlobalOp globalOp) {
    StringRef symName = globalOp.getSymName();

    if (symName.starts_with("__obfs_") || symName.starts_with("llvm."))
      return;

    if (symName.starts_with("__cxx_global_var_init") ||
        symName.starts_with("_GLOBAL__sub_I_") ||
        symName.starts_with("__cxx_global_array_dtor") ||
        symName.starts_with("__dtor_") ||
        symName.starts_with("__ctor_") ||
        symName.starts_with("GCC_except_table") ||
        symName.starts_with("__func__") ||
        symName.starts_with("__PRETTY_FUNCTION__") ||
        symName.starts_with("__FUNCTION__"))
      return;

    if (globalOp.getSection().has_value())
      return;

    if (auto strAttr = globalOp.getValueAttr()) {
      if (auto stringAttr = llvm::dyn_cast<StringAttr>(strAttr)) {
        std::string original = stringAttr.getValue().str();

        if (original.empty())
          return;

        if (original.size() < 2)
          return;

        std::string encrypted = xorEncrypt(original, key);

        globalOp.setValueAttr(StringAttr::get(ctx, encrypted));

        globalOp.setConstant(false);

        encryptedGlobals.push_back({symName.str(), original.size(), 8});
        return;
      }

      if (auto denseAttr = llvm::dyn_cast<DenseElementsAttr>(strAttr)) {
        auto shapedType = llvm::dyn_cast<ShapedType>(denseAttr.getType());
        if (!shapedType || !shapedType.hasStaticShape())
          return;

        auto elemType = llvm::dyn_cast<IntegerType>(shapedType.getElementType());
        if (!elemType || elemType.getWidth() != 16)
          return;

        size_t numElements = shapedType.getNumElements();
        if (numElements < 2)
          return;

        SmallVector<APInt> encryptedValues;
        encryptedValues.reserve(numElements);

        size_t keyLen = key.size();
        if (keyLen == 0)
          return;

        size_t idx = 0;
        for (APInt val : denseAttr.getValues<APInt>()) {
          uint16_t v = static_cast<uint16_t>(val.getZExtValue());
          uint8_t keyLo = static_cast<uint8_t>(key[(idx * 2) % keyLen]);
          uint8_t keyHi = static_cast<uint8_t>(key[(idx * 2 + 1) % keyLen]);
          uint16_t mask = static_cast<uint16_t>(keyLo) | (static_cast<uint16_t>(keyHi) << 8);
          uint16_t enc = v ^ mask;
          encryptedValues.push_back(APInt(16, enc));
          idx++;
        }

        globalOp.setValueAttr(DenseElementsAttr::get(shapedType, encryptedValues));
        globalOp.setConstant(false);

        encryptedGlobals.push_back({symName.str(), numElements, 16});
        needsDecrypt16 = true;
        return;
      }
    }
  });

  if (encryptedGlobals.empty())
    return;

  Location loc = module.getLoc();

  auto i8Type = IntegerType::get(ctx, 8);
  auto i16Type = IntegerType::get(ctx, 16);
  auto i32Type = IntegerType::get(ctx, 32);
  auto i64Type = IntegerType::get(ctx, 64);
  auto i8PtrType = LLVM::LLVMPointerType::get(ctx);
  auto i16PtrType = LLVM::LLVMPointerType::get(ctx);
  auto voidType = LLVM::LLVMVoidType::get(ctx);

  if (!module.lookupSymbol<LLVM::GlobalOp>("__obfs_key")) {
    OpBuilder::InsertionGuard guard(moduleBuilder);
    moduleBuilder.setInsertionPointToStart(module.getBody());
    auto keyArrayType = LLVM::LLVMArrayType::get(i8Type, key.size());
    auto keyGlobal = moduleBuilder.create<LLVM::GlobalOp>(
        loc,
        keyArrayType,
        true,
        LLVM::Linkage::Private,
        "__obfs_key",
        moduleBuilder.getStringAttr(key)
    );
    keyGlobal.setUnnamedAddr(LLVM::UnnamedAddr::Global);
  }

  if (!module.lookupSymbol<LLVM::LLVMFuncOp>("__obfs_decrypt")) {
    OpBuilder::InsertionGuard guard(moduleBuilder);
    moduleBuilder.setInsertionPointToEnd(module.getBody());
    auto funcType = LLVM::LLVMFunctionType::get(voidType, {i8PtrType, i32Type}, false);
    auto decryptFunc = moduleBuilder.create<LLVM::LLVMFuncOp>(
        loc, "__obfs_decrypt", funcType, LLVM::Linkage::Internal);
    decryptFunc.setNoInline(true);

    OpBuilder funcBuilder(ctx);
    Block *entryBlock = decryptFunc.addEntryBlock(funcBuilder);
    funcBuilder.setInsertionPointToStart(entryBlock);

    Value strPtr = entryBlock->getArgument(0);
    Value len = entryBlock->getArgument(1);
    Value keyAddr = funcBuilder.create<LLVM::AddressOfOp>(loc, i8PtrType, "__obfs_key");

    Value zero32 = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(0));
    Value one32 = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(1));
    Value keyLenVal = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(key.size()));

    Value iPtr = funcBuilder.create<LLVM::AllocaOp>(loc, i8PtrType, i32Type, one32);
    funcBuilder.create<LLVM::StoreOp>(loc, zero32, iPtr);

    Block *loopCond = decryptFunc.addBlock();
    Block *loopBody = decryptFunc.addBlock();
    Block *loopEnd = decryptFunc.addBlock();

    funcBuilder.create<LLVM::BrOp>(loc, loopCond);

    funcBuilder.setInsertionPointToStart(loopCond);
    Value i = funcBuilder.create<LLVM::LoadOp>(loc, i32Type, iPtr);
    Value cond = funcBuilder.create<LLVM::ICmpOp>(loc, LLVM::ICmpPredicate::slt, i, len);
    funcBuilder.create<LLVM::CondBrOp>(loc, cond, loopBody, loopEnd);

    funcBuilder.setInsertionPointToStart(loopBody);
    Value iLoad = funcBuilder.create<LLVM::LoadOp>(loc, i32Type, iPtr);

    Value iExt = funcBuilder.create<LLVM::SExtOp>(loc, i64Type, iLoad);
    Value strElemPtr = funcBuilder.create<LLVM::GEPOp>(loc, i8PtrType, i8Type, strPtr, ValueRange{iExt});
    Value strChar = funcBuilder.create<LLVM::LoadOp>(loc, i8Type, strElemPtr);

    Value keyIdx = funcBuilder.create<LLVM::SRemOp>(loc, iLoad, keyLenVal);
    Value keyIdxExt = funcBuilder.create<LLVM::SExtOp>(loc, i64Type, keyIdx);
    Value keyElemPtr = funcBuilder.create<LLVM::GEPOp>(loc, i8PtrType, i8Type, keyAddr, ValueRange{keyIdxExt});
    Value keyChar = funcBuilder.create<LLVM::LoadOp>(loc, i8Type, keyElemPtr);

    Value xored = funcBuilder.create<LLVM::XOrOp>(loc, strChar, keyChar);
    funcBuilder.create<LLVM::StoreOp>(loc, xored, strElemPtr);

    Value iNext = funcBuilder.create<LLVM::AddOp>(loc, iLoad, one32);
    funcBuilder.create<LLVM::StoreOp>(loc, iNext, iPtr);
    funcBuilder.create<LLVM::BrOp>(loc, loopCond);

    funcBuilder.setInsertionPointToStart(loopEnd);
    funcBuilder.create<LLVM::ReturnOp>(loc, ValueRange{});
  }

  if (needsDecrypt16 && !module.lookupSymbol<LLVM::LLVMFuncOp>("__obfs_decrypt16")) {
    OpBuilder::InsertionGuard guard(moduleBuilder);
    moduleBuilder.setInsertionPointToEnd(module.getBody());
    SmallVector<Type> args16{ i16PtrType, i32Type };
    auto funcType = LLVM::LLVMFunctionType::get(voidType, args16, false);
    auto decryptFunc = moduleBuilder.create<LLVM::LLVMFuncOp>(
        loc, "__obfs_decrypt16", funcType, LLVM::Linkage::Internal);
    decryptFunc.setNoInline(true);

    OpBuilder funcBuilder(ctx);
    Block *entryBlock = decryptFunc.addEntryBlock(funcBuilder);
    funcBuilder.setInsertionPointToStart(entryBlock);

    Value strPtr = entryBlock->getArgument(0);
    Value len = entryBlock->getArgument(1);
    Value keyAddr = funcBuilder.create<LLVM::AddressOfOp>(loc, i8PtrType, "__obfs_key");

    Value zero32 = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(0));
    Value one32 = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(1));
    Value two32 = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(2));
    Value keyLenVal = funcBuilder.create<LLVM::ConstantOp>(loc, i32Type, funcBuilder.getI32IntegerAttr(key.size()));

    Value iPtr = funcBuilder.create<LLVM::AllocaOp>(loc, i8PtrType, i32Type, one32);
    funcBuilder.create<LLVM::StoreOp>(loc, zero32, iPtr);

    Block *loopCond = decryptFunc.addBlock();
    Block *loopBody = decryptFunc.addBlock();
    Block *loopEnd = decryptFunc.addBlock();

    funcBuilder.create<LLVM::BrOp>(loc, loopCond);

    funcBuilder.setInsertionPointToStart(loopCond);
    Value i = funcBuilder.create<LLVM::LoadOp>(loc, i32Type, iPtr);
    Value cond = funcBuilder.create<LLVM::ICmpOp>(loc, LLVM::ICmpPredicate::slt, i, len);
    funcBuilder.create<LLVM::CondBrOp>(loc, cond, loopBody, loopEnd);

    funcBuilder.setInsertionPointToStart(loopBody);
    Value iLoad = funcBuilder.create<LLVM::LoadOp>(loc, i32Type, iPtr);

    Value iExt = funcBuilder.create<LLVM::SExtOp>(loc, i64Type, iLoad);
    Value strElemPtr = funcBuilder.create<LLVM::GEPOp>(loc, i16PtrType, i16Type, strPtr, ValueRange{iExt});
    Value strChar = funcBuilder.create<LLVM::LoadOp>(loc, i16Type, strElemPtr);

    Value iMul2 = funcBuilder.create<LLVM::MulOp>(loc, iLoad, two32);
    Value keyIdxLo = funcBuilder.create<LLVM::SRemOp>(loc, iMul2, keyLenVal);
    Value keyIdxHi = funcBuilder.create<LLVM::SRemOp>(
        loc,
        funcBuilder.create<LLVM::AddOp>(loc, iMul2, one32),
        keyLenVal);

    Value keyIdxLoExt = funcBuilder.create<LLVM::SExtOp>(loc, i64Type, keyIdxLo);
    Value keyIdxHiExt = funcBuilder.create<LLVM::SExtOp>(loc, i64Type, keyIdxHi);

    Value keyElemPtrLo = funcBuilder.create<LLVM::GEPOp>(loc, i8PtrType, i8Type, keyAddr, ValueRange{keyIdxLoExt});
    Value keyElemPtrHi = funcBuilder.create<LLVM::GEPOp>(loc, i8PtrType, i8Type, keyAddr, ValueRange{keyIdxHiExt});

    Value keyCharLo = funcBuilder.create<LLVM::LoadOp>(loc, i8Type, keyElemPtrLo);
    Value keyCharHi = funcBuilder.create<LLVM::LoadOp>(loc, i8Type, keyElemPtrHi);

    Value keyLo16 = funcBuilder.create<LLVM::ZExtOp>(loc, i16Type, keyCharLo);
    Value keyHi16 = funcBuilder.create<LLVM::ZExtOp>(loc, i16Type, keyCharHi);
    Value keyHiShift = funcBuilder.create<LLVM::ShlOp>(
        loc,
        keyHi16,
        funcBuilder.create<LLVM::ConstantOp>(loc, i16Type, funcBuilder.getI16IntegerAttr(8)));
    Value keyMask = funcBuilder.create<LLVM::OrOp>(loc, keyLo16, keyHiShift);

    Value xored = funcBuilder.create<LLVM::XOrOp>(loc, strChar, keyMask);
    funcBuilder.create<LLVM::StoreOp>(loc, xored, strElemPtr);

    Value iNext = funcBuilder.create<LLVM::AddOp>(loc, iLoad, one32);
    funcBuilder.create<LLVM::StoreOp>(loc, iNext, iPtr);
    funcBuilder.create<LLVM::BrOp>(loc, loopCond);

    funcBuilder.setInsertionPointToStart(loopEnd);
    funcBuilder.create<LLVM::ReturnOp>(loc, ValueRange{});
  }

  auto initFuncType = LLVM::LLVMFunctionType::get(voidType, {}, false);

  if (!module.lookupSymbol<LLVM::LLVMFuncOp>("__obfs_init")) {
    OpBuilder::InsertionGuard guard(moduleBuilder);
    moduleBuilder.setInsertionPointToEnd(module.getBody());
    auto initFunc = moduleBuilder.create<LLVM::LLVMFuncOp>(
        loc, "__obfs_init", initFuncType, LLVM::Linkage::External);
    initFunc.setNoInline(true);

    OpBuilder funcBuilder(ctx);
    Block *entryBlock = initFunc.addEntryBlock(funcBuilder);
    funcBuilder.setInsertionPointToStart(entryBlock);

    for (const auto &info : encryptedGlobals) {
      Value lenVal = funcBuilder.create<LLVM::ConstantOp>(
          loc,
          i32Type,
          funcBuilder.getI32IntegerAttr(info.originalLength));
      if (info.elementBitWidth == 16) {
        Value globalAddr = funcBuilder.create<LLVM::AddressOfOp>(loc, i16PtrType, info.globalName);
        funcBuilder.create<LLVM::CallOp>(
            loc,
            TypeRange{},
            "__obfs_decrypt16",
            ValueRange{globalAddr, lenVal});
      } else {
        Value globalAddr = funcBuilder.create<LLVM::AddressOfOp>(loc, i8PtrType, info.globalName);
        funcBuilder.create<LLVM::CallOp>(
            loc,
            TypeRange{},
            "__obfs_decrypt",
            ValueRange{globalAddr, lenVal});
      }
    }

    funcBuilder.create<LLVM::ReturnOp>(loc, ValueRange{});
  }

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

    newCtors.push_back(FlatSymbolRefAttr::get(ctx, "__obfs_init"));
    newPriorities.push_back(moduleBuilder.getI32IntegerAttr(101));
    newData.push_back(LLVM::ZeroAttr::get(ctx));

    moduleBuilder.setInsertionPoint(existingCtors);
    moduleBuilder.create<LLVM::GlobalCtorsOp>(
        loc,
      moduleBuilder.getArrayAttr(newCtors),
      moduleBuilder.getArrayAttr(newPriorities),
      moduleBuilder.getArrayAttr(newData)
    );
    existingCtors.erase();
  } else {

    SmallVector<Attribute> ctors;
    SmallVector<Attribute> priorities;
    SmallVector<Attribute> data;

    ctors.push_back(FlatSymbolRefAttr::get(ctx, "__obfs_init"));

    priorities.push_back(moduleBuilder.getI32IntegerAttr(101));
    data.push_back(LLVM::ZeroAttr::get(ctx));

    moduleBuilder.setInsertionPointToEnd(module.getBody());
    moduleBuilder.create<LLVM::GlobalCtorsOp>(
        loc,
      moduleBuilder.getArrayAttr(ctors),
      moduleBuilder.getArrayAttr(priorities),
      moduleBuilder.getArrayAttr(data)
    );
  }
}

std::unique_ptr<Pass> mlir::obs::createConstantObfuscationPass(llvm::StringRef key) {
  return std::make_unique<ConstantObfuscationPass>(key.str());
}
