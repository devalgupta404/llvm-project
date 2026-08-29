#include "mlir/Transforms/Obfuscation/Passes.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;
using namespace mlir::obs;

namespace {

static bool shouldSkipFunction(StringRef name) {
  return name == "dlopen" || name == "dlsym" ||
         name.starts_with("__") ||
         name.starts_with("llvm.") ||
         name.starts_with("__obfs_");
}

} // namespace

//==============================================================
// Pass Implementation
//==============================================================

void ImportObfuscationPass::getDependentDialects(
    DialectRegistry &registry) const {
  registry.insert<LLVM::LLVMDialect>();
}

void ImportObfuscationPass::runOnOperation() {
  ModuleOp module = getOperation();
  MLIRContext *ctx = module.getContext();
  OpBuilder builder(ctx);

  // Collect all external functions
  SmallVector<LLVM::LLVMFuncOp> externals;
  module.walk([&](LLVM::LLVMFuncOp fn) {
    if (fn.isExternal() && !shouldSkipFunction(fn.getSymName()))
      externals.push_back(fn);
  });

  for (auto extFunc : externals) {
    StringRef name = extFunc.getSymName();
    Location loc = extFunc.getLoc();
    auto fnType = extFunc.getFunctionType();
    // Avoid void-return callsite rewrite until we rebuild the call op with
    // zero results instead of mutating callee in place.
    if (mlir::isa<LLVM::LLVMVoidType>(fnType.getReturnType()))
      continue;
    std::string wrapperName = ("__obfs_wrap_" + name).str();

    if (module.lookupSymbol<LLVM::LLVMFuncOp>(wrapperName))
      continue;

    // --- Wrapper function ---
    builder.setInsertionPointToStart(module.getBody());
    auto wrapper = builder.create<LLVM::LLVMFuncOp>(loc, wrapperName, fnType);
    if (wrapper.getBody().empty()) wrapper.addEntryBlock(builder);
    auto &entry = wrapper.getBody().front();
    builder.setInsertionPointToStart(&entry);

    // Forward to original external symbol from an internal wrapper.
    SmallVector<Value> args(wrapper.getArguments().begin(), wrapper.getArguments().end());
    auto call = builder.create<LLVM::CallOp>(loc, fnType.getReturnType(), name, args);
    builder.create<LLVM::ReturnOp>(loc, call.getResult());

    // --- Replace all original calls ---
    SmallVector<LLVM::CallOp> calls;
    module.walk([&](LLVM::CallOp call) {
      if (call.getCallee() && *call.getCallee() == name) calls.push_back(call);
    });
    for (auto call : calls) {
      call.setCallee(wrapperName);
      if (fnType.isVarArg())
        call.setVarCalleeType(fnType);
    }
  }
}



//==============================================================
// Factory
//==============================================================

std::unique_ptr<Pass>
mlir::obs::createImportObfuscationPass(
    bool encryptStrings,
    llvm::StringRef key) {
  return std::make_unique<ImportObfuscationPass>(
      encryptStrings,
      key.str());
}
