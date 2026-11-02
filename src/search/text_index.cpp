#include "search/text_index.hpp"
#include "fs/path_sanitizer.hpp"

#include "fs/file_extensions.hpp"
#include "fs/glob_paths.hpp"
#include "search/host.hpp"
#include "search/source_adapter_registry.hpp"

#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace media::search {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::uint64_t k_default_ingest_max_file_size_bytes = 256ull * 1024ull * 1024ull;

std::string lower_ascii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_default_excluded_name(const fs::path& path)
{
    const std::string name = media::path::path_to_utf8(path.filename());
    if (name.empty())
        return false;
    if (name.front() == '.')
        return true;

    const std::string n = lower_ascii(name);
    static const std::set<std::string> excluded = {
        "vfs-settings.json",
        ".git", ".svn", ".hg",
        "node_modules", ".npm", ".yarn", ".pnp.cjs", ".pnp.loader.mjs",
        "thumbs.db", "desktop.ini", "ehthumbs.db",
        ".ds_store", ".appledouble", ".lsoverride", ".spotlight-v100", ".trashes",
        ".fseventsd", ".temporaryitems", "__macosx",
        ".vscode", ".idea", ".fleet",
        ".env", ".env.local", ".env.production",
        ".cache", ".turbo", ".next", ".nuxt", ".parcel-cache", ".tsbuildinfo",
        ".angular", ".svelte-kit",
        "__pycache__", ".venv", "venv", ".eggs", ".mypy_cache", ".pytest_cache", ".ruff_cache",
        "target",
        "coverage", ".nyc_output",
        "npm-debug.log", "yarn-debug.log", "yarn-error.log",
        ".terraform", ".vagrant", "bower_components",
        "cmakefiles", "cmakecache.txt", "cmake_install.cmake", "cmakescripts",
        "out", "compile_commands.json",
        ".vs", "debug", "release", "x64", "x86", "ipch", "packages", "testresults",
    };
    return excluded.contains(n);
}

std::uint64_t effective_max_file_size_bytes(const Request& req)
{
    return req.max_file_size_bytes > 0 ? req.max_file_size_bytes : k_default_ingest_max_file_size_bytes;
}

bool text_like_adapter_requires_plaintext_sanity(const ISourceAdapter* adapter)
{
    if (!adapter)
        return false;
    const std::string id = adapter->id() ? adapter->id() : "";
    return id == "plain-text" || id == "code-heuristic";
}

std::wstring lower_ext(const fs::path& path)
{
    std::wstring ext = path.extension().wstring();
    for (wchar_t& ch : ext)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

bool is_office_family_adapter(const std::string& id)
{
    return id == "office-officecli" || id == "opendocument-anydoc";
}

bool adapter_matches_content(const ISourceAdapter* adapter, const fs::path& path, Content content)
{
    if (!adapter)
        return false;

    const std::string id = adapter->id() ? adapter->id() : "";
    const std::wstring ext = lower_ext(path);
    switch (content) {
    case Content::Any:
        // One default store: markdown/text, code, Office, and image tags.
        // OCR and vision stay behind an explicit image adapter.
        return id == "code-heuristic"
            || is_office_family_adapter(id)
            || id == "image-tags"
            || (id == "plain-text" && pmui::is_vector_ingest_text_ext(ext));
    case Content::Code:
        return id == "code-heuristic";
    case Content::Image:
        return id == "image-tags" || id == "ocr-local" || id == "llm-meta";
    case Content::Office:
        return is_office_family_adapter(id);
    case Content::Document:
        return is_office_family_adapter(id) || (id == "plain-text" && pmui::is_vector_ingest_text_ext(ext));
    case Content::Text:
        return id == "code-heuristic"
            || (id == "plain-text" && pmui::is_vector_ingest_text_ext(ext));
    case Content::Pdf:
    case Content::Catalog:
    default:
        return false;
    }
}

ISourceAdapter* adapter_for_content(SourceAdapterRegistry& registry,
                                    const fs::path& path,
                                    Content content,
                                    const std::string& adapter_id = {})
{
    if (!adapter_id.empty() && adapter_id != "auto") {
        for (ISourceAdapter* candidate : registry.adapters_for(path)) {
            if (!candidate || adapter_id != candidate->id())
                continue;
            // An explicit adapter on content=any is opt-in (ocr-local, llm-meta).
            if (content != Content::Any && !adapter_matches_content(candidate, path, content))
                continue;
            return candidate;
        }
        return nullptr;
    }

    for (ISourceAdapter* candidate : registry.adapters_for(path)) {
        if (adapter_matches_content(candidate, path, content))
            return candidate;
    }
    return nullptr;
}

std::string to_string_i64(std::int64_t v)
{
    return std::to_string(v);
}

std::string sha256_hex(const std::string& text)
{
    std::string hex;
    picosha2::hash256_hex_string(text.begin(), text.end(), hex);
    return hex;
}

json provenance_json(const Request& req, const std::string& model_path, const json& stats)
{
    json adapter = {
        {"id", req.adapter},
        {"options", req.adapter_options},
        {"optionsHash", sha256_hex(req.adapter_options.dump())},
    };
    if (req.adapter_options.contains("metaPrompt") && req.adapter_options["metaPrompt"].is_string())
        adapter["promptHash"] = sha256_hex(req.adapter_options["metaPrompt"].get<std::string>());

    json out = {
        {"embedding", {
            {"backend", to_string(Backend::Vector)},
            {"model", model_path},
            {"threads", req.threads},
            {"gpu", stats.value("gpu", false)},
            {"gpuLayers", stats.value("gpu_layers", 0)},
        }},
        {"adapter", std::move(adapter)},
        {"fingerprint", {
            {"mode", req.fingerprint},
            {"path", stats.value("fingerprint_path", std::string{})},
        }},
        {"ingest", {
            {"content", to_string(req.content)},
            {"mode", to_string(Mode::Semantic)},
            {"backend", to_string(Backend::Vector)},
            {"chunkSize", req.chunk_size},
            {"chunkOverlap", req.chunk_overlap},
            {"extract", req.extract},
        }},
    };
    if (!req.run_name.empty() || !req.user.empty()) {
        out["run"] = json::object();
        if (!req.run_name.empty())
            out["run"]["name"] = req.run_name;
        if (!req.user.empty())
            out["run"]["user"] = req.user;
    }
    return out;
}

bool filename_glob_match(const fs::path& path, const std::vector<std::string>& globs)
{
    if (globs.empty())
        return true;
    const std::string name = media::path::path_to_generic_utf8(path.filename());
    for (const auto& glob : globs) {
        if (media::path_matches_path_glob(name, glob))
            return true;
    }
    return false;
}

bool excluded_by_glob(const fs::path& path, const std::vector<std::string>& globs)
{
    if (globs.empty())
        return false;
    const std::string name = media::path::path_to_generic_utf8(path.filename());
    const std::string full = media::path::path_to_generic_utf8(path);
    for (const auto& glob : globs) {
        if (media::path_matches_path_glob(name, glob)
            || media::path_matches_path_glob(full, glob)) {
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& effective_exclude_dirs(const Request& req)
{
    if (!req.exclude_dirs.empty())
        return req.exclude_dirs;
    static const std::vector<std::string> defaults = [] {
        std::vector<std::string> out;
        for (const char* name : pmui::k_search_default_exclude_dirs)
            out.emplace_back(name);
        return out;
    }();
    return defaults;
}

bool dir_is_excluded(const fs::path& dir, const Request& req)
{
    const std::string name = media::path::path_to_utf8(dir.filename());
    if (name == ".pixlwiz" || is_default_excluded_name(dir))
        return true;
    const auto& dirs = effective_exclude_dirs(req);
    return std::find(dirs.begin(), dirs.end(), name) != dirs.end();
}

fs::path glob_anchor_dir(const std::string& raw)
{
    const std::string input = media::expand_home_dir(raw);
    const std::size_t glob_pos = input.find_first_of("*?");
    if (glob_pos == std::string::npos)
        return {};

    const std::size_t slash_pos = input.find_last_of("/\\", glob_pos);
    if (slash_pos == std::string::npos) {
        std::error_code ec;
        return fs::current_path(ec);
    }

    fs::path anchor = media::path::path_from_utf8(input.substr(0, slash_pos));
    if (anchor.empty()) {
        std::error_code ec;
        return fs::current_path(ec);
    }
    if (!anchor.is_absolute()) {
        std::error_code ec;
        anchor = fs::absolute(anchor, ec);
        if (ec)
            return {};
    }
    return anchor.lexically_normal();
}

void maybe_add_text_file(const fs::path& path, const Request& req, std::vector<fs::path>& out)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return;
    if (is_default_excluded_name(path))
        return;
    if (!req.include_hidden && media::path::path_to_utf8(path.filename()).starts_with("."))
        return;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > effective_max_file_size_bytes(req))
        return;
    SourceAdapterRegistry& registry = source_adapter_registry();
    ISourceAdapter* adapter = adapter_for_content(registry, path, req.content, req.adapter);
    if (!adapter)
        return;
    if (req.skip_binary && text_like_adapter_requires_plaintext_sanity(adapter)
        && !pmui::regular_file_sniffs_as_plaintext(path.wstring())) {
        return;
    }
    if (!filename_glob_match(path, req.include_globs))
        return;
    if (excluded_by_glob(path, req.exclude_globs))
        return;
    out.push_back(fs::absolute(path, ec));
    if (ec)
        out.back() = path;
}

std::int64_t file_mtime_tick(const fs::path& path)
{
    std::error_code ec;
    const auto ft = fs::last_write_time(path, ec);
    if (ec)
        return 0;
    return static_cast<std::int64_t>(ft.time_since_epoch().count());
}

} // namespace

TextFileInventory collect_text_files(const Request& req)
{
    TextFileInventory inventory;

    for (const std::string& raw : req.inputs) {
        std::string input = media::expand_home_dir(raw);
        std::vector<std::string> expanded;
        if (media::has_glob_tokens(input)) {
            std::string err;
            expanded = media::expand_input_paths(input, err);
        } else {
            expanded.push_back(input);
        }

        for (const std::string& item : expanded) {
            fs::path path = media::path::path_from_utf8(item);
            std::error_code ec;
            if (fs::is_directory(path, ec) && !ec) {
                if (req.recursive) {
                    fs::recursive_directory_iterator it(
                        path,
                        req.follow_symlinks ? fs::directory_options::follow_directory_symlink
                                            : fs::directory_options::none,
                        ec);
                    fs::recursive_directory_iterator end;
                    for (; !ec && it != end; it.increment(ec)) {
                        const fs::path current = it->path();
                        if (it->is_directory(ec)) {
                            if (dir_is_excluded(current, req))
                                it.disable_recursion_pending();
                            continue;
                        }
                        maybe_add_text_file(current, req, inventory.files);
                    }
                } else {
                    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
                        maybe_add_text_file(it->path(), req, inventory.files);
                }
            } else {
                maybe_add_text_file(path, req, inventory.files);
            }
        }
    }

    std::sort(inventory.files.begin(), inventory.files.end());
    inventory.files.erase(std::unique(inventory.files.begin(), inventory.files.end()), inventory.files.end());
    return inventory;
}

fs::path default_vector_index_root(const Request& req)
{
    std::error_code ec;
    if (!req.inputs.empty()) {
        fs::path first = media::path::path_from_utf8(media::expand_home_dir(req.inputs.front()));
        if (media::has_glob_tokens(media::path::path_to_utf8(first)))
            first = glob_anchor_dir(req.inputs.front());
        else if (fs::is_regular_file(first, ec))
            first = first.parent_path();
        if (!ec && !first.empty())
            return first / ".pixlwiz" / "search";
    }
    return fs::current_path(ec) / ".pixlwiz" / "search";
}

const char* default_vector_store_name(Content content, const std::string& adapter)
{
    if (adapter == "image-tags")
        return "image-tags.vstore";
    if (adapter == "ocr-local")
        return "image-ocr.vstore";
    if (adapter == "llm-meta")
        return "image-meta.vstore";
    if (adapter == "code-heuristic")
        return "code.vstore";
    if (adapter == "office-officecli" || adapter == "opendocument-anydoc")
        return "office.vstore";
    if (adapter == "plain-text")
        return "text.vstore";
    if (content == Content::Any)
        return "index.vstore";
    if (content == Content::Code)
        return "code.vstore";
    if (content == Content::Office)
        return "office.vstore";
    if (content == Content::Image) {
        if (adapter == "ocr-local")
            return "image-ocr.vstore";
        if (adapter == "llm-meta")
            return "image-meta.vstore";
        return "image-tags.vstore";
    }
    return "text.vstore";
}

fs::path default_vector_index_dir(const Request& req)
{
    if (!req.index_dir.empty())
        return media::path::path_from_utf8(req.index_dir);
    return default_vector_index_root(req) / default_vector_store_name(req.content, req.adapter);
}

namespace {

// Expands a glob that targets vector store *directories* (e.g. `*.vstore`).
// The file-oriented media::expand_input_paths only yields regular files, so a
// store glob needs its own directory-aware walk anchored at the longest
// glob-free prefix and matched with path_matches_path_glob.
std::vector<fs::path> expand_store_glob(const std::string& raw)
{
    std::vector<fs::path> out;
    std::string pattern = raw;
    for (char& c : pattern)
        if (c == '\\')
            c = '/';

    const bool recursive = pattern.find("**") != std::string::npos;
    const std::size_t glob_pos = pattern.find_first_of("*?");
    const std::size_t slash_before = pattern.rfind('/', glob_pos);
    std::string anchor = slash_before == std::string::npos ? std::string(".")
                                                           : pattern.substr(0, slash_before);
    if (anchor.empty())
        anchor = "/";

    std::string pat = pattern;
    if (pat.rfind("./", 0) == 0)
        pat = pat.substr(2);

    const auto consider = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_directory(p, ec))
            return;
        std::string s = media::path::path_to_generic_utf8(p);
        if (s.rfind("./", 0) == 0)
            s = s.substr(2);
        if (media::path_matches_path_glob(s, pat))
            out.push_back(p);
    };

    std::error_code ec;
    const fs::path anchor_path = media::path::path_from_utf8(anchor);
    if (!fs::exists(anchor_path, ec))
        return out;
    if (recursive) {
        for (fs::recursive_directory_iterator it(anchor_path,
                 fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            consider(it->path());
    } else {
        for (fs::directory_iterator it(anchor_path,
                 fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            consider(it->path());
    }
    return out;
}

} // namespace

std::vector<fs::path> resolve_search_index_dirs(const Request& req)
{
    std::vector<std::string> raw = req.index_dirs;
    if (raw.empty() && !req.index_dir.empty())
        raw.push_back(req.index_dir);

    // No explicit store: content=any reads every sibling store, including the
    // default index.vstore and any older per-type stores still on disk.
    if (raw.empty() && req.content == Content::Any) {
        std::vector<fs::path> out;
        const fs::path root = default_vector_index_root(req);
        std::error_code ec;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec))
                continue;
            const fs::path dir = it->path();
            if (dir.extension() == ".vstore")
                out.push_back(dir);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        if (!out.empty())
            return out;
    }

    // No explicit/discovered store: fall back to the input-derived default store.
    if (raw.empty())
        return {default_vector_index_dir(req)};

    std::vector<fs::path> out;
    std::set<std::string> seen;
    const auto add = [&](fs::path p) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec))
            p = p.parent_path();
        const std::string key = media::path::path_to_utf8(p.lexically_normal());
        if (seen.insert(key).second)
            out.push_back(std::move(p));
    };

    for (const std::string& entry : raw) {
        const std::string expanded = media::expand_home_dir(entry);
        if (media::has_glob_tokens(expanded)) {
            for (fs::path& match : expand_store_glob(expanded))
                add(std::move(match));
        } else {
            add(media::path::path_from_utf8(expanded));
        }
    }
    // An explicit but unmatched glob intentionally yields no stores so callers
    // can report "no store" rather than silently searching a default.
    return out;
}

fs::path vector_cache_path(const fs::path& index_dir)
{
    return index_dir / "file-cache.json";
}

fs::path search_manifest_path(const fs::path& index_dir)
{
    return index_dir / "search-manifest.json";
}

std::uint64_t file_size_u64(const fs::path& path)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

json load_vector_file_cache(const fs::path& index_dir)
{
    std::ifstream in(vector_cache_path(index_dir));
    if (!in)
        return json{{"version", 1}, {"files", json::object()}, {"directories", json::object()}, {"roots", json::array()}};
    try {
        json j = json::parse(in);
        if (!j.contains("files") || !j["files"].is_object())
            j["files"] = json::object();
        if (!j.contains("directories") || !j["directories"].is_object())
            j["directories"] = json::object();
        if (!j.contains("roots") || !j["roots"].is_array())
            j["roots"] = json::array();
        return j;
    } catch (...) {
        host::log_warn("search index: ignoring unreadable file-cache.json");
        return json{{"version", 1}, {"files", json::object()}, {"directories", json::object()}, {"roots", json::array()}};
    }
}

bool save_vector_file_cache(const fs::path& index_dir,
                            const Request& req,
                            const std::string& model_path,
                            const std::vector<fs::path>& files,
                            const std::map<std::string, int>& chunk_counts,
                            const std::map<std::string, std::string>& adapter_ids,
                            const std::map<std::string, std::vector<std::string>>& unit_uris)
{
    json cache;
    cache["version"] = 1;
    cache["roots"] = req.inputs;
    cache["ingest"] = {
        {"content", to_string(req.content)},
        {"backend", to_string(Backend::Vector)},
        {"mode", to_string(Mode::Semantic)},
        {"model", model_path},
        {"threads", req.threads},
        {"gpuLayers", req.gpu_layers},
        {"chunkSize", req.chunk_size},
        {"chunkOverlap", req.chunk_overlap},
        {"extract", req.extract},
        {"adapter", req.adapter},
        {"adapterOptions", req.adapter_options},
        {"fingerprint", req.fingerprint},
    };
    if (!req.run_name.empty())
        cache["ingest"]["runName"] = req.run_name;
    if (!req.user.empty())
        cache["ingest"]["user"] = req.user;
    cache["files"] = json::object();
    cache["directories"] = json::object();

    std::map<std::string, int> dir_counts;
    for (const fs::path& path : files) {
        const std::string p = media::path::path_to_utf8(path);
        const std::string dir = media::path::path_to_utf8(path.parent_path());
        ++dir_counts[dir];
        cache["files"][p] = {
            {"size", file_size_u64(path)},
            {"mtime", to_string_i64(file_mtime_tick(path))},
            {"chunks", chunk_counts.contains(p) ? chunk_counts.at(p) : 0},
            {"adapter", adapter_ids.contains(p) ? adapter_ids.at(p) : ""},
            {"unitUris", unit_uris.contains(p) ? unit_uris.at(p) : std::vector<std::string>{p}},
        };
    }
    for (const auto& [dir, count] : dir_counts)
        cache["directories"][dir] = {{"files", count}};

    std::error_code ec;
    fs::create_directories(index_dir, ec);
    fs::path tmp = vector_cache_path(index_dir);
    tmp += fs::path(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << cache.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
        if (!out)
            return false;
    }
    fs::rename(tmp, vector_cache_path(index_dir), ec);
    if (ec) {
        fs::remove(vector_cache_path(index_dir), ec);
        ec.clear();
        fs::rename(tmp, vector_cache_path(index_dir), ec);
    }
    return !ec;
}

bool cache_ingest_matches(const json& cache, const Request& req, const std::string& model_path)
{
    if (!cache.contains("ingest") || !cache["ingest"].is_object())
        return false;
    const json& ingest = cache["ingest"];
    // threads and gpuLayers change speed, not the stored vectors.
    auto identity_options = [](const json& opts) {
        json copy = opts.is_object() ? opts : json::object();
        copy.erase("threads");
        copy.erase("gpuLayers");
        return copy;
    };
    return ingest.value("content", std::string{}) == to_string(req.content)
        && ingest.value("backend", std::string{}) == to_string(Backend::Vector)
        && ingest.value("mode", std::string{}) == to_string(Mode::Semantic)
        && ingest.value("model", std::string{}) == model_path
        && ingest.value("chunkSize", 0) == req.chunk_size
        && ingest.value("chunkOverlap", 0) == req.chunk_overlap
        && ingest.value("extract", std::string{}) == req.extract
        && ingest.value("adapter", std::string{}) == req.adapter
        && identity_options(ingest.value("adapterOptions", json::object()))
            == identity_options(req.adapter_options)
        && ingest.value("fingerprint", std::string{}) == req.fingerprint;
}

bool save_search_manifest(const fs::path& index_dir,
                          const Request& req,
                          const json& adapter_registry,
                          const json& stats)
{
    json manifest;
    manifest["schemaVersion"] = 1;
    manifest["kind"] = req.content == Content::Image ? "image-vector" : "text-vector";
    manifest["content"] = to_string(req.content);
    manifest["mode"] = to_string(Mode::Semantic);
    manifest["backend"] = to_string(Backend::Vector);
    manifest["indexPolicy"] = to_string(req.index_policy);
    manifest["roots"] = req.inputs;
    if (!req.run_name.empty())
        manifest["runName"] = req.run_name;
    if (!req.user.empty())
        manifest["user"] = req.user;
    manifest["adapterRegistry"] = adapter_registry;
    manifest["model"] = stats.value("model", std::string{});
    manifest["gpu"] = stats.value("gpu", false);
    manifest["gpuLayers"] = stats.value("gpu_layers", 0);
    manifest["chunkSize"] = req.chunk_size;
    manifest["chunkOverlap"] = req.chunk_overlap;
    manifest["extract"] = req.extract;
    manifest["adapter"] = req.adapter;
    manifest["adapterOptions"] = req.adapter_options;
    manifest["fingerprint"] = req.fingerprint;
    manifest["provenance"] = provenance_json(req, manifest["model"].get<std::string>(), stats);
    manifest["stats"] = stats;

    std::error_code ec;
    fs::create_directories(index_dir, ec);
    fs::path tmp = search_manifest_path(index_dir);
    tmp += fs::path(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << manifest.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
        if (!out)
            return false;
    }
    fs::rename(tmp, search_manifest_path(index_dir), ec);
    if (ec) {
        fs::remove(search_manifest_path(index_dir), ec);
        ec.clear();
        fs::rename(tmp, search_manifest_path(index_dir), ec);
    }
    return !ec;
}

bool cache_entry_matches(const json& cache, const fs::path& path)
{
    const std::string p = media::path::path_to_utf8(path);
    if (!cache.contains("files") || !cache["files"].contains(p))
        return false;
    const json& entry = cache["files"][p];
    const auto cached_size = entry.value("size", std::uint64_t{0});
    const auto cached_mtime = entry.value("mtime", std::string{});
    return cached_size == file_size_u64(path)
        && cached_mtime == to_string_i64(file_mtime_tick(path));
}

std::vector<std::string> cache_unit_uris_for_path(const json& cache, const std::string& path)
{
    std::vector<std::string> out;
    if (!cache.contains("files") || !cache["files"].contains(path))
        return out;
    const json& entry = cache["files"][path];
    if (entry.contains("unitUris") && entry["unitUris"].is_array()) {
        for (const auto& item : entry["unitUris"]) {
            if (item.is_string())
                out.push_back(item.get<std::string>());
        }
    }
    if (out.empty())
        out.push_back(path);
    return out;
}

} // namespace media::search
