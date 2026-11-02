#pragma once

#include "search/search_request.hpp"

namespace media::search {

Result index_vector(const Request& req);
Result search_vector(const Request& req, Backend backend, Mode mode);

} // namespace media::search
