# MLIR-level obfuscation passes, exposed as a loadable mlir-opt pass plugin.
#
# CryptoHashPass hashes symbol names with SHA-256/BLAKE2b; BLAKE2b has no
# in-tree equivalent, so OpenSSL is required here and isolated to the plugin.
find_package(OpenSSL REQUIRED)

add_subdirectory(Plugin)
