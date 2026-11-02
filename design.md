# Search package

`src/search` is the orchestration layer (`media::search`). It is not a portable library yet. Tanit compiles the ten `.cpp` files straight into `pm-media` (`CMakeLists.txt` around the `src/search/` entries). `tanit-cli search` (`src/cli/pm_image_cmd_search.cpp`) is the only CLI, and it prints human text, not the package JSON contract.

A second host (pm-pics or anything else) needs `packages/search.cpp`: static lib `pm-search-lib`, test exe `pm-search`, namespace stays `media::search`. The exe links only what exact search needs. Semantic, image, office, and remote inputs stay hooks with a clear error when nothing is installed.

This note is the follow-up list. It is not the integration contract. Do not move sources until the cut below is agreed.

## What is actually in the tree

| File | Job | Can travel alone |
| --- | --- | --- |
| `search_request.hpp` | `Request`, `Hit`, `Result`, JSON parse, mode enums | Almost. `Hit` embeds `pm::viewer::TextRange` |
| `search_facade.cpp` | `unified_search`, `unified_index`, `unified_detail`, `count_file_globs` | No. It is the switchboard |
| `text_index.cpp` | On-disk text cache, sha256 via picosha2 | After path/glob/extensions are commons includes |
| `fingerprint.cpp` | Cheap fingerprint over chunks | Needs `lib/llm/chunking_md.hpp` |
| `search_markdown.cpp` | Render hits as markdown | Yes |
| `source_adapter.cpp` | Plain text extract | Office and OpenDocument are `#if` siblings in the same file |
| `source_adapter_code.cpp` | Code heuristic | Tree-sitter is `#if FEATURE_CODE_TREE_SITTER` |
| `source_adapter_image.cpp` | EXIF, layout OCR, VLM caption | Host stack. Do not link this into `pm-search` |
| `source_adapter_registry.cpp` | Process-global adapter list | Singleton. See below |
| `vector_backend.cpp` | Semantic index and query | `FEATURE_LLAMA` and `media::llm::VectorStore` |
| `search_presets.cpp` | Cwd + bookmark roots | Settings store and VFS URI checks |

The header comment in `search_request.hpp` is still true: exact name/grep lives in `src/core/search.cpp` (`search_files`). Image metadata as a command lives in `src/core/find.cpp`. Semantic documents live in `src/lib/llm/local_vector_store/`. `src/search` calls those. It does not own them.

`src/core/search.hpp` is a second API (`SearchOptions`, `SearchResult`, `BatchControl*`). The facade calls `search_files` for the `"own"` indexer. Moving `src/search` without that walker leaves name/grep with nothing to run.

## Host facts the facade reaches for

These are the reasons a naive `add_subdirectory` does not configure outside Tanit.

- **Own indexer.** `search_files` takes `batch_queue.hpp` / `BatchControl`. Cancel for the CLI is a `std::function<bool()>`, but the signature still requires the batch type. A standalone walker has to drop `BatchControl` or hide it behind the host hook.
- **Ripgrep.** `ENABLE_RG_FILE` is a `static const bool` in `search_facade.cpp`, not a CMake option. The rg path is `SearchPathW(L"rg.exe")` plus `media::process::run` (`process/process_runner.hpp`) and `core/app_exe_directory.hpp`. There is no vendored rg target in this package.
- **OS indexer.** `exact_indexer=os` is Windows Search / Spotlight / locate inside the same facade. It is not a separate library.
- **Remote inputs.** A non-file input goes through `pm::vfs::is_remote_uri` and `pm::vfs::ops::collect_copy_nodes` (`vfs/operations/copy.hpp`). That pulls the VFS copy engine into every search link if it stays a direct include.
- **Bookmarks.** `resolve()` is already pure (cwd + a list of folder paths). `resolve_from_settings()` and `resolve_from_bookmark_items()` include `core/settings_store.hpp` and `media::settings::BookmarkItem`. Presets also special-case remote URIs and reject `\dist\win-x64`.
- **Viewer ranges.** `Hit::range` is `pm::viewer::TextRange` from `core/viewer_locate.hpp`, which includes `media_fragment.hpp`. Grep hit conversion calls `utf8_byte_offsets_to_utf16_range`. That type is a viewer/LSP coordinate, not a search rule.
- **Export macro.** Public functions are `POLYMECH_API`. A package outside `pm-media.dll` cannot include `polymech_export.h`.
- **Logger.** Several `.cpp` files include `logger/logger.h`.

## Backends that are optional products, not the library

Each one is a compile-time wall today. For another app they become an installed function. Unset means that mode returns `ok:false` and names the missing backend. Exact search still runs.

| Backend | Gate | What it includes |
| --- | --- | --- |
| Semantic | `FEATURE_LLAMA` | `VectorStore.hpp` → llama `EmbeddingModel`, GGUF, `vectors.bin` / `docs.jsonl` / `manifest.json`. `k_default_gpu_layers` is `-1` only when CUDA, Vulkan, or Metal is on |
| Image tags | always compiled in the image adapter | `core/image_tags.hpp` |
| Layout OCR | `FEATURE_ONNX` | `OnnxSession`, `OnnxDocLayout` |
| VLM caption | llama VLM | `llm/llama_vlm_client.hpp`, `PictureTools.hpp`, `core/local_model_catalog.hpp`, `core/resize.hpp` (libvips) |
| Image model discovery | settings | `core/settings_runtime.hpp` |
| Office | `FEATURE_OFFICE_EXTRACTOR` | `lib/llm/office_source_adapter.hpp`, officecli. Adapter id `office-officecli` |
| OpenDocument | `FEATURE_COMMAND_MARKDOWN` | anydoc. Adapter id `opendocument-anydoc`. Same `.cpp` as plain text |
| Code symbols | `FEATURE_CODE_TREE_SITTER` | `tree_sitter/api.h`, `tree-sitter-cpp.h`. The heuristic path compiles without it |
| Markdown chunking | fingerprint | `lib/llm/chunking_md.hpp` |

`source_adapter_image.cpp` is the file that makes the package look host-bound. It is one translation unit that calls resize, ONNX, VLM, tags, and the model catalog. Leave it on the Tanit target until those calls are hooks. Do not `#ifdef` the whole product matrix inside `pm-search-lib`.

`vector_backend.cpp` is the same shape: the store format is simple, the embedder is llama. The hook is "embed these strings" and "open this store directory". The brute-force cosine files can move later. The GGUF runtime does not move with search.

## Singleton and two copies

`source_adapter_registry.cpp` keeps the adapter list in a function-local static under a mutex. `pm-media.dll` and an exe that also links the static lib each get a copy (same rule as `PM_COMMONS_HOST_SINGLETONS`). When Tanit links the package, set `PM_SEARCH_HOST_SINGLETONS` and compile that one TU into `pm-media` only. `pm-search` and a host that does not load the DLL compile it themselves. Default of the option stays OFF so a standalone checkout still links.

Do not compile `src/search/*.cpp` in `pm-media` and again inside `pm-search-lib`. One of those lists has to drop the files when the host switches.

`std::regex` is already in `search_facade.cpp` and `source_adapter_code.cpp`. Leave it. A new parser is a behavior change, not part of the move.

## Standalone CLI

`pm-search` exists only when the package is the top-level project. JSON on stdout, one object, no GUI. Exit 0 / 1 / 2 as in the extract-package CLI contract. `PM_SEARCH_CLI` overrides the binary. Tests are new `tests/test-*.mjs` files. `tests/orchestrator/test-search.mjs` stays a Tanit test until a call site is wired. It drives `tanit-cli`, not this exe.

Subcommands match the host command, with a smaller first slice:

| Command | First CLI | Later, when a hook is installed |
| --- | --- | --- |
| `search` | `exact_indexer=own`, name and grep, globs, cancel | `rg`, `os`, semantic, metadata |
| `index` | text cache + cheap fingerprint | vector store |
| `detail` | plain text and code heuristic | office, image, pdf |
| `roots` | the pure `resolve()` from paths the user passes | `resolve_from_settings` stays on Tanit |
| `action` | not in the first exe | replace stays a host action |

Self-contained link of that exe:

- `pm-search-lib`
- CLI11 v2.4.2 and nlohmann v3.11.3, fetched only when the targets are missing
- `pm-commons-lib` for `fs/path_sanitizer.hpp`, `fs/glob_paths.hpp`, `fs/file_extensions.hpp`, `string/string_sanitizers.hpp`
- picosha2 (header)
- the own walker, once it no longer includes `batch_queue.hpp`

Not on that link: llama, onnxruntime, vips, officecli, anydoc, tree-sitter, VFS, the settings store, Win32 UI, `polymech_export.h`. Ripgrep is an external binary. The CLI accepts a path to `rg` and shells out with a small `std::function` runner. Tanit installs `media::process::run` and its `rg.exe` lookup. The package does not vendor rg and does not SearchPath the host dist folder.

Logger calls in moved sources go through a hook that defaults to silent. The CLI may print log lines on stderr. They are not part of the JSON object.

## Cut, in order

1. **Types.** Copy `Request` / `Hit` / `Result` / parsers into the package. Replace `pm::viewer::TextRange` with a search-owned range (line, UTF-16 column, same half-open rule). Tanit converts at the facade edge. Drop `POLYMECH_API` inside the package.
2. **Own exact search.** Split `search_files` off `BatchControl`, or move the walker into the package and leave a Tanit wrapper that still accepts `BatchControl*`. This is the blocking loose end. The facade cannot be the first file that moves.
3. **Plain extract + text index + markdown + fingerprint.** Fingerprint takes a chunk function. The default is a local splitter. Tanit may pass `chunking_md`.
4. **Hooks** in one header, commons style (`host/services.hpp`): run process, locate rg, embed, open vector store, extract office, extract image, list remote children, log. Getters no-op or return the error string for that mode.
5. **CLI** for steps 1–3. `npm test` in the package before any host edit.
6. **Presets.** Move `resolve()`, `normalize_path_utf8`, and the reject-root check. Keep `resolve_from_settings()` in Tanit. The `\dist\win-x64` reject is a Tanit option on `Options`, not a hardcoded package rule.
7. **Tanit switch.** `add_subdirectory` + `target_link_libraries(pm-media PRIVATE pm-search-lib)`, delete the `src/search/*.cpp` entries, install hooks from one host TU. Image, vector, office, and the rg/`process_runner` path stay host TUs until each hook exists. Registry TU follows `PM_SEARCH_HOST_SINGLETONS`.
8. **Feature macros** (`FEATURE_LLAMA`, `FEATURE_ONNX`, `FEATURE_OFFICE_EXTRACTOR`, `FEATURE_COMMAND_MARKDOWN`, `FEATURE_CODE_TREE_SITTER`) stay on the host TUs that still have the `#if`. Do not `PUBLIC` them onto every search consumer.

## Loose ends to leave written down

- `docs/search.md` is named by `src/core/search.hpp` and is not in the tree.
- `ENABLE_RG_FILE` overrides the request unless `exact_indexer` is set. A package build must not hide that behind a cpp bool.
- Chat typeahead does not use `search_presets` yet. The header says so. Do not fold that builder into the first move.
- `FsTools` and `search_tools` call `unified_search` / `resolve_from_settings` from the LLM tool host. They keep working only if the host still installs bookmarks and the vector hook.
- Native overlay (`Mainfrm_commands.cpp`) uses `CwdOnlyWhenPresent` and `resolve_from_settings`. That call site stays.
- Code and plain-text adapters share a registry that is filled with `#if` on office and markdown. A lean build that registers only plain text and code is the intended default. Tanit registers the rest.
- Vector store thread-safety is "none" (`VectorStore.hpp`). The facade mutex does not make llama embed calls safe. The hook must document that.
- Android is not a search target. Do not claim it.
- `std::regex` and the UTF-16 column rule are locked behavior. Tests have to pin them before anyone replaces the walker.
