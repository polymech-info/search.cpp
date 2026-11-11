#pragma once
// Search source adapter boundary.
//
// Built-in text files use PlainTextSourceAdapter today. Office/PDF/SolidWorks
// can later arrive as foreign adapters/plugins that implement this small shape
// and feed extracted units into the same vector backend.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::search {

struct SourceUnit {
    std::string uri;       // stable dedup key; usually file path or file#part
    std::string path;      // origin path for file-level cache/removal
    std::string title;
    std::string type;      // text | document | sheet | slide | model | ...
    std::string text;
    std::vector<std::string> tags;
    nlohmann::json facts = nlohmann::json::object();   // adapter-specific searchable facts
    nlohmann::json locator = nlohmann::json::object(); // adapter-owned detail locator
    bool chunked = true; // false = index as one compact unit instead of text chunks
};

struct DetailRequest {
    std::string selector;
    std::string office_path;
    std::string include_detail = "auto"; // auto | row | page | section | object | metadata
    int context_before = 0;
    int context_after = 0;
    nlohmann::json options = nlohmann::json::object();
};

struct SourceDetail {
    bool ok = false;
    std::string error;
    std::string adapter_id;
    std::string path;
    std::string text;
    nlohmann::json locator = nlohmann::json::object();
    nlohmann::json data = nlohmann::json::object();
};

class ISourceAdapter {
public:
    virtual ~ISourceAdapter() = default;

    virtual const char* id() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual bool can_extract(const std::filesystem::path& file) const = 0;
    virtual std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                            std::uint64_t max_bytes = 0) = 0;
    virtual SourceDetail detail(const std::filesystem::path& file,
                                const DetailRequest& req);
    virtual const std::string& error() const noexcept = 0;
};

class CodeSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "code-heuristic"; }
    const char* name() const noexcept override { return "Code heuristic"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};

class ImageTagsSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "image-tags"; }
    const char* name() const noexcept override { return "Image EXIF IPTC XMP"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};

class ImageSidecarSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "image-sidecar"; }
    const char* name() const noexcept override { return "Image sidecar metadata"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};

class OcrLocalSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "ocr-local"; }
    const char* name() const noexcept override { return "Local OCR"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};

class LlmMetaSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "llm-meta"; }
    const char* name() const noexcept override { return "Image understanding metadata"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};

void set_source_adapter_options_for_thread(const nlohmann::json& options);
void clear_source_adapter_options_for_thread();

class PlainTextSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "plain-text"; }
    const char* name() const noexcept override { return "Plain text"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};

#if defined(FEATURE_OFFICE_EXTRACTOR) && FEATURE_OFFICE_EXTRACTOR
class OfficeSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "office-officecli"; }
    const char* name() const noexcept override { return "Office via officecli"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};
#endif

// .odt / .ods / .odp. Text comes from the office library markdown reader.
#if defined(FEATURE_OFFICE_EXTRACTOR) && FEATURE_OFFICE_EXTRACTOR
class OpenDocumentSourceAdapter final : public ISourceAdapter {
public:
    const char* id() const noexcept override { return "opendocument-anydoc"; }
    const char* name() const noexcept override { return "OpenDocument"; }
    bool can_extract(const std::filesystem::path& file) const override;
    std::vector<SourceUnit> extract(const std::filesystem::path& file,
                                    std::uint64_t max_bytes = 0) override;
    SourceDetail detail(const std::filesystem::path& file,
                        const DetailRequest& req) override;
    const std::string& error() const noexcept override { return error_; }

private:
    std::string error_;
};
#endif

} // namespace media::search
