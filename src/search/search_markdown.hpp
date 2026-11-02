#pragma once

#include "search/search_request.hpp"
#include "search/source_adapter.hpp"

#include <string>

namespace media::search {

PM_SEARCH_API std::string result_to_markdown(const Request& req, const Result& result);
PM_SEARCH_API std::string detail_to_markdown(const SourceDetail& detail);

} // namespace media::search
