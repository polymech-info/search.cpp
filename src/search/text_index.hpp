#pragma once

#include "search/search_request.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::search {

struct TextFileInventory {
    std::vector<std::filesystem::path> files;
};

TextFileInventory collect_text_files(const Request& req);

std::filesystem::path default_vector_index_dir(const Request& req);

// Resolves the set of stores a search should read. Expands ${...} and globs in
// `req.index_dirs`, de-duplicates while preserving order, and maps a sidecar
// file (manifest.json / fingerprints.jsonl) to its parent store directory.
// Falls back to {default_vector_index_dir(req)} when nothing is specified.
std::vector<std::filesystem::path> resolve_search_index_dirs(const Request& req);
std::filesystem::path vector_cache_path(const std::filesystem::path& index_dir);
std::filesystem::path search_manifest_path(const std::filesystem::path& index_dir);

nlohmann::json load_vector_file_cache(const std::filesystem::path& index_dir);
bool save_vector_file_cache(const std::filesystem::path& index_dir,
                            const Request& req,
                            const std::string& model_path,
                            const std::vector<std::filesystem::path>& files,
                            const std::map<std::string, int>& chunk_counts,
                            const std::map<std::string, std::string>& adapter_ids,
                            const std::map<std::string, std::vector<std::string>>& unit_uris);
bool cache_ingest_matches(const nlohmann::json& cache,
                          const Request& req,
                          const std::string& model_path);
bool cache_entry_matches(const nlohmann::json& cache, const std::filesystem::path& path);
std::vector<std::string> cache_unit_uris_for_path(const nlohmann::json& cache,
                                                  const std::string& path);

bool save_search_manifest(const std::filesystem::path& index_dir,
                          const Request& req,
                          const nlohmann::json& adapter_registry,
                          const nlohmann::json& stats);

std::uint64_t file_size_u64(const std::filesystem::path& path);

} // namespace media::search
