# Search follow-ups

The move into `pm-search-lib` is done. This list is what is still outside the library. It is not the integration contract. See `README.md`.

1. **CLI.** No `pm-search` exe, no `tests/` in this package. `npm run test:search` drives `tanit-cli`. A package CLI is JSON on stdout, exit 0/1/2, `PM_SEARCH_CLI`, and links commons plus the hooks it installs. Do not vendor rg, llama, or vfs.
2. **Own walker.** `src/core/search.cpp` (`media::search_files`) stays on the host. The facade calls `host::search_own`. Moving the walker means dropping `BatchControl` from the package signature first.
3. **Image and office.** `src/search/source_adapter_image.cpp` and `source_adapter_office.cpp` stay host TUs. The registry's built-ins are plain text and code. The host TU registers the rest.
4. **Slots.** `search/host.cpp` is the process-wide factory table. Tanit sets `PM_SEARCH_HOST_SINGLETONS` and compiles that TU into `pm-media` only. `tanit-chat-plugin` links `pm-search-lib` and runs `file_search`; a copy of the table in the static lib stays empty in the plugin. Standalone search leaves the TU in the static lib.
5. **Remote.** `host::is_remote_uri` / `host::collect_remote` map to host `src/vfs` (`collect_copy_nodes`). Do not link `pm-vfs-lib` for this.
6. **Leave `std::regex`.** Replacing it in `search_facade.cpp` or `source_adapter_code.cpp` changes match behavior.
7. Android is not a search target.
