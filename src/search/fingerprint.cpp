#include "search/fingerprint.hpp"
#include "fs/path_sanitizer.hpp"

#include "search/host.hpp"
#include "search/text_index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include "string/string_utils.hpp"

namespace media::search {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

using media::strutil::lower_ascii;

bool is_stopword(const std::string& token)
{
    static const std::set<std::string> words = {
        "about", "after", "again", "also", "because", "between", "could", "from",
        "have", "into", "more", "should", "that", "their", "there", "these",
        "this", "through", "with", "would", "your",
    };
    return words.contains(token);
}

std::vector<std::string> top_keywords(const std::string& text, std::size_t limit)
{
    std::map<std::string, int> counts;
    std::string token;
    const auto flush = [&] {
        if (token.size() >= 3 && !is_stopword(token))
            ++counts[token];
        token.clear();
    };
    for (char ch : text) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '-') {
            token.push_back(static_cast<char>(std::tolower(uch)));
        } else {
            flush();
        }
    }
    flush();

    std::vector<std::pair<std::string, int>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });

    std::vector<std::string> out;
    for (const auto& [word, count] : ranked) {
        (void)count;
        out.push_back(word);
        if (out.size() >= limit)
            break;
    }
    return out;
}

std::vector<std::string> query_terms(const std::string& text)
{
    return top_keywords(text, 32);
}

bool json_array_contains_term(const json& value, const std::string& term)
{
    if (!value.is_array())
        return false;
    for (const auto& item : value) {
        if (item.is_string() && lower_ascii(item.get<std::string>()).find(term) != std::string::npos)
            return true;
    }
    return false;
}

std::vector<std::string> json_string_array(const json& value)
{
    std::vector<std::string> out;
    if (!value.is_array())
        return out;
    for (const auto& item : value) {
        if (item.is_string())
            out.push_back(item.get<std::string>());
    }
    return out;
}

std::string join_strings(const std::vector<std::string>& values, char sep = ' ')
{
    std::ostringstream out;
    bool first = true;
    for (const std::string& value : values) {
        if (!first)
            out << sep;
        out << value;
        first = false;
    }
    return out.str();
}

std::string slug_tokenize(std::string value)
{
    value = lower_ascii(std::move(value));
    for (char& ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch))
            ch = '-';
    }
    std::string out;
    bool prev_dash = false;
    for (char ch : value) {
        if (ch == '-') {
            if (!prev_dash && !out.empty())
                out.push_back(ch);
            prev_dash = true;
        } else {
            out.push_back(ch);
            prev_dash = false;
        }
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out;
}

json source_path_facts(const std::string& path_text)
{
    const fs::path path = media::path::path_from_utf8(path_text);
    std::vector<std::string> segments;
    std::vector<std::string> tokens;
    for (const fs::path& part : path) {
        const std::string raw = media::path::path_to_utf8(part);
        if (raw.empty() || raw == "/" || raw == "\\")
            continue;
        const std::string slug = slug_tokenize(raw);
        if (slug.empty())
            continue;
        segments.push_back(slug);
        std::stringstream in(slug);
        std::string token;
        while (std::getline(in, token, '-')) {
            if (token.size() >= 2)
                tokens.push_back(token);
        }
    }
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());

    return {
        {"filename", media::path::path_to_utf8(path.filename())},
        {"stem", media::path::path_to_utf8(path.stem())},
        {"extension", lower_ascii(media::path::path_to_utf8(path.extension()))},
        {"slug", join_strings(segments, '/')},
        {"segments", segments},
        {"tokens", tokens},
    };
}

double score_record(const json& record, const std::vector<std::string>& terms)
{
    if (terms.empty())
        return 0.0;

    const std::string title = lower_ascii(record.value("titleGuess", std::string{}));
    const std::string type = lower_ascii(record.value("typeGuess", std::string{}));
    const std::string summary = lower_ascii(record.value("summary1Line", std::string{}));
    const std::string path = lower_ascii(record.value("path", std::string{}));
    const std::string kind = lower_ascii(record.value("kind", std::string{}));
    const std::string facts = lower_ascii(record.contains("facts") ? record["facts"].dump() : std::string{});
    const std::string locator = lower_ascii(record.contains("locator") ? record["locator"].dump() : std::string{});
    const json source_path = record.contains("facts") && record["facts"].contains("sourcePath")
        ? record["facts"]["sourcePath"]
        : json::object();
    const std::string source_slug = lower_ascii(source_path.value("slug", std::string{}));
    const std::vector<std::string> source_segments = json_string_array(source_path.value("segments", json::array()));
    const std::vector<std::string> source_tokens = json_string_array(source_path.value("tokens", json::array()));

    double score = 0.0;
    for (const std::string& term : terms) {
        if (title.find(term) != std::string::npos)
            score += 4.0;
        if (json_array_contains_term(record.value("keywords", json::array()), term))
            score += 3.0;
        if (json_array_contains_term(record.value("entities", json::array()), term))
            score += 3.0;
        if (type.find(term) != std::string::npos || kind.find(term) != std::string::npos)
            score += 2.0;
        if (summary.find(term) != std::string::npos)
            score += 1.5;
        if (path.find(term) != std::string::npos)
            score += 1.0;
        if (source_slug.find(term) != std::string::npos)
            score += 2.0;
        if (std::find(source_segments.begin(), source_segments.end(), term) != source_segments.end())
            score += 2.0;
        if (std::find(source_tokens.begin(), source_tokens.end(), term) != source_tokens.end())
            score += 2.5;
        if (facts.find(term) != std::string::npos || locator.find(term) != std::string::npos)
            score += 0.5;
    }
    return score / static_cast<double>(terms.size());
}

std::vector<std::string> simple_entities(const std::string& text, std::size_t limit)
{
    std::map<std::string, int> counts;
    std::istringstream in(text.substr(0, 16000));
    std::string word;
    while (in >> word) {
        while (!word.empty() && std::ispunct(static_cast<unsigned char>(word.front())))
            word.erase(word.begin());
        while (!word.empty() && std::ispunct(static_cast<unsigned char>(word.back())))
            word.pop_back();
        if (word.size() < 3)
            continue;
        const unsigned char first = static_cast<unsigned char>(word.front());
        if (std::isupper(first))
            ++counts[word];
    }

    std::vector<std::pair<std::string, int>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });

    std::vector<std::string> out;
    for (const auto& [entity, count] : ranked) {
        (void)count;
        out.push_back(entity);
        if (out.size() >= limit)
            break;
    }
    return out;
}

std::string trim_ascii(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string extension_for_unit(const SourceUnit& unit)
{
    if (unit.facts.contains("extension") && unit.facts["extension"].is_string())
        return lower_ascii(unit.facts["extension"].get<std::string>());
    return lower_ascii(media::path::path_to_utf8(media::path::path_from_utf8(unit.path).extension()));
}

bool is_markdown_ext(const std::string& ext)
{
    return ext == ".md" || ext == ".markdown" || ext == ".mdown";
}

bool is_code_ext(const std::string& ext)
{
    static const std::set<std::string> exts = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
        ".js", ".jsx", ".ts", ".tsx", ".py", ".rs", ".go", ".java",
        ".cs", ".mm", ".m", ".swift", ".kt",
    };
    return exts.contains(ext);
}

bool is_delimited_ext(const std::string& ext)
{
    return ext == ".csv" || ext == ".tsv";
}

host::MarkdownDoc markdown_frontmatter_for_unit(const SourceUnit& unit)
{
    host::MarkdownDoc fm;
    fm.body = unit.text;
    const std::string ext = extension_for_unit(unit);
    if (is_markdown_ext(ext) || host::markdown_looks_like(unit.text))
        return host::parse_markdown(unit.text);
    return fm;
}

std::vector<std::string> markdown_headings(const std::string& text, std::size_t limit)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() != '#')
            continue;
        std::size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#')
            ++hashes;
        if (hashes == 0 || hashes > 6 || hashes >= line.size() || line[hashes] != ' ')
            continue;
        std::string heading = trim_ascii(line.substr(hashes + 1));
        if (!heading.empty())
            out.push_back(std::move(heading));
        if (out.size() >= limit)
            break;
    }
    return out;
}

std::vector<std::string> delimited_headers(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ascii(line);
        if (!line.empty())
            break;
    }
    if (line.empty())
        return {};
    const char delimiter = line.find('\t') != std::string::npos ? '\t' : ',';
    std::vector<std::string> out;
    std::stringstream row(line);
    std::string cell;
    while (std::getline(row, cell, delimiter)) {
        cell = trim_ascii(cell);
        if (!cell.empty())
            out.push_back(cell);
        if (out.size() >= 64)
            break;
    }
    return out.size() >= 2 ? out : std::vector<std::string>{};
}

std::vector<std::string> code_imports(const std::string& text, std::size_t limit)
{
    std::vector<std::string> out;
    std::istringstream in(text.substr(0, 32000));
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim_ascii(line);
        if (trimmed.starts_with("#include")
            || trimmed.starts_with("import ")
            || trimmed.starts_with("from ")
            || trimmed.starts_with("using ")
            || trimmed.starts_with("require(")) {
            out.push_back(trimmed.substr(0, std::min<std::size_t>(trimmed.size(), 180)));
        }
        if (out.size() >= limit)
            break;
    }
    return out;
}

std::vector<std::string> code_symbols(const std::string& text, std::size_t limit)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::istringstream in(text.substr(0, 64000));
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim_ascii(line);
        const std::vector<std::string> markers = {
            "class ", "struct ", "enum ", "function ", "def ", "fn ",
            "const ", "let ", "var ",
        };
        for (const std::string& marker : markers) {
            const std::size_t pos = trimmed.find(marker);
            if (pos == std::string::npos)
                continue;
            std::size_t start = pos + marker.size();
            while (start < trimmed.size() && (trimmed[start] == '*' || trimmed[start] == '&'))
                ++start;
            std::size_t end = start;
            while (end < trimmed.size()) {
                const unsigned char ch = static_cast<unsigned char>(trimmed[end]);
                if (!std::isalnum(ch) && trimmed[end] != '_')
                    break;
                ++end;
            }
            std::string symbol = trimmed.substr(start, end - start);
            if (symbol.size() >= 2 && seen.insert(symbol).second)
                out.push_back(std::move(symbol));
            break;
        }
        if (out.size() >= limit)
            break;
    }
    return out;
}

std::string text_family(const SourceUnit& unit)
{
    const std::string ext = extension_for_unit(unit);
    if (is_markdown_ext(ext))
        return "markdown";
    if (is_code_ext(ext))
        return "code";
    if (is_delimited_ext(ext))
        return "table";
    if (ext == ".json" || ext == ".yaml" || ext == ".yml" || ext == ".toml")
        return "structured";
    if (ext == ".log")
        return "log";
    if (unit.tags.end() != std::find(unit.tags.begin(), unit.tags.end(), "office"))
        return "office";
    if (unit.tags.end() != std::find(unit.tags.begin(), unit.tags.end(), "image")
        || lower_ascii(unit.type).find("image") != std::string::npos)
        return "image";
    return "text";
}

std::string title_guess(const SourceUnit& unit)
{
    const std::string family = text_family(unit);
    if (family == "markdown") {
        const host::MarkdownDoc fm = markdown_frontmatter_for_unit(unit);
        if (!fm.title.empty())
            return fm.title;
        const auto headings = markdown_headings(fm.body, 1);
        if (!headings.empty())
            return headings.front();
    }
    return unit.title;
}

json structure_for_unit(const SourceUnit& unit)
{
    const std::string family = text_family(unit);
    json structure = {
        {"tags", unit.tags},
        {"textType", family},
    };
    if (family == "markdown") {
        const host::MarkdownDoc fm = markdown_frontmatter_for_unit(unit);
        structure["headings"] = markdown_headings(fm.body, 32);
        structure["frontMatter"] = {
            {"found", fm.found},
            {"title", fm.title},
            {"summary", fm.summary},
            {"author", fm.author},
            {"tags", fm.tags},
        };
    }
    else if (family == "table")
        structure["columns"] = delimited_headers(unit.text);
    else if (family == "code") {
        if (unit.facts.contains("imports") && unit.facts["imports"].is_array())
            structure["imports"] = unit.facts["imports"];
        else
            structure["imports"] = code_imports(unit.text, 32);
        std::vector<std::string> symbols = code_symbols(unit.text, 64);
        const std::string symbol = unit.facts.value("symbol", std::string{});
        const std::string qualified = unit.facts.value("qualifiedSymbol", std::string{});
        if (!symbol.empty() && std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
            symbols.push_back(symbol);
        if (!qualified.empty() && std::find(symbols.begin(), symbols.end(), qualified) == symbols.end())
            symbols.push_back(qualified);
        structure["symbols"] = symbols;
        if (unit.facts.contains("symbolKind"))
            structure["symbolKind"] = unit.facts["symbolKind"];
        if (unit.facts.contains("namespace"))
            structure["namespace"] = unit.facts["namespace"];
        if (unit.facts.contains("parent"))
            structure["parent"] = unit.facts["parent"];
        if (unit.facts.contains("signature"))
            structure["signature"] = unit.facts["signature"];
        if (unit.facts.contains("docComment"))
            structure["docComment"] = unit.facts["docComment"];
    }
    else if (family == "image") {
        structure["image"] = {
            {"extension", unit.facts.value("extension", std::string{})},
            {"contentType", unit.facts.value("contentType", std::string{})},
            {"mediaKind", unit.facts.value("mediaKind", std::string{})},
        };
        if (unit.facts.contains("sidecars"))
            structure["sidecars"] = unit.facts["sidecars"];
    }
    return structure;
}

std::string type_guess(const SourceUnit& unit)
{
    const std::string type = lower_ascii(unit.type);
    const std::string family = text_family(unit);
    const std::string title = lower_ascii(title_guess(unit));
    const std::string text = lower_ascii(unit.text.substr(0, 12000));
    if (title.find("invoice") != std::string::npos || text.find("invoice") != std::string::npos)
        return "invoice";
    if (title.find("manual") != std::string::npos || text.find("manual") != std::string::npos)
        return "manual";
    if (family == "code" || type.find("code") != std::string::npos)
        return "source";
    if (family == "image" || type.find("image") != std::string::npos)
        return "image";
    if (family == "table")
        return "table";
    if (type.find("text") != std::string::npos)
        return "notes";
    return "unknown";
}

std::string one_line_summary(const SourceUnit& unit)
{
    std::string text = unit.text;
    if (text_family(unit) == "markdown") {
        const host::MarkdownDoc fm = markdown_frontmatter_for_unit(unit);
        if (!fm.summary.empty())
            return fm.summary.size() > 180 ? fm.summary.substr(0, 177) + "..." : fm.summary;
        text = fm.body;
    }
    const std::size_t newline = text.find_first_of("\r\n");
    if (newline != std::string::npos)
        text.resize(newline);
    if (text.size() > 180)
        text = text.substr(0, 177) + "...";
    return text;
}

std::string fingerprint_text_for_unit(const SourceUnit& unit, const std::string& title)
{
    const json source_path = source_path_facts(unit.path);
    const std::string path_text = source_path.value("slug", std::string{}) + "\n"
        + join_strings(json_string_array(source_path.value("tokens", json::array())));
    if (text_family(unit) != "markdown")
        return title + "\n" + path_text + "\n" + unit.text;
    const host::MarkdownDoc fm = markdown_frontmatter_for_unit(unit);
    std::ostringstream text;
    text << title << '\n' << path_text << '\n' << fm.summary << '\n';
    for (const std::string& tag : fm.tags)
        text << tag << '\n';
    text << fm.body;
    return text.str();
}

} // namespace

fs::path fingerprint_path(const fs::path& index_dir)
{
    return index_dir / "fingerprints.jsonl";
}

std::vector<json> load_fingerprints_for_path(const fs::path& index_dir,
                                             const std::string& source_path)
{
    std::vector<json> out;
    std::ifstream in(fingerprint_path(index_dir));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        try {
            json record = json::parse(line);
            if (record.value("path", std::string{}) == source_path)
                out.push_back(std::move(record));
        } catch (...) {
        }
    }
    return out;
}

std::vector<json> cheap_fingerprints_for_units(const std::string& adapter_id,
                                               const std::vector<SourceUnit>& units)
{
    std::vector<json> out;
    out.reserve(units.size());
    for (const SourceUnit& unit : units) {
        json facts = unit.facts;
        const std::string family = text_family(unit);
        facts["textType"] = family;
        facts["sourcePath"] = source_path_facts(unit.path);
        if (family == "markdown") {
            const host::MarkdownDoc fm = markdown_frontmatter_for_unit(unit);
            facts["frontMatter"] = fm.found;
        }
        const std::string title = title_guess(unit);
        const std::string fingerprint_text = fingerprint_text_for_unit(unit, title);
        out.push_back({
            {"version", 1},
            {"mode", "cheap"},
            {"uri", unit.uri},
            {"path", unit.path},
            {"adapter", adapter_id},
            {"kind", unit.type},
            {"titleGuess", title},
            {"typeGuess", type_guess(unit)},
            {"keywords", top_keywords(fingerprint_text, 20)},
            {"entities", simple_entities(fingerprint_text, 20)},
            {"summary1Line", one_line_summary(unit)},
            {"facts", std::move(facts)},
            {"locator", unit.locator},
            {"structure", structure_for_unit(unit)},
        });
    }
    return out;
}

bool save_fingerprints(const fs::path& index_dir, const std::vector<json>& records)
{
    std::error_code ec;
    fs::create_directories(index_dir, ec);
    fs::path tmp = fingerprint_path(index_dir);
    tmp += fs::path(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        for (const json& record : records)
            out << record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
        if (!out)
            return false;
    }
    fs::rename(tmp, fingerprint_path(index_dir), ec);
    if (ec) {
        fs::remove(fingerprint_path(index_dir), ec);
        ec.clear();
        fs::rename(tmp, fingerprint_path(index_dir), ec);
    }
    return !ec;
}

std::string indexed_detail_text_for_record(const fs::path& index_dir, const json& record)
{
    const std::string uri = record.value("uri", std::string{});
    const std::string path = record.value("path", std::string{});
    if (uri.empty() && path.empty())
        return {};

    std::ifstream in(index_dir / "docs.jsonl");
    if (!in)
        return {};

    std::string fallback;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        try {
            json doc = json::parse(line);
            const std::string doc_path = doc.value("path", std::string{});
            if (!uri.empty() && doc_path == uri)
                return doc.value("chunk", std::string{});
            if (fallback.empty() && !path.empty() && doc_path == path)
                fallback = doc.value("chunk", std::string{});
        } catch (...) {
        }
    }
    return fallback;
}

Result search_fingerprints(const Request& req)
{
    Result out;
    out.ok = false;
    out.mode = Mode::Fingerprint;
    out.backend = Backend::Fingerprint;
    out.content = req.content;

    const std::vector<fs::path> dirs = resolve_search_index_dirs(req);
    const std::vector<std::string> terms = query_terms(req.query);
    struct ScoredRecord {
        json record;
        double score = 0.0;
        fs::path index_dir;
    };
    std::vector<ScoredRecord> scored;
    json stores = json::array();
    int scanned = 0;
    int opened = 0;
    int missing = 0;

    for (const fs::path& index_dir : dirs) {
        const fs::path sidecar = fingerprint_path(index_dir);
        std::ifstream in(sidecar);
        if (!in) {
            ++missing;
            stores.push_back({{"index", media::path::path_to_utf8(index_dir)}, {"ok", false},
                              {"fingerprint_path", media::path::path_to_utf8(sidecar)}});
            continue;
        }
        ++opened;
        int store_scanned = 0;
        int store_matched = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            try {
                json record = json::parse(line);
                ++scanned;
                ++store_scanned;
                const double score = score_record(record, terms);
                if (score > 0.0) {
                    scored.push_back({std::move(record), score, index_dir});
                    ++store_matched;
                }
            } catch (...) {
            }
        }
        stores.push_back({{"index", media::path::path_to_utf8(index_dir)}, {"ok", true},
                          {"fingerprint_path", media::path::path_to_utf8(sidecar)},
                          {"records", store_scanned}, {"matched", store_matched}});
    }

    if (opened == 0) {
        const fs::path first = dirs.empty() ? fs::path{} : dirs.front();
        out.error = "fingerprint index does not exist: " + media::path::path_to_utf8(fingerprint_path(first));
        out.stats = {
            {"index_path", media::path::path_to_utf8(first)},
            {"fingerprint_path", media::path::path_to_utf8(fingerprint_path(first))},
            {"stores", stores},
            {"stores_missing", missing},
            {"matches", 0},
        };
        return out;
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredRecord& a, const ScoredRecord& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.record.value("path", std::string{}) < b.record.value("path", std::string{});
    });

    const int offset = std::max(0, req.offset);
    const int limit = req.limit > 0 ? req.limit : 20;
    const int end = std::min(static_cast<int>(scored.size()), offset + limit);
    for (int i = offset; i < end; ++i) {
        const ScoredRecord& sr = scored[static_cast<std::size_t>(i)];
        const json& record = sr.record;
        Hit h;
        h.path = record.value("path", std::string{});
        h.title = record.value("titleGuess", std::string{});
        h.snippet = record.value("summary1Line", std::string{});
        h.source = "fingerprint";
        h.kind = record.value("kind", std::string{});
        h.score = sr.score;
        h.metadata = {
            {"uri", record.value("uri", std::string{})},
            {"adapter", record.value("adapter", std::string{})},
            {"typeGuess", record.value("typeGuess", std::string{})},
            {"keywords", record.value("keywords", json::array())},
            {"entities", record.value("entities", json::array())},
            {"facts", record.value("facts", json::object())},
            {"locator", record.value("locator", json::object())},
            {"structure", record.value("structure", json::object())},
            {"index", media::path::path_to_utf8(sr.index_dir)},
        };
        if (!req.include_detail.empty()) {
            const std::string indexed_text = indexed_detail_text_for_record(sr.index_dir, record);
            if (!indexed_text.empty())
                h.metadata["__indexed_detail_text"] = indexed_text;
        }
        out.hits.push_back(std::move(h));
    }

    const fs::path first = dirs.front();
    out.ok = true;
    out.stats = {
        {"index_path", media::path::path_to_utf8(first)},
        {"fingerprint_path", media::path::path_to_utf8(fingerprint_path(first))},
        {"stores", stores},
        {"stores_opened", opened},
        {"stores_missing", missing},
        {"multi_store", dirs.size() > 1},
        {"records_scanned", scanned},
        {"matches", static_cast<int>(out.hits.size())},
        {"total_matches", static_cast<int>(scored.size())},
        {"applied_limit", limit},
        {"applied_offset", offset},
    };
    return out;
}

} // namespace media::search
