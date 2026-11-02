#pragma once
//
// Shared search scope / root resolution for native overlay, CLI, and (later) Web.
// Chat typeahead keeps its own builder for now; this is the C++ source of truth
// for "cwd + folder bookmarks → scopes / inputs / display roots".
//
// Policies:
//   CwdOnlyWhenPresent — search current folder only; bookmarks are display roots
//                        (and become inputs only when cwd is missing). Native overlay.
//   CwdPlusBookmarks   — cwd + every searchable folder bookmark not nested under cwd.
//   CwdOnly            — current folder only; bookmarks never become inputs or
//                        display roots (file-panel inline search).
//   FileSearch         — file_search path tool when inputs/path are omitted.
//                        Order: current directory (if any), then searchable folder
//                        bookmarks. Further directory sources append after bookmarks.
//

#include "search/api.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace media::search::presets {

enum class ScopeKind {
    Cwd,
    Bookmark,
    Session,
};

enum class InputPolicy {
    /// Prefer a single current-directory root; bookmarks only if cwd absent.
    CwdOnlyWhenPresent,
    /// Union cwd with folder bookmarks (skip bookmarks nested under cwd).
    CwdPlusBookmarks,
    /// Current folder only. Bookmarks are ignored even when cwd is missing.
    CwdOnly,
    /// file_search default. Current directory (if any), then searchable folder
    /// bookmarks. Additional directory sources append after bookmarks.
    FileSearch,
};

struct Scope {
    std::string id;    ///< "cwd" | "bookmark:<pathkey>" | "session:…"
    ScopeKind   kind = ScopeKind::Bookmark;
    std::string path;  ///< UTF-8 filesystem root (empty for session).
    std::string label; ///< UI label ("This Directory", last path segments, …).
};

struct Options {
    InputPolicy policy = InputPolicy::CwdOnlyWhenPresent;
    /// When non-empty, reject roots under this directory (exe / install tree).
    std::string install_dir_utf8;
    /// Reject paths containing `\dist\win-x64` (build-output trees).
    bool reject_dist_win_x64 = true;
};

struct Resolved {
    std::vector<Scope>       scopes;       ///< UI grouping (cwd first, then bookmarks).
    std::vector<std::string> inputs;       ///< Roots for searchStart / rg.
    std::vector<std::string> display_roots; ///< Path shortening (cwd + all usable bookmarks).
};

/// Normalize separators to `\`, trim trailing separators (keep drive root).
PM_SEARCH_API std::string normalize_path_utf8(std::string_view path);

/// Case-insensitive path equality after normalize.
PM_SEARCH_API bool paths_equal_utf8(std::string_view a, std::string_view b);

/// True when @p path is @p root or a descendant (case-insensitive, `\` paths).
PM_SEARCH_API bool path_under_utf8(std::string_view path, std::string_view root);

/// Reject install / build-output roots used as search inputs.
PM_SEARCH_API bool is_rejected_search_root(
    std::string_view path_utf8,
    std::string_view install_dir_utf8,
    bool reject_dist_win_x64 = true);

/// Short folder label: last two path components when useful.
PM_SEARCH_API std::string folder_label_utf8(std::string_view path_utf8);

/// Pure resolve from an already-loaded list of folder bookmark paths (UTF-8).
PM_SEARCH_API Resolved resolve(
    std::string_view current_folder_utf8,
    const std::vector<std::string>& folder_bookmark_paths_utf8,
    const Options& options = {});

/// Load folder bookmarks through the installed host and resolve.
PM_SEARCH_API Resolved resolve_from_settings(
    std::string_view current_folder_utf8,
    const Options& options,
    std::string* bookmark_error = nullptr);

/// Serialize folder scopes as chat-compatible `typeahead_presets` JSON
/// (kind=folder only; cwd is supplied separately as host `folder`).
PM_SEARCH_API nlohmann::json bookmark_scopes_to_typeahead_presets_json(
    const Resolved& resolved);

} // namespace media::search::presets
