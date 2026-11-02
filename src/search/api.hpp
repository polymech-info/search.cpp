#pragma once

#if defined(PM_SEARCH_STATIC)
#  define PM_SEARCH_API
#elif defined(POLYMECH_API)
#  define PM_SEARCH_API POLYMECH_API
#else
#  include "polymech_export.h"
#  define PM_SEARCH_API POLYMECH_API
#endif
