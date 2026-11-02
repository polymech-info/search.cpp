#pragma once

#include "search/search_request.hpp"
#include "search/source_adapter.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::search {

std::filesystem::path fingerprint_path(const std::filesystem::path& index_dir);

std::vector<nlohmann::json> load_fingerprints_for_path(const std::filesystem::path& index_dir,
                                                       const std::string& source_path);

std::vector<nlohmann::json> cheap_fingerprints_for_units(const std::string& adapter_id,
                                                         const std::vector<SourceUnit>& units);

bool save_fingerprints(const std::filesystem::path& index_dir,
                       const std::vector<nlohmann::json>& records);

Result search_fingerprints(const Request& req);

} // namespace media::search
