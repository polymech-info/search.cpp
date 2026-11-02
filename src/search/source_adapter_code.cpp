#include "search/source_adapter.hpp"
#include "fs/path_sanitizer.hpp"

#include "string/string_sanitizers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(FEATURE_CODE_TREE_SITTER) && FEATURE_CODE_TREE_SITTER
#include <cstring>
#include <tree_sitter/api.h>
#include <tree-sitter-cpp.h>
#endif

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

bool is_code_ext(std::wstring_view ext)
{
    return ext == L".c" || ext == L".cc" || ext == L".cpp" || ext == L".cxx"
        || ext == L".h" || ext == L".hh" || ext == L".hpp" || ext == L".hxx"
        || ext == L".inl" || ext == L".js" || ext == L".jsx" || ext == L".ts"
        || ext == L".tsx" || ext == L".py" || ext == L".rs" || ext == L".go"
        || ext == L".java" || ext == L".cs" || ext == L".swift" || ext == L".kt";
}

std::string trim_ascii(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string code_language_from_ext(std::wstring_view ext)
{
    if (ext == L".c" || ext == L".h")
        return "c";
    if (ext == L".cc" || ext == L".cpp" || ext == L".cxx" || ext == L".hh"
        || ext == L".hpp" || ext == L".hxx" || ext == L".inl")
        return "cpp";
    if (ext == L".js" || ext == L".jsx" || ext == L".mjs" || ext == L".cjs")
        return "javascript";
    if (ext == L".ts" || ext == L".tsx")
        return "typescript";
    if (ext == L".py")
        return "python";
    if (ext == L".rs")
        return "rust";
    if (ext == L".go")
        return "go";
    if (ext == L".java")
        return "java";
    if (ext == L".cs")
        return "csharp";
    if (ext == L".swift")
        return "swift";
    if (ext == L".kt")
        return "kotlin";
    return "code";
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);
    if (text.ends_with('\n'))
        lines.push_back({});
    return lines;
}

std::vector<std::string> code_import_lines(const std::vector<std::string>& lines)
{
    std::vector<std::string> out;
    for (const std::string& line : lines) {
        const std::string trimmed = trim_ascii(line);
        if (trimmed.starts_with("#include")
            || trimmed.starts_with("import ")
            || trimmed.starts_with("from ")
            || trimmed.starts_with("using ")
            || trimmed.starts_with("require(")) {
            out.push_back(trimmed);
        }
        if (out.size() >= 64)
            break;
    }
    return out;
}

std::string line_range_text(const std::vector<std::string>& lines, int start_line, int end_line)
{
    std::ostringstream out;
    const int first = std::max(1, start_line);
    const int last = std::min(static_cast<int>(lines.size()), std::max(first, end_line));
    for (int line = first; line <= last; ++line)
        out << lines[static_cast<std::size_t>(line - 1)] << '\n';
    return out.str();
}

int brace_range_end(const std::vector<std::string>& lines, int start_line)
{
    int depth = 0;
    bool seen_open = false;
    for (int line = start_line; line <= static_cast<int>(lines.size()); ++line) {
        for (char ch : lines[static_cast<std::size_t>(line - 1)]) {
            if (ch == '{') {
                ++depth;
                seen_open = true;
            } else if (ch == '}' && seen_open) {
                --depth;
                if (depth <= 0)
                    return line;
            }
        }
        if (!seen_open && line > start_line + 8)
            break;
    }
    return start_line;
}

bool is_control_symbol_name(const std::string& name)
{
    return name == "if" || name == "for" || name == "while" || name == "switch"
        || name == "catch" || name == "return";
}

struct CodeSymbolInfo {
    std::string symbol;
    std::string symbol_kind;
    std::string parent;
    std::string namespace_name;
    std::string qualified_symbol;
    std::string signature;
    std::string doc_comment;
    int line_start = 1;
    int line_end = 1;
    std::uint32_t byte_start = 0;
    std::uint32_t byte_end = 0;
};

#if defined(FEATURE_CODE_TREE_SITTER) && FEATURE_CODE_TREE_SITTER
bool ts_type_is(TSNode node, const char* type)
{
    return std::strcmp(ts_node_type(node), type) == 0;
}

std::string ts_node_source_text(const std::string& source, TSNode node)
{
    const std::uint32_t start = ts_node_start_byte(node);
    const std::uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > source.size())
        return {};
    return source.substr(start, end - start);
}

std::string join_scope(const std::vector<std::string>& parts)
{
    std::ostringstream out;
    bool first = true;
    for (const std::string& part : parts) {
        if (part.empty())
            continue;
        if (!first)
            out << "::";
        out << part;
        first = false;
    }
    return out.str();
}

std::string normalize_scope_text(std::string value)
{
    value = trim_ascii(std::move(value));
    while (!value.empty() && value.back() == ':')
        value.pop_back();
    return trim_ascii(std::move(value));
}

bool ts_find_child_by_type(TSNode node, const char* type, TSNode& out)
{
    const std::uint32_t count = ts_node_child_count(node);
    for (std::uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (ts_type_is(child, type)) {
            out = child;
            return true;
        }
    }
    return false;
}

bool ts_find_descendant_by_type(TSNode node, const char* type, TSNode& out)
{
    if (ts_type_is(node, type)) {
        out = node;
        return true;
    }
    const std::uint32_t count = ts_node_child_count(node);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (ts_find_descendant_by_type(ts_node_child(node, i), type, out))
            return true;
    }
    return false;
}

std::string ts_field_text(const std::string& source, TSNode node, const char* field)
{
    TSNode child = ts_node_child_by_field_name(node, field, static_cast<std::uint32_t>(std::strlen(field)));
    if (ts_node_is_null(child))
        return {};
    return normalize_scope_text(ts_node_source_text(source, child));
}

std::string ts_symbol_name_for_node(const std::string& source, TSNode node, const std::string& kind)
{
    TSNode name_node{};
    if (kind == "function" || kind == "method") {
        TSNode declarator{};
        if (ts_find_descendant_by_type(node, "function_declarator", declarator)) {
            if (ts_find_descendant_by_type(declarator, "identifier", name_node)
                || ts_find_descendant_by_type(declarator, "field_identifier", name_node)
                || ts_find_descendant_by_type(declarator, "qualified_identifier", name_node)) {
                return ts_node_source_text(source, name_node);
            }
        }
    } else {
        if (ts_find_child_by_type(node, "type_identifier", name_node)
            || ts_find_descendant_by_type(node, "type_identifier", name_node)
            || ts_find_descendant_by_type(node, "identifier", name_node)) {
            return ts_node_source_text(source, name_node);
        }
    }
    return {};
}

std::string ts_signature_for_node(const std::string& source, TSNode node)
{
    const std::uint32_t start = ts_node_start_byte(node);
    std::uint32_t end = ts_node_end_byte(node);
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
        end = ts_node_start_byte(body);
    if (start >= end || end > source.size())
        return {};
    std::string signature = source.substr(start, end - start);
    const std::size_t newline = signature.find_first_of("\r\n");
    if (newline != std::string::npos)
        signature.resize(newline);
    if (signature.size() > 240)
        signature = signature.substr(0, 237) + "...";
    return trim_ascii(std::move(signature));
}

bool looks_like_line_comment(const std::string& trimmed)
{
    return trimmed.starts_with("//") || trimmed.starts_with("///")
        || trimmed.starts_with("*") || trimmed.starts_with("/*")
        || trimmed.starts_with("/**") || trimmed.starts_with("*/");
}

std::string clean_comment_line(std::string line)
{
    line = trim_ascii(std::move(line));
    while (line.starts_with("///") || line.starts_with("//"))
        line = trim_ascii(line.substr(line.starts_with("///") ? 3 : 2));
    if (line.starts_with("/**"))
        line = trim_ascii(line.substr(3));
    else if (line.starts_with("/*"))
        line = trim_ascii(line.substr(2));
    if (line.starts_with("*"))
        line = trim_ascii(line.substr(1));
    if (line.ends_with("*/"))
        line = trim_ascii(line.substr(0, line.size() - 2));
    return line;
}

std::string leading_doc_comment(const std::vector<std::string>& lines, int line_start)
{
    std::vector<std::string> block;
    for (int line = line_start - 1; line >= 1; --line) {
        const std::string trimmed = trim_ascii(lines[static_cast<std::size_t>(line - 1)]);
        if (trimmed.empty()) {
            if (block.empty())
                continue;
            break;
        }
        if (!looks_like_line_comment(trimmed))
            break;
        block.push_back(clean_comment_line(trimmed));
        if (block.size() >= 16)
            break;
        if (trimmed.starts_with("/*") || trimmed.starts_with("/**"))
            break;
    }
    std::reverse(block.begin(), block.end());
    std::ostringstream out;
    bool first = true;
    for (const std::string& line : block) {
        if (line.empty())
            continue;
        if (!first)
            out << '\n';
        out << line;
        first = false;
    }
    return out.str();
}

struct TsContext {
    std::vector<std::string> namespaces;
    std::vector<std::string> parents;
};

std::string qualified_symbol_for(const TsContext& ctx, const std::string& symbol)
{
    std::vector<std::string> parts = ctx.namespaces;
    parts.insert(parts.end(), ctx.parents.begin(), ctx.parents.end());
    parts.push_back(symbol);
    return join_scope(parts);
}

void ts_collect_symbols_recursive(const std::string& source,
                                  const std::vector<std::string>& lines,
                                  TSNode node,
                                  TsContext ctx,
                                  std::vector<CodeSymbolInfo>& out)
{
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "namespace_definition") == 0) {
        TsContext nested = ctx;
        const std::string name = ts_field_text(source, node, "name");
        if (!name.empty()) {
            std::stringstream scopes(name);
            std::string part;
            while (std::getline(scopes, part, ':')) {
                part = trim_ascii(std::move(part));
                if (!part.empty())
                    nested.namespaces.push_back(part);
            }
        }
        const std::uint32_t count = ts_node_child_count(node);
        for (std::uint32_t i = 0; i < count; ++i)
            ts_collect_symbols_recursive(source, lines, ts_node_child(node, i), nested, out);
        return;
    }

    std::string kind;
    if (std::strcmp(type, "function_definition") == 0)
        kind = ctx.parents.empty() ? "function" : "method";
    else if (std::strcmp(type, "class_specifier") == 0 || std::strcmp(type, "struct_specifier") == 0)
        kind = "class";
    else if (std::strcmp(type, "enum_specifier") == 0)
        kind = "enum";

    std::string class_symbol_for_children;
    if (!kind.empty()) {
        const std::string symbol = ts_symbol_name_for_node(source, node, kind);
        if (!symbol.empty() && !is_control_symbol_name(symbol)) {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            const std::string namespace_name = join_scope(ctx.namespaces);
            const std::string parent = ctx.parents.empty() ? namespace_name : join_scope(ctx.parents);
            const std::string qualified = qualified_symbol_for(ctx, symbol);
            out.push_back({
                symbol,
                kind,
                parent,
                namespace_name,
                qualified.empty() ? symbol : qualified,
                ts_signature_for_node(source, node),
                leading_doc_comment(lines, static_cast<int>(start.row) + 1),
                static_cast<int>(start.row) + 1,
                static_cast<int>(end.row) + 1,
                ts_node_start_byte(node),
                ts_node_end_byte(node),
            });
            if (kind == "class")
                class_symbol_for_children = symbol;
        }
    }

    if (!class_symbol_for_children.empty())
        ctx.parents.push_back(class_symbol_for_children);

    const std::uint32_t count = ts_node_child_count(node);
    for (std::uint32_t i = 0; i < count; ++i)
        ts_collect_symbols_recursive(source, lines, ts_node_child(node, i), ctx, out);
}

std::vector<CodeSymbolInfo> tree_sitter_cpp_symbols(const std::string& source,
                                                    const std::vector<std::string>& lines)
{
    std::vector<CodeSymbolInfo> out;
    TSParser* parser = ts_parser_new();
    if (!parser)
        return out;
    if (!ts_parser_set_language(parser, tree_sitter_cpp())) {
        ts_parser_delete(parser);
        return out;
    }
    TSTree* tree = ts_parser_parse_string(
        parser,
        nullptr,
        source.c_str(),
        static_cast<std::uint32_t>(source.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return out;
    }
    ts_collect_symbols_recursive(source, lines, ts_tree_root_node(tree), {}, out);
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    std::sort(out.begin(), out.end(), [](const CodeSymbolInfo& a, const CodeSymbolInfo& b) {
        if (a.line_start != b.line_start)
            return a.line_start < b.line_start;
        return a.symbol < b.symbol;
    });
    return out;
}
#endif

} // namespace

bool CodeSourceAdapter::can_extract(const fs::path& file) const
{
    std::error_code ec;
    return fs::is_regular_file(file, ec) && !ec
        && is_code_ext(lower_ext(file));
}

std::vector<SourceUnit> CodeSourceAdapter::extract(const fs::path& file,
                                                   std::uint64_t max_bytes)
{
    error_.clear();

    const std::uint64_t size = file_size_u64(file);
    const std::uint64_t cap = max_bytes > 0 ? max_bytes : (4ull * 1024ull * 1024ull);
    if (size > cap) {
        error_ = "file too large for code ingest";
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

    const std::vector<std::string> lines = split_lines(text);
    const std::string path_s = media::path::path_to_utf8(file);
    const std::string language = code_language_from_ext(lower_ext(file));
    const std::vector<std::string> imports = code_import_lines(lines);

    std::vector<SourceUnit> units;
    SourceUnit file_unit;
    file_unit.uri = path_s + "#file";
    file_unit.path = path_s;
    file_unit.title = media::path::path_to_utf8(file.filename());
    file_unit.type = "code-file";
    file_unit.text = text;
    file_unit.tags = {"code", language};
    file_unit.facts = {
        {"language", language},
        {"extension", media::path::path_to_utf8(file.extension())},
        {"imports", imports},
    };
    file_unit.locator = {
        {"kind", "file"},
        {"language", language},
        {"lineStart", 1},
        {"lineEnd", static_cast<int>(lines.size())},
    };
    units.push_back(std::move(file_unit));

    std::vector<CodeSymbolInfo> symbols;
#if defined(FEATURE_CODE_TREE_SITTER) && FEATURE_CODE_TREE_SITTER
    if (language == "cpp" || language == "c")
        symbols = tree_sitter_cpp_symbols(text, lines);
#endif
    if (symbols.empty()) {
        const std::regex class_re(R"(\b(class|struct|enum)\s+([A-Za-z_][A-Za-z0-9_:]*))");
        const std::regex function_re(R"(([A-Za-z_~][A-Za-z0-9_:~]*)\s*\([^;]*\)\s*(const\s*)?(\{|$))");
        for (int index = 0; index < static_cast<int>(lines.size()); ++index) {
            const std::string trimmed = trim_ascii(lines[static_cast<std::size_t>(index)]);
            if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("*"))
                continue;

            std::smatch match;
            CodeSymbolInfo symbol;
            if (std::regex_search(trimmed, match, class_re)) {
                symbol.symbol_kind = match[1].str() == "enum" ? "enum" : "class";
                symbol.symbol = match[2].str();
            } else if (trimmed.find('(') != std::string::npos
                       && trimmed.find(')') != std::string::npos
                       && std::regex_search(trimmed, match, function_re)) {
                symbol.symbol = match[1].str();
                if (is_control_symbol_name(symbol.symbol))
                    continue;
                symbol.symbol_kind = "function";
            } else {
                continue;
            }
            symbol.line_start = index + 1;
            symbol.line_end = brace_range_end(lines, symbol.line_start);
            symbols.push_back(std::move(symbol));
        }
    }

    for (const CodeSymbolInfo& symbol_info : symbols) {
        SourceUnit unit;
        unit.uri = path_s + "#" + symbol_info.symbol_kind + "=" + symbol_info.symbol;
        unit.path = path_s;
        unit.title = symbol_info.symbol;
        unit.type = "code-" + symbol_info.symbol_kind;
        unit.text = line_range_text(lines, symbol_info.line_start, symbol_info.line_end);
        unit.tags = {"code", language, symbol_info.symbol_kind};
        unit.facts = {
            {"language", language},
            {"symbol", symbol_info.symbol},
            {"symbolKind", symbol_info.symbol_kind},
            {"extension", media::path::path_to_utf8(file.extension())},
            {"imports", imports},
        };
        if (!symbol_info.parent.empty())
            unit.facts["parent"] = symbol_info.parent;
        if (!symbol_info.namespace_name.empty())
            unit.facts["namespace"] = symbol_info.namespace_name;
        if (!symbol_info.qualified_symbol.empty())
            unit.facts["qualifiedSymbol"] = symbol_info.qualified_symbol;
        if (!symbol_info.signature.empty())
            unit.facts["signature"] = symbol_info.signature;
        if (!symbol_info.doc_comment.empty())
            unit.facts["docComment"] = symbol_info.doc_comment;
        if (symbol_info.byte_end > symbol_info.byte_start) {
            unit.facts["byteStart"] = symbol_info.byte_start;
            unit.facts["byteEnd"] = symbol_info.byte_end;
        }
        unit.locator = {
            {"kind", "symbol"},
            {"language", language},
            {"symbol", symbol_info.symbol},
            {"symbolKind", symbol_info.symbol_kind},
            {"lineStart", symbol_info.line_start},
            {"lineEnd", symbol_info.line_end},
        };
        if (!symbol_info.parent.empty())
            unit.locator["parent"] = symbol_info.parent;
        if (!symbol_info.namespace_name.empty())
            unit.locator["namespace"] = symbol_info.namespace_name;
        if (!symbol_info.qualified_symbol.empty())
            unit.locator["qualifiedSymbol"] = symbol_info.qualified_symbol;
        if (symbol_info.byte_end > symbol_info.byte_start) {
            unit.locator["byteStart"] = symbol_info.byte_start;
            unit.locator["byteEnd"] = symbol_info.byte_end;
        }
        units.push_back(std::move(unit));
    }

    return units;
}

SourceDetail CodeSourceAdapter::detail(const fs::path& file,
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

    std::string requested_symbol;
    if (req.selector.starts_with("symbol:"))
        requested_symbol = req.selector.substr(std::string("symbol:").size());

    const SourceUnit* selected = &units.front();
    if (!requested_symbol.empty()) {
        for (const SourceUnit& unit : units) {
            if (unit.facts.value("symbol", std::string{}) == requested_symbol
                || unit.facts.value("qualifiedSymbol", std::string{}) == requested_symbol) {
                selected = &unit;
                break;
            }
        }
    }

    out.ok = true;
    out.text = selected->text;
    out.locator = selected->locator;
    out.locator["includeDetail"] = req.include_detail;
    out.data = selected->facts;
    return out;
}

} // namespace media::search
