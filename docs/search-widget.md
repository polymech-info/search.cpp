# Search Widget — Cleanup & Widgetization Plan

> Status: **works fine as-is**. This is a forward-looking refactor backlog for clarity,
> modularity, encapsulation, and making the widget drop-in for real apps.
> Todos only — no behavior changes intended. Tackle top-down; each box is independently shippable.

## Context

Shared widget lives in `apps/shared/search/`; host wiring lives in `apps/search/src/`
(`SearchApp.tsx`) with a mock host in the playground (`usePlaygroundSearch.ts`).

Current pain points (why this plan exists):

- `SearchWidget` takes a **~65-prop flat bag** — hard to consume, easy to break.
- **Collect state** (segments / cursor / focus token / caret context) is owned by hosts, so every real app re-implements the same plumbing.
- `SearchApp.tsx` is **~1040 lines** of mixed concerns (RPC, state, typeahead nav, index, thumbnails).
- Widget **styles live in app-level `search.css` (~1900 lines)** — not shipped with the widget.
- **No public entry point** (`index.ts`) — consumers deep-import internals.
- Typeahead selection logic is **duplicated** between `SearchWidget` and `usePlaygroundSearch`.
- Deprecated `SearchCollectComposer` / `segmentsFromLegacyComposer` still present.

---

## 1. Public API surface (encapsulation)

- [ ] Add `apps/shared/search/index.ts` barrel exporting only the public surface (`SearchWidget`, `SearchCollectInlineEditor`, presets, public types, plain-text codec).
- [ ] Mark internals (`SearchResultItem`, `SearchExtraItemRow`, `typeaheadPlacement`, `searchCollectDocument`) as internal — not re-exported from the barrel.
- [ ] Update all `@pm/shared/search/...` deep imports across `apps/search`, `apps/chat-next`, `apps/settings` to import from the barrel.
- [ ] Add a short "public vs internal" note at the top of the barrel so future edits know the contract.

## 2. Group the `SearchWidget` prop bag (clarity)

- [ ] Split `SearchWidgetProps` into cohesive sub-objects: `search` (options/hits/status), `index` (index options/progress), `results` (sort/group/actions), `session` (sessionInfo/paths/adapters), `collect`, `extras`.
- [ ] Keep a thin back-compat adapter (old flat props → grouped) during migration; delete after callers move.
- [ ] Move `minQueryLength: 2` and other magic numbers into a single `SEARCH_WIDGET_CONFIG` constant.
- [ ] Co-locate `SearchWidgetFeatures` + presets into `searchWidgetPresets.ts` (keep `resolveSearchWidgetFeatures` there).

## 3. Encapsulate collect state in a hook (modularity)

- [ ] Create `useSearchCollect()` owning `segments`, `cursor`, `focusRequestToken`, `caretContext`, and exposing `collectHit` / `collectExtraItem` / `removePill` / `reset` / `getPlainText`.
- [ ] Refactor `SearchPlaygroundInlineCase` and `SearchPlaygroundChatCase` to consume `useSearchCollect()` (proves the hook covers both cases).
- [ ] Document the hook as the recommended integration path for real apps in `apps/shared/search/README.md`.
- [ ] Consider a `<SearchCollectProvider>` context so composer + widget share collect state without prop drilling.

## 4. Decompose host orchestration (`SearchApp.tsx`)

- [ ] Extract `useSearchSession()` — RPC start/cancel/page, hits, status, running.
- [ ] Extract `useSearchIndexing()` — index options, submit/cancel, progress.
- [ ] Extract `useTypeaheadNav()` — selected index + keyboard nav (shared with `usePlaygroundSearch`, kill the duplication).
- [ ] Extract `useThumbnails()` — thumbnail loading/caching.
- [ ] Leave `SearchApp.tsx` as a thin composition shell wiring the hooks to `SearchWidget`.

## 5. Ship widget styles with the widget

- [ ] Extract the `search-collect-*`, `pm-search-*`, and typeahead-overlay rules out of app-level `search.css` into a widget-owned stylesheet under `apps/shared/search/`.
- [ ] Ensure all widget CSS uses theme tokens (`--xblox-*`) only — no app-specific selectors.
- [ ] Document how a host imports widget styles (single import) in the README.
- [ ] Keep playground-only styling (`search-playground-*`) in the app, not the shared widget.

## 6. Naming & file consistency

- [ ] Standardize module naming: components `PascalCase.tsx`, logic/`*Model`/codecs `camelCase.ts` (audit and rename outliers).
- [ ] Rename `searchCollectPlainText.ts` exports under a clear `collectCodec` namespace (`serialize` / `parse` / `insert` / `splice`) for discoverability.
- [ ] Add a one-line JSDoc header to each shared module stating its single responsibility.

## 7. Remove deprecated / dead code

- [ ] Migrate remaining `SearchCollectComposer` users to `SearchCollectInlineEditor`, then delete the deprecated component.
- [ ] Remove `segmentsFromLegacyComposer` once no callers remain.
- [ ] Sweep for unused exports across `apps/shared/search/` and prune.

## 8. Tests & docs (safety net for the refactor)

- [ ] Unit-test the plain-text codec round-trip (`serialize → parse → serialize`, incl. escapes `\\ \| \»`).
- [ ] Unit-test `searchCollectDocument` cursor stepping (pill↔pill, empty-gap skip, boundary cases).
- [ ] Add a minimal integration test for collect: type → collect hit → copy → paste restores pills.
- [ ] Update README with the grouped-props API, `useSearchCollect`, style import, and clipboard wire format.
- [ ] Document the known limitation: native shift-select only spans one text segment (cross-pill selection via Ctrl/Cmd+A).

---

## Suggested order

1 → 6/7 (low-risk cleanup) → 2 → 3 → 4 → 5 → 8, landing each section as its own small change while the widget keeps working throughout.
