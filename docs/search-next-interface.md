# Central Search Interface

surfaces : cli, llm tool, info tool, web ( streaming )

## Sources / Context

- Current Folder | Current/Open File(s)
- Registered Stores
- MRU - Folders/Files

## Configuration

- default types : text/code/images/documents
- default modes : name | grep | fingerprint (for given stores)
- default sort : m/c-time | relevance | ranker

## Search Results

- default ranking : name/grep first
- adjustable default max per group : 10
- adjustable : typeahead list results and detail pane ( folder list / file preview )
- portable json format ( open results in virtual browser)

## Streaming (web, CLI)

Search is collect-and-return by default, but the library exposes an optional
streaming sink on `media::search::Request` so results arrive incrementally and
the surface can render progressively and cancel mid-flight:

- `on_hit(const Hit&) -> bool` — fired as each fan-out task (and store) produces
  a hit, in task order. Return `false` to request a cooperative early stop
  (used to honor `--head-limit`). Streamed hits are **compact** (no inline
  detail); fetch detail lazily via `search detail` for a specific hit.
- `on_event(json)` — coarse progress, e.g. `{"event":"task","content":..,"mode":..,"backend":..,"matches":N,"ok":..}`.

The facade only fires `on_hit`/`on_event`; the `start`/`done` framing belongs to
the surface so it can wrap the whole request. Cancellation is the existing
shared flag: `Request::cancel_requested` → `media::cli::cancel_requested()`
(console Ctrl+C, or `request_cancel()` from the IPC `commandRunCancel` action).

### CLI: `--stream` (NDJSON)

`tanit-cli search search ... --stream` emits one compact JSON object per
line, flushed immediately:

```jsonc
{"event":"start","query":..,"content":..,"mode":..,"backend":..,"fanOut":bool,"sort":..,"inputs":N,"stores":N}
{"event":"hit","index":0,"hit":{ path, title, snippet, source, score, contentType?, line?, column?, metadata? }}
{"event":"task","content":..,"mode":..,"backend":..,"matches":N,"added":N,"ok":true}
{"event":"done","ok":..,"emitted":N,"matches":N,"stopped":bool,"cancelled":bool,"stats":{..}}
```

`--head-limit N` and `--offset N` apply to the streamed hit count; `done.stopped`
is `true` when the stream ended early at the head-limit, `done.cancelled` when
Ctrl+C / IPC cancel fired. Hits arrive in task order (name → grep → semantic),
independent of `--sort`, which still orders the non-stream `hits[]` envelope.

Example:

```bash
tanit-cli search search ./docs --backend exact --content any --mode name --mode grep --query image --sort path --stream --head-limit 6
```

### Two transports, one event shape

The `on_hit` / `on_event` sink and the `start | hit | task | done` event shape
are **transport-agnostic**. Two surfaces consume them:

- **CLI `--stream` (out-of-process):** headless/testing, the LLM tool, and any
  caller that wants a child process. Could be driven from the web via a captured
  `commandRun` + NDJSON parse, but that **reloads the embedding model per search**
  and cannot share open stores — so it is *not* the web path.
- **Direct in-process host (`CWebView_Search`):** the WebView app path. Runs
  `unified_search` / `unified_index` on a worker thread inside the app process,
  reusing the already-loaded embedding model and open vector stores, and posts
  the same events to the page via `CWebViewManager::PostToFromAnyThread`. See
  **Direct Web Host** below.

## Direct Web Host (`CWebView_Search`)

In-process search & indexing host for the WebView app. New unit
`src/win/web/CWebView_Search.{h,cpp}`, owned by `CWebViewManager` (constructed
in `ConfigureHost`, like the command sinks). The manager routes `search*` /
`index*` / `store*` providerRpc methods to it from `HandleBuiltinMessage`; the
host runs the unified facade on worker threads and streams results back to the
originating webview via `PostToFromAnyThread`. **UI is out of scope** here — this
is purely the host-side engine + protocol. Design goal: an arbitrary number of
**concurrent** search and indexing tasks, each independently configurable and
independently cancelable.

### Principles

- Library does the work; `CWebView_Search` is only orchestration + transport.
  Same `media::search::Request` / `unified_search` / `unified_index` /
  `Request::on_hit` / `on_event` used by the CLI.
- Every task gets a unique `taskId` (host-assigned, string). The page never
  guesses ids; it receives the `taskId` in the `accepted` reply and on the
  `start` event.
- Off the UI thread always: each task runs on a worker (bounded pool, see
  Concurrency). Events are posted with `PostToFromAnyThread`.
- Per-task cancellation — **never** the process-global `media::cli::cancel_requested()`
  (it cannot distinguish concurrent runs). Each task owns a
  `std::shared_ptr<std::atomic_bool>` wired into `Request::cancel_requested`.

### RPC surface (page → host, `providerRpc`)

Mirror the CLI surface so anything expressible on the CLI is expressible here.

| method | payload (selected) | reply |
| --- | --- | --- |
| `searchStart` | full request config (see Config) | `{ taskId, accepted, error? }` |
| `searchCancel` | `{ taskId }` | `{ taskId, cancelled }` |
| `indexStart` | request config + ingest opts | `{ taskId, accepted, error? }` |
| `indexCancel` | `{ taskId }` | `{ taskId, cancelled }` |
| `searchTasksGet` | — | `{ tasks: [{ taskId, kind, phase, query, startedMs, hits, ... }] }` |
| `searchCancelAll` | `{ kind? }` | `{ cancelled: N }` |
| `searchStoresGet` | `{ roots?: string[] }` | `{ stores: [{ path, model, docs, ok }] }` |
| `searchAdaptersGet` | — | `{ adapters: [{ id, kinds, options }] }` |
| `searchConfigGet` | — | defaults + enum domains (modes/contents/backends/sorts) for building UI later |

`searchStart` / `indexStart` reply **immediately** (`accepted:true` + `taskId`)
after spawning the worker; results arrive only as events. A rejected request
(bad config, pool saturated with `queue:false`) replies `accepted:false,error`.

### Event protocol (host → page)

One envelope kind, discriminated by `event`, carrying `taskId` so the page can
demux concurrent tasks. Posted via `PostToFromAnyThread(fromId, …)`.

```jsonc
{ "kind": "hostSearchEvent", "taskId": "...", "event": "start",
  "request": { content, mode, backend, fanOut, sort, inputs, stores } }
{ "kind": "hostSearchEvent", "taskId": "...", "event": "hit",
  "index": 0, "hit": { path, title, snippet, source, score, contentType?, line?, column?, metadata? } }
{ "kind": "hostSearchEvent", "taskId": "...", "event": "task",      // per fan-out task progress
  "content":.., "mode":.., "backend":.., "matches": N, "added": N, "ok": true }
{ "kind": "hostSearchEvent", "taskId": "...", "event": "done",
  "ok":.., "emitted": N, "matches": N, "stopped": bool, "cancelled": bool, "stats": {..} }
```

Indexing adds progress events (needs a small lib addition, see Todos):

```jsonc
{ "kind": "hostSearchEvent", "taskId": "...", "event": "index-start", "files": N, "index": "<store>" }
{ "kind": "hostSearchEvent", "taskId": "...", "event": "index-file",
  "path": "...", "done": N, "total": N, "chunks": N, "status": "indexed|skipped|cached|error", "error"? }
{ "kind": "hostSearchEvent", "taskId": "...", "event": "index-done",
  "ok":.., "indexedFiles": N, "indexedChunks": N, "cancelled": bool, "index": "<store>", "stats": {..} }
```

This is the **same event vocabulary** as the CLI NDJSON; only the framing differs
(`kind:"hostSearchEvent"` + `taskId` instead of one object per stdout line).

### Configuration (full `Request` surface)

`searchStart`/`indexStart` payloads map 1:1 onto `media::search::Request`, so the
host is "fully configurable":

- routing: `query`, `inputs[]`, `content`/`mode`/`backend` **or** their plural
  fan-out arrays (`contents[]`/`modes[]`/`backends[]`), `sort`.
- store/index: `index`/`indexDirs[]` (repeatable, glob-aware), `indexPolicy`,
  `model`, `threads`, `gpuLayers`, `reindex`, `createIndex`, `extract`,
  `adapter`, `adapterOptions{}`, `fingerprint`, `chunkSize`, `chunkOverlap`.
- exact: `regex`, `caseSensitive`, `wholeWord`, `namesOnly`, `recursive`,
  `includeHidden`, `followSymlinks`, `skipBinary`, `contextLines`/`before`/`after`,
  `multiline`, `includeGlobs[]`, `excludeGlobs[]`, `excludeDirs[]`, `typeFilter`,
  `maxFileSize`, `maxMatchesPerFile`, `outputMode`.
- limits/stream: `limit`, `headLimit`, `offset`, plus host-only stream caps
  (`maxPerGroup` default 10, `eventCoalesceMs`).
- detail stays lazy: streamed hits are compact; the page calls the existing
  `searchDetail` (→ `unified_detail`) for a selected hit.

A shared `request_from_json(const nlohmann::json&)` helper builds the `Request`
(also reusable by the CLI/LLM-tool to converge parsing).

### Concurrency & run registry

- `CWebView_Search` keeps a registry: `taskId → { kind, fromId, phase, cancel
  (shared_ptr<atomic_bool>), thread/future, startedMs, counters }` under a mutex.
- Bounded worker pool (config, default e.g. 2 search + 1 index concurrent);
  excess requests either queue (`queue:true`, default) or are rejected
  (`accepted:false, error:"busy"`). Indexing is heavy → small cap.
- `searchTasksGet` snapshots the registry for a future "active tasks" panel.
- On worker completion: post `done`/`index-done`, then drop the registry entry.

### Shared model / store safety (must verify)

Concurrent runs reusing one resident embedding model and the same `.vstore`:

- Confirm `VectorStore::open` + `search` are safe under concurrent readers; if
  not, serialize per-store opens or cache one shared handle per store path with
  a read lock.
- Confirm the embedding model handle tolerates concurrent `embed()` calls; if
  not, guard embedding with a mutex (serialize the embed step, parallelize IO).
- Indexing **writes** a store → exclusive per-store lock; reject/queue a search
  or second index against a store that is mid-ingest (keyed by resolved store
  path). Reuse / extend any existing store-level lock.

### Lifecycle & cleanup

- `searchStart` registers `fromId` as the event target (like
  `AddCommandEventTarget`) for the task's lifetime.
- On `UnregisterExternal(id)` / view close: cancel all tasks whose `fromId == id`
  (set their cancel flags), detach, and drop targets — no posts to dead views
  (`PostToFromAnyThread` already no-ops on missing targets).
- `~CWebView_Search`: cancel-all + join/await workers.

### Errors & backpressure

- Worker exceptions → `done.ok=false, error`. Never crash the UI thread.
- Cap hits per group (`maxPerGroup`) and coalesce rapid `hit`/progress posts
  (`eventCoalesceMs`) so a huge grep can't flood the WebView message pump — this
  is the concrete answer to "nb of search results sent, detached from main
  thread".

### Relationship to the CLI

Both paths share `request_from_json`, `unified_search`/`unified_index`, and the
`on_hit`/`on_event` sink. CLI `--stream` = stdout NDJSON framing; `CWebView_Search`
= `hostSearchEvent` + `taskId` framing. No logic duplication.

## Search Result Actions

- Open in explorer, open in app, add to chat context

## Using in chat

- user types '@' => we offer by default open files, then via type-ahead and fuzzy search name match files in current folder

## References

- [Code Search](./search-next-code.md)
- [Image Search](./search-next-images.md)
- [Search Implementation](./search-next.md)

### Examples

tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/**/*.xlsx' --content document --backend exact --no-index-create --mode grep --query amazon

# rg-backed file-name search (requires ENABLE_RG_FILE=true in search_facade.cpp)
tanit-cli search search ./src --mode name --backend exact --indexer rg --query "search_facade"

# rg-backed FTS search
tanit-cli search search ./src --mode grep --backend exact --indexer rg --query "unified_search" --json

tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/' --content any --backend any --no-index-create --mode grep --query amazon

see@pixlwiz/docs/search/search-next-interface.md

we're heading for the general search interface, cli first -

tanit-cli search search '${KNOWNFOLDER:Desktop}/eduardo/' --type any --content document --backend exact --no-index-create --mode name --query '*details*'

## Todos

### Done

- [x] Verify result schema completeness (context, scoring, vector-store info) — present on `Hit.metadata` / `Result.stats`.
- [x] Library streaming sink (`Request::on_hit` / `on_event`) driven from `unified_search`, per-task + per-hit, with cooperative early stop.
- [x] CLI `--stream` NDJSON (start/hit/task/done) with `--head-limit` / `--offset` and `cancelled` reporting; smoke-tested (`tests/orchestrator/test-search.mjs` §2c).

### Phase 0 — library prerequisites (shared by CLI + web)

- [x] `request_from_json(const nlohmann::json&) -> Request` helper (single source of config parsing for CLI/web/LLM-tool).
- [x] Per-run cancel: facade/indexing use `Request::cancel_requested`, so a per-task `atomic_bool` fully controls a run (CLI may still wire the global flag, but host runs can stay isolated).
- [x] Index progress sink: `index_vector` emits `on_event` (`index-start` / `index-file` / `index-done`) and polls cancel in the ingest loop.
- [x] Concurrency guard: per-store shared/exclusive lock in `vector_backend` (search readers use shared lock, indexing uses exclusive lock).
- [x] User run fields for agent contexts: `Request.run_name` + `Request.user` propagated to search/index stats and persisted to `search-manifest.json`/provenance (for indexing), with search staying non-persistent.

### Phase 1 — `CWebView_Search` host (direct, in-process)

- [x] New `src/win/web/CWebView_Search.{h,cpp}`; owned + constructed by `CWebViewManager::ConfigureHost`.
- [x] Task registry: `taskId → { kind, fromId, phase, cancel, worker, counters }` + bounded concurrency/queue-or-reject scheduling (`maxConcurrentSearch`, `maxConcurrentIndex`, `queue`).
- [x] `searchStart` / `indexStart`: build `Request` via `request_from_json`, assign `taskId`, run on worker, stream `hostSearchEvent` via `PostToFromAnyThread`; reply `{ taskId, accepted }` immediately.
- [x] `searchCancel` / `indexCancel` / `searchCancelAll` flip per-task cancel flags.
- [x] Stream caps: `maxPerGroup` (default 10) + `eventCoalesceMs` coalescing for progress events (`task`/`store`/`index-file`/`index-start`).
- [x] Lifecycle: cancel tasks on `UnregisterExternal`; cancel-all + join in dtor.

### Phase 2 — host introspection + bridge types

- [x] `searchTasksGet`, `searchStoresGet`, `searchAdaptersGet`, `searchConfigGet` (defaults + enum domains).
- [x] Route `search*`/`index*`/`store*` methods from `CWebViewManager::HandleBuiltinMessage` to `CWebView_Search`.
- [~] `apps/shared/web/hostBridge.ts`: `HostSearchEvent` type + `callSearchStart/Cancel/...` helpers + search-event subscription implemented (`onSearchEvent`); task-specific demux helper wrapper still pending.
- [ ] Smoke path: drive `searchStart`/events/`searchCancel` against the host (CLI `--stream` already validates the shared event shape).
- [ ] rg backend smoke: enable `ENABLE_RG_FILE`, run name + grep modes against `./src`, compare hit counts with the `own` indexer.
- [ ] rg backend: graduate from trial to default when smoke is stable; add `--indexer rg` to CLI docs and auto-select `rg` when the binary is present and `mode=name`.
