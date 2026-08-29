# CLAUDE.md — Engineering Rules for this LLVM Obfuscation Fork

This is a **fork of the LLVM Project** used to build an obfuscation toolchain (OaaS).
It carries the full upstream LLVM/MLIR tree plus a set of **obfuscation passes**:

- **LLVM IR passes** under `llvm/lib/Transforms/Obfuscation/` (Bogus Control Flow,
  Instruction Substitution, Basic-Block Splitting, Control-Flow Flattening,
  Linear MBA).
- **MLIR passes** under `mlir/lib/Transforms/Obfuscation/` (String Encryption,
  Symbol Obfuscation / stripping, Constant Obfuscation, Crypto-Hash symbol renaming,
  SCF opaque predicates, Import-table hiding).

Because this is a large, upstream-tracking codebase, the rules below are **strict and
non-negotiable**. They exist to keep the fork clean, mergeable, and cheap to maintain.

---

## 1. Non-negotiable rules

1. **This is an open-source project. Write production-grade, upstream-quality code.**
   Every line must be code you would be willing to send as an LLVM patch.
2. **No redundant code.** Do not duplicate logic, headers, helpers, or dependencies.
   If two passes need the same helper, factor it into one shared location
   (`Obfuscation/Utils`), do not copy-paste.
3. **Be efficient.** Prefer the algorithm and data structure that does the least work.
   No unnecessary allocations, no repeated `module.walk()` when one pass collects
   everything, no O(n²) symbol lookups when a `StringMap`/`SymbolTable` is available.
4. **Do not break upstream files.** Changes to core LLVM/MLIR files must be the
   minimum required to wire the passes in (one `add_subdirectory`, one component
   name, one exported symbol). Never refactor unrelated upstream code.
5. **Preserve features.** When moving or cleaning a pass, its observable behaviour
   must stay identical unless the change is an explicit, stated improvement.
6. **Never commit build artifacts, generated files, or local paths.** Nothing under
   any `build/` directory, no absolute `/home/...` paths in tracked files.

## 2. Code style (follow LLVM, not our own)

- Follow the **LLVM Coding Standards** and **LLVM Programmer's Manual** exactly:
  `PascalCase` types, `camelCase`/`CamelCase` per subproject convention, 80-col limit,
  `///` doxygen headers, LLVM `//===----===//` file banners.
- Use LLVM/MLIR data structures (`SmallVector`, `StringRef`, `StringMap`,
  `DenseMap`, `ArrayRef`) — **not** `std::vector`/`std::map`/`std::string` in hot paths.
- Use `llvm::` facilities over the STL where an equivalent exists
  (`llvm::SHA256`, `llvm::SipHash`, `llvm::raw_ostream`, `llvm::cl`).
- No `using namespace std;`. No raw `new`/`delete` where RAII/`unique_ptr` fits.
- No stray debug `llvm::errs()`/`printf` left in committed passes. Use `LLVM_DEBUG`.
- No dead code, no commented-out blocks, no empty `if`/`walk` bodies left as
  placeholders. If a feature is not implemented, do not ship a stub that pretends to.

## 3. Passes

- Every pass must have a **clear single responsibility** and a stable text
  argument (e.g. `string-encrypt`, `flattening`) plus a one-line description.
- LLVM passes register through the new Pass Manager plugin
  (`llvm/lib/Transforms/Obfuscation/Plugin`). MLIR passes register through the
  MLIR pass-plugin entry point (`mlirGetPassPluginInfo`).
- Keep pass options explicit and documented. Deterministic output for a fixed
  seed/key — obfuscation must be reproducible.
- Correctness first: an obfuscated module **must** remain semantically equivalent
  and must verify (`verifyModule` / `mlir::verify`). A pass that can corrupt IR is a bug.

## 4. Dependencies

- Prefer in-tree LLVM support libraries. Only introduce an external dependency
  (e.g. OpenSSL) when no in-tree equivalent exists, and **isolate it to the single
  target that needs it** — never add it to a core LLVM/MLIR component.
- Do not pull new third-party code into the tree.

## 5. Build & test

- The tree must configure and build with a standard LLVM CMake invocation.
  Do not add machine-specific flags to committed `CMakeLists.txt`.
- Add a `test/` entry (lit/FileCheck) for any new pass behaviour before relying on it.
- **Do not start long builds automatically.** Prepare changes, then let a human
  kick off the build.

## 6. Workflow

- Read the surrounding code before editing; match its idiom, naming, and comment density.
- Make the smallest change that fully solves the task. Finish the whole task.
- When unsure whether something is redundant, search the tree first
  (`Obfuscation/Utils.*`, existing helpers) before adding new code.
