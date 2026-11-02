#include "search/source_adapter.hpp"
#include "fs/path_sanitizer.hpp"

#include "string/string_sanitizers.hpp"
#include "fs/file_extensions.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace media::search {
namespace {

namespace fs = std::filesystem;

std::uint64_t file_size_u64(const fs::path& path)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

std::wstring lower_ext(const fs::path& path)
{
    std::wstring ext = path.extension().wstring();
    for (wchar_t& ch : ext)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

} // namespace

SourceDetail ISourceAdapter::detail(const fs::path& file, const DetailRequest& req)
{
    (void)req;
    SourceDetail out;
    out.adapter_id = id() ? id() : "";
    out.path = media::path::path_to_utf8(file);
    out.error = "detail is not implemented for adapter: " + out.adapter_id;
    return out;
}

bool PlainTextSourceAdapter::can_extract(const fs::path& file) const
{
    std::error_code ec;
    return fs::is_regular_file(file, ec) && !ec
        && pmui::is_text_ext(lower_ext(file));
}

std::vector<SourceUnit> PlainTextSourceAdapter::extract(const fs::path& file,
                                                        std::uint64_t max_bytes)
{
    error_.clear();

    const std::uint64_t size = file_size_u64(file);
    const std::uint64_t cap = max_bytes > 0 ? max_bytes : (4ull * 1024ull * 1024ull);
    if (size > cap) {
        error_ = "file too large for semantic text ingest";
        return {};
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error_ = "open failed";
        return {};
    }
    std::string text;
    text.resize(static_cast<std::size_t>(size));
    if (!text.empty())
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in && !in.eof()) {
        error_ = "read failed";
        return {};
    }
    text = media::str_san::apply_text_sanitizers(text, media::str_san::k_common_text_sanitizers);

    SourceUnit unit;
    unit.uri = media::path::path_to_utf8(file);
    unit.path = media::path::path_to_utf8(file);
    unit.title = media::path::path_to_utf8(file.filename());
    unit.type = "text";
    unit.text = std::move(text);
    unit.facts = {
        {"extension", media::path::path_to_utf8(file.extension())},
        {"size", size},
    };
    unit.locator = {
        {"kind", "file"},
        {"path", media::path::path_to_utf8(file)},
    };
    return { std::move(unit) };
}

SourceDetail PlainTextSourceAdapter::detail(const fs::path& file,
                                            const DetailRequest& req)
{
    SourceDetail out;
    out.adapter_id = id();
    out.path = media::path::path_to_utf8(file);
    const auto units = extract(file, 0);
    if (units.empty()) {
        out.error = error_;
        return out;
    }
    out.ok = true;
    out.text = units.front().text;
    out.locator = {
        {"includeDetail", req.include_detail},
    };
    return out;
}

} // namespace media::search

