# Code Search Next

This note narrows the unified search plan for source code. The goal is to turn
code files from large text blobs into searchable units with stable locators:
file, symbol, function, class, comment block, and line range.

Main search design remains in `docs/search/search-next.md`.

## Current Starting Point

- Tree-sitter core is vendored under `packages/tree-sitter`.
- The C++ grammar is vendored under `packages/tree-sitter-cpp`.
- `FEATURE_CODE_TREE_SITTER` is documented and enabled in `CMakePresets.json`.
  When the feature is on and both vendor trees are present, CMake builds
  `pm-tree-sitter-core` and `pm-tree-sitter-cpp`, then defines
  `FEATURE_CODE_TREE_SITTER` for `pm-media`.
- `CodeSourceAdapter` still exposes adapter id `code-heuristic`, but now uses
  Tree-sitter for C/C++ symbol extraction when available and falls back to the
  previous heuristic scan otherwise.
- It emits a file unit plus parser-backed class/struct/enum and function units
  with line and byte locators, imports/includes, source path tokens, and cheap
  fingerprints.

## Target Layout

```txt
src/search/
  source_adapter.hpp
  source_adapter.cpp              # text and Office adapters
  source_adapter_code.cpp         # CodeSourceAdapter and Tree-sitter helpers
  source_adapter_registry.cpp
  CodeSourceAdapter              # Tree-sitter-backed for C/C++, heuristic fallback
  code_tree_sitter.hpp           # optional future lower-level parser wrapper
  code_tree_sitter_cpp.cpp       # optional future lower-level C/C++ binding
  fingerprint.cpp
  vector_backend.cpp
```

Dependency layout:

```txt
packages/tree-sitter/
  lib/                         # vendored Tree-sitter core C library

packages/tree-sitter-cpp/
  src/parser.c
  src/scanner.c
  bindings/c/tree-sitter-cpp.h
```

Build target direction:

```txt
pm-tree-sitter-core
pm-tree-sitter-cpp
pm-media links both when FEATURE_CODE_TREE_SITTER=ON and the vendor trees are present
```

## Adapter Shape

`CodeSourceAdapter` should emit `SourceUnit` records into the same pipeline as
text, Office, PDF, and future image metadata.

File unit:

```json
{
  "uri": "src/search/vector_backend.cpp#file",
  "path": "src/search/vector_backend.cpp",
  "type": "code-file",
  "title": "vector_backend.cpp",
  "facts": {
    "language": "cpp",
    "sourcePath": {},
    "imports": ["search/vector_backend.hpp"]
  },
  "locator": {
    "kind": "file",
    "lineStart": 1,
    "lineEnd": 272
  }
}
```

Symbol unit:

```json
{
  "uri": "src/search/vector_backend.cpp#function=index_vector",
  "path": "src/search/vector_backend.cpp",
  "type": "code-function",
  "title": "index_vector",
  "text": "Result index_vector(const Request& req) ...",
  "facts": {
    "language": "cpp",
    "symbol": "index_vector",
    "symbolKind": "function",
    "parent": "media::search",
    "namespace": "media::search",
    "qualifiedSymbol": "media::search::index_vector",
    "signature": "Result index_vector(const Request& req)",
    "docComment": "",
    "imports": ["search/vector_backend.hpp"]
  },
  "locator": {
    "kind": "symbol",
    "symbol": "index_vector",
    "qualifiedSymbol": "media::search::index_vector",
    "lineStart": 50,
    "lineEnd": 223,
    "byteStart": 1200,
    "byteEnd": 7400
  }
}
```

## Units To Extract

First slice:

- `code-file`
  whole file fallback and imports/includes.
- `code-function`
  free functions and methods.
- `code-class`
  classes/structs.
- `code-enum`
  enums.
- `code-comment`
  leading comment block attached to the next symbol when easy.
- symbol facts
  include `namespace`, `parent`, `qualifiedSymbol`, `signature`, and
  `docComment` when Tree-sitter can infer them.

Later:

- constructors/destructors/operators.
- call expressions and symbol references.
- incremental parsing for editor/live-index use.

## Locators

Every code unit should include a detail locator:

```json
{
  "kind": "symbol",
  "language": "cpp",
  "symbol": "MachineController::start",
  "symbolKind": "method",
  "lineStart": 120,
  "lineEnd": 168,
  "byteStart": 4200,
  "byteEnd": 5900
}
```

`search detail` can then expand:

- exact line range;
- nearby lines;
- containing class/namespace;
- file imports/includes;
- full symbol body.

## Fingerprints

Cheap fingerprints should consume Tree-sitter facts when available:

```json
{
  "typeGuess": "source",
  "keywords": ["vector", "index", "chunk", "adapter"],
  "structure": {
    "imports": ["search/vector_backend.hpp"],
    "symbols": ["index_vector", "search_vector"],
    "classes": [],
    "functions": ["index_vector", "search_vector"]
  },
  "facts": {
    "language": "cpp",
    "sourcePath": {},
    "symbolKind": "function",
    "namespace": "media::search",
    "parent": "media::search",
    "qualifiedSymbol": "media::search::index_vector",
    "signature": "Result index_vector(const Request& req)"
  }
}
```

`--mode fingerprint` should remain the fast path for LLM/tool routing:

```powershell
tanit-cli search search ./src --content code --mode fingerprint --index-policy same-dir --query "vector index chunks" --json
```

If the hit is promising, the LLM/tool can pull semantic search or detail:

```powershell
tanit-cli search search ./src --content code --mode semantic --backend vector --index-policy same-dir --query "where vector index chunks are written" --json

tanit-cli search detail --adapter code-heuristic --path ./src/search/vector_backend.cpp --selector 'symbol:index_vector' --include-detail section --context-before 3 --context-after 3 --json
```

## CLI Smoke Fixtures

Use the real `./src` tree for smoke tests so examples stay useful:

```powershell
# Build/update code index with cheap fingerprints.
tanit-cli search index ./src --content code --backend vector --index-policy same-dir --fingerprint cheap --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --log-level trace --json

# Fast sidecar-only search. Should not open the embedding model.
tanit-cli search search ./src --content code --mode fingerprint --index-policy same-dir --query "vector backend chunks" --json

# Semantic code search after indexing.
tanit-cli search search ./src --content code --mode semantic --backend vector --index-policy same-dir --no-index-create --query "function that indexes source units into vector store" --json

# Detail expansion through the code adapter.
tanit-cli search detail --adapter code-heuristic --path ./src/search/vector_backend.cpp --selector 'symbol:index_vector' --include-detail section --context-before 2 --context-after 2 --json
```

Expected smoke assertions:

- index exits `0`;
- `search-manifest.json` records `content=code`;
- `fingerprints.jsonl` contains records from `./src/search`;
- fingerprint search for `vector backend chunks` returns
  `src/search/vector_backend.cpp`;
- semantic search can still return file/symbol units;
- detail returns line-range text once the Tree-sitter adapter is active;
- parser-backed fingerprints include namespace, parent, qualified symbol,
  signature, doc comment, and byte locator assertions.

## Implementation TODOs

1. Improve C++ name extraction for constructors, destructors, operators, and
   template-heavy declarations.
2. Add language-specific adapters or a registry for more grammars.
3. Expand `search detail` for containing class/namespace and import context.
4. Add call/reference facts once the unit schema settles.

## Non-Goals For First Slice

- No full multi-language registry yet.
- No dynamic grammar loading yet.
- No call graph or references index yet.
- No replacement/refactor actions yet.
- No attempt to make Tree-sitter required for exact grep/name search.
