#include "search/search_presets.hpp"

#include "search/host.hpp"

#include <algorithm>
#include <cctype>
#include "string/string_utils.hpp"

namespace media::search::presets {
namespace {


std::string path_key(std::string_view path)
{
    return media::strutil::lower_ascii(normalize_path_utf8(path));
}

void append_unique(std::vector<std::string>& values, const std::string& raw)
{
    if (raw.empty())
        return;
    for (const std::string& existing : values) {
        if (paths_equal_utf8(existing, raw))
            return;
    }
    values.push_back(raw);
}

bool usable_root(std::string_view raw, const Options& options)
{
    return !is_rejected_search_root(
        raw, options.install_dir_utf8, options.reject_dist_win_x64);
}

} // namespace

std::string normalize_path_utf8(std::string_view path)
{
    std::string out(path);
    if (media::search::host::is_remote_uri(out)) {
        std::replace(out.begin(), out.end(), '\\', '/');
        while (out.size() > 8 && out.back() == '/')
            out.pop_back();
        return out;
    }
    std::replace(out.begin(), out.end(), '/', '\\');
    while (out.size() > 3 && (out.back() == '\\' || out.back() == '/'))
        out.pop_back();
    return out;
}

bool paths_equal_utf8(std::string_view a, std::string_view b)
{
    const std::string ka = path_key(a);
    const std::string kb = path_key(b);
    return !ka.empty() && ka == kb;
}

bool path_under_utf8(std::string_view path, std::string_view root)
{
    const std::string p = normalize_path_utf8(path);
    const std::string r = normalize_path_utf8(root);
    if (p.empty() || r.empty())
        return false;
    if (paths_equal_utf8(p, r))
        return true;
    if (p.size() <= r.size())
        return false;
    const std::string pl = media::strutil::lower_ascii(p);
    const std::string rl = media::strutil::lower_ascii(r);
    const char sep = media::search::host::is_remote_uri(r) ? '/' : '\\';
    return pl.size() > rl.size()
        && pl[rl.size()] == sep
        && pl.compare(0, rl.size(), rl) == 0;
}

bool is_rejected_search_root(
    std::string_view path_utf8,
    std::string_view install_dir_utf8,
    bool reject_dist_win_x64)
{
    const std::string path = normalize_path_utf8(path_utf8);
    if (path.empty())
        return true;
    if (media::search::host::is_remote_uri(path))
        return false;
    const std::string lower = media::strutil::lower_ascii(path);
    if (reject_dist_win_x64
        && lower.find("\\dist\\win-x64") != std::string::npos) {
        return true;
    }
    const std::string install = normalize_path_utf8(install_dir_utf8);
    if (!install.empty() && path_under_utf8(path, install))
        return true;
    return false;
}

std::string folder_label_utf8(std::string_view path_utf8)
{
    std::string p = normalize_path_utf8(path_utf8);
    while (!p.empty() && (p.back() == '\\' || p.back() == '/'))
        p.pop_back();
    if (p.empty())
        return {};
    const bool remote = media::search::host::is_remote_uri(p);
    const char sep = remote ? '/' : '\\';
    const auto sep1 = p.rfind(sep);
    if (sep1 == std::string::npos)
        return p;
    const std::string seg1 = p.substr(sep1 + 1);
    const std::string parent = p.substr(0, sep1);
    const auto sep2 = parent.rfind(sep);
    const std::string seg2 =
        sep2 != std::string::npos ? parent.substr(sep2 + 1) : parent;
    if (!seg2.empty() && seg2.size() > 3)
        return remote ? (seg2 + "/" + seg1) : (seg2 + "\\" + seg1);
    return seg1;
}

Resolved resolve(
    std::string_view current_folder_utf8,
    const std::vector<std::string>& folder_bookmark_paths_utf8,
    const Options& options)
{
    Resolved out;

    std::string cwd = normalize_path_utf8(current_folder_utf8);
    if (!usable_root(cwd, options))
        cwd.clear();

    if (!cwd.empty()) {
        out.scopes.push_back(Scope{
            .id = "cwd",
            .kind = ScopeKind::Cwd,
            .path = cwd,
            .label = "This Directory",
        });
        append_unique(out.display_roots, cwd);
    }

    if (options.policy != InputPolicy::CwdOnly) {
        std::vector<std::string> bookmark_paths;
        bookmark_paths.reserve(folder_bookmark_paths_utf8.size());
        for (const std::string& raw : folder_bookmark_paths_utf8) {
            const std::string path = normalize_path_utf8(raw);
            if (!usable_root(path, options))
                continue;
            bool seen = false;
            for (const std::string& existing : bookmark_paths) {
                if (paths_equal_utf8(existing, path)) {
                    seen = true;
                    break;
                }
            }
            if (seen)
                continue;
            bookmark_paths.push_back(path);
        }

        for (const std::string& path : bookmark_paths) {
            append_unique(out.display_roots, path);
            // Nested under cwd → covered by cwd search; keep display root only.
            if (!cwd.empty()
                && (paths_equal_utf8(path, cwd) || path_under_utf8(path, cwd))) {
                continue;
            }
            out.scopes.push_back(Scope{
                .id = "bookmark:" + path_key(path),
                .kind = ScopeKind::Bookmark,
                .path = path,
                .label = folder_label_utf8(path),
            });
        }
    }

    switch (options.policy) {
    case InputPolicy::CwdOnly:
        if (!cwd.empty())
            append_unique(out.inputs, cwd);
        break;
    case InputPolicy::CwdOnlyWhenPresent:
        if (!cwd.empty()) {
            append_unique(out.inputs, cwd);
        } else {
            for (const Scope& scope : out.scopes) {
                if (scope.kind == ScopeKind::Bookmark && !scope.path.empty())
                    append_unique(out.inputs, scope.path);
            }
        }
        break;
    case InputPolicy::CwdPlusBookmarks:
        for (const Scope& scope : out.scopes) {
            if (!scope.path.empty())
                append_unique(out.inputs, scope.path);
        }
        break;
    case InputPolicy::FileSearch:
        // Order is fixed: current directory (already first in scopes), then
        // searchable folder bookmarks. Further directory sources append here.
        for (const Scope& scope : out.scopes) {
            if (!scope.path.empty())
                append_unique(out.inputs, scope.path);
        }
        break;
    }

    return out;
}

Resolved resolve_from_settings(
    std::string_view current_folder_utf8,
    const Options& options,
    std::string* bookmark_error)
{
    std::vector<std::string> folders;
    std::string err;
    if (!media::search::host::load_search_folders(folders, err) && !err.empty()) {
        if (bookmark_error)
            *bookmark_error = err;
        media::search::host::log_warn("[search-presets] bookmark load failed: " + err);
    } else if (bookmark_error) {
        bookmark_error->clear();
    }
    return resolve(current_folder_utf8, folders, options);
}

nlohmann::json bookmark_scopes_to_typeahead_presets_json(const Resolved& resolved)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const Scope& scope : resolved.scopes) {
        if (scope.kind != ScopeKind::Bookmark || scope.path.empty())
            continue;
        arr.push_back({
            {"kind", "folder"},
            {"id", scope.id},
            {"label", scope.label},
            {"path", scope.path},
        });
    }
    return arr;
}

} // namespace media::search::presets
