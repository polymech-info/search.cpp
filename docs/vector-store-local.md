---
title: Local Vector Store
summary: Offline GGUF-backed vector search — no network, no server, no Python.
tags: [llm, embeddings, vector-store, xblox, gguf, local]
---

# Local Vector Store

Fully offline semantic search. Embed documents with any GGUF embedding model
(e.g. `bge-small-en-v1.5`) using llama.cpp, store vectors in a flat binary
index on disk, and search with brute-force cosine similarity.

Works entirely in-process — no network calls, no LiteLLM, no database server.
Designed for document corpora up to ~100 k chunks on commodity hardware.

---

## How it works

```
Source file / text
       │
       ▼
  [IChunker]──────────────────────────────────────┐
  PlainTextChunker  paragraph → sentence → word    │
  MarkdownChunker   heading-boundary + md4c SAX    │
                    frontmatter via yaml-cpp        │
       │                                           │
       ▼                                           ▼
  Chunk { uri, title, tags,            embedding_text =
          summary, content }           title + tags + summary + content_head
       │
       ▼
  [EmbeddingModel] — llama.cpp GGUF (e.g. bge-small-en-v1.5-q8_0.gguf)
       │
       ▼
  VectorStore ─ dist ──► manifest.json  (model name, dims, timestamps)
                      ──► docs.jsonl    (VStoreDoc per chunk, human-readable)
                      ──► vectors.bin   (magic + f32[count × dims] + u64[count])
```

Search: embed the query string → dot-product with all stored L2-normalised vectors
→ sort descending → return top-K `VStoreSearchResult` with cosine scores.

---

## Source files

| File | Purpose |
| --- | --- |
| `src/lib/llm/local_vector_store/VectorStore.hpp` | Public API — `VectorStore`, `VStoreDoc`, `VStoreSearchResult`, `VStoreManifest` |
| `src/lib/llm/local_vector_store/VectorStore.cpp` | Implementation — embed, brute-force scan, atomic save, model discovery |
| `src/lib/llm/chunking.hpp` | `Chunk` struct · `IChunker` interface · `PlainTextChunker` · `make_embedding_text` |
| `src/lib/llm/chunking.cpp` | `PlainTextChunker` — paragraph/sentence/word splits + `string_sanitizers` normalisation |
| `src/lib/llm/chunking_md.hpp` | `MarkdownChunker` · `FrontMatter` · `parse_frontmatter` · `looks_like_markdown` |
| `src/lib/llm/chunking_md.cpp` | md4c SAX callbacks + yaml-cpp frontmatter + sub-chunking via `PlainTextChunker` |
| `src/xblox/blocks/vector_blocks_local.cpp` | XBlox blocks — `vectorLocalOpen/AddText/AddFile/Search` + process-scope store registry |
| `src/core/string_sanitizers.hpp` | BOM strip, UTF-8 repair, whitespace normalisation used by both chunkers |

### Store on disk

```
<storePath>/
  manifest.json   — { version, embedding_model, dims, metric:"cosine", created_at, updated_at }
  docs.jsonl      — one VStoreDoc JSON per line (id, source, path, title, chunk, tags)
  vectors.bin     — magic + header + float32[N × dims] + uint64[N]  (atomic write via .tmp)
```

---

## XBlox blocks

All four blocks share common params:

| Param | Type | Default | Description |
| --- | --- | --- | --- |
| `storePath` | string | required | Store directory (created if absent). Supports `${var}`. |
| `model` | string | `""` | GGUF model path. Empty = auto-discover (see below). |
| `threads` | int | 4 | llama.cpp decode threads. |

### `vectorLocalOpen`

Open (or create) a store, load the model. Returns `{docCount, dims, modelPath}`.

```yaml
roots:
  - kind: vectorLocalOpen
    storePath: out/my-store
    model: dist/models/bge-small-en-v1.5-q8_0.gguf
    storeAs: info
```

Model auto-discovery order (when `model` is empty):
1. `PM_EMBED_MODEL` environment variable
2. `{config_dir}/models/all-MiniLM-L6-v2.Q4_K_M.gguf`
3. `dist/models/` and parent-directory variants

### `vectorLocalAddText`

Embed an inline string. Long texts are automatically chunked.

```yaml
  - kind: vectorLocalAddText
    storePath: out/my-store
    text: "Semantic search uses dense vectors to retrieve similar documents."
    source: notes
    path: inline://note-1
    title: Note 1
    chunkSize: 500
    chunkOverlap: 100
    storeAs: added
```

Returns `{ids:[…], count:N, storePath}`.

### `vectorLocalAddFile`

Read a file from disk, pick the right chunker (markdown or plain-text),
parse frontmatter, embed all chunks, and save. Skips files already indexed
(deduplication via `has_path`).

```yaml
  - kind: vectorLocalAddFile
    storePath: out/my-store
    file: docs/llm/local-llms.md
    source: docs
    chunkSize: 500
    storeAs: res
```

Returns `{ids:[…], count:N, skipped:bool, storePath}`.

Markdown detection: `.md`, `.markdown`, `.mdx` → `MarkdownChunker`; everything
else → `PlainTextChunker`.

### `vectorLocalSearch`

Embed a query and return top-K ranked hits.

```yaml
  - kind: vectorLocalSearch
    storePath: out/my-store
    query: "how to run embedding models offline"
    topK: 5
    storeAs: hits
```

Returns `{hits:[{id, score, title, chunk, path, source, tags}], count:N}`.
Scores are cosine similarity in `[0, 1]`. Higher = more similar.

Optional filters: `filterSource`, `filterPath` (substring match).

---

## Models

Download pre-converted GGUF models to `dist/models/`:

```powershell
# Windows
npm run download:bge-models:win

# Linux / macOS / WSL
npm run download:bge-models
```

| Model | Dims | Size (q8\_0) | Notes |
| --- | ---: | --- | --- |
| `bge-small-en-v1.5-q8_0.gguf` | 384 | ~33 MB | Default. Fast, good quality. |
| `bge-small-en-v1.5-f16.gguf` | 384 | ~66 MB | Higher precision, 2× size. |
| `bge-base-en-v1.5-q8_0.gguf` | 768 | ~109 MB | Better quality, ~2× slower. |
| `bge-base-en-v1.5-f16.gguf` | 768 | ~218 MB | Best quality, largest. |

Source: [CompendiumLabs/bge-small-en-v1.5-gguf](https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf) ·
[CompendiumLabs/bge-base-en-v1.5-gguf](https://huggingface.co/CompendiumLabs/bge-base-en-v1.5-gguf)

Override at runtime:
```bash
PM_EMBED_MODEL=/path/to/your.gguf npm run test:xblox:vector-store-local
```

---

## Build

Requires feature flags in CMake:

```cmake
-DFEATURE_LLAMA=1
-DFEATURE_VECTOR_LOCAL_XBLOX=1
```

Dependencies fetched automatically via `FetchContent`:
- `md4c` — CommonMark + GFM SAX parser (header: `${CMAKE_BINARY_DIR}/_deps/md4c-src/src`)
- `yaml-cpp` — YAML frontmatter parsing (`jbeder/yaml-cpp`, tag `yaml-cpp-0.9.0`)

```bash
npm run build:cpp          # full build (see AGENTS.md)
```

---

## Tests

### Functional tests (48 assertions)

Covers block registration, open, add-text, add-md, add-plain, deduplication, search, top-K, error handling.

```bash
npm run test:xblox:vector-store-local
# or with explicit model:
PM_EMBED_MODEL=dist/models/bge-small-en-v1.5-q8_0.gguf npm run test:xblox:vector-store-local
```

Model auto-detected from `dist/models/` if present. Set `SKIP_EMBED_TEST=1` to run manifest-only checks without a model.

Output: `tests/test-report-last.md`

### Multi-model quality + speed benchmark

Ingests all 163 `docs/**/*.md` files per model, runs 10 semantic + 10 edge-case queries, and generates a side-by-side comparison report.

```bash
# All detected models
npm run test:xblox:vector-store-local:bench

# Specific models (slug match)
node tests/orchestrator/test-xblox-vector-store-local-bench.mjs --models small-q8,base-q8

# Reuse existing stores (fast re-run, no re-ingest)
node tests/orchestrator/test-xblox-vector-store-local-bench.mjs --skip-ingest

# Only semantic or only edge queries
node tests/orchestrator/test-xblox-vector-store-local-bench.mjs --only semantic
node tests/orchestrator/test-xblox-vector-store-local-bench.mjs --only edge
```

Output:
- `tests/test-report-last.md` — always overwritten
- `tests/test-report-vector-store-bench.md` — dedicated benchmark report

Benchmark report includes:
- **Comparison table** — Model · Dims · Size MB · Chunks · Chunks/s · Avg ms · p50 ms · p95 ms · Quality %
- **Per-query score table** — cosine similarity per model per query
- **Per-query speed table** — ms per model per query
- **Per-model detail** — full hit list, score bars, edge-case verdicts
- **Speed summary** — min/avg/p50/p95/max across all queries

Speed unit = `ms/query` = wall time of one `vectorLocalSearch` xblox invocation
(includes process spawn + query embedding + brute-force cosine scan over all chunks).
In-process cached scenario: **< 10 ms/query** for ≤ 6 000 chunks.

### Benchmark stores

Each model gets its own store under `out/bench-stores/<model-slug>/` so models never cross-contaminate. Delete to force re-ingest:

```bash
# PowerShell
Remove-Item out\bench-stores -Recurse -Force
```

---

## Known limitations

| Limitation | Notes |
| --- | --- |
| **Scale** | Brute-force scan — suitable for ≤ 100 k chunks; beyond that consider HNSW (USearch) |
| **No negation** | `"not fast"` and `"fast"` produce similar embeddings; polarity is ignored |
| **Synonym gaps** | Model-dependent; `bge-base` covers more synonyms than `bge-small` |
| **Code vs prose** | Code-fragment queries retrieve prose docs with related identifiers, not exact symbol match |
| **Degenerate queries** | Single tokens, pure punctuation, or pure numbers return arbitrary results |
| **Cross-store incompatibility** | Opening an existing store with a different model (different dims) returns an error; delete the store directory to reset |
| **Single-threaded** | `VectorStore` is not thread-safe; XBlox blocks run one at a time |
| **No incremental update** | File deduplication is path-based; changed files must be re-indexed by deleting the store |

## References

https://github.com/nmslib/hnswlib
