add_llvm_library(LLVMObfuscationPlugin MODULE
  PluginRegistration.cpp
  ../AntiDebug.cpp
  ../BogusControlFlow.cpp
  ../CryptoUtils.cpp
  ../Flattening.cpp
  ../LinearMBA.cpp
  ../OpaquePredicates.cpp
  ../SplitBasicBlocks.cpp
  ../StripSignature.cpp
  ../Substitution.cpp
  ../Utils.cpp
  ../../Utils/LowerSwitch.cpp

  PLUGIN_TOOL opt

  DEPENDS
  intrinsics_gen
  )

target_include_directories(LLVMObfuscationPlugin PRIVATE
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Obfuscation
  )
