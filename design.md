# Search follow-ups

The move into `pm-search-lib` is done. This list is what is still outside the library. It is not the integration contract. See `README.md`.

1. **CLI.** No `pm-search` exe, no `tests/` in this package. `npm run test:search` drives `tanit-cli`. A package CLI is JSON on stdout, exit 0/1/2, `PM_SEARCH_CLI`, and links commons plus the hooks it installs. Do not vendor rg, llama, or vfs.
2. **Own walker.** `src/core/search.cpp` (`media::search_files`) stays on the host. The facade calls `host::search_own`. Moving the walker means dropping `BatchControl` from the package signature first.
3. **Image and office.** `src/search/source_adapter_image.cpp` and `source_adapter_office.cpp` stay host TUs. The registry's built-ins are plain text and code. The host TU registers the rest.
4. **Slots.** The factory table lives in `search/host.cpp` inside the static lib, one copy per image. Do not switch that to `PM_SEARCH_HOST_SINGLETONS`. That option is for a process-global table that must exist only inside `pm-media.dll`.
5. **Remote.** `host::is_remote_uri` / `host::collect_remote` map to host `src/vfs` (`collect_copy_nodes`). Do not link `pm-vfs-lib` for this.
6. **Leave `std::regex`.** Replacing it in `search_facade.cpp` or `source_adapter_code.cpp` changes match behavior.
7. Android is not a search target.
