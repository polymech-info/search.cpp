#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::search {

struct VectorDoc {
    std::uint64_t id = 0;
    std::string source;
    std::string path;
    std::string title;
    std::string chunk;
    std::vector<std::string> tags;
    nlohmann::json facts = nlohmann::json::object();
    nlohmann::json locator = nlohmann::json::object();
};

struct VectorHit {
    VectorDoc doc;
    float score = 0.f;
};

// Host-supplied embedding store. The package owns indexing and retrieval.
// Tanit installs a GGUF-backed implementation when FEATURE_LLAMA is on.
class IVectorStore {
public:
    virtual ~IVectorStore() = default;

    virtual const std::string& error() const noexcept = 0;
    virtual int context_tokens() const noexcept = 0;
    virtual const std::string& model_path() const noexcept = 0;
    virtual void set_embed_task(const std::string& task) = 0;
    virtual bool save() = 0;
    virtual std::size_t doc_count() const noexcept = 0;
    virtual bool gpu() const noexcept = 0;
    virtual int gpu_layers() const noexcept = 0;
    virtual std::size_t remove_path(const std::string& target) = 0;
    virtual bool has_path(const std::string& path) const noexcept = 0;
    virtual std::vector<std::uint64_t> add_chunked(
        std::string_view text,
        const std::string& source,
        const std::string& path,
        const std::string& title,
        const std::vector<std::string>& tags,
        int max_chars,
        int overlap_chars,
        const nlohmann::json& facts,
        const nlohmann::json& locator) = 0;
    virtual std::uint64_t add_text(
        std::string_view text,
        const std::string& source,
        const std::string& path,
        const std::string& title,
        const std::vector<std::string>& tags,
        const nlohmann::json& facts,
        const nlohmann::json& locator) = 0;
    virtual std::vector<VectorHit> search(
        std::string_view query,
        std::size_t top_k,
        const std::string& filter_source) const = 0;
};

} // namespace media::search
