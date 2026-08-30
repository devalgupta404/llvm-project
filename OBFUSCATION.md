# Obfuscation techniques in this fork

This fork carries a set of obfuscation passes on top of the normal LLVM and
MLIR trees. There are two groups. The first group works on LLVM IR and runs
inside `opt`. The second group works on MLIR and runs inside `mlir-opt`. Both
groups are shipped as loadable plugins, so you turn them on by loading the
plugin and naming the pass you want.

Below every technique you get a short description, the file that implements it,
and the flag you use to switch it on. Keep in mind that the LLVM IR passes
expect optimized IR, so compile with `-O1` first when you want things like
virtualization to actually kick in.

## How to load the plugins

For the LLVM IR passes:

```
opt -load-pass-plugin=build/lib/LLVMObfuscationPlugin.so -passes='<flag>' in.ll -o out.ll
```

When you mix a function pass with a module pass in the same list, wrap the
function pass like `function(opaque-pred)`. That is just how the new pass
manager parses a pipeline.

For the MLIR passes:

```
mlir-opt --load-pass-plugin=build/lib/MLIRObfuscationPlugin.so --<flag> in.mlir -o out.mlir
```

## LLVM IR passes

These live under `llvm/lib/Transforms/Obfuscation/` and register through
`llvm/lib/Transforms/Obfuscation/Plugin/PluginRegistration.cpp`.

### Control flow flattening
Flattens a function into a single dispatch loop driven by a state variable, so
the original block order and the shape of the control flow are gone.
File: `llvm/lib/Transforms/Obfuscation/Flattening.cpp`
Flag: `flattening`

### Bogus control flow
Clones basic blocks and guards them with predicates that never actually change
what runs, which buries the real path inside fake branches.
File: `llvm/lib/Transforms/Obfuscation/BogusControlFlow.cpp`
Flag: `boguscf`

### Instruction substitution
Rewrites simple arithmetic and logic operations into longer equivalent forms,
so an add or a xor no longer looks like a plain add or xor.
File: `llvm/lib/Transforms/Obfuscation/Substitution.cpp`
Flag: `substitution`

### Basic block splitting
Breaks basic blocks into smaller pieces so a single straight run of code turns
into several blocks, which makes the control flow graph harder to read.
File: `llvm/lib/Transforms/Obfuscation/SplitBasicBlocks.cpp`
Flag: `split`

### Linear MBA
Replaces bitwise operations with mixed boolean and arithmetic expressions that
compute the same result but are much harder to recognize by eye or by tooling.
File: `llvm/lib/Transforms/Obfuscation/LinearMBA.cpp`
Flag: `linear-mba`

### Opaque predicates
Wraps every conditional branch with an always true predicate built from a
runtime value, so the real condition hides behind arithmetic the optimizer
cannot fold away.
File: `llvm/lib/Transforms/Obfuscation/OpaquePredicates.cpp`
Flag: `opaque-pred`

### Signature stripping
Removes the toolchain fingerprints a module carries, like the compiler ident,
the module identifier, and the source file name, so the object is harder to
attribute to a compiler and version.
File: `llvm/lib/Transforms/Obfuscation/StripSignature.cpp`
Flag: `strip-signature`

### Anti debug
Injects a ptrace self attach check that runs before main, and if a debugger is
already attached the program just exits, which gets in the way of dynamic
analysis on Linux.
File: `llvm/lib/Transforms/Obfuscation/AntiDebug.cpp`
Flag: `anti-debug`

### VM virtualization
Compiles a function body into a small register based bytecode and replaces the
function with a call to an interpreter that runs that bytecode, so the original
instructions only exist as data at runtime.
File: `llvm/lib/Transforms/Obfuscation/Virtualization.cpp`
Flag: `virtualize`

## MLIR passes

These live under `mlir/lib/Transforms/Obfuscation/` and register through
`mlir/lib/Transforms/Obfuscation/Plugin/PluginRegistration.cpp`.

### String encryption
Encrypts string globals in the module and adds a small decrypt routine that
runs at startup, so the plaintext strings are not sitting in the binary.
File: `mlir/lib/Transforms/Obfuscation/Passes.cpp`
Flag: `string-encrypt`

### Symbol obfuscation
Renames functions and globals to random names, which strips away the readable
symbol names that usually give away what the code is doing.
File: `mlir/lib/Transforms/Obfuscation/SymbolPass.cpp`
Flag: `symbol-obfuscate`

### Constant obfuscation
Encrypts constant string and integer data in globals so the raw values are not
visible in a static dump of the binary.
File: `mlir/lib/Transforms/Obfuscation/ConstantObfuscationPass.cpp`
Flag: `constant-obfuscate`

### Crypto hash renaming
Renames symbols using a cryptographic hash of the original name, which gives
stable but unreadable names and can use SHA256 or BLAKE2b.
File: `mlir/lib/Transforms/Obfuscation/CryptoHashPass.cpp`
Flag: `crypto-hash`

### SCF opaque predicates
Adds opaque predicates to structured control flow, so an scf.if gets an extra
always true guard that hides the real branch condition.
File: `mlir/lib/Transforms/Obfuscation/SCFPass.cpp`
Flag: `scf-obfuscate`

### Import hiding
Replaces direct calls to external functions with wrapper calls, so the import
table does not spell out exactly which library functions the code relies on.
File: `mlir/lib/Transforms/Obfuscation/ImportObfuscationPass.cpp`
Flag: `import-obfuscate`

## A full example on a C file

```
clang -O1 -S -emit-llvm prog.c -o prog.ll
opt -load-pass-plugin=build/lib/LLVMObfuscationPlugin.so \
  -passes='strip-signature,virtualize,function(opaque-pred),anti-debug' \
  -S prog.ll -o prog.obf.ll
clang prog.obf.ll -o prog_obf
```

If you also want the MLIR passes, take the IR through `mlir-translate
--import-llvm`, run the MLIR plugin, and bring it back with `mlir-translate
--mlir-to-llvmir` before the final compile.
