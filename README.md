# pm-search

Exact and semantic search orchestration for Tanit. Namespace stays `media::search`. The static library is `pm-search-lib`. There is no `pm-search` CLI yet. Tanit exercises it with `npm run test:search`.

The library does not replace `src/core/search.cpp` (the own walker), the image adapter, or the office adapter. Those stay on the host and are installed through `search/host.hpp`.

## Build and test

From the repo root, after commons:

```text
npm run build:cpp
npm run test:search
```

`PM_SEARCH_CLI` is unused until a package CLI exists. Include root is this package's `src/`, so callers keep `#include "search/..."`.

## Integration

The host links the static library. It does not compile `cli/` (there is none).

```cmake
if(NOT TARGET pm-commons-lib)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/packages/commons.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/pm-commons-lib EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET pm-search-lib)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/packages/search.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/pm-search-lib EXCLUDE_FROM_ALL)
endif()
target_link_libraries(${HOST_TARGET} PUBLIC pm-search-lib)
```

Commons is added first. When this package is not top-level it links `pm-commons-lib` for `fs/path_sanitizer.hpp`, `fs/glob_paths.hpp`, `fs/file_extensions.hpp`, and `string/string_sanitizers.hpp`. `PM_SEARCH_STATIC=1` is PUBLIC so the export macro is empty.

`search/host.hpp` is the hook table (process runner, rg lookup, own walker, remote collect, UTF-16 ranges, bookmark folders, vector store, markdown, extra adapters). Unset hooks return a named error. The host installs them from `src/search/search_host.cpp`. When `pm-media` is shared, that TU is compiled into `pm-media`, `tanit.exe`, and `tanit-cli.exe`, because each image has its own copy of the static lib. The settings store stays on the host.

## What is intentionally absent

- A link to `pm-vfs-lib`. Remote listing is `host::collect_remote`. The desktop walker `collect_copy_at` is not this package and not `packages/vfs.cpp`.
- llama, ONNX, vips, officecli, and anydoc. Image and office adapters are host TUs.
- `polymech_export.h`. Do not PUBLIC `POLYMECH_STATIC_BUILD` from this library.
- Sockets, the settings store, and a Win32 UI.
