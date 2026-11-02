#pragma once
// Unified search request/result types.
//
// This layer is intentionally an orchestration contract. Exact grep/name search
// stays in src/core/search.cpp; image metadata stays in src/core/find.cpp;
// semantic document retrieval stays in VectorStore-backed code.

#include "search/api.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::search {

struct TextPosition {
    std::uint32_t line = 1;
    std::uint32_t column = 1;

    friend bool operator==(const TextPosition&, const TextPosition&) = default;
};

struct TextRange {
    TextPosition start;
    TextPosition end;

    friend bool operator==(const TextRange&, const TextRange&) = default;
};

inline constexpr std::size_t k_max_anchor_bytes = 1024;

inline void to_json(nlohmann::json& j, const TextPosition& value)
{
    j = {{"line", value.line}, {"column", value.column}};
}

inline void to_json(nlohmann::json& j, const TextRange& value)
{
    j = {{"start", value.start}, {"end", value.end}};
}

// Embedding device for search ingest. -1 offloads every layer when a GPU
// backend is compiled in. Vectors do not depend on this; it only changes speed.
#if (defined(FEATURE_LLAMA_CUDA) && FEATURE_LLAMA_CUDA) \
    || (defined(FEATURE_LLAMA_VULKAN) && FEATURE_LLAMA_VULKAN) \
    || (defined(FEATURE_LLAMA_METAL) && FEATURE_LLAMA_METAL)
inline constexpr int k_default_gpu_layers = -1;
#else
inline constexpr int k_default_gpu_layers = 0;
#endif

enum class Content {
    Any,
    Text,
    Code,
    Image,
    Document,
    Office,
    Pdf,
    Catalog,
};

enum class Mode {
    Auto,
    Name,
    Grep,
    Semantic,
    Metadata,
    Fingerprint,
};

enum class Backend {
    Auto,
    Exact,
    ImageMeta,
    Vector,
    Fingerprint,
    Catalog,
};

enum class IndexPolicy {
    SameDirDefault,
    SpecificDir,
    Disabled,
};

// Result ordering for merged (multi-task) and single-task searches.
enum class SortOrder {
    Auto,  // preserve task / backend order (name/grep first, then user order)
    None,  // insertion order, identical to Auto today
    Score, // descending relevance score, stable
    Path,  // ascending path, then line/column, stable
    MTimeDesc, // newest files first, then path, stable
    MTimeAsc,  // oldest files first, then path, stable
};

enum class ActionKind {
    None,
    Replace,
};

struct Hit; // defined below; referenced by Request's streaming sink

struct Request {
    // Optional caller-scoped labeling used by agents/web host to correlate
    // and group searches/indexing runs ("find all runs named X").
    std::string run_name;
    nlohmann::json user = nlohmann::json::object();

    std::string query;
    std::vector<std::string> inputs;

    Content content = Content::Any;
    Mode mode = Mode::Auto;
    Backend backend = Backend::Auto;

    // Optional multi-dimensional fan-out. When any of these holds more than one
    // entry the facade runs the cartesian product of (content x mode x backend)
    // tasks in explicit order, skipping incompatible pairs, and unions the hits.
    // Empty vectors mean "use the single content/mode/backend above".
    std::vector<Content> contents;
    std::vector<Mode> modes;
    std::vector<Backend> backends;
    SortOrder sort = SortOrder::Auto;

    std::vector<std::string> include_globs;
    std::vector<std::string> exclude_globs;
    std::vector<std::string> exclude_dirs;
    bool recursive = true;
    bool include_hidden = false;
    bool follow_symlinks = false;
    /// Own walker and rg both honor `.gitignore` unless this is false.
    /// The directory or file named as an input is still searched.
    bool respect_gitignore = true;

    int limit = 0;       // 0 = backend default / unlimited
    int head_limit = 0;  // 0 = unlimited
    int offset = 0;

    bool case_sensitive = false;
    bool regex = false;
    bool whole_word = false;
    bool names_only = false;
    /// Name mode: on a literal miss, score filenames (own walker). Not grep,
    /// not semantic. Explicit `*` / `?` queries do not fall through.
    bool fuzzy_names = false;
    /// Fuzzy name hits to keep. 0 uses 20.
    int fuzzy_limit = 20;
    bool skip_binary = true;
    int context_lines = 0;
    int context_before = 0;
    int context_after = 0;
    bool multiline = false;
    std::string output_mode = "content";
    std::string type_filter;
    // Selects the exact-search indexer engine for Mode::Name / Mode::Grep.
    //   "own" (default) — built-in std::filesystem walker + std::regex grep.
    //   "os"            — OS-provided indexer (Windows Search, Spotlight, locate).
    //   "rg"            — bundled ripgrep binary:
    //                       Name → rg --files -g "*<query>*" [inputs]
    //                       Grep → rg [flags] <query> [inputs]
    //                     Gate: ENABLE_RG_FILE must be true in search_facade.cpp.
    std::string exact_indexer = "own";
    std::uint64_t max_file_size_bytes = 0;
    std::uint32_t max_matches_per_file = 0;
    bool dry_run = false;

    IndexPolicy index_policy = IndexPolicy::SameDirDefault;
    std::string index_dir;
    // Optional explicit search-time store set. Each entry is a vector store
    // directory or a glob that expands to several. A named store that already
    // has a manifest is searched as-is, even when inputs are also set.
    // --reindex still rebuilds it. Empty falls back to index_dir / the
    // input-derived default store, which is created on first search.
    std::vector<std::string> index_dirs;
    std::string model;
    int threads = 4;
    int gpu_layers = k_default_gpu_layers;
    bool reindex = false;
    bool create_index = true;
    std::string extract = "auto"; // auto | text | office | pdf
    std::string adapter; // adapter id hint, e.g. ocr-local | llm-meta | office-officecli | opendocument-anydoc
    nlohmann::json adapter_options = nlohmann::json::object();
    std::string fingerprint = "off"; // off | cheap
    int chunk_size = 500;
    int chunk_overlap = 100;
    std::string include_detail; // row | page | section | object | metadata | auto
    int detail_max = 0;         // 0 = expand every returned hit

    ActionKind action = ActionKind::None;
    std::string replacement;
    std::vector<std::string> reference_images;

    std::function<bool()> cancel_requested;

    // ── Streaming (optional) ───────────────────────────────────────────────
    // When on_hit is set, unified_search delivers hits incrementally as each
    // task (and store) produces them, in addition to collecting them into
    // Result::hits. Streamed hits are compact (no inline detail) — callers
    // fetch detail lazily via a follow-up `search detail` for a specific hit.
    // Returning false from on_hit requests an early, cooperative stop.
    //
    // on_event receives coarse progress objects as JSON, e.g.
    //   {"event":"task","content":..,"mode":..,"backend":..,"matches":N,"ok":true,"skipped":false}
    //   {"event":"store","index":"..","matches":N}
    // The facade never emits "start"/"done"; that framing belongs to the
    // surface (CLI --stream / web host) so it can wrap the whole request.
    std::function<bool(const Hit&)> on_hit;
    std::function<void(const nlohmann::json&)> on_event;
};

struct Hit {
    std::string path;
    std::string title;
    std::string snippet;
    std::string source;
    std::string content_type;
    std::string kind;
    std::uint64_t line = 0;
    std::uint64_t column = 0;
    // Canonical 1-based UTF-16, end-exclusive text coordinates. `ranges`
    // contains every normalized submatch when a backend reports more than one.
    std::optional<TextRange> range;
    std::vector<TextRange> ranges;
    double score = 0.0;
    nlohmann::json metadata = nlohmann::json::object();
};

struct Result {
    bool ok = false;
    std::string error;
    Mode mode = Mode::Auto;
    Content content = Content::Any;
    Backend backend = Backend::Auto;
    std::vector<Hit> hits;
    nlohmann::json stats = nlohmann::json::object();
};

PM_SEARCH_API const char* to_string(Content value) noexcept;
PM_SEARCH_API const char* to_string(Mode value) noexcept;
PM_SEARCH_API const char* to_string(Backend value) noexcept;
PM_SEARCH_API const char* to_string(IndexPolicy value) noexcept;
PM_SEARCH_API const char* to_string(SortOrder value) noexcept;
PM_SEARCH_API const char* to_string(ActionKind value) noexcept;

PM_SEARCH_API bool parse_content(const std::string& text, Content& out) noexcept;
PM_SEARCH_API bool parse_mode(const std::string& text, Mode& out) noexcept;
PM_SEARCH_API bool parse_backend(const std::string& text, Backend& out) noexcept;
PM_SEARCH_API bool parse_index_policy(const std::string& text, IndexPolicy& out) noexcept;
PM_SEARCH_API bool parse_sort(const std::string& text, SortOrder& out) noexcept;
PM_SEARCH_API bool parse_action_kind(const std::string& text, ActionKind& out) noexcept;
PM_SEARCH_API bool request_from_json(const nlohmann::json& j, Request& out, std::string* error = nullptr) noexcept;

} // namespace media::search
