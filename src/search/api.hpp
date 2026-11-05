#pragma once

#if defined(PM_SEARCH_STATIC)
#  define PM_SEARCH_API
#elif defined(POLYMECH_API)
#  define PM_SEARCH_API POLYMECH_API
#else
#  include "polymech_export.h"
#  define PM_SEARCH_API POLYMECH_API
#endif

// The factory table is one process slot inside pm-media. tanit-chat-plugin
// links pm-search-lib itself and runs file_search; a table inside the static
// lib is a different pointer in that plugin, so rg and the own indexer look
// uninstalled. Standalone search keeps the table in the static lib.
#if defined(PM_SEARCH_HOST_SINGLETONS)
#  if defined(POLYMECH_STATIC_BUILD)
#    define PM_SEARCH_HOST_API
#  elif defined(_WIN32)
#    if defined(POLYMECH_BUILDING_LIBRARY)
#      define PM_SEARCH_HOST_API __declspec(dllexport)
#    else
#      define PM_SEARCH_HOST_API __declspec(dllimport)
#    endif
#  else
#    if defined(POLYMECH_BUILDING_LIBRARY)
#      define PM_SEARCH_HOST_API __attribute__((visibility("default")))
#    else
#      define PM_SEARCH_HOST_API
#    endif
#  endif
#else
#  define PM_SEARCH_HOST_API
#endif
