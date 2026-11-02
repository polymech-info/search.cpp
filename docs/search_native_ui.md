# Native Search Overlay — Design and Implementation Plan

Status: initial filename-first native overlay implemented. The first slice includes
the Contents toggle and shares the existing `CWebViewSearch` execution path through
`CWebViewManager`; the broader options/preview phases remain open.

Current implementation:

- `src/win/search_native/search_native_popup.hpp/.cpp`;
- centered, resizable, rounded Win32 tool window with light/dark theming;
- font/DPI-derived query-row sizing and vertically balanced single-line edit;
- filename search by default; Contents adds `grep`;
- streamed virtual owner-draw results, keyboard navigation, and activation;
- exact text-range navigation plus same-file match decoration;
- current-folder and folder-bookmark roots with install/build-root rejection;
- `togglesearchnative` command plus ribbon-tab and status-bar mini-toolbar buttons;
- Tabler `file-search.svg` icon immediately before Command Palette;
- no default shortcut yet; the web overlay retains `Ctrl+Space`.

The popup does not instantiate a WebView or load `search.pmbundle`. It currently
uses the browser-feature search host as a temporary transport seam, so builds
still require `FEATURE_BROWSER` until Phase 1 extracts the typed session service.

## Goal

Add a `togglesearchnative` app command that opens a fast, WebView-free Win32
search overlay. Its first release should match the focused overlay experience
from `apps/search/src/SearchApp.tsx`, not the full docked Search application.

The native overlay must:

- open centered over the main frame at approximately the current 640 × 420
  logical-pixel size;
- use the current folder, viewer path, Explorer selection, and folder bookmarks
  as search context;
- default to file/path-name search;
- optionally add file-content search;
- stream results while the user types;
- support full keyboard navigation and activation;
- open text hits at their exact range and decorate other matches in the file;
- share the existing search execution/session implementation rather than
  creating a second search engine;
- follow the app theme, UI font, DPI, and accessibility behavior;
- remain usable in builds that do not include WebView2 after the transport-neutral
  session extraction. The initial shared-host slice is still `FEATURE_BROWSER`-gated.

## Scope boundary

### Native overlay v1

Port the behavior represented by the web app's `variant=overlay` branch:

- query edit;
- inline Contents toggle (case-sensitive, whole-word, and regex follow later);
- debounced typeahead search;
- compact flat result rows;
- result total and lifecycle status;
- cancel;
- open selected result;
- close on Escape and after opening a result;
- folder/bookmark scope resolution;
- first result page (load-more support remains a follow-up).

### Explicitly deferred

Do not port the full Search application in v1:

- indexing configuration and progress UI;
- adapter/store/debug panels;
- content-type matrices;
- arbitrary backend and sort selectors;
- grouped results;
- thumbnails;
- case-sensitive, whole-word, and regex toggles;
- per-row Explorer, folder, and Chat action buttons;
- persisted search history;
- docked native Search panel;
- replacement actions.

These can be added after the native overlay proves the shared session layer and
result model.

## Existing behavior to preserve

The web overlay is currently composed in:

- `apps/search/src/SearchApp.tsx`
- `apps/search/src/hooks/useSearchSession.ts`
- `apps/shared/search/SearchWidget.tsx`
- `apps/shared/search/SearchOptions.tsx`
- `apps/shared/search/SearchResults.tsx`
- `apps/shared/search/SearchResultItem.tsx`
- `apps/shared/search/searchOptionsModel.ts`

The native UI should preserve these observable rules:

1. Overlay defaults come from `FILE_NAME_SEARCH_OPTIONS`:
   - mode `name`;
   - backend `exact`;
   - content `any`;
   - sort `path`;
   - first page limit `200`.
2. Contents adds `grep` while retaining `name`.
3. Search starts only after the trimmed query reaches two characters.
4. Input is debounced for 220 ms.
5. Clearing or shortening the query cancels the active task and clears results.
6. Starting a different query cancels or supersedes the previous task.
7. Results arrive incrementally.
8. Up/Down changes the selected result while focus remains in the query edit.
9. Enter activates the selected result.
10. Escape closes the overlay.
11. The active row is scrolled into view.
12. Result rows show a file-type glyph, filename, and compact relative directory.
13. Opening a text hit uses its range and decorates the other ranges for the
    same file.
14. Opening any result closes the overlay.
15. The overlay waits for valid host context and must not search the install or
    build-output directory as an accidental fallback root.

## Product decisions

### Command identity

Add a distinct app command:

- enum: `Command::ToggleSearchNative`
- canonical id: `togglesearchnative`
- label: `Toggle native search`
- aliases: `nativesearch searchnative togglenativesearch`
- group: `View Toggles`

Keep `togglesearchoverlay` as the existing WebView implementation during
development and rollout.

Do not assign a second default shortcut initially. `Ctrl+Space` currently belongs
to `ToggleSearchOverlay`; two commands must not claim it. After parity testing,
move `Ctrl+Space` to `ToggleSearchNative` and leave the web overlay available by
command id as a fallback.

The final typed-service version should not be gated by `FEATURE_BROWSER`. The
current MVP is temporarily gated because it intentionally reuses
`CWebViewManager`/`CWebViewSearch`; remove that gate with the Phase 1 extraction.

### Window behavior

Use an owner-associated, modeless top-level Win32 tool window:

- owner: main frame;
- centered in the main-frame client viewport;
- initial client size: 640 × 420 logical pixels;
- minimum size: 480 × 260 logical pixels;
- resizable with a normal sizing frame;
- hidden and reused between invocations;
- always raise and focus the query edit when shown;
- toggle command closes it when already visible;
- Escape closes it;
- owner destruction destroys it;
- owner minimize hides it;
- opening a hit closes it;
- do not disable the main frame.

Do not close merely because focus briefly moves between child controls. Closing
on top-level deactivation may be considered after testing, but it must not race
with result activation or native menus.

### Visual layout

Use two vertical bands:

1. Search band
   - one-line edit filling available width;
   - clear/cancel affordance at the trailing edge;
   - compact toggle buttons on the same row:
     - `Contents` — implemented;
     - `Aa` — case-sensitive (deferred);
     - `ab` — whole-word (deferred);
     - `.*` — regex (deferred);
   - one-pixel separator below the band.
2. Results band
   - owner-drawn virtual list;
   - optional single-line status strip at the bottom;
   - no column headers and no group headers in v1.

Use `pmui::theme_palette()`, `pmui::ui_font_for_hwnd()`, DPI-scaled constants,
`apply_dwm_toplevel_frame_tint()`, and the established dark scrollbar/theme
helpers. Avoid hardcoded light/dark RGB values in the window class.

Implemented polish includes explicit DWM rounded corners, themed owner-draw for
the Contents toggle, a full-width status strip, and query-control height derived
from the active UI font instead of a fixed pixel value. The Command Palette now
uses the same explicit rounded-corner helper.

Put logical dimensions in `helpers/ui_constants.hpp`, for example
`NativeSearchOverlayLayout`, rather than scattering numbers through the window
procedure.

### Result rows and rendering decision

Keep the existing native `LISTBOX` with `LBS_NODATA`,
`LBS_OWNERDRAWFIXED`, and app-owned row data. Do not add Direct2D, DirectWrite,
GacUI, RichEdit, or a separate UI framework for the result list.

Use one fixed height for the active presentation mode:

- filename mode: 30–34 logical px;
- Contents mode: approximately 84–96 logical px.

Changing Contents may change the list's global item height, but rows within one
mode remain the same height. This keeps scrolling arithmetic native, rendering
predictable, and the memory footprint small.

Compact filename rows contain:

- a 16 px file-category icon or generic file glyph;
- filename and best-root-relative directory;
- selected/focus treatment matching the Command Palette.

Rich content rows contain:

- filename and compact relative directory;
- one context line before when available;
- the matching `snippet`, styled more strongly than context;
- one context line after when available;
- line/column plus optional source/mtime metadata.

The exact query/range is already known. Highlight it with ordinary GDI text
runs (`DrawTextW`/`ExtTextOutW`) using font weight, foreground, or a small
background accent. Full syntax highlighting is explicitly out of scope.

Keep one row per unique content range, because each row activates a different
location. In Contents mode, suppress or replace the range-less filename-only
hit once content hits for that path exist; do not collapse distinct line/range
matches.

Long filename, path, and snippet text must ellipsize independently. The full
absolute path remains available for tooltip/accessibility work.

The list remains virtual/owner-data rather than inserting strings. Search can
return up to 200 rows in the first page and later append more.

### Swappable renderer seam

Separate presentation from the popup/session code before adding rich rows.
Use a small renderer strategy, not a second control hierarchy:

```cpp
struct SearchResultDisplayRow {
    std::wstring filename;
    std::wstring directory;
    std::wstring snippet;
    std::vector<std::wstring> contextBefore;
    std::vector<std::wstring> contextAfter;
    int line = 0;
    int column = 0;
    bool contentMatch = false;
};

struct SearchResultPaintContext {
    HDC dc = nullptr;
    RECT bounds{};
    UINT dpi = 96;
    bool selected = false;
    bool focused = false;
};

class ISearchResultRenderer {
public:
    virtual ~ISearchResultRenderer() = default;
    virtual int row_height(UINT dpi, bool contentsMode) const = 0;
    virtual void draw(const SearchResultDisplayRow& row,
                      const SearchResultPaintContext& context) = 0;
};
```

`GdiSearchResultRenderer` is the only v1 implementation. A virtual call per
visible row is negligible; the boundary makes rendering replaceable without
moving search state, activation JSON, or window behavior. Keep raw hit JSON in
the popup's activation model and pass only typed display data to the renderer.

Cache GDI fonts, brushes, and pens per theme/DPI refresh. Do not create/delete
GDI objects for every row paint. The renderer owns presentation resources and
rebuilds them when theme, font, or DPI changes.

## Architecture

### Do not duplicate `CWebViewSearch`

`src/win/web/CWebView_Search.cpp` currently owns more than WebView transport:

- task ids and lifecycle;
- queue/concurrency limits;
- cancellation;
- request parsing;
- streaming/deduplication;
- JSONL result persistence;
- first-page limiting;
- pagination;
- config, store, and adapter payloads.

Calling `media::search::unified_search()` directly from the popup would duplicate
most of that behavior and cause web/native drift. Conversely, making the native
window pretend to be a WebView RPC client would preserve JSON transport and the
`CWebViewSearch` naming/coupling in the wrong layer.

Extract the task/session responsibilities into a transport-neutral service.

### Architecture decision and migration seam

There are two viable ways to reach the existing host:

1. Add a narrow `CWebViewManager` facade such as
   `InvokeSearchProviderRpc(...)`, register the native popup under a synthetic
   source id (`search-native-overlay`), and forward JSON events to the popup.
2. Extract a typed, transport-neutral search session service and make both
   `CWebViewSearch` and the native popup clients of it.

The manager facade is the smaller implementation change, but it makes a native
control impersonate a WebView surface and leaves all reusable task behavior
inside a class named and owned as web infrastructure. Direct use of
`media::search::unified_search()` has the opposite problem: it avoids JSON but
duplicates queueing, cancellation, paging, persistence, and deduplication.

The target architecture is therefore option 2. If risk requires incremental
delivery, option 1 may be used as a temporary compatibility seam to prove the
popup UI, with these constraints:

- expose an explicit search-provider method; do not expose `m_searchHost`;
- use the stable source id `search-native-overlay`;
- deliver events through `PostMessage`, never directly to controls;
- keep JSON parsing in a small controller adapter outside the window procedure;
- record removal of the facade/JSON adapter in Phase 1;
- do not build new native-only behavior on top of the temporary transport.

This preserves a fast prototype path without making the WebView manager the
long-term native search API.

The MVP uses that temporary seam:

- `CWebViewManager::InvokeSearchProviderRpc(...)` is the narrow entry point;
- the popup registers as `search-native-overlay`;
- streamed JSON events are marshalled to the popup with `PostMessage`;
- generation/run-name and task-id checks reject stale events;
- the popup never calls `media::search::unified_search()` directly.

### Proposed layers

#### 1. Core search facade — unchanged

Existing:

- `search/search_request.hpp`
- `search/search_facade.hpp`
- `media::search::Request`
- `media::search::Hit`
- `media::search::unified_search()`

This remains the backend execution API.

#### 2. Shared desktop search session service — new

Suggested files:

- `src/win/search/search_session_service.hpp`
- `src/win/search/search_session_service.cpp`

Responsibilities:

- own task state, worker threads, cancellation flags, and queue limits;
- generate stable task ids;
- execute `media::search::unified_search`;
- deduplicate hits by normalized path plus range;
- preserve JSONL persistence and paging;
- expose typed lifecycle events;
- marshal no UI calls itself;
- support multiple named clients/surfaces.

Suggested typed API:

```cpp
namespace pmui::search {

struct StartRequest {
    std::string clientId;
    media::search::Request request;
    int pageSize = 200;
    bool queueWhenBusy = true;
};

struct SearchEvent {
    enum class Kind { Started, Hit, Progress, Completed };
    Kind kind;
    std::string taskId;
    media::search::Hit hit;
    nlohmann::json progress;
    bool ok = false;
    bool cancelled = false;
    int totalMatches = 0;
    std::string error;
};

using EventSink = std::function<void(SearchEvent)>;

class SearchSessionService {
public:
    StartResult start(StartRequest request, EventSink sink);
    bool cancel(std::string_view taskId);
    PageResult page(std::string_view taskId, int offset, int limit);
    void cancel_client(std::string_view clientId);
};

}
```

Exact names may change, but the boundary should be typed. JSON belongs at the
WebView adapter, not between the native window and the service.

#### 3. Web search adapter — refactor existing

`CWebViewSearch` remains the provider-RPC adapter:

- parse web RPC JSON into typed service requests;
- translate typed events back to `hostSearchEvent` JSON;
- expose current `searchStart`, `searchCancel`, `searchPage`, index, config,
  store, adapter, and thumbnail methods unchanged.

The extraction must be behavior-preserving for the existing web Search app.

Index tasks can remain in `CWebViewSearch` during the first extraction if moving
them would make the change too broad. Search task execution, cancellation,
deduplication, persistence, and paging are the required shared seam.

#### 4. Native overlay controller/window — implemented MVP

Current files:

- `src/win/search_native/search_native_popup.hpp`
- `src/win/search_native/search_native_popup.cpp`

Responsibilities:

- create and reuse the Win32 window and child controls;
- own presentation state and the current result page;
- build a typed `media::search::Request`;
- debounce query edits;
- start/cancel sessions through `SearchSessionService`;
- reject stale task events;
- post worker events to the UI thread;
- draw rows and handle keyboard/mouse input;
- call host callbacks to open a hit;
- apply theme, font, DPI, and accessibility metadata.

The MVP knows only the narrow `CWebViewManager::InvokeSearchProviderRpc` and
external-event registration seam; it does not know about web bundles or React
option models. After Phase 1 it should depend on `SearchSessionService` directly
and no longer know about `CWebViewManager` or provider-RPC JSON.

#### 5. Main-frame integration

`CMainFrame` owns or accesses one app-lifetime `SearchSessionService` and
supplies callbacks/context to the overlay:

- resolve folder with `ResolveContextFolder()`;
- obtain active viewer path;
- use Explorer selection when visible;
- fall back to active viewer path when appropriate;
- load folder bookmarks/typeahead presets;
- open a result internally;
- locate and decorate text ranges;
- log lifecycle outcomes.

Keep the popup implementation independent of `CMainFrame` by passing a small
callbacks/context structure, as the command palette does.

## Native state model

The window owns:

- `query`;
- `contentsEnabled`;
- `caseSensitive`;
- `wholeWord`;
- `regex`;
- resolved input roots and display roots;
- `results`;
- selected row;
- total match count;
- active task id;
- monotonically increasing request generation;
- lifecycle state;
- status/error text;
- debounce timer id;
- whether more results are available.

Lifecycle states:

- `Hidden`
- `Idle`
- `Debouncing`
- `Submitting`
- `Searching`
- `Cancelling`
- `Complete`
- `Error`

Every query/options/context change increments the request generation. A posted
event is accepted only when both task id and generation match the active
request. This prevents an old worker from repopulating the list after a newer
query or after the popup is reopened.

## Threading and lifetime rules

- Search work never runs on the UI thread.
- The service event sink may run on a worker thread.
- Worker callbacks copy their payload into heap-owned posted-message data.
- The overlay handles those messages on its owning UI thread and frees payloads.
- Never capture a raw popup pointer in a callback whose lifetime exceeds the
  window. Use a stable owner/token plus generation, or a shared weak state.
- Hiding the popup cancels its active task but does not block waiting for worker
  completion.
- Destruction unregisters the client, cancels outstanding work, removes
  subclasses/timers, and drains or invalidates posted events safely.
- Do not use `SendMessage` from a worker to the UI window.

## Search request mapping

For native overlay v1:

- `query`: trimmed edit text;
- `inputs`: minimal covering set of current folder plus bookmark roots;
- `contents`: `any`;
- `modes`: `name`, plus `grep` when Contents is enabled;
- `backends`: `exact`;
- `sort`: `path`;
- `regex`, `case_sensitive`, `whole_word`: toggle values;
- `head_limit`: `200`;
- `context_before`: `2`;
- `context_after`: `2`;
- `max_matches_per_file`: use current overlay/backend default;
- `run_name`: `search-native-overlay`;
- `user.surface`: `search-native-overlay`.

Port the existing covering-root and install/build-output rejection behavior into
a shared helper rather than independently implementing slightly different path
rules in React and C++.

## Result activation

The main-frame callback receives a copied result value.

For text-searchable hits:

1. Build the primary range from `hit.range`, falling back to line/column.
2. Gather unique ranges from other current results with the same normalized
   path, capped at 256.
3. Open the path in the internal viewer.
4. Apply primary locate plus secondary decorations in `decorate` mode.

For PDF, image, Office/document, catalog, and hits without a valid text range:

1. Open the path internally without text locate.

Only close the overlay after the host accepts the activation. If opening fails,
keep it open and show/log a concise error.

## Input behavior

When focus is in the query edit:

- printable input edits the query;
- Up/Down selects previous/next result;
- Page Up/Page Down moves by the visible page;
- Ctrl+Home/Ctrl+End selects first/last result;
- Enter opens the selected result;
- Escape closes the overlay;
- Ctrl+Backspace uses normal edit behavior;
- the app's `togglesearchnative` shortcut closes the visible overlay.

When focus is in the result list:

- the same navigation keys work;
- Enter opens;
- Escape closes;
- typing a printable character returns focus to the edit and inserts it.

Mouse:

- hover may update hot tracking but should not steal keyboard selection;
- single click selects;
- double click activates;
- do not copy the web overlay's single-click activation until user testing
  confirms it is desirable for native desktop behavior.

## Theme, DPI, and accessibility

- Use one reentrancy guard around recursive theme application, as already
  required by the command palette.
- Recreate cached brushes/pens/icons when the palette or DPI changes.
- Handle `WM_DPICHANGED`, `WM_THEMECHANGED`, `WM_SETTINGCHANGE`, and font
  refresh.
- Use the app UI font on every child control and in owner-draw measurement.
- Set edit cue text such as `Search files`.
- Expose the results as a list/listbox with selectable child rows.
- Ensure selected, hot, disabled, and error states retain sufficient contrast.
- Never encode toggle state using color alone; use pressed/check state.
- Preserve standard focus cues for keyboard users.

## Logging

Use structured prefixes without per-row noise:

- `[search-native] show roots=N`
- `[search-native] query generation=N chars=N contents=0|1`
- `[search-native] submit generation=N taskId=...`
- `[search-native] cancel taskId=... reason=...`
- `[search-native] done taskId=... ok=... cancelled=... shown=... total=...`
- `[search-native] activate index=... path=... line=...`
- `[search-native] stale event ignored taskId=... generation=...`
- `[search-native] error ...`

Do not log every hit at info/trace level. A result count on completion is enough.

## File-level implementation map

Implemented:

- `src/win/app_commands.hpp`
  - `ToggleSearchNative`.
- `src/win/app_commands.cpp`
  - id, label, aliases, availability, and grouping.
- `src/policy/app_policy.cpp`
  - Search policy gate.
- `src/win/search_native/search_native_popup.hpp/.cpp`
  - native overlay, temporary search-host adapter, result presentation, activation.
- `src/win/search_native/search_result_renderer.hpp/.cpp`
  - typed display rows, swappable GDI renderer, hit-to-display parsing.
- `src/win/web/CWebViewManager.h/.cpp`
  - narrow `InvokeSearchProviderRpc(...)` seam reused by web and native clients.
- `src/win/ui_next/helpers/ui_constants.hpp`
  - `NativeSearchOverlayLayout`.
- `src/win/ui_next/Mainfrm.h`
  - `ToggleSearchNative()`.
- `src/win/ui_next/Mainfrm_commands.cpp`
  - context/root resolution, app-command dispatch, activation callback, and
    mini-toolbar routing.
- `src/win/ui_next/OwnRibbonTab.h`
  - shared native-search mini-toolbar command id.
- `src/win/ui_next/helpers/own_ribbon_tool_buttons.hpp/.cpp`
  - `file-search.svg` button shared by ribbon-tab and status-bar hosts.
- `src/win/ui_next/helpers/theme.hpp/.cpp`
  - reusable explicit DWM rounded-corner helper.
- `src/win/command_palette/command_palette_popup.cpp`
  - uses the same rounded-corner helper.
- `CMakeLists.txt`
  - feature-gated popup and renderer sources.

Still planned:

- `src/win/search/search_session_service.hpp/.cpp`
  - shared typed task/session implementation, removing the browser feature gate.
- `src/win/web/CWebView_Search.h/.cpp`
  - reduce to the provider-RPC adapter after service extraction.
- `docs/keyboard.md`
  - document the shortcut after migration.

Avoid placing the native overlay implementation in `CSearchView.*`; that class
is specifically the WebView Search surface.

## Implementation TODOs

### Next implementation slice — rich fixed-height GDI rows

- [x] Move display-only fields out of the popup's raw JSON usage into
      `SearchResultDisplayRow`.
- [x] Add `ISearchResultRenderer` and `GdiSearchResultRenderer` in a focused
      renderer module; keep the current `LISTBOX`.
- [x] Cache normal, bold/match, metadata, and optional monospace fonts plus
      brushes/pens; rebuild only on theme/font/DPI changes.
- [x] Parse `snippet`, line, column, source, mtime, and at most one context line
      before/after from each streamed hit.
- [x] Use the compact 30–34 px item height with Contents off and one fixed
      84–96 px item height with Contents on.
- [x] Draw filename/path, context, emphasized matching line, and compact
      metadata with independent ellipsis/clipping.
- [x] Highlight the known matching range/query with GDI text runs; do not add
      syntax highlighting.
- [x] Preserve one row per unique content range while suppressing the
      range-less filename row after content hits for that path arrive.
- [x] Keep selection stable when a filename row is replaced and ensure the
      selected row remains visible.
- [ ] Verify repaint/scroll behavior with 200 rows and confirm GDI object counts
      remain stable across repeated searches, DPI changes, and theme changes.

### Phase 0 — lock behavior with tests

- [ ] Add tests for request defaults used by the web overlay.
- [ ] Add tests for two-character threshold and query signature changes.
- [ ] Add tests for normalized path/range deduplication.
- [ ] Add tests for covering-root reduction.
- [ ] Add tests that reject install/build-output fallback roots.
- [ ] Add tests for stale task/generation rejection.

### Phase 1 — extract the shared session service

- [ ] Introduce typed service request/result/event structures.
- [ ] Move search task ownership, queueing, cancellation, and worker lifecycle
      out of `CWebViewSearch`.
- [ ] Move hit deduplication and first-page limiting into the service.
- [ ] Move JSONL persistence and page reads into the service.
- [ ] Keep current task ids, lifecycle ordering, and limits compatible.
- [ ] Convert `CWebViewSearch` into a JSON/provider-RPC adapter.
- [ ] Verify the docked and web overlay Search surfaces behave unchanged.
- [ ] Add shutdown tests with active and queued tasks.

### Phase 2 — command and window shell

- [x] Add `Command::ToggleSearchNative`.
- [x] Add `CMainFrame::ToggleSearchNative()`.
- [x] Add native popup files and feature-gated CMake entry.
- [x] Create/reuse/hide the popup with owner-relative placement.
- [x] Implement DPI-aware layout constants and minimum tracking size.
- [x] Apply theme, DWM frame tint, font, and themed controls.
- [x] Add query edit, Contents toggle, result list, and status line.
- [x] Implement deterministic focus, Escape, toggle-close, and owner shutdown.
- [x] Add explicit rounded corners and font-derived query/edit sizing.
- [x] Add ribbon-tab and status-bar mini-toolbar entry before Command Palette.

### Phase 3 — native search session

- [x] Resolve current folder, selection fallback, viewer path, and bookmarks.
- [x] Build minimal covering search roots.
- [x] Add 220 ms edit debounce and two-character threshold.
- [x] Cancel/supersede the previous task.
- [x] Stream shared-host hits to the UI thread using posted messages.
- [x] Track generation plus task id and discard stale events.
- [x] Show lifecycle, empty, cancelled, and error states.
- [x] Append first-page hits without resetting selection unnecessarily.
- [ ] Add page retrieval when more than 200 results are available.

### Phase 4 — result presentation and activation

- [x] Add virtual owner-draw rows.
- [x] Extract the swappable renderer seam and cached GDI renderer.
- [x] Add rich fixed-height Contents rows with snippet/context/metadata.
- [x] Distinguish and coalesce filename hits versus unique content-range hits.
- [ ] Add file-category icons with a generic fallback.
- [x] Add filename and best-root-relative directory labels.
- [ ] Add selection, hot tracking, tooltip, and ensure-visible behavior.
- [x] Implement edit-focused keyboard navigation.
- [x] Implement list-focused keyboard navigation and type-to-edit.
- [x] Implement click selection and double-click activation.
- [x] Open text hits with primary range plus same-file decorations.
- [x] Open non-text hits normally.
- [x] Close only after successful activation.

### Phase 5 — hardening

- [ ] Test rapid typing, repeated show/hide, and close during active search.
- [ ] Test changing Contents/case/word/regex while a search is running.
- [ ] Test duplicate hits from name plus grep modes.
- [ ] Test empty context and bookmark-only context.
- [ ] Test long paths, Unicode, RTL text, and invalid/deleted paths.
- [ ] Test 100%, 125%, 150%, and 200% DPI.
- [ ] Test light/dark/system-theme changes while visible.
- [ ] Test keyboard-only and screen-reader basics.
- [ ] Check for GDI object, thread, timer, and posted-payload leaks.
- [x] Add focused logging for rejected requests and stale events.

### Phase 6 — rollout

- [ ] Dogfood `togglesearchnative` without a default shortcut.
- [ ] Compare result order/count and activation against
      `togglesearchoverlay`.
- [ ] Resolve parity gaps or document intentional native differences.
- [ ] Move `Ctrl+Space` default from web overlay to native overlay.
- [ ] Keep `togglesearchoverlay` as an explicit fallback for one release.
- [ ] Decide whether the native overlay replaces or coexists with the WebView
      overlay long term.

### Later enhancements

- [ ] Persist overlay size.
- [ ] Add search history and recent queries.
- [ ] Add grouping/sorting controls.
- [ ] Add thumbnails for image results.
- [ ] Add optional inline preview/details pane backed by the shared result model.
- [ ] Add context menu actions: reveal, open folder, reference in Chat.
- [ ] Add full native Search panel/indexing UI only as a separate project.

## Acceptance status for v1

- [x] `togglesearchnative` opens/closes without loading `search.pmbundle` or
      creating a WebView.
- [x] Initial show focuses and selects the query text.
- [x] Two-character queries begin searching after roughly 220 ms.
- [x] Search execution remains off the UI thread.
- [x] Old search events cannot overwrite a newer query.
- [x] Contents affects the next search; filename-only remains the default.
- [x] Results stream into the list and remain keyboard navigable.
- [x] Enter and double click open the correct path.
- [x] Text results locate the primary match and decorate same-file matches.
- [x] Escape and command-toggle cancel/close active work.
- [x] Light/dark controls, rounded frame, and DPI/font-derived layout are wired.
- [x] Existing web Search surfaces retain their provider-RPC path.
- [ ] Contents rows explain repeated file matches with snippet, context, and
      line/column metadata.
- [ ] Result painting is isolated behind the cached GDI renderer seam.
- [ ] Case-sensitive, whole-word, and regex toggles.
- [ ] Paging beyond the first 200 streamed results.
- [ ] File-category icons, tooltips, and richer accessibility metadata.
- [ ] Automated shutdown/leak and multi-DPI/theme coverage.

## Settled behavior and remaining rollout questions

Settled in the MVP:

- single click selects; Enter or double click activates;
- query and Contents state remain process-local across hide/show;
- hiding cancels immediately;
- no second default shortcut is assigned;
- both mini-toolbar hosts expose Native Search before Command Palette;
- result rendering stays on the native owner-drawn LISTBOX/GDI path;
- filename and Contents modes use separate fixed global row heights;
- content matches remain one row per unique range, with redundant range-less
  filename hits suppressed once content matches exist.

Still open:

- Should paging be explicit or automatic near the list bottom?
- After parity, should `Ctrl+Space` move from the web overlay to native search?
- Should the web overlay remain user-visible or become a diagnostic fallback?
