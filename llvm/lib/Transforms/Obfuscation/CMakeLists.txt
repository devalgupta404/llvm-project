add_llvm_component_library(LLVMObfuscation
  BogusControlFlow.cpp
  CryptoUtils.cpp
  Flattening.cpp
  SplitBasicBlocks.cpp
  Substitution.cpp
  Utils.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Obfuscation

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Core
  Support
  TransformUtils
  )

add_subdirectory(Plugin)
