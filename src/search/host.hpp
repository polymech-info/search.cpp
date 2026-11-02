#pragma once

#include "search/search_request.hpp"
#include "search/vector_store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace media::search {

class SourceAdapterRegistry;

namespace host {

enum class LogLevel { Trace, Debug, Info, Warn };

struct ProcessRun {
    std::string executable;
    std::vector<std::string> args;
    int timeout_ms = 0;
    bool bind_to_parent = false;
    std::size_t capture_max_bytes = 0;
    std::function<bool()> on_cancel_check;
    std::function<void(const std::string& stream, const std::string& chunk)> on_output_chunk;
};

struct ProcessResult {
    bool ok = false;
    int exit_code = -1;
    std::string error;
    std::string stdout_text;
    bool timed_out = false;
    bool cancelled = false;
};

enum class OwnOutput { Content, FilesWithMatches, Count };

struct OwnQuery {
    std::string query;
    bool is_regex = false;
    bool case_sensitive = false;
    bool whole_word = false;
    bool image_only = false;
    bool grep = false;
    bool names_only = false;
    bool name_fuzzy = false;
    int name_fuzzy_limit = 20;
    bool recursive = true;
    bool include_hidden = false;
    bool follow_symlinks = false;
    bool respect_gitignore = true;
    std::vector<std::string> include_globs;
    std::vector<std::string> exclude_globs;
    std::vector<std::string> exclude_dirs;
    bool skip_binary = true;
    std::uint64_t max_file_size_bytes = 0;
    std::uint32_t max_matches_per_file = 0;
    int context_lines = 0;
    int context_before_lines = 0;
    int context_after_lines = 0;
    bool multiline = false;
    std::string type_filter;
    bool use_os_indexer = false;
    bool exclude_sensitive = true;
    OwnOutput output_mode = OwnOutput::Content;
    int max_results = 0;
    int head_limit = 0;
    int offset = 0;
    bool dry_run = false;
    std::function<bool()> cancel_requested;
};

struct OwnMatch {
    std::string path;
    std::uint64_t line = 0;
    std::uint64_t column = 0;
    std::uint64_t match_length = 0;
    std::string matched_line;
    std::string preview;
    std::string source;
    double score = 1.0;
    std::vector<std::string> context_before;
    std::vector<std::string> context_after;
};

struct OwnStats {
    int files_visited = 0;
    int files_scanned = 0;
    int files_skipped = 0;
    int files_binary = 0;
    int files_sensitive = 0;
    int matches = 0;
};

struct OwnResult {
    bool ok = false;
    std::string error;
    std::vector<OwnMatch> matches;
    OwnStats stats;
    int total_matches = 0;
    int applied_limit = 0;
    int applied_offset = 0;
    bool truncated = false;
};

struct RemoteNode {
    std::string src;
    bool is_dir = false;
};

struct RemoteList {
    std::string error;
    std::vector<RemoteNode> nodes;
};

struct MarkdownDoc {
    std::string title;
    std::string summary;
    std::string author;
    std::string body;
    std::vector<std::string> tags;
    bool found = false;
};

using LogFn = std::function<void(LogLevel, std::string)>;
using RunProcessFn = std::function<ProcessResult(const ProcessRun&)>;
using SearchFilesFn = std::function<OwnResult(const std::vector<std::string>&, const OwnQuery&)>;
using IsRemoteFn = std::function<bool(std::string_view)>;
using CollectRemoteFn = std::function<RemoteList(std::string_view)>;
using Utf16RangeFn = std::function<std::optional<TextRange>(std::string_view, std::size_t, std::size_t)>;
using LoadFoldersFn = std::function<bool(std::vector<std::string>&, std::string&)>;
using OpenVectorStoreFn = std::function<std::unique_ptr<IVectorStore>(
    const std::filesystem::path&, const std::string&, int, int, std::string&)>;
using MarkdownFn = std::function<MarkdownDoc(std::string_view)>;
using MarkdownLooksFn = std::function<bool(std::string_view)>;
using RegisterAdaptersFn = std::function<void(SourceAdapterRegistry&)>;

void set_log(LogFn fn);
void set_run_process(RunProcessFn fn);
void set_rg_candidates(std::function<std::vector<std::filesystem::path>()> fn);
void set_rg_file_enabled(bool enabled);
void set_search_files(SearchFilesFn fn);
void set_is_remote_uri(IsRemoteFn fn);
void set_collect_remote(CollectRemoteFn fn);
void set_utf16_range(Utf16RangeFn fn);
void set_load_search_folders(LoadFoldersFn fn);
void set_open_vector_store(OpenVectorStoreFn fn);
void set_parse_markdown(MarkdownFn fn);
void set_markdown_looks_like(MarkdownLooksFn fn);
void set_register_adapters(RegisterAdaptersFn fn);

void log_trace(std::string message);
void log_debug(std::string message);
void log_info(std::string message);
void log_warn(std::string message);

ProcessResult run_process(const ProcessRun& run);
std::vector<std::filesystem::path> rg_candidates();
bool rg_file_enabled();
OwnResult search_own(const std::vector<std::string>& inputs, const OwnQuery& query);
bool is_remote_uri(std::string_view uri);
RemoteList collect_remote(std::string_view input);
std::optional<TextRange> utf16_range(std::string_view text, std::size_t byte_start, std::size_t byte_end);
bool load_search_folders(std::vector<std::string>& folders, std::string& error);
std::unique_ptr<IVectorStore> open_vector_store(
    const std::filesystem::path& dir,
    const std::string& model,
    int threads,
    int gpu_layers,
    std::string& error);
MarkdownDoc parse_markdown(std::string_view text);
bool markdown_looks_like(std::string_view text);
void register_adapters(SourceAdapterRegistry& registry);

} // namespace host
} // namespace media::search
