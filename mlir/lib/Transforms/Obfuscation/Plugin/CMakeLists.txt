# Loadable pass plugin exposing the MLIR obfuscation passes to mlir-opt via
# --load-pass-plugin=MLIRObfuscationPlugin.so
#
# The pass sources are compiled directly into the MODULE. MLIR/LLVM symbols are
# resolved from the mlir-opt host at load time (PLUGIN_TOOL), so the plugin must
# NOT link the MLIR/LLVM libraries — doing so would register their cl::opt
# globals a second time and abort on dlopen. Only OpenSSL, which the host does
# not provide, is linked in.
add_llvm_library(MLIRObfuscationPlugin
  MODULE
  PluginRegistration.cpp
  ../Passes.cpp
  ../SymbolPass.cpp
  ../CryptoHashPass.cpp
  ../ConstantObfuscationPass.cpp
  ../SCFPass.cpp
  ../ImportObfuscationPass.cpp

  DEPENDS
  MLIRLLVMDialect
  MLIRFuncDialect
  MLIRSCFDialect
  MLIRArithDialect

  PLUGIN_TOOL
  mlir-opt
  )

target_link_libraries(MLIRObfuscationPlugin PRIVATE OpenSSL::Crypto)
