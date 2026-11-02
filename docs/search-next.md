# Search / Find Infrastructure Snapshot

Goal: consolidate the current search pieces into one flexible search surface for
documents, images, code, and plain text files. This is a short inventory of what
exists today, which files own it, and which backend technology each path uses.

## Current Surfaces

### Code and Text File Search

Purpose: find files by name or grep file contents.

Involved files:

- `src/core/search.hpp`
- `src/core/search.cpp`
- CLI surface: `src/cli/pm_image_cmd_search.hpp`
- CLI implementation: `src/cli/pm_image_cmd_search.cpp`
- LLM tool surface: `src/llm/tools/fs/FsTools.hpp`
- LLM tool implementation: `src/llm/tools/fs/FsTools.cpp`
- LLM dispatch: `src/llm/path_tool_executor.cpp`

Content types:

- Any regular file, filtered by include/exclude globs.
- Image filenames when `SearchType::Image` is selected.
- Code/text content when `grep` is enabled.
- Built-in type filters: `c`, `cpp`, `cs`, `css`, `go`, `html`, `java`,
  `js`, `json`, `kt`, `md`, `py`, `rs`, `ruby`, `sh`, `swift`, `ts`, `toml`,
  `txt`, `xml`, `yaml`.

Backend/tech:

- Own recursive `std::filesystem` walker.
- `std::regex` or case-folded substring/whole-word matching.
- Streaming grep with binary-file probing and max file size limits.
- Sensitive path filtering via `llm_fs_guard`.
- Pause/cancel via `BatchControl` and caller-provided
  `SearchOptions::cancel_requested`.

Exposed through:

- CLI: `tanit-cli search ...` registers options in
  `pm_image_register_search()` and calls `media::search_files()` from
  `pm_image_cmd_search()`.
- LLM path tool: `file_search` is registered by `make_file_search()`.
  `path_tool_executor.cpp` dispatches it to `do_file_search()`, which maps tool
  JSON into `media::SearchOptions` and delegates to `media::search_files()`.

Status:

- This is the cleanest general-purpose local file search path.
- `SearchIndexer::Os` is only scaffolded. Windows Search, Spotlight, and
  `locate` are documented in code but not implemented.
- Search itself is library code. CLI and LLM tools provide presentation,
  cancellation, logging, and IPC/activity surfacing.

### LLM Filesystem Tools

Purpose: expose filesystem discovery, text reads, exact search, and writes to
agents/path tools.

Involved files:

- `src/llm/tools/fs/FsTools.hpp`
- `src/llm/tools/fs/FsTools.cpp`
- `src/llm/path_tool_executor.cpp`
- `src/llm/path_tool_catalog.cpp`
- legacy buffer tool: `src/llm/tool_executor.cpp`
- logging/context helpers: `src/llm/tools/commons/PathToolCommons.cpp`

Content types:

- `file_glob`: path metadata, including text, binary, media, archive, and image
  paths, subject to filesystem guard and output caps. Results include extension,
  size, and best-effort `media::probe` metadata (`contentType`, `kind`,
  `preferredExt` when available).
- `file_read`: UTF-8 text only. Rejects sensitive paths, hidden/disallowed
  paths, binary-looking content, raster image/media/archive extensions, and files
  above the read limit.
- `file_search`: exact filename/content search over files reachable from the
  requested path/root. Defaults to grep/content search, recursive, case
  insensitive, and capped output.
- `write_file`: UTF-8 text writes.

Backend/tech:

- `list_images` uses shared image predicates from `src/file_extensions.hpp`
  instead of a local image extension list.
- `path_tool_executor.cpp` dispatches tool names:
  `file_read -> do_file_read_path()`, `file_search -> do_file_search()`.
- `do_file_read_path()` resolves Explorer/cwd-relative paths with
  `resolve_tool_path_string()`, then delegates to the older buffer tool through
  `media::llm::execute("file_read", {"path": abs})`.
- The delegated buffer `file_read` lives in `src/llm/tool_executor.cpp` and
  enforces size, binary sniffing, extension blocklist, and `llm_fs_guard`.
- `do_file_search()` builds `media::SearchOptions`, applies JSON options through
  `apply_search_options_from_json()`, and calls `media::search_files()`.
- `file_glob` adds best-effort file classification from `media::probe::probe_file`
  but remains a path listing tool, not a content reader.
- Tool-call logging is slimmed/redacted in `PathToolCommons.cpp`.

Status:

- `file_search` is the LLM-facing exact search route, not a separate search
  engine.
- `file_read` currently takes a two-step path-tool -> legacy-buffer-tool route.
  That is functional but worth simplifying later if the path tool layer becomes
  the only supported tool surface.

### Image Find

Purpose: search image files by name, sidecar metadata, EXIF, or an LLM judge.

Involved files:

- `src/core/find.hpp`
- implementation counterpart: `src/core/find.cpp`
- metadata dependency: `src/core/meta.hpp` and meta extraction pipeline

Content types:

- Image files only. Inputs may be files, directories, or globs.
- Candidate metadata comes from `<stem>.md`, `<stem>.json`, EXIF fields, and
  optionally generated sidecars.
- Optional reference images can be sent to the judge.

Backend/tech:

- Name/folder substring matching for cheap local mode.
- Metadata corpus assembly from sidecars and EXIF.
- Optional `media::meta_extract` sidecar generation.
- LLM/multimodal judge using the same provider/model/api-key plumbing as meta.

Status:

- Useful for image-specific prompt search.
- Separate from `search_files`, even though both walk paths and share image
  extension concepts.

### Local Vector Store Search

Purpose: semantic search over embedded document chunks.

Involved files:

- `src/lib/llm/local_vector_store/VectorStore.cpp`
- `src/lib/llm/local_vector_store/VectorStore.hpp`
- `src/xblox/blocks/vector_blocks_local.cpp`
- `src/lib/llm/llama/EmbeddingModel.cpp`
- `src/core/local_model_catalog.cpp`

Content types:

- Direct text via `vectorLocalAddText`.
- Single files via `vectorLocalAddFile`.
- Directories via `vectorLocalAddDir`.
- Directory-supported extensions: `.txt`, `.md`, `.markdown`, `.mdx`, `.csv`,
  `.json`, `.yaml`, `.yml`.
- Office formats when `FEATURE_OFFICE_EXTRACTOR` is enabled: `.docx`, `.xlsx`,
  `.pptx`.

Backend/tech:

- Local GGUF embedding model through llama.cpp.
- Model resolution through `local_model_catalog` and `PM_EMBED_MODEL`.
- Optional GPU offload through `gpuLayers`.
- Markdown and plain-text chunkers.
- Cosine similarity over persisted vectors.
- Store persistence in `manifest.json`, `docs.jsonl`, and `vectors.bin`.
- Store-level dedup by document path.

Status:

- Best current path for semantic document search.
- Not integrated with `search_files`; callers must choose vector store blocks
  explicitly.
- Office extraction is whole-file today; per-sheet/per-slide extraction is noted
  as TODO.

### Office Text Extraction

Purpose: convert Office files into text units for vector ingestion.

Involved files:

- `src/lib/llm/office_source_adapter.cpp`
- `src/lib/llm/office_source_adapter.hpp`
- used by `src/xblox/blocks/vector_blocks_local.cpp`

Content types:

- `.docx`
- `.xlsx`
- `.pptx`

Backend/tech:

- External `officecli` binary discovered on `PATH`.
- Runs `officecli view "<file>" text`.
- Output capped at 4 MiB.

Status:

- Good enough for whole-file document ingestion.
- Needs finer document structure later: sheets, slides, sections, page ranges.

### Runtime Info Lookup

Purpose: search runtime commands, path tools, MCP tools, and installed apps.

Involved files:

- `src/llm/tools/info/InfoTool.cpp`
- `src/llm/tools/info/AppFinder_Win.cpp`
- related app finder headers/implementation under `src/llm/tools/info/`

Content types:

- CLI commands and custom commands.
- Path tools and MCP tools.
- Installed desktop apps.

Backend/tech:

- Fuzzy string matching through `matches_query` / `match_score`.
- Optional embedding fallback when `FEATURE_COMMAND_LLAMA_INFERENCE` is enabled.
- Runtime command registry and path tool catalog.
- MCP tool inventory.
- Windows app discovery from App Paths registry keys, uninstall registry keys,
  Start Menu shortcuts, and shallow known install directories.

Status:

- This is discovery/search for capabilities, not document search.
- Keep it as a separate "catalog search" backend in any unified search model.

## Consolidation Direction

One frontend/API should route to multiple backends instead of forcing callers to
know implementation details:

- `text` / `code`: use `search_files` for name and grep.
- `agent-file-search`: expose `search_files` through the `file_search` path tool
  with tool-safe defaults, result caps, and guard policy.
- `agent-file-read`: keep `file_read` for opening exact text paths discovered by
  `file_glob` or `file_search`.
- `image`: use `find_images` for prompt/image metadata search; use
  `search_files` only for image filename search.
- `document`: use `VectorStore` for semantic retrieval; use `search_files` for
  exact text grep over source files.
- `office`: extract through `OfficeSourceAdapter`, then index/search through
  `VectorStore`.
- `catalog`: use `InfoTool` for commands, tools, MCP tools, and installed apps.

Likely shared request fields:

- `query`
- `contentType`: `any`, `text`, `code`, `image`, `document`, `office`, `catalog`
- `mode`: `name`, `grep`, `semantic`, `metadata`, `auto`
- `inputs`
- `include` / `exclude`
- `recursive`
- `limit` / `offset`
- `backend`: `own`, `os`, `vector`, `image-meta`, `catalog`, `auto`

## Proposed Core Code

Do not turn `src/core/search.cpp` into a document/image/vector mega-engine.
Keep it as the exact-search backend and add a small orchestration layer above
the existing backends.

### New Files

- `src/core/search_request.hpp`
- `src/core/search_facade.hpp`
- `src/core/search_facade.cpp`
- `src/lib/llm/source_adapter.hpp` (already exists; grow this contract if needed)
- `src/lib/llm/pdf_source_adapter.hpp`
- `src/lib/llm/pdf_source_adapter.cpp`
- optional later: `src/lib/llm/document_index.hpp` /
  `src/lib/llm/document_index.cpp` for automatic per-folder stores.

### Public Request / Result Shape

```cpp
namespace media {

enum class UnifiedSearchContent {
    Any,
    Text,
    Code,
    Image,
    Document,
    Office,
    Pdf,
    Catalog,
};

enum class UnifiedSearchMode {
    Auto,
    Name,
    Grep,
    Semantic,
    Metadata,
};

enum class UnifiedSearchBackend {
    Auto,
    Exact,      // search_files
    ImageMeta,  // find_images
    Vector,     // VectorStore
    Catalog,    // InfoTool / app/tool registry
};

struct UnifiedSearchRequest {
    std::string query;
    std::vector<std::string> inputs;

    UnifiedSearchContent content = UnifiedSearchContent::Any;
    UnifiedSearchMode mode = UnifiedSearchMode::Auto;
    UnifiedSearchBackend backend = UnifiedSearchBackend::Auto;

    std::vector<std::string> include_globs;
    std::vector<std::string> exclude_globs;
    bool recursive = true;

    int limit = 50;
    int offset = 0;

    // Exact search options.
    bool case_sensitive = false;
    bool regex = false;
    bool whole_word = false;
    int context_before = 0;
    int context_after = 0;

    // Semantic/vector options.
    std::string index_path;
    std::string model;
    int threads = 4;
    int gpu_layers = 0;

    std::function<bool()> cancel_requested;
};

struct UnifiedSearchHit {
    std::string path;
    std::string title;
    std::string snippet;
    std::string source;     // name | folder | content | image-meta | vector | catalog
    std::string content_type;
    std::string kind;       // text | code | image | document | ...
    uint64_t line = 0;
    uint64_t column = 0;
    double score = 0.0;
    nlohmann::json metadata = nlohmann::json::object();
};

struct UnifiedSearchResult {
    bool ok = false;
    std::string error;
    std::vector<UnifiedSearchHit> hits;
    nlohmann::json stats = nlohmann::json::object();
};

UnifiedSearchResult unified_search(const UnifiedSearchRequest& req);

} // namespace media
```

### Routing Rules

- `backend=Exact` or `mode=Name/Grep`:
  call `media::search_files()`.
- `content=Image` and `mode=Metadata/Semantic`:
  call `media::find_images()`.
- `content=Document/Office/Pdf` and `mode=Semantic`:
  extract source units, index/search through `media::llm::VectorStore`.
- `content=Catalog`:
  call the existing info/catalog lookup layer, not file search.
- `mode=Auto`:
  start with exact filename/content search for cheap deterministic matches; use
  image/vector backends only when the content type or request implies them.

### Source Adapters

Format extraction is an adapter layer that feeds `VectorStore`; PDF, Office,
CAD, and other foreign formats should not put parsing logic into
`src/core/search.cpp` or `src/search/search_facade.cpp`.

Existing:

- `src/search/source_adapter.hpp`
  defines the search-facing adapter boundary.
- `src/search/source_adapter_registry.hpp/.cpp`
  owns the in-process adapter registry. Adapters have unique stable IDs, optional
  display names, can match multiple extensions, and are tried in registration
  order. There is no user config yet.
- `PlainTextSourceAdapter`
  registers as `plain-text`, reads text-like files, and returns one `SourceUnit`
  per file.
- `office-officecli`
  wraps `src/lib/llm/office_source_adapter.*` and extracts `.docx`, `.xlsx`,
  `.pptx` through `officecli`.

Add:

- `PdfSourceAdapter`: `.pdf` through a local extractor. Candidate backends:
  `poppler/pdftotext`, `mutool`, or a library wrapper if we vendor one later.
- External/DLL adapters for domain formats such as SolidWorks.

Current contract:

```cpp
namespace media::search {

struct SourceUnit {
    std::string uri;       // stable dedup key, e.g. file.pdf#page=3
    std::string path;      // origin path for cache/removal
    std::string title;
    std::string type;      // text | document | sheet | slide | model | ...
    std::string text;
    std::vector<std::string> tags;
};

class ISourceAdapter {
public:
    virtual ~ISourceAdapter() = default;
    virtual const char* id() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual bool can_extract(const std::filesystem::path& file) const = 0;
    virtual std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                            std::uint64_t max_bytes = 0) = 0;
    virtual SourceDetail detail(const std::filesystem::path& file,
                                const DetailRequest& req);
    virtual const std::string& error() const noexcept = 0;
};

} // namespace media::search
```

Detail contract:

```cpp
namespace media::search {

struct SourceLocator {
    std::string adapter_id;      // plain-text | office-officecli | pdf-* | image-*
    nlohmann::json selectors;    // adapter-owned stable coordinates
};

struct DetailRequest {
    std::string selector;
    std::string office_path;
    std::string include_detail;  // row | page | section | object | metadata | auto
    int context_before = 0;
    int context_after = 0;
    nlohmann::json options = nlohmann::json::object();
};

struct SourceDetail {
    bool ok = false;
    std::string error;
    std::string adapter_id;
    std::string path;
    std::string text;
    nlohmann::json locator = nlohmann::json::object();
    nlohmann::json data = nlohmann::json::object();
};

} // namespace media::search
```

Search hits should stay compact but carry enough adapter coordinates to ask for
more context later:

```json
{
  "path": "C:\\...\\invoice_details.xlsx",
  "adapter": "office-officecli",
  "locator": {
    "kind": "excel-row",
    "officePath": "/invoice_details/row[6]",
    "selector": "invoice_details!row[Customer~=\"Waste Prevention Sl\"]",
    "sheet": "invoice_details",
    "row": 6
  }
}
```

Adapter detail expansion:

- Office reuses `officecli` selectors and paths from `packages/office/SKILL.md`.
  Excel can return full rows and nearby rows; Word can return paragraph/section
  context; PowerPoint can return slide/shape context.
- PDF adapters should return page, text block, bounding box, and nearby page
  text when available.
- Image adapters should return metadata, detected objects, OCR spans, captions,
  crop references, or other adapter-specific context.
- The core search facade should not understand rows, pages, shapes, or objects;
  it passes `SourceLocator` and `DetailRequest` back to the owning adapter.

### Unified CLI

`tanit-cli search` is the single CLI branch for unified search. It requires a
subverb:

- `search search`: search files. Supports exact filename search, exact grep, and
  text semantic search through the local vector backend. It can also expand
  matching hits inline with `--include-detail` when the source adapter can map
  the hit back to a row, page, section, object, or metadata record.
- `search index`: build/update an index only. This is the cron/background entry.
- `search detail`: expand one hit through the owning source adapter. This is the
  LLM context entry point for full rows, nearby paragraphs, PDF pages, image
  metadata, object crops, and similar adapter-specific context.
- `search action`: reserved for search-scoped custom actions such as replace.

Primary flags:

- `--mode name|grep|semantic|metadata|fingerprint|auto`
  - `name`: exact filename/folder match through `search_files()`.
  - `grep`: exact content search through `search_files()`.
  - `semantic`: vector/document retrieval.
  - `metadata`: image metadata / sidecar / EXIF search.
  - `fingerprint`: fast search over `fingerprints.jsonl` only.
  - `auto`: route by content type and available index.
- `--content any|text|code|image|document|office|pdf|catalog`
  selects the content family.
- `--backend auto|exact|image-meta|vector|fingerprint|catalog`
  selects or overrides the backend.

Multi-dimensional fan-out:

- `--content`, `--mode`, and `--backend` are repeatable and accept
  comma-separated lists, e.g. `--mode name,grep` or `--content document
  --content office`.
- When more than one value is supplied for any dimension the facade runs the
  cartesian product of `(content x mode x backend)` as independent tasks, in the
  explicit order given, and unions the hits. This is a library-level fan-out in
  `unified_search`; the CLI is only a thin surface, so the same behaviour is
  available to the LLM tool and web/streaming front-ends.
- Incompatible `(mode, backend)` pairs (for example `--mode semantic --backend
  exact`) are skipped instead of failing the whole search. Each attempt, with
  its resolved `content/mode/backend`, `ok`, `matches`, and any `error`, is
  recorded under `stats.tasks` and `stats.multi` is set.
- Hits are de-duplicated by `path` + `line` + `column`; the first task to
  produce a given location wins, so task order doubles as a ranking. Every
  merged hit carries `metadata.task` with its originating `content/mode/backend`.
- Single-value invocations keep their previous behaviour exactly, including the
  convenience inference for `--adapter`, `--grep`, `--type image`, and
  `--fingerprint`; that inference is intentionally skipped in fan-out mode where
  the caller is explicit.
- `--sort auto|none|score|path` orders the merged result set. `auto` (default)
  and `none` preserve task/insertion order, `score` sorts by descending
  relevance, and `path` sorts by path then line/column. `--max` caps the merged
  total after sorting.

Supported content types:

| Content | Current inputs | Search modes | Index / backend | Detail / locator status |
| --- | --- | --- | --- | --- |
| `text` | Known text, markdown, config, logs, CSV/TSV, JSON/YAML/TOML, and source-like text files. | `name`, `grep`, `semantic`, `fingerprint` | Exact search via `search_files`; vector index via `PlainTextSourceAdapter`; optional `fingerprints.jsonl`. | Whole-file detail today; cheap fingerprints add text type, headings, columns, imports, symbols, facts, and file locator. |
| `code` | Source files matched by exact `--type` filters and known source extensions during vector ingest. | `name`, `grep`, `semantic`, `fingerprint` | Exact search via `search_files`; vector/fingerprint via `CodeSourceAdapter` (`code-heuristic`). C/C++ uses Tree-sitter when `FEATURE_CODE_TREE_SITTER` is enabled, with heuristic fallback. | File-level and symbol/function/class/enum locators today; C/C++ parser-backed units include line and byte ranges for `search detail`. |
| `document` | Text-friendly documents plus Office files when the Office feature is enabled. | `semantic`, `fingerprint`, exact `name`/`grep` for text-like files | Local `VectorStore`; adapter registry chooses plain text or Office adapter. | Adapter-owned locators; Office row detail works, PDF/page detail is future. |
| `office` | `.docx`, `.xlsx`, `.pptx` through `office-officecli`. | `semantic`, `fingerprint` | Foreign adapter extraction into vector store; optional cheap fingerprints. | `search detail` supports Office selectors/native paths and Excel row context; richer sheet/slide units are future. |
| `pdf` | Planned adapter. | Future `semantic`, `fingerprint` | Not implemented in this slice. | Future page/paragraph locators. |
| `image` | Raster image paths. Cheap adapter: `image-sidecar`; model-backed adapters: `ocr-local` and `llm-meta`. | `name`, `semantic`, `fingerprint`, `metadata` | Exact path/name; vector/fingerprint via `image-sidecar`, `ocr-local`, or `llm-meta` into `image.vstore`; legacy `metadata` routes to `find_images()`. | File/image metadata, OCR page, and LLM metadata locators today; bbox/block/line locators are next. |
| `catalog` | App/catalog entries via `InfoTool` direction. | Future catalog lookup | Not wired in unified search yet. | Not applicable. |

Compatibility aliases and exact-search options:

- `--grep` means `--mode grep`.
- `--type image` maps to `--content image --backend exact` unless
  `--mode metadata` or `--backend image-meta` is also set.
- `--type cpp|ts|py|md|...` remains an exact-search file type
  filter.
- `--indexer own|os|rg` applies only to `--backend exact`.
  - `rg` routes `--mode name` to `rg --files -g "*<query>*"` and `--mode grep`
    to `rg --column --line-number` using the bundled `dist/win-x64/bin/rg`
    binary (resolved via `resolve_third_party_executable`).
    Gated behind `ENABLE_RG_FILE` in `search_facade.cpp`; set to `false` by
    default (trial).
- `--output-mode content|files_with_matches|count` applies only to
  exact search. Unified output can still include `--json`.
- Input paths/globs are resolved through command variables first, so
  `${ENV:NAME}` and `${KNOWNFOLDER:Desktop}` work in CLI arguments.

Vector/document flags:

- `--index-policy same-dir|specific-dir|disabled`
  controls where local/vector indexes are created or whether creation is
  forbidden. `same-dir` is the default for now.
- `--index PATH`
  vector store path. Empty means use the default document index policy.
  Repeatable and comma-separated, and each entry may be a glob that matches
  store directories (e.g. `--index "./**/*.vstore"`). When more than one store
  is given — or whenever no input path is supplied — semantic and fingerprint
  search read the existing store(s) directly without ingesting or pruning, then
  merge and rank the hits across stores. Each hit carries `metadata.index` with
  its source store, and `stats.stores` reports per-store status. All searched
  stores should share the embedding model passed with `--model`; a store whose
  manifest model does not match is reported as a skipped store rather than
  failing the whole search. Indexing (`search index`) still writes a single
  store (the first `--index`).
- `--model MODEL_OR_PATH`
  embedding model hint/path.
- `--gpu-layers N`
  llama.cpp GPU offload for embedding (`0` CPU, `-1` all layers).
- `--reindex`
  force re-ingest documents before searching.
- `--no-index-create`
  fail if a semantic/document search needs an index that does not exist.

Document extraction flags:

- `--extract auto|text|office|pdf`
  force or constrain extraction backend before vector indexing.
- `--fingerprint off|cheap`
  optionally write `fingerprints.jsonl` beside the vector store. `cheap` uses
  deterministic keywords/entities/type guesses from `SourceUnit` text, facts,
  and locators. It does not change ranking yet.
- `--chunk-size N`
- `--chunk-overlap N`

Detail flags:

- `--adapter ID`
  adapter that owns the hit locator, for example `office-officecli`.
- `--path FILE`
  source file to expand.
- `--selector SELECTOR`
  adapter-owned selector. Office selectors should match `officecli query` /
  `officecli get` conventions, for example
  `invoice_details!row[Customer~="Waste Prevention Sl"]`.
- `--office-path PATH`
  Office XML/native path such as `/invoice_details/row[6]` or
  `/body/p[@paraId=1A2B3C4D]`.
- `--include-detail auto|row|page|section|object|metadata`
  requested context shape. Adapters may map this to the closest supported
  detail type. On `search search`, this enriches returned hits inline. On
  `search detail`, it expands one explicit locator.
- `--detail-max N`
  on `search search`, cap inline detail expansion to the first N returned hits.
  `0` means expand every returned hit.
- `--context-before N`, `--context-after N`
  nearby units for rows, paragraphs, pages, slides, objects, or OCR spans.

Office detail notes:

- Inline Office row detail is compact by default: the hit carries row text and
  locator metadata, not the raw `officecli` cell tree. Full raw adapter payloads
  are reserved for explicit `search detail` calls.
- Office row context is fetched as one Excel range per hit when possible, not one
  subprocess per row.
- Search detail disables `officecli` auto-resident mode so short CLI calls do not
  wait for the resident process idle timeout after output is printed.
- Markdown output renders Office row context as tables.

Examples:

```powershell
# Exact filename search

tanit-cli search search ./docs --mode name --content text --backend exact --query "gemini" --json

# Exact grep search

tanit-cli search search ./docs --mode grep --content text --backend exact --query "gemini" --json

# Exact code search

tanit-cli search search ./src --mode grep --content code --backend exact --type cpp --query "SearchOptions"

# Fan-out: filename match first, then content grep, in one shot.
# Modes/contents/backends are repeatable or comma-separated.
tanit-cli search search ./docs --backend exact --mode name --mode grep --query "images" --sort path --json

# Fan-out across content families with a single backend
tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/' --content document,office --backend exact --mode grep --query "amazon" --json

# Mixed modes; incompatible (mode,backend) pairs are skipped, not fatal.
# Here semantic+exact is recorded as a skipped task while grep runs.
tanit-cli search search ./docs --backend exact --mode grep,semantic --query "fingerprint" --sort score --json

# Index-only entry for cron/background jobs
tanit-cli search index ./docs --content document --backend vector --index-policy same-dir --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --fingerprint cheap --log-level trace --json

# Office smoke target (foreign adapter path). In PowerShell, single-quote
# `${...}` variables so PixlWiz receives and expands them, not the shell.
tanit-cli search index '${KNOWNFOLDER:Desktop}/eduardo/**/*.xlsx' --content document --backend vector --index-policy same-dir --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --log-level trace

# Search the same Office index
tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/**/*.xlsx' --mode semantic --content document --backend vector --index-policy same-dir --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --no-index-create --query "Waste Prevention Sl" --json

# Direct Office grep without a store or embedding model. This extracts each
# workbook in memory through `office-officecli` and scans the extracted units.
tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/**/*.xlsx' --mode grep --content document --backend exact --query "Waste Prevention Sl" --md

# Search and include full-row detail inline for each hit
tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/**/*.xlsx' --mode semantic --content document --backend vector --index-policy same-dir --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --no-index-create --query "rosa" --include-detail row --context-before 2 --context-after 2 --json

# Same, but expand only the first three hits and print Markdown tables
tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/**/*.xlsx' --mode semantic --content document --backend vector --index-policy same-dir --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --no-index-create --query "rosa" --include-detail row --detail-max 3 --context-before 2 --context-after 2 --md --markdown plain

# Expand a selected Office hit into full-row context for LLM use
tanit-cli search detail --adapter office-officecli --path '${KNOWNFOLDER:Desktop}/eduardo/2023-Q4/invoice_details.xlsx' --office-path '/invoice_details/row[6]' --include-detail row --context-before 2 --context-after 2 --json

# Same idea through an officecli selector
tanit-cli search detail --adapter office-officecli --path '${KNOWNFOLDER:Desktop}/eduardo/2023-Q4/invoice_details.xlsx' --selector 'invoice_details!row[Customer~="Waste Prevention Sl"]' --include-detail row --context-before 2 --context-after 2 --json

# Text semantic search through local VectorStore
tanit-cli search search ./docs --mode semantic --content document --backend vector --index-policy same-dir --no-index-create --query "search consolidation" --json

# Fast metadata-only search through fingerprints.jsonl. This does not open the
# embedding model; use semantic search or search detail only after a compact hit.
tanit-cli search search ./docs --mode fingerprint --content document --index-policy same-dir --query "invoice rosa overdue" --json

# Search across multiple stores at once — no input path needed, the stores
# define the corpus. Hits merge and rank across stores; each is tagged with
# metadata.index. Works for fingerprint (no model) and semantic (shared --model).
tanit-cli search search --mode fingerprint --index ./tests/search/images/.pixlwiz/search/image.vstore --index ./src/.pixlwiz/search/text.vstore --query "receipt total" --json

# Glob across store sources instead of listing each one.
tanit-cli search search --mode fingerprint --index "./**/*.vstore" --query "invoice total" --json

# OCR-backed image indexing against the checked-in smoke receipt. `ocr-local`
# uses PP-DocLayoutV3 only to choose layout/table mode, then indexes OCR text
# from the preferred OCR VLM. No crop/overlay files are written during index.
# Here `--model` is the vector embedding model, not the OCR VLM.
tanit-cli search index ./tests/search/images/ocr-reciept-1.png --content image --backend vector --adapter ocr-local --index-policy specific-dir --index ./tests/search/images/.pixlwiz/search/ocr-local.vstore --fingerprint cheap --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --log-level info --json

# Search the OCR image index semantically.
tanit-cli search search ./tests/search/images/ocr-reciept-1.png --content image --mode semantic --backend vector --adapter ocr-local --index-policy specific-dir --index ./tests/search/images/.pixlwiz/search/ocr-local.vstore --fingerprint cheap --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --no-index-create --query "basic receipt template total amount paid" --json

# Fast OCR fingerprint search without loading the embedding model. After indexing
# with --fingerprint cheap, --adapter ocr-local implies image content and
# --fingerprint cheap implies fingerprint lookup when --mode is omitted.
tanit-cli search search --adapter ocr-local --path ./tests/search/images/ocr-reciept-1.png --index-policy specific-dir --index ./tests/search/images/.pixlwiz/search/ocr-local.vstore --fingerprint cheap --include-detail page --query "receipt template total" --json

# Expand OCR detail directly from the adapter.
tanit-cli search detail --adapter ocr-local --path ./tests/search/images/ocr-reciept-1.png --include-detail page --json

# External LLM image metadata indexing through the same route as image_understand.
# Provider/model/credentials come from the configured Chat image recognition
# settings; auto-resize defaults to 768px unless --resize-width/--no-resize is
# passed. Here `--model`
# is only the vector embedding model used after metadata extraction.
tanit-cli search index ./tests/search/images/test-photo.jpg --content image --backend vector --adapter llm-meta --index-policy specific-dir --index ./tests/search/images/.pixlwiz/search/llm-meta.vstore --fingerprint cheap --meta-prompt "Describe this image for search indexing in one short paragraph." --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --log-level info --json

# Search the LLM metadata image index.
tanit-cli search search ./tests/search/images/test-photo.jpg --content image --mode semantic --backend vector --adapter llm-meta --index-policy specific-dir --index ./tests/search/images/.pixlwiz/search/llm-meta.vstore --fingerprint cheap --meta-prompt "Describe this image for search indexing in one short paragraph." --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --no-index-create --query "Japanese city street traffic mountains" --json

# Expand LLM metadata detail directly; this does not require sidecar files.
tanit-cli search detail --adapter llm-meta --path ./tests/search/images/test-photo.jpg --include-detail metadata --meta-prompt "Describe this image for search indexing in one short paragraph." --json

# Same-dir creates/reuses ./docs/.pixlwiz/search/text.vstore.
# For globs, same-dir anchors at the non-glob prefix; the Office example uses
# ${KNOWNFOLDER:Desktop}/eduardo/.pixlwiz/search/text.vstore.
# Same flow against another folder: change the input folder/glob.

# Future custom action entry
tanit-cli search action ./docs --mode grep --content text --query "oldName" --action replace --replacement "newName"

# Future image metadata search through find_images()
tanit-cli search search ./photos --content image --mode metadata --query "red product photo" --json

# Future semantic document search through VectorStore
tanit-cli search search ./docs --content document --mode semantic --query "GPU embedding ingest" --index .pixlwiz/search/docs.vstore --json

# Future PDF semantic search, allowing extraction + index creation
tanit-cli search search ./reports --content document --mode semantic --extract auto --query "annual forecast" --index .pixlwiz/search/reports.vstore
```

Default routing:

- `tanit-cli search` is only the command branch; use a subverb.
- `search search`: search files. No `--mode` means `name`; no `--backend`
  means `exact`.
- `--grep`: still maps to `--mode grep` on `search search`.
- `--content document|office --mode grep --backend exact`: direct adapter grep.
  Office files are extracted in memory and scanned without creating/opening a
  vector store. Use this for one-off `.xlsx`/`.docx`/`.pptx` exact searches.
- `--content text|document --mode semantic --backend vector`: local
  `VectorStore`. Known text/source/config files and Office files (`.docx`,
  `.xlsx`, `.pptx` through `office-officecli`) are indexed.
- `--mode fingerprint`: fast sidecar-only search against
  `fingerprints.jsonl`. It matches title/type/keywords/entities/summary/path,
  returns adapter facts and locators, and does not load the vector model.
- `--content image --mode metadata`: legacy `find_images()`.
- `--content image --mode semantic --backend vector --adapter image-sidecar|ocr-local|llm-meta`:
  image vector ingest/search through `image.vstore`.
- `--content document|office|pdf --mode semantic`: adapters + `VectorStore`.
- `--content catalog`: `InfoTool`/catalog lookup.

### Current Core Layout

The unified search code is under `src/search/`:

- `search_request.hpp`
  defines request/result enums and shared result structs.
- `search_facade.hpp/.cpp`
  routes requests. Exact name/grep goes to `src/core/search.cpp`; vector search
  goes to `vector_backend`.
- `source_adapter.hpp/.cpp`
  defines the adapter boundary (`ISourceAdapter`, `SourceUnit`) and the built-in
  `PlainTextSourceAdapter` plus `office-officecli` when
  `FEATURE_OFFICE_EXTRACTOR` is enabled. The same adapter boundary owns
  `detail()` expansion.
- `source_adapter_code.cpp`
  owns `CodeSourceAdapter`: code-file and code-symbol extraction, Tree-sitter
  C/C++ parsing when `FEATURE_CODE_TREE_SITTER` is enabled, heuristic fallback,
  and symbol detail expansion.
- `source_adapter_image.cpp`
  owns image adapters: `image-sidecar` for cheap path/probe metadata,
  `ocr-local` for layout-guided OCR VLM page text, and `llm-meta` for configured
  external/local image understanding.
- `source_adapter_registry.hpp/.cpp`
  owns unique adapter registration and adapter lookup by path.
- `text_index.hpp/.cpp`
  owns text file discovery, glob expansion, default index path selection, and
  search-owned sidecars (`file-cache.json`, `search-manifest.json`).
- `vector_backend.hpp/.cpp`
  owns `VectorStore` open/index/search orchestration and maps vector hits into
  unified hits.

This is intentionally plugin-shaped. Office already follows this adapter path;
PDF, SolidWorks, or other external extractors should feed `SourceUnit` values
into the same vector backend instead of adding format logic to the facade.

Code search has its own Tree-sitter-focused design note:
`docs/search/search-next-code.md`.

Image indexing has its own OCR/visual-metadata design note:
`docs/search/search-next-images.md`.

Adapter unit shape:

- `SourceAdapter::extract()` emits `SourceUnit` records. The stable fields are
  `uri`, `path`, `title`, `type`, `text`, and `tags`.
- `facts` is adapter-specific searchable metadata such as file extension,
  Office sheet hints, code symbols, image EXIF, or PDF page metadata.
- `locator` is adapter-owned detail context such as file, line range, page, row,
  slide, section, or image bounding box.
- Vector ingest stores the unit `uri` as the vector document path and preserves
  `facts` / `locator` in `docs.jsonl`, so semantic hits can return compact
  adapter locators before `search detail` expands them.

Index cache / eviction:

- Text vector stores now keep search-owned sidecars next to `manifest.json`,
  `docs.jsonl`, and `vectors.bin`:
  `file-cache.json`, `search-manifest.json`, and optionally
  `fingerprints.jsonl`.
- The cache tracks input roots, per-directory file counts, and per-file
  `size`, `mtime`, chunk counts, adapter ID, and indexed unit URIs used for
  vector-store add/remove.
- The cache also tracks ingest settings (`content`, vector backend/mode, resolved
  model path, chunk size/overlap, extraction mode, and fingerprint mode). If
  these change, matching files are treated as stale and re-ingested even when
  `size`/`mtime` did not change.
- The search manifest tracks schema version, index kind, content/backend/mode,
  index policy, roots, adapter registry snapshot, model/GPU settings, chunk
  settings, extraction mode, fingerprint mode, and latest stats.
- `fingerprints.jsonl` currently stores cheap, inspectable unit descriptors:
  unit URI/path, adapter, kind, title guess, type guess, keywords, simple
  entities, one-line summary, facts, locator, and structure tags. Retrieval does
  not use embeddings; `--mode fingerprint` scans this sidecar directly for fast
  compact hits.
- Cheap text fingerprints add format hints:
  Markdown reuses the built-in front-matter parser (`title`, `summary`,
  `author`, `tags`) and falls back to the first H1; headings are recorded from
  the front-matter-stripped body. CSV/TSV records header columns; code-like files
  record imports/includes and symbol names from `CodeSourceAdapter`; C/C++ uses
  Tree-sitter locators when available and falls back to a heuristic scan.
- Fingerprints also record `facts.sourcePath` with filename, stem, extension,
  normalized path slug, path segments, and path tokens. `--mode fingerprint`
  boosts matches on these path tokens so folder names like `eduardo`, `2024-q3`,
  or `search-playbook` can produce compact hits before a semantic/detail pass.
- Reruns skip unchanged files, reindex changed files, and remove vector chunks
  for files that disappeared from the scanned inputs.
- Use `--reindex` to force removal/re-ingest of matching current files.
- Use `--log-level trace` on `search index` or `search search` to see progress
  logs (`scan`, `indexed`, `unchanged`, `removed`, `done`) through
  `logger::info` / `logger::trace`.

Output:

- `tanit-cli search search ... --json` emits the unified envelope:

```json
{
  "ok": true,
  "query": "gemini",
  "mode": "grep",
  "content": "text",
  "backend": "exact",
  "hits": [],
  "stats": {}
}
```

- Non-JSON output should stay grep-like for exact search and concise for semantic
  search: path, score, title/snippet.

Code smoke examples against `./src`:

```powershell
# Build/update a code index with cheap fingerprints.
tanit-cli search index ./src --content code --backend vector --index-policy same-dir --fingerprint cheap --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --log-level trace --json

# Fast sidecar-only fingerprint search. This should not load the embedding model.
tanit-cli search search ./src --content code --mode fingerprint --index-policy same-dir --query "vector backend chunks" --json

# Semantic search over the code index.
tanit-cli search search ./src --content code --mode semantic --backend vector --index-policy same-dir --no-index-create --query "function that indexes source units into vector store" --json

# Code adapter detail lookup. Adapter id remains code-heuristic while it uses
# Tree-sitter internally for C/C++ when FEATURE_CODE_TREE_SITTER is enabled.
tanit-cli search detail --adapter code-heuristic --path ./src/search/vector_backend.cpp --selector 'symbol:index_vector' --include-detail section --context-before 2 --context-after 2 --json
```

### rg Exact-Search Backend

`src/search/search_facade.cpp` wraps the bundled `rg` binary for both
file-name and full-text search. Activated by setting `exact_indexer = "rg"` in
the `Request` (CLI: `--indexer rg`) **and** enabling the `ENABLE_RG_FILE`
compile-time constant in `search_facade.cpp` (default `false` — trial gate).

File-name mode (`--mode name --indexer rg`):

```
rg --files [-g "!<excl>"] -g "*<query>*" [inputs...]
```

FTS mode (`--mode grep --indexer rg`):

```
rg -i [-w] [--column --line-number --no-heading --color never]
   [-B N] [-A N] [-t type] [-g "!<excl>"] <query> [inputs...]
```

Both respect `req.limit`, `req.offset`, `req.recursive`, `req.include_hidden`,
`req.include_globs`, `req.exclude_globs`, and `req.cancel_requested`.
The resolved command is recorded in `Result::stats["rg_cmd"]` for diagnostics.

Context lines (`-B N` / `-A N`) are forwarded to rg and attached to hits via
the `{"type":"context"}` JSON events. `context_before` lines (line number less
than the match) are collected in a pending buffer and attached when the match
event arrives. `context_after` lines (line number greater than the match) are
patched onto the last hit directly. Streaming `on_hit` receives hits without
`context_after` (it fires before the after-context is known — same behaviour as
the `own` streamed path); the collected `Result::hits` carry both sides.

Both modes use `media::process::run(RunMode::Captured)` — not `popen`:
- `on_output_chunk` streams stdout lines as they arrive → no buffering of the full output.
- `on_cancel_check` is polled periodically; sets `stop` atomically so the chunk callback also stops mid-stream.
- `timeout_ms = 30 000` hard-kills rg on hung network mounts or runaway trees.
- `bind_to_parent = true` adds the child to the kill-on-close job object so rg cannot outlive the host process.
- rg exit code 1 (no matches) is treated as `ok=true`. Timeout and hard failures set `ok=false` with `stats.timed_out`/`stats.exit_code`.

Binary lookup order: `<exe_dir>/bin/rg[.exe]` → `PATH`.

### Status And TODOs

Done in the current slice:

- Unified CLI shape: `search search`, `search index`, `search detail`, and
  placeholder `search action`.
- Adapter registry with unique adapter IDs.
- Built-in adapters:
  - `plain-text`
  - `code-heuristic` with Tree-sitter-backed C/C++ extraction when vendored
    `packages/tree-sitter` and `packages/tree-sitter-cpp` are present.
  - `office-officecli` when `FEATURE_OFFICE_EXTRACTOR` is enabled.
- Vector indexing/search for text, code-like text, and Office documents.
- Same-dir and specific-dir vector index policies.
- `file-cache.json` and `search-manifest.json` sidecars.
- Cache invalidation by file size/mtime plus ingest settings:
  content/backend/mode, resolved model, embedding threads/GPU layers, chunking,
  extraction mode, adapter options, and fingerprint mode.
- Structured manifest provenance for embedding backend/model/thread/GPU settings,
  adapter options and option hashes, prompt hashes when configured, and
  fingerprint sidecar path.
- Unit URI based add/remove for vector chunks.
- `SourceUnit::facts` and `SourceUnit::locator` persisted into `docs.jsonl`.
- `--fingerprint cheap` and `fingerprints.jsonl`.
- `--mode fingerprint` sidecar-only fast search, no embedding model load.
- Markdown front matter reuse through `parse_frontmatter()`.
- Source path facts and path-token scoring.
- Tree-sitter C/C++ symbol extraction for code units, with heuristic fallback.
- Code symbol quality bar: parser-backed namespace, parent, qualified symbol,
  signature, doc comment, line range, and byte range facts are stored in
  fingerprints and vector metadata.
- `FEATURE_CODE_TREE_SITTER` documented and enabled through
  `CMakePresets.json`, with CMake fallback to heuristic extraction when vendored
  generated sources are missing.
- Office detail expansion and inline `--include-detail`, including row context
  and Markdown table output.
- `--detail-max` for limiting inline detail expansion.
- Image adapters:
  - `image-sidecar` for cheap path/probe/adjacent sidecar metadata.
  - `ocr-local` for page-level OCR through PP-DocLayoutV3 table detection plus
    preferred OCR VLM. It indexes a compact, non-chunked page unit so initial
    image search returns one hit per OCR unit.
  - `llm-meta` for image-understanding metadata through the configured Chat image
    recognition route, with `--meta-prompt`, `--resize-width`, and `--no-resize`.
    It indexes a compact, non-chunked metadata unit.
- Multi-dimensional fan-out in `unified_search`: repeatable/comma-separated
  `--content/--mode/--backend` run the cartesian product of tasks in explicit
  order, skip incompatible `(mode, backend)` pairs, de-duplicate by
  path/line/column, tag each hit with its originating task, and merge under one
  result with per-task provenance in `stats.tasks`. `--sort auto|none|score|path`
  orders the merged set. Single-value calls keep identical behaviour. The CLI is
  a thin surface over this library entry point for the LLM tool and web reuse.
- Multi-store search: `--index` is repeatable / comma-separated / glob-aware.
  When more than one store is given, or when no input path is supplied,
  semantic and fingerprint search read the existing store(s) directly (never
  ingesting or pruning), then merge and rank hits across stores. Each hit is
  tagged with `metadata.index` and `stats.stores` reports per-store status.
  `resolve_search_index_dirs()` in the library does the expansion so all
  surfaces share it.
- IPC registry visibility for search/index runs and vector model instances.
- Bundled sidecar executable resolution via app `bin`.
- Smoke coverage in `tests/orchestrator/test-search.mjs`, including code
  namespace/parent/qualified-symbol/signature/doc-comment/byte-range assertions.

Next implementation candidates:

1. Image indexing polish.
   See `docs/search/search-next-images.md`. Next steps are richer image
   fingerprints, bbox/block/line detail locators, and keeping model-backed
   OCR/meta smoke coverage opt-in unless CI gets stable model/router access.
2. Code search parser polish.
   See `docs/search/search-next-code.md`. Next steps are constructors,
   destructors, operators, template-heavy declarations, and multi-language
   grammar registration.
3. Richer Office units.
   Add workbook/sheet/slide/row units at indexing time instead of mostly
   whole-document text, and persist native Office locators in fingerprints.
4. Fingerprint search polish.
   Add Markdown output optimized for compact LLM/operator review and expose
   score components for debugging.
5. LLM/tool integration.
   Surface `search search --mode fingerprint`, semantic search, and
   `search detail` as LLM tools with compact hit schemas and explicit detail
   follow-up.
6. PDF adapter.
   Add page/paragraph units and page locators as another `SourceAdapter`.
7. Hash-based invalidation.
   Add optional content hashes for same-size/same-mtime edge cases if practical.
8. Plugin boundary.
   Move from built-in adapters to loadable DLL/plugin adapters for SolidWorks and
   other foreign formats.
9. Custom actions.
   Implement only after hit schemas and detail locators are stable.

### What Not To Do

- Do not put PDF parsing into `src/core/search.cpp`.
- Do not put embedding/model/GPU state into `src/core/search.cpp`.
- Do not make `find_images()` depend on `VectorStore`.
- Do not make `file_search` silently do semantic search; it should stay exact
  and predictable.

Remaining cleanup items:

- Implement or remove the stale OS indexer path in `search.cpp`.
- Share path expansion, glob filtering, sensitive path handling, and image
  extension lists between `search` and `find`.
- Decide whether `file_read` should keep delegating to
  `media::llm::execute("file_read")` or move the read implementation fully into
  `FsTools`.
- Decide whether vector stores are explicit user-managed indexes or automatic
  per-folder/per-library indexes.
- Keep converging on one result schema for exact, fingerprint, semantic, image
  metadata, and catalog matches.
