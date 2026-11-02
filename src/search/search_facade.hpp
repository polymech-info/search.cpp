#pragma once

#include "search/search_request.hpp"
#include "search/source_adapter.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace media::search {

/// Fast `rg --files -g` count (same binary as native search). Does not
/// store paths. Ignores .gitignore / .ignore so the count matches the
/// file panel (e.g. a folder named `test/`). ok=false when rg is missing
/// or the run fails.
struct FileGlobCount {
    bool ok = false;
    bool used_rg = false;
    int matches = 0;
    std::string error;
};

PM_SEARCH_API FileGlobCount count_file_globs(
    const std::string& folder,
    const std::vector<std::string>& globs,
    bool recursive,
    const std::function<bool()>& cancel_requested = {});

PM_SEARCH_API Result unified_search(const Request& req);
PM_SEARCH_API Result unified_index(const Request& req);
PM_SEARCH_API SourceDetail unified_detail(const std::filesystem::path& path,
                                         const std::string& adapter_id,
                                         const DetailRequest& req);

} // namespace media::search
