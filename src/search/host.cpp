#include "search/host.hpp"

#include "search/source_adapter_registry.hpp"

#include <cctype>

namespace media::search::host {
namespace {

LogFn g_log;
RunProcessFn g_run;
std::function<std::vector<std::filesystem::path>()> g_rg_candidates;
bool g_rg_file = true;
SearchFilesFn g_search;
IsRemoteFn g_is_remote;
CollectRemoteFn g_collect_remote;
Utf16RangeFn g_utf16;
LoadFoldersFn g_folders;
OpenVectorStoreFn g_vectors;
MarkdownFn g_markdown;
MarkdownLooksFn g_markdown_looks;
RegisterAdaptersFn g_adapters;

void emit(LogLevel level, std::string message) {
  if (g_log)
    g_log(level, std::move(message));
}

} // namespace

void set_log(LogFn fn) { g_log = std::move(fn); }
void set_run_process(RunProcessFn fn) { g_run = std::move(fn); }
void set_rg_candidates(std::function<std::vector<std::filesystem::path>()> fn) {
  g_rg_candidates = std::move(fn);
}
void set_rg_file_enabled(bool enabled) { g_rg_file = enabled; }
void set_search_files(SearchFilesFn fn) { g_search = std::move(fn); }
void set_is_remote_uri(IsRemoteFn fn) { g_is_remote = std::move(fn); }
void set_collect_remote(CollectRemoteFn fn) {
  g_collect_remote = std::move(fn);
}
void set_utf16_range(Utf16RangeFn fn) { g_utf16 = std::move(fn); }
void set_load_search_folders(LoadFoldersFn fn) { g_folders = std::move(fn); }
void set_open_vector_store(OpenVectorStoreFn fn) { g_vectors = std::move(fn); }
void set_parse_markdown(MarkdownFn fn) { g_markdown = std::move(fn); }
void set_markdown_looks_like(MarkdownLooksFn fn) {
  g_markdown_looks = std::move(fn);
}
void set_register_adapters(RegisterAdaptersFn fn) {
  g_adapters = std::move(fn);
}

void log_trace(std::string message) {
  emit(LogLevel::Trace, std::move(message));
}
void log_debug(std::string message) {
  emit(LogLevel::Debug, std::move(message));
}
void log_info(std::string message) { emit(LogLevel::Info, std::move(message)); }
void log_warn(std::string message) { emit(LogLevel::Warn, std::move(message)); }

ProcessResult run_process(const ProcessRun &run) {
  if (!g_run) {
    ProcessResult missing;
    missing.error = "process runner is not installed";
    return missing;
  }
  return g_run(run);
}

std::vector<std::filesystem::path> rg_candidates() {
  if (!g_rg_candidates)
    return {};
  return g_rg_candidates();
}

bool rg_file_enabled() { return g_rg_file; }

OwnResult search_own(const std::vector<std::string> &inputs,
                     const OwnQuery &query) {
  if (!g_search) {
    OwnResult missing;
    missing.error = "own indexer is not installed";
    return missing;
  }
  return g_search(inputs, query);
}

bool is_remote_uri(std::string_view uri) {
  return g_is_remote && g_is_remote(uri);
}

RemoteList collect_remote(std::string_view input) {
  if (!g_collect_remote) {
    RemoteList missing;
    missing.error = "remote search is not installed";
    return missing;
  }
  return g_collect_remote(input);
}

std::optional<TextRange> utf16_range(std::string_view text,
                                     std::size_t byte_start,
                                     std::size_t byte_end) {
  if (!g_utf16)
    return std::nullopt;
  return g_utf16(text, byte_start, byte_end);
}

bool load_search_folders(std::vector<std::string> &folders,
                         std::string &error) {
  folders.clear();
  if (!g_folders) {
    error.clear();
    return true;
  }
  return g_folders(folders, error);
}

std::unique_ptr<IVectorStore>
open_vector_store(const std::filesystem::path &dir, const std::string &model,
                  int threads, int gpu_layers, std::string &error) {
  if (!g_vectors) {
    error = "semantic vector search requires FEATURE_LLAMA";
    return {};
  }
  return g_vectors(dir, model, threads, gpu_layers, error);
}

MarkdownDoc parse_markdown(std::string_view text) {
  if (g_markdown)
    return g_markdown(text);
  MarkdownDoc doc;
  doc.body = std::string(text);
  return doc;
}

bool markdown_looks_like(std::string_view text) {
  if (g_markdown_looks)
    return g_markdown_looks(text);
  for (char ch : text) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
      continue;
    return ch == '#' || ch == '-';
  }
  return false;
}

void register_adapters(SourceAdapterRegistry &registry) {
  if (g_adapters)
    g_adapters(registry);
}

} // namespace media::search::host
