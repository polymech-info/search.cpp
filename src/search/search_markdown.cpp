#include "search/search_markdown.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

namespace media::search {
namespace {

std::string md_cell(std::string text)
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '|') {
            text.insert(i, "\\");
            ++i;
        } else if (text[i] == '\r') {
            text.erase(i, 1);
            --i;
        } else if (text[i] == '\n' || text[i] == '\t') {
            text[i] = ' ';
        }
    }
    return text;
}

std::string truncate_cell(const std::string& text, std::size_t max_len)
{
    if (text.size() <= max_len)
        return text;
    if (max_len <= 3)
        return text.substr(0, max_len);
    return text.substr(0, max_len - 3) + "...";
}

std::string fence_text(const std::string& text)
{
    std::string fence = "```";
    while (text.find(fence) != std::string::npos)
        fence += "`";
    return fence + "\n" + text + "\n" + fence + "\n";
}

std::string score_text(double value)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << value;
    return os.str();
}

std::vector<std::string> split_char(const std::string& text, char delim)
{
    std::vector<std::string> out;
    std::string cell;
    std::istringstream in(text);
    while (std::getline(in, cell, delim))
        out.push_back(cell);
    if (!text.empty() && text.back() == delim)
        out.emplace_back();
    return out;
}

bool starts_with_row_path(const std::string& line)
{
    return !line.empty() && line.front() == '/' && line.find("/row[") != std::string::npos;
}

std::string row_detail_table_markdown(const std::string& text)
{
    struct Row {
        std::string path;
        std::vector<std::string> cells;
    };

    std::vector<Row> rows;
    std::istringstream in(text);
    std::string line;
    std::string pending_path;
    std::size_t max_cols = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (starts_with_row_path(line)) {
            pending_path = line;
            continue;
        }
        if (pending_path.empty() || line.find('\t') == std::string::npos)
            return {};
        Row row;
        row.path = std::move(pending_path);
        pending_path.clear();
        row.cells = split_char(line, '\t');
        while (!row.cells.empty() && row.cells.back().empty())
            row.cells.pop_back();
        max_cols = std::max(max_cols, row.cells.size());
        rows.push_back(std::move(row));
    }

    if (rows.empty() || max_cols == 0)
        return {};

    std::ostringstream md;
    md << "| Row |";
    for (std::size_t i = 0; i < max_cols; ++i)
        md << " C" << (i + 1) << " |";
    md << "\n| :--- |";
    for (std::size_t i = 0; i < max_cols; ++i)
        md << " :--- |";
    md << "\n";

    for (const Row& row : rows) {
        md << "| `" << md_cell(row.path) << "` |";
        for (std::size_t i = 0; i < max_cols; ++i) {
            const std::string cell = i < row.cells.size() ? row.cells[i] : std::string{};
            md << " " << md_cell(truncate_cell(cell, 90)) << " |";
        }
        md << "\n";
    }
    md << "\n";
    return md.str();
}

std::string detail_text_markdown(const std::string& text)
{
    if (std::string table = row_detail_table_markdown(text); !table.empty())
        return table;
    return fence_text(text) + "\n";
}

} // namespace

std::string result_to_markdown(const Request& req, const Result& result)
{
    std::ostringstream md;
    md << "## Search Results\n\n";
    md << "| Field | Value |\n";
    md << "| :--- | :--- |\n";
    md << "| Query | `" << md_cell(req.query) << "` |\n";
    md << "| Mode | `" << to_string(result.mode) << "` |\n";
    md << "| Content | `" << to_string(result.content) << "` |\n";
    md << "| Backend | `" << to_string(result.backend) << "` |\n";
    md << "| Hits | " << result.hits.size() << " |\n\n";

    if (!result.error.empty())
        md << "> Error: " << md_cell(result.error) << "\n\n";

    md << "| # | Score | Kind | Source | Path | Snippet |\n";
    md << "| ---: | ---: | :--- | :--- | :--- | :--- |\n";
    if (result.hits.empty()) {
        md << "|  |  |  |  | *(none)* |  |\n";
    } else {
        int i = 1;
        for (const Hit& hit : result.hits) {
            md << "| " << i++
               << " | " << score_text(hit.score)
               << " | " << md_cell(hit.kind)
               << " | " << md_cell(hit.source)
               << " | `" << md_cell(hit.path) << "`"
               << " | " << md_cell(truncate_cell(hit.snippet, 220))
               << " |\n";
        }
    }
    md << "\n";

    if (!result.stats.empty()) {
        md << "### Stats\n\n";
        md << "| Key | Value |\n";
        md << "| :--- | :--- |\n";
        for (auto it = result.stats.begin(); it != result.stats.end(); ++it)
            md << "| `" << md_cell(it.key()) << "` | `" << md_cell(it.value().dump()) << "` |\n";
        md << "\n";
    }

    int detail_index = 1;
    for (const Hit& hit : result.hits) {
        if (hit.metadata.contains("detail") && hit.metadata["detail"].is_object()) {
            const auto& detail = hit.metadata["detail"];
            md << "### Detail " << detail_index << "\n\n";
            md << "`" << md_cell(hit.path) << "`\n\n";
            if (detail.contains("locator"))
                md << "Locator: `" << md_cell(detail["locator"].dump()) << "`\n\n";
            if (detail.contains("text") && detail["text"].is_string())
                md << detail_text_markdown(detail["text"].get<std::string>());
        } else if (hit.metadata.contains("detail_error")) {
            md << "### Detail " << detail_index << "\n\n";
            md << "`" << md_cell(hit.path) << "`\n\n";
            md << "> Detail error: " << md_cell(hit.metadata["detail_error"].dump()) << "\n\n";
        }
        ++detail_index;
    }
    return md.str();
}

std::string detail_to_markdown(const SourceDetail& detail)
{
    std::ostringstream md;
    md << "## Search Detail\n\n";
    md << "| Field | Value |\n";
    md << "| :--- | :--- |\n";
    md << "| OK | `" << (detail.ok ? "true" : "false") << "` |\n";
    md << "| Adapter | `" << md_cell(detail.adapter_id) << "` |\n";
    md << "| Path | `" << md_cell(detail.path) << "` |\n";
    if (!detail.locator.empty())
        md << "| Locator | `" << md_cell(detail.locator.dump()) << "` |\n";
    if (!detail.error.empty())
        md << "| Error | " << md_cell(detail.error) << " |\n";
    md << "\n";

    if (!detail.text.empty()) {
        md << "### Text\n\n";
        md << detail_text_markdown(detail.text);
    }
    if (!detail.data.empty()) {
        md << "### Data\n\n";
        md << fence_text(detail.data.dump(2)) << "\n";
    }
    return md.str();
}

} // namespace media::search
