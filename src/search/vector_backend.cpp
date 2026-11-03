#include "search/vector_backend.hpp"
#include "fs/path_sanitizer.hpp"

#include "search/fingerprint.hpp"
#include "search/host.hpp"
#include "search/source_adapter_registry.hpp"
#include "search/text_index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <vector>
#include "string/string_utils.hpp"

namespace media::search {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

bool is_text_vector_content(Content content)
{
    return content == Content::Any
        || content == Content::Text
        || content == Content::Code
        || content == Content::Image
        || content == Content::Document
        || content == Content::Office;
}

std::string truncate_snippet(const std::string& text, std::size_t max_len = 500)
{
    if (text.size() <= max_len)
        return text;
    return text.substr(0, max_len) + "...";
}

std::vector<json> load_existing_fingerprints_for_current_paths(const fs::path& index_dir,
                                                               const std::set<std::string>& current_paths)
{
    std::vector<json> out;
    std::ifstream in(fingerprint_path(index_dir));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        try {
            json record = json::parse(line);
            const std::string path = record.value("path", std::string{});
            if (!path.empty() && current_paths.contains(path))
                out.push_back(std::move(record));
        } catch (...) {
        }
    }
    return out;
}

void erase_fingerprints_for_path(std::vector<json>& records, const std::string& path)
{
    records.erase(std::remove_if(records.begin(), records.end(),
                      [&](const json& record) {
                          return record.value("path", std::string{}) == path;
                      }),
                  records.end());
}

std::size_t remove_cached_units(IVectorStore& store,
                                const nlohmann::json& cache,
                                const std::string& file_path)
{
    std::size_t removed = store.remove_path(file_path);
    for (const std::string& uri : cache_unit_uris_for_path(cache, file_path)) {
        if (uri != file_path)
            removed += store.remove_path(uri);
    }
    return removed;
}

bool has_cached_units(const IVectorStore& store,
                      const nlohmann::json& cache,
                      const std::string& file_path)
{
    if (store.has_path(file_path))
        return true;
    for (const std::string& uri : cache_unit_uris_for_path(cache, file_path)) {
        if (store.has_path(uri))
            return true;
    }
    return false;
}

nlohmann::json adapter_registry_json(SourceAdapterRegistry& registry)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const ISourceAdapter* adapter : registry.adapters()) {
        if (!adapter)
            continue;
        arr.push_back({
            {"id", adapter->id()},
            {"name", adapter->name()},
        });
    }
    return arr;
}

bool adapter_should_checkpoint_each_file(const std::string& adapter_id)
{
    return adapter_id == "ocr-local" || adapter_id == "llm-meta";
}

std::mutex& store_lock_registry_mutex()
{
    static std::mutex mu;
    return mu;
}

std::map<std::string, std::shared_ptr<std::shared_mutex>>& store_lock_registry()
{
    static std::map<std::string, std::shared_ptr<std::shared_mutex>> locks;
    return locks;
}

std::shared_ptr<std::shared_mutex> store_lock_for_path(const fs::path& store_dir)
{
    std::lock_guard<std::mutex> lk(store_lock_registry_mutex());
    const std::string key = media::path::path_to_utf8(store_dir.lexically_normal());
    auto& locks = store_lock_registry();
    auto it = locks.find(key);
    if (it != locks.end())
        return it->second;
    auto created = std::make_shared<std::shared_mutex>();
    locks.emplace(key, created);
    return created;
}

} // namespace

Result index_vector(const Request& req)
{
    Result out;
    out.ok = false;
    out.backend = Backend::Vector;
    out.mode = Mode::Semantic;
    out.content = req.content;

    if (!is_text_vector_content(req.content)) {
        out.error = "vector indexing supports content=any|text|code|image|document|office only";
        return out;
    }

    const fs::path index_dir = default_vector_index_dir(req);
    // Exclusive per-store guard: indexing mutates the vector store and sidecars.
    // Readers take shared locks in search paths.
    auto store_lock = store_lock_for_path(index_dir);
    std::unique_lock<std::shared_mutex> write_guard(*store_lock);
    if (req.index_policy == IndexPolicy::SpecificDir && req.index_dir.empty()) {
        out.error = "--index-policy specific-dir requires --index";
        return out;
    }
    if ((req.index_policy == IndexPolicy::Disabled || !req.create_index)
        && !fs::exists(index_dir / "manifest.json")) {
        out.error = "semantic index does not exist and index creation is disabled";
        return out;
    }

    std::string open_error;
    auto store_owner = host::open_vector_store(index_dir, req.model, req.threads, req.gpu_layers, open_error);
    if (!store_owner) {
        out.error = open_error;
        return out;
    }
    IVectorStore& store = *store_owner;

    Request ingest = req;
    if (ingest.chunk_size <= 500) {
        const int ctx = store.context_tokens();
        if (ctx > 512) {
            const int sized = std::min(ctx * 3, 8192);
            ingest.chunk_size = sized;
            if (ingest.chunk_overlap <= 100)
                ingest.chunk_overlap = std::max(64, sized / 8);
            host::log_info("search index: chunk_size=" + std::to_string(sized)
                + " overlap=" + std::to_string(ingest.chunk_overlap)
                + " from model context " + std::to_string(ctx));
        }
    }

    host::log_info("search index: scanning text inputs -> " + media::path::path_to_utf8(index_dir));
    const TextFileInventory inventory = collect_text_files(req);
    json file_cache = load_vector_file_cache(index_dir);
    const bool ingest_cache_match = cache_ingest_matches(file_cache, ingest, store.model_path());
    if (!ingest_cache_match) {
        std::string model_name = store.model_path();
        const auto slash = model_name.find_last_of("/\\");
        if (slash != std::string::npos)
            model_name.erase(0, slash + 1);
        model_name = media::strutil::lower_ascii(std::move(model_name));
        if (model_name.find("nomic-embed") != std::string::npos)
            store.set_embed_task("nomic");
    }
    host::log_info("search index: candidates=" + std::to_string(inventory.files.size())
        + " cached=" + std::to_string(file_cache["files"].size())
        + " model=" + store.model_path()
        + (ingest_cache_match ? "" : " cache=stale"));

    int indexed_files = 0;
    int skipped_files = 0;
    int unchanged_files = 0;
    int changed_files = 0;
    int indexed_chunks = 0;
    int removed_chunks = 0;
    int removed_files = 0;
    int fingerprinted_units = 0;
    std::map<std::string, int> chunk_counts;
    std::map<std::string, std::string> adapter_ids;
    std::map<std::string, std::vector<std::string>> unit_uris;
    std::vector<nlohmann::json> fingerprint_records;

    SourceAdapterRegistry& adapters = source_adapter_registry();
    const auto emit_event = [&](nlohmann::json ev) {
        if (!req.on_event)
            return;
        if (!ev.contains("runName") && !req.run_name.empty())
            ev["runName"] = req.run_name;
        if (!ev.contains("user") && !req.user.empty())
            ev["user"] = req.user;
        req.on_event(std::move(ev));
    };

    emit_event({
        {"event", "index-start"},
        {"index", media::path::path_to_utf8(index_dir)},
        {"files", static_cast<int>(inventory.files.size())},
        {"content", to_string(req.content)},
        {"backend", to_string(Backend::Vector)},
        {"mode", to_string(Mode::Semantic)},
    });

    if (req.index_policy != IndexPolicy::Disabled && req.create_index) {
        std::set<std::string> current_paths;
        for (const fs::path& path : inventory.files)
            current_paths.insert(media::path::path_to_utf8(path));
        if (req.fingerprint == "cheap") {
            fingerprint_records = load_existing_fingerprints_for_current_paths(index_dir, current_paths);
            fingerprinted_units = static_cast<int>(fingerprint_records.size());
        }

        if (file_cache.contains("files") && file_cache["files"].is_object()) {
            for (auto it = file_cache["files"].begin(); it != file_cache["files"].end(); ++it) {
                const std::string cached_path = it.key();
                if (current_paths.contains(cached_path))
                    continue;
                const std::size_t removed = remove_cached_units(store, file_cache, cached_path);
                if (removed > 0) {
                    removed_chunks += static_cast<int>(removed);
                    ++removed_files;
                    host::log_info("search index: removed missing file path=" + cached_path
                        + " chunks=" + std::to_string(removed));
                }
            }
        }

        int files_since_checkpoint = 0;
        const auto checkpoint_store = [&](const char* reason) -> bool {
            if (!store.save()) {
                out.error = store.error();
                return false;
            }
            if (!save_vector_file_cache(index_dir, ingest, store.model_path(), inventory.files,
                                        chunk_counts, adapter_ids, unit_uris)) {
                host::log_warn("search index: failed to checkpoint file-cache.json");
            }
            if (req.fingerprint == "cheap" && !save_fingerprints(index_dir, fingerprint_records))
                host::log_warn("search index: failed to checkpoint fingerprints.jsonl");
            host::log_trace(std::string("search index: checkpoint reason=") + reason
                + " index=" + media::path::path_to_utf8(index_dir));
            return true;
        };

        for (const fs::path& path : inventory.files) {
            if (req.cancel_requested && req.cancel_requested()) {
                out.error = "cancelled";
                emit_event({
                    {"event", "index-done"},
                    {"ok", false},
                    {"cancelled", true},
                    {"index", media::path::path_to_utf8(index_dir)},
                    {"stats", {
                        {"candidate_files", static_cast<int>(inventory.files.size())},
                        {"indexed_files", indexed_files},
                        {"indexed_chunks", indexed_chunks},
                        {"changed_files", changed_files},
                        {"unchanged_files", unchanged_files},
                        {"removed_files", removed_files},
                        {"removed_chunks", removed_chunks},
                        {"skipped_files", skipped_files},
                    }},
                });
                return out;
            }
            const std::string path_s = media::path::path_to_utf8(path);
            const bool cache_match = ingest_cache_match && !req.reindex && cache_entry_matches(file_cache, path);
            if (cache_match && has_cached_units(store, file_cache, path_s)) {
                chunk_counts[path_s] = file_cache["files"][path_s].value("chunks", 0);
                adapter_ids[path_s] = file_cache["files"][path_s].value("adapter", std::string{});
                unit_uris[path_s] = cache_unit_uris_for_path(file_cache, path_s);
                ++unchanged_files;
                host::log_trace("search index: unchanged path=" + path_s);
                emit_event({
                    {"event", "index-file"},
                    {"path", path_s},
                    {"status", "cached"},
                    {"done", indexed_files + unchanged_files + skipped_files},
                    {"total", static_cast<int>(inventory.files.size())},
                    {"chunks", chunk_counts[path_s]},
                });
                continue;
            }

            if (req.reindex || has_cached_units(store, file_cache, path_s)) {
                const std::size_t removed = remove_cached_units(store, file_cache, path_s);
                if (removed > 0) {
                    removed_chunks += static_cast<int>(removed);
                    ++changed_files;
                    host::log_info("search index: reindex path=" + path_s
                        + " removed_chunks=" + std::to_string(removed));
                }
            } else if (!file_cache.contains("files") || !file_cache["files"].contains(path_s)) {
                host::log_trace("search index: new path=" + path_s);
            }

            if (!req.reindex && !cache_match && has_cached_units(store, file_cache, path_s)) {
                host::log_trace("search index: existing uncached path=" + path_s);
                ++skipped_files;
                emit_event({
                    {"event", "index-file"},
                    {"path", path_s},
                    {"status", "skipped"},
                    {"done", indexed_files + unchanged_files + skipped_files},
                    {"total", static_cast<int>(inventory.files.size())},
                });
                continue;
            }

            ISourceAdapter* adapter = nullptr;
            if (!req.adapter.empty() && req.adapter != "auto") {
                for (ISourceAdapter* candidate : adapters.adapters_for(path)) {
                    if (candidate && req.adapter == candidate->id()) {
                        adapter = candidate;
                        break;
                    }
                }
            } else {
                adapter = adapters.adapter_for(path);
            }
            if (!adapter) {
                ++skipped_files;
                host::log_trace("search index: no adapter path=" + path_s);
                emit_event({
                    {"event", "index-file"},
                    {"path", path_s},
                    {"status", "skipped"},
                    {"done", indexed_files + unchanged_files + skipped_files},
                    {"total", static_cast<int>(inventory.files.size())},
                    {"error", "no adapter"},
                });
                continue;
            }

            set_source_adapter_options_for_thread(req.adapter_options);
            auto units = adapter->extract(path, req.max_file_size_bytes);
            clear_source_adapter_options_for_thread();
            if (units.empty()) {
                ++skipped_files;
                host::log_warn("search index: skip path=" + path_s
                    + " adapter=" + adapter->id()
                    + " reason=" + adapter->error());
                emit_event({
                    {"event", "index-file"},
                    {"path", path_s},
                    {"status", "error"},
                    {"done", indexed_files + unchanged_files + skipped_files},
                    {"total", static_cast<int>(inventory.files.size())},
                    {"error", adapter->error()},
                });
                continue;
            }

            int file_chunks = 0;
            std::vector<std::string> file_unit_uris;
            for (const SourceUnit& unit : units) {
                const std::string unit_uri = unit.uri.empty() ? unit.path : unit.uri;
                nlohmann::json unit_facts = unit.facts;
                if (unit_facts.is_object() && !unit_facts.contains("adapter"))
                    unit_facts["adapter"] = adapter->id();
                if (unit.chunked) {
                    auto ids = store.add_chunked(unit.text, "file", unit_uri, unit.title, unit.tags,
                                                 ingest.chunk_size, ingest.chunk_overlap,
                                                 unit_facts, unit.locator);
                    file_chunks += static_cast<int>(ids.size());
                    if (!ids.empty())
                        file_unit_uris.push_back(unit_uri);
                } else {
                    const uint64_t id = store.add_text(unit.text, "file", unit_uri, unit.title,
                                                       unit.tags, unit_facts, unit.locator);
                    if (id != 0) {
                        ++file_chunks;
                        file_unit_uris.push_back(unit_uri);
                    }
                }
            }
            if (file_chunks <= 0) {
                ++skipped_files;
                host::log_warn("search index: no chunks path=" + path_s);
                emit_event({
                    {"event", "index-file"},
                    {"path", path_s},
                    {"status", "skipped"},
                    {"done", indexed_files + unchanged_files + skipped_files},
                    {"total", static_cast<int>(inventory.files.size())},
                    {"error", "no chunks"},
                });
                continue;
            }
            ++indexed_files;
            indexed_chunks += file_chunks;
            chunk_counts[path_s] = file_chunks;
            adapter_ids[path_s] = adapter->id();
            unit_uris[path_s] = std::move(file_unit_uris);
            if (req.fingerprint == "cheap") {
                erase_fingerprints_for_path(fingerprint_records, path_s);
                auto records = cheap_fingerprints_for_units(adapter->id(), units);
                fingerprint_records.insert(fingerprint_records.end(),
                                           std::make_move_iterator(records.begin()),
                                           std::make_move_iterator(records.end()));
                fingerprinted_units = static_cast<int>(fingerprint_records.size());
            }
            host::log_info("search index: indexed path=" + path_s
                + " adapter=" + adapter->id()
                + " bytes=" + std::to_string(file_size_u64(path))
                + " chunks=" + std::to_string(file_chunks));
            emit_event({
                {"event", "index-file"},
                {"path", path_s},
                {"status", "indexed"},
                {"done", indexed_files + unchanged_files + skipped_files},
                {"total", static_cast<int>(inventory.files.size())},
                {"chunks", file_chunks},
                {"adapter", adapter->id()},
            });
            ++files_since_checkpoint;
            const int checkpoint_every = adapter_should_checkpoint_each_file(adapter->id()) ? 1 : 40;
            if (files_since_checkpoint >= checkpoint_every) {
                if (!checkpoint_store(adapter->id()))
                    return out;
                files_since_checkpoint = 0;
            }
        }
        if ((indexed_files > 0 || removed_chunks > 0) && !store.save()) {
            out.error = store.error();
            return out;
        }
        if (!save_vector_file_cache(index_dir, ingest, store.model_path(), inventory.files,
                                    chunk_counts, adapter_ids, unit_uris))
            host::log_warn("search index: failed to save file-cache.json");
        if (req.fingerprint == "cheap" && (indexed_files > 0 || removed_files > 0 || removed_chunks > 0)
            && !save_fingerprints(index_dir, fingerprint_records))
            host::log_warn("search index: failed to save fingerprints.jsonl");
    }

    host::log_info("search index: done index=" + media::path::path_to_utf8(index_dir)
        + " indexed_files=" + std::to_string(indexed_files)
        + " unchanged_files=" + std::to_string(unchanged_files)
        + " removed_files=" + std::to_string(removed_files)
        + " doc_count=" + std::to_string(store.doc_count()));

    out.ok = true;
    out.stats = {
        {"index_path", media::path::path_to_utf8(index_dir)},
        {"cache_path", media::path::path_to_utf8(vector_cache_path(index_dir))},
        {"search_manifest_path", media::path::path_to_utf8(search_manifest_path(index_dir))},
        {"model", store.model_path()},
        {"gpu", store.gpu()},
        {"gpu_layers", store.gpu_layers()},
        {"candidate_files", static_cast<int>(inventory.files.size())},
        {"indexed_files", indexed_files},
        {"indexed_chunks", indexed_chunks},
        {"changed_files", changed_files},
        {"unchanged_files", unchanged_files},
        {"removed_files", removed_files},
        {"removed_chunks", removed_chunks},
        {"skipped_files", skipped_files},
        {"fingerprint_mode", req.fingerprint},
        {"fingerprint_path", media::path::path_to_utf8(fingerprint_path(index_dir))},
        {"fingerprinted_units", fingerprinted_units},
        {"doc_count", static_cast<int>(store.doc_count())},
    };
    if (!req.run_name.empty())
        out.stats["run_name"] = req.run_name;
    if (!req.user.empty())
        out.stats["user"] = req.user;
    emit_event({
        {"event", "index-done"},
        {"ok", true},
        {"cancelled", false},
        {"index", media::path::path_to_utf8(index_dir)},
        {"stats", out.stats},
    });
    if (!save_search_manifest(index_dir, req, adapter_registry_json(adapters), out.stats))
        host::log_warn("search index: failed to save search-manifest.json");
    return out;
}

namespace {

Hit vector_hit_from_doc(const Request& req, const VectorHit& vh,
                        const fs::path& index_dir)
{
    Hit h;
    h.path = vh.doc.path;
    h.title = vh.doc.title;
    h.snippet = truncate_snippet(vh.doc.chunk);
    h.source = "semantic";
    h.kind = to_string(req.content);
    h.score = vh.score;
    h.metadata = {
        {"doc_id", vh.doc.id},
        {"tags", vh.doc.tags},
        {"facts", vh.doc.facts},
        {"locator", vh.doc.locator},
        {"index", media::path::path_to_utf8(index_dir)},
    };
    if (vh.doc.facts.contains("adapter") && vh.doc.facts["adapter"].is_string())
        h.metadata["adapter"] = vh.doc.facts["adapter"].get<std::string>();
    if (!req.include_detail.empty())
        h.metadata["__indexed_detail_text"] = vh.doc.chunk;
    return h;
}

// Reads one or more existing vector stores and merges their hits without
// ingesting or pruning. Used for explicit multi-store search and whenever no
// inputs are supplied (so an existing store is never mutated by a query).
Result search_vector_existing(const Request& req,
                              const std::vector<fs::path>& dirs,
                              Backend backend,
                              Mode mode)
{
    Result out;
    out.ok = false;
    out.backend = backend;
    out.mode = mode;
    out.content = req.content;

    const std::size_t top_k = static_cast<std::size_t>(req.limit > 0 ? req.limit : 10);
    json stores = json::array();
    int opened = 0;
    int missing = 0;
    std::string last_error;
    std::vector<Hit> all;

    for (const fs::path& dir : dirs) {
        auto store_lock = store_lock_for_path(dir);
        std::shared_lock<std::shared_mutex> read_guard(*store_lock);
        if (req.cancel_requested && req.cancel_requested()) {
            out.error = "cancelled";
            return out;
        }
        if (!fs::exists(dir / "manifest.json")) {
            ++missing;
            stores.push_back({{"index", media::path::path_to_utf8(dir)}, {"ok", false}, {"error", "missing manifest.json"}});
            continue;
        }
        std::string open_error;
        auto store_owner = host::open_vector_store(dir, req.model, req.threads, req.gpu_layers, open_error);
        if (!store_owner) {
            last_error = open_error;
            stores.push_back({{"index", media::path::path_to_utf8(dir)}, {"ok", false}, {"error", open_error}});
            continue;
        }
        IVectorStore& store = *store_owner;
        ++opened;
        const auto hits = store.search(req.query, top_k, "file");
        for (const auto& vh : hits)
            all.push_back(vector_hit_from_doc(req, vh, dir));
        if (req.on_event) {
            nlohmann::json ev = {
                {"event", "store"},
                {"index", media::path::path_to_utf8(dir)},
                {"matches", static_cast<int>(hits.size())},
                {"docs", static_cast<int>(store.doc_count())},
                {"model", store.model_path()},
            };
            if (!req.run_name.empty())
                ev["runName"] = req.run_name;
            if (!req.user.empty())
                ev["user"] = req.user;
            req.on_event(std::move(ev));
        }
        stores.push_back({
            {"index", media::path::path_to_utf8(dir)},
            {"ok", true},
            {"matches", static_cast<int>(hits.size())},
            {"docs", static_cast<int>(store.doc_count())},
            {"model", store.model_path()},
        });
        host::log_info("search vector: store=" + media::path::path_to_utf8(dir)
            + " hits=" + std::to_string(hits.size())
            + " docs=" + std::to_string(store.doc_count()));
    }

    std::stable_sort(all.begin(), all.end(),
                     [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (all.size() > top_k)
        all.resize(top_k);

    out.ok = opened > 0;
    if (!out.ok)
        out.error = last_error.empty()
            ? "no vector store available for search"
            : last_error;
    out.hits = std::move(all);
    out.stats = {
        {"index_path", dirs.empty() ? std::string{} : media::path::path_to_utf8(dirs.front())},
        {"stores", stores},
        {"stores_opened", opened},
        {"stores_missing", missing},
        {"multi_store", dirs.size() > 1},
        {"matches", static_cast<int>(out.hits.size())},
    };
    return out;
}

} // namespace

Result search_vector(const Request& req, Backend backend, Mode mode)
{
    const std::vector<fs::path> dirs = resolve_search_index_dirs(req);
    const bool explicit_store = !req.index_dir.empty() || !req.index_dirs.empty();
    bool have_store = false;
    if (explicit_store) {
        for (const fs::path& dir : dirs) {
            if (fs::exists(dir / "manifest.json")) {
                have_store = true;
                break;
            }
        }
    }
    // A named store that already exists is searched. Passing a folder next to
    // --index must not embed that folder again. --reindex still rebuilds it.
    const bool search_only = !req.reindex
        && (req.inputs.empty() || dirs.size() > 1 || (explicit_store && have_store));
    if (search_only)
        return search_vector_existing(req, dirs, backend, mode);

    Result indexed = index_vector(req);
    indexed.backend = backend;
    indexed.mode = mode;
    if (!indexed.ok)
        return indexed;

    const fs::path index_dir = media::path::path_from_utf8(
        indexed.stats.value("index_path", std::string{}));
    auto store_lock = store_lock_for_path(index_dir);
    std::shared_lock<std::shared_mutex> read_guard(*store_lock);
    std::string open_error;
    auto store_owner = host::open_vector_store(index_dir, req.model, req.threads, req.gpu_layers, open_error);
    if (!store_owner) {
        indexed.ok = false;
        indexed.error = open_error;
        return indexed;
    }
    IVectorStore& store = *store_owner;

    const std::size_t top_k = static_cast<std::size_t>(req.limit > 0 ? req.limit : 10);
    const auto hits = store.search(req.query, top_k, "file");
    indexed.hits.reserve(hits.size());
    for (const auto& vh : hits)
        indexed.hits.push_back(vector_hit_from_doc(req, vh, index_dir));
    indexed.stats["matches"] = static_cast<int>(indexed.hits.size());
    return indexed;
}

} // namespace media::search
