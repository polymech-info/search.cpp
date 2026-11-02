#include "search/search_facade.hpp"
#include "fs/path_sanitizer.hpp"

#include "search/fingerprint.hpp"
#include "search/host.hpp"
#include "search/source_adapter_registry.hpp"
#include "search/text_index.hpp"
#include "search/vector_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace media::search {
namespace {

std::string lower_ascii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool eq(std::string text, const char* ref)
{
    return lower_ascii(std::move(text)) == ref;
}

// ── rg helpers ────────────────────────────────────────────────────────────────

// Probe and cache the working rg binary path.
// Tries the bundled binary first; if it fails to execute (e.g. machine type
// mismatch), walks the standard PATH looking for a usable rg.
// Result is cached after the first call so the probe only runs once per session.
std::filesystem::path resolve_rg()
{
    static std::once_flag  s_flag;
    static std::filesystem::path s_path; // empty = unavailable

    std::call_once(s_flag, []() {
        namespace fs = std::filesystem;

        // Candidates: bundled bin/ first, then bare name (PATH search via
        // resolve_third_party_executable which already walks both).
        // We call it once; if the result fails the version probe we try PATH
        // directly by looking up "rg" without the bundled directory prefix.
        const std::vector<fs::path> candidates = host::rg_candidates();

        for (const fs::path& candidate : candidates) {
            if (candidate.empty()) continue;

            host::ProcessRun opts;
            opts.executable        = media::path::path_to_utf8(candidate);
            opts.args              = {"--version"};
            opts.timeout_ms        = 3000;
            opts.bind_to_parent    = false;
            opts.capture_max_bytes = 256;

            const host::ProcessResult rr = host::run_process(opts);
            if (rr.ok && rr.exit_code == 0) {
                s_path = candidate;
                // Log first line of version string for diagnostics.
                std::string ver = rr.stdout_text;
                const auto nl = ver.find('\n');
                if (nl != std::string::npos) ver.resize(nl);
                while (!ver.empty() && (ver.back() == '\r' || ver.back() == '\n'))
                    ver.pop_back();
                host::log_info("rg: using " + media::path::path_to_utf8(candidate) + " (" + ver + ")");
                return;
            }
            host::log_info("rg: probe failed for " + media::path::path_to_utf8(candidate)
                         + " exit=" + std::to_string(rr.exit_code)
                         + (rr.error.empty() ? "" : " err=" + rr.error));
        }
        host::log_info("rg: no usable binary found (tried " + std::to_string(candidates.size()) + " candidates)");
    });

    return s_path;
}

// Default hard-kill timeout for rg child processes (ms).
// Prevents zombie rg on huge trees or hanging network mounts.
static constexpr int k_rg_timeout_ms = 30'000;

// Strip trailing CR/LF in-place.
void rg_strip_eol(std::string& s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
}

std::optional<std::string> decode_base64(std::string_view input)
{
    static constexpr unsigned char invalid = 0xff;
    static constexpr auto table = [] {
        std::array<unsigned char, 256> out{};
        out.fill(invalid);
        for (unsigned char i = 0; i < 26; ++i) {
            out[static_cast<unsigned char>('A' + i)] = i;
            out[static_cast<unsigned char>('a' + i)] = static_cast<unsigned char>(26 + i);
        }
        for (unsigned char i = 0; i < 10; ++i)
            out[static_cast<unsigned char>('0' + i)] = static_cast<unsigned char>(52 + i);
        out[static_cast<unsigned char>('+')] = 62;
        out[static_cast<unsigned char>('/')] = 63;
        return out;
    }();

    if (input.empty())
        return std::string{};
    if (input.size() % 4 != 0)
        return std::nullopt;
    std::string out;
    out.reserve((input.size() / 4) * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        unsigned char values[4]{};
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const unsigned char ch = static_cast<unsigned char>(input[i + static_cast<std::size_t>(j)]);
            if (ch == '=') {
                if (j < 2)
                    return std::nullopt;
                values[j] = 0;
                ++padding;
            } else {
                if (padding != 0 || table[ch] == invalid)
                    return std::nullopt;
                values[j] = table[ch];
            }
        }
        if (padding > 2 || (padding != 0 && i + 4 != input.size()))
            return std::nullopt;
        out.push_back(static_cast<char>((values[0] << 2) | (values[1] >> 4)));
        if (padding < 2)
            out.push_back(static_cast<char>((values[1] << 4) | (values[2] >> 2)));
        if (padding == 0)
            out.push_back(static_cast<char>((values[2] << 6) | values[3]));
    }
    return out;
}

std::optional<std::string> rg_text_value(const nlohmann::json& value)
{
    if (!value.is_object())
        return std::nullopt;
    if (value.contains("text") && value["text"].is_string())
        return value["text"].get<std::string>();
    if (value.contains("bytes") && value["bytes"].is_string())
        return decode_base64(value["bytes"].get<std::string>());
    return std::nullopt;
}

std::optional<TextRange> offset_range_lines(
    TextRange range,
    std::uint64_t first_line)
{
    if (first_line == 0)
        return std::nullopt;
    const std::uint64_t delta = first_line - 1;
    const std::uint64_t start_line = static_cast<std::uint64_t>(range.start.line) + delta;
    const std::uint64_t end_line = static_cast<std::uint64_t>(range.end.line) + delta;
    if (start_line > (std::numeric_limits<std::uint32_t>::max)()
        || end_line > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::nullopt;
    }
    range.start.line = static_cast<std::uint32_t>(start_line);
    range.end.line = static_cast<std::uint32_t>(end_line);
    return range;
}

void set_primary_range(Hit& hit, const TextRange& range)
{
    hit.range = range;
    hit.line = range.start.line;
    hit.column = range.start.column;
}

// Shared state threaded through the on_output_chunk callback.
// Declared outside the lambdas so both rg functions share the same line-buffer
// + hit-accumulation logic without duplicating the parsing loop.
struct RgChunkState {
    // Partial line carried across chunk boundaries.
    std::string line_buf;

    // Accumulated hits (also mirrored to the caller's Result::hits via pointer).
    std::vector<Hit>* hits = nullptr;

    // Streaming sink from the Request.
    std::function<bool(const Hit&)> on_hit;

    // Cancellation helpers.
    std::function<bool()> cancel_requested;
    std::atomic<bool>     stop{false}; // set true to signal on_cancel_check

    int limit  = 0;
    int offset = 0;
    int skipped = 0;

    std::string kind;

    // Returns false when the caller should stop (limit reached or on_hit
    // returned false or cancel was requested).
    bool push(Hit h)
    {
        if (skipped < offset) {
            ++skipped;
            return true;
        }
        if (on_hit && !on_hit(h)) {
            stop = true;
            return false;
        }
        hits->push_back(std::move(h));
        if (cancel_requested && cancel_requested()) {
            stop = true;
            return false;
        }
        if (limit > 0 && static_cast<int>(hits->size()) >= limit) {
            stop = true;
            return false;
        }
        return true;
    }
};

// Run `rg --files -g "*<glob>*" [inputs...]` and collect matching file paths.
// Uses process_runner Captured mode: proper cancel + timeout, no popen zombie.
Result run_rg_file_name(const Request& req, Backend backend, Mode mode)
{
    Result out;
    out.backend = backend;
    out.mode    = mode;
    out.content = req.content;

    const std::filesystem::path rg_bin = resolve_rg();
    if (rg_bin.empty()) {
        out.ok    = false;
        out.error = "rg not found (checked app bin and PATH)";
        return out;
    }

    // Build glob: "*<query>*" unless the query already contains wildcards.
    const std::string& q = req.query;
    const bool has_glob  = q.find('*') != std::string::npos
                        || q.find('?') != std::string::npos;
    const std::string glob_pat = has_glob ? q : ("*" + q + "*");

    const std::vector<std::string> inputs =
        req.inputs.empty() ? std::vector<std::string>{"."} : req.inputs;

    // Build structured argv — no shell quoting needed with process_runner.
    std::vector<std::string> args;
    args.push_back("--files");
    if (!req.recursive)          args.push_back("--max-depth=1");
    if (req.include_hidden)      args.push_back("--hidden");
    if (req.follow_symlinks)     args.push_back("-L");
    if (!req.respect_gitignore)  args.push_back("--no-ignore");
    if (req.max_file_size_bytes > 0)
        args.push_back("--max-filesize=" + std::to_string(req.max_file_size_bytes));
    args.push_back("-g");
    args.push_back(glob_pat);
    for (const auto& inc : req.include_globs) { args.push_back("-g"); args.push_back(inc); }
    for (const auto& exc : req.exclude_globs) { args.push_back("-g"); args.push_back("!" + exc); }
    for (const auto& path : inputs)
        args.push_back(path);

    {
        std::string dbg = "rg_file_name: bin=" + media::path::path_to_utf8(rg_bin) + " args=[";
        for (const auto& a : args) dbg += "\"" + a + "\" ";
        dbg += "]";
        host::log_trace(dbg);
    }

    RgChunkState state;
    state.hits             = &out.hits;
    state.on_hit           = req.on_hit;
    state.cancel_requested = req.cancel_requested;
    state.limit            = req.limit  > 0 ? req.limit  : 0;
    state.offset           = req.offset > 0 ? req.offset : 0;
    state.kind             = to_string(req.content);

    host::ProcessRun opts;
    opts.executable        = media::path::path_to_utf8(rg_bin);
    opts.args              = std::move(args);
    opts.timeout_ms        = k_rg_timeout_ms;
    opts.bind_to_parent    = true;
    opts.capture_max_bytes = 32 * 1024 * 1024;

    opts.on_cancel_check = [&state, &req]() -> bool {
        return state.stop.load(std::memory_order_relaxed)
            || (req.cancel_requested && req.cancel_requested());
    };

    opts.on_output_chunk = [&state](const std::string& stream,
                                    const std::string& chunk) {
        if (stream != "stdout" || state.stop.load(std::memory_order_relaxed))
            return;
        try {
            state.line_buf += chunk;
            std::size_t pos = 0;
            while (true) {
                const std::size_t nl = state.line_buf.find('\n', pos);
                if (nl == std::string::npos)
                    break;
                std::string line = state.line_buf.substr(pos, nl - pos);
                rg_strip_eol(line);
                pos = nl + 1;
                if (line.empty())
                    continue;
                Hit h;
                h.path   = std::move(line);
                h.source = "rg-file";
                h.kind   = state.kind;
                h.score  = 1.0;
                if (!state.push(std::move(h)))
                    return;
            }
            if (pos <= state.line_buf.size())
                state.line_buf = state.line_buf.substr(pos);
            else
                state.line_buf.clear();
        } catch (const std::exception& ex) {
            host::log_trace(std::string("rg_file_name chunk: exception: ") + ex.what());
        } catch (...) {
            host::log_trace("rg_file_name chunk: unknown exception");
        }
    };

    const host::ProcessResult rr = host::run_process(opts);

    // Flush any partial line left in the buffer (rg without trailing newline).
    if (!state.line_buf.empty() && !state.stop) {
        rg_strip_eol(state.line_buf);
        if (!state.line_buf.empty()) {
            Hit h;
            h.path   = std::move(state.line_buf);
            h.source = "rg-file";
            h.kind   = state.kind;
            h.score  = 1.0;
            state.push(std::move(h));
        }
    }

    // rg --files exits 0 (files found) or 1 (no files / no matches) — both ok.
    // Exit 2 means a usage/IO error.
    out.ok = rr.ok || rr.exit_code == 0 || rr.exit_code == 1;
    if (!out.ok)
        out.error = rr.error.empty() ? "rg failed (exit " + std::to_string(rr.exit_code) + ")" : rr.error;
    out.stats = {
        {"matches",   static_cast<int>(out.hits.size())},
        {"timed_out", rr.timed_out},
        {"cancelled", rr.cancelled},
        {"exit_code", rr.exit_code},
    };
    host::log_trace("rg_file_name: " + std::to_string(out.hits.size())
                  + " hits exit=" + std::to_string(rr.exit_code)
                  + " timed_out=" + (rr.timed_out ? "1" : "0")
                  + " cancelled=" + (rr.cancelled  ? "1" : "0")
                  + (rr.error.empty() ? "" : " err=" + rr.error)
                  + (out.error.empty() ? "" : " out_err=" + out.error));
    return out;
}

// Run `rg --json [flags] <query> [inputs...]` for full-text grep via rg.
//
// rg --json emits one NDJSON object per line. Relevant types:
//   {"type":"match","data":{"path":{"text":"..."},"line_number":N,
//     "absolute_offset":N,"lines":{"text":"..."},
//     "submatches":[{"match":{"text":"..."},"start":N,"end":N}]}}
//   {"type":"context",...}   -- context lines (ignored: we carry them inline)
//   {"type":"begin",...}     -- file open (ignored)
//   {"type":"end",...}       -- file close + per-file stats (captured in stats)
//   {"type":"summary",...}   -- final aggregate stats (captured in stats)
//
// Using --json avoids the fragile colon-split parser: paths with colons
// (Windows drives, URLs) are unambiguous, and binary/error events are typed.
//
// Uses process_runner Captured mode: proper cancel + timeout, no popen zombie.
Result run_rg_grep(const Request& req, Backend backend, Mode mode)
{
    Result out;
    out.backend = backend;
    out.mode    = mode;
    out.content = req.content;

    const std::filesystem::path rg_bin = resolve_rg();
    if (rg_bin.empty()) {
        out.ok    = false;
        out.error = "rg not found (checked app bin and PATH)";
        return out;
    }

    const std::vector<std::string> inputs =
        req.inputs.empty() ? std::vector<std::string>{"."} : req.inputs;

    std::vector<std::string> args;
    args.push_back("--json"); // structured NDJSON output
    if (!req.regex)            args.push_back("-F"); // treat query as literal, not regex
    if (!req.case_sensitive)   args.push_back("-i");
    if (req.whole_word)        args.push_back("-w");
    if (!req.recursive)        args.push_back("--max-depth=1");
    if (req.include_hidden)    args.push_back("--hidden");
    if (req.follow_symlinks)   args.push_back("-L");
    if (!req.respect_gitignore) args.push_back("--no-ignore");
    if (!req.skip_binary)      args.push_back("-a"); // search binary files as text
    if (req.multiline)         args.push_back("-U");
    if (req.max_file_size_bytes > 0)
        args.push_back("--max-filesize=" + std::to_string(req.max_file_size_bytes));
    if (req.max_matches_per_file > 0) {
        args.push_back("--max-count");
        args.push_back(std::to_string(req.max_matches_per_file));
    }
    if (req.context_before > 0) { args.push_back("-B"); args.push_back(std::to_string(req.context_before)); }
    if (req.context_after  > 0) { args.push_back("-A"); args.push_back(std::to_string(req.context_after));  }
    if (!req.type_filter.empty()) { args.push_back("-t"); args.push_back(req.type_filter); }
    for (const auto& inc : req.include_globs) { args.push_back("-g"); args.push_back(inc); }
    for (const auto& exc : req.exclude_globs) { args.push_back("-g"); args.push_back("!" + exc); }
    args.push_back("--"); // separator: everything after is the pattern/paths, not flags
    args.push_back(req.query);
    for (const auto& path : inputs)
        args.push_back(path);

    RgChunkState state;
    state.hits             = &out.hits;
    state.on_hit           = req.on_hit;
    state.cancel_requested = req.cancel_requested;
    state.limit            = req.limit  > 0 ? req.limit  : 0;
    state.offset           = req.offset > 0 ? req.offset : 0;
    state.kind             = to_string(req.content);

    // Aggregate stats collected from rg "summary" event.
    nlohmann::json rg_summary = nlohmann::json::object();
    // Per-file errors logged from "end" events where binary_offset is set.
    int rg_binary_files = 0;

    // Context accumulator: rg emits context events before and after each match.
    // We key them by line number and attach them to the nearest match hit.
    // context_before lines arrive before the match event; context_after arrive
    // after. Both are patched onto Result::hits (not on the streamed on_hit,
    // which fires before context_after is known — same as the own backend).
    struct CtxLine { std::uint64_t line_no; std::string text; };
    std::vector<CtxLine> pending_ctx; // lines seen before the next match
    // Index into out.hits of the last emitted match (for patching context_after).
    std::size_t last_hit_idx = std::string::npos;

    host::ProcessRun opts;
    opts.executable        = media::path::path_to_utf8(rg_bin);
    opts.args              = std::move(args);
    opts.timeout_ms        = k_rg_timeout_ms;
    opts.bind_to_parent    = true;
    opts.capture_max_bytes = 32 * 1024 * 1024; // 32 MB safety cap on buffered text

    opts.on_cancel_check = [&state, &req]() -> bool {
        return state.stop.load(std::memory_order_relaxed)
            || (req.cancel_requested && req.cancel_requested());
    };

    opts.on_output_chunk = [&state, &rg_summary, &rg_binary_files,
                            &pending_ctx, &last_hit_idx](
                               const std::string& stream,
                               const std::string& chunk)
    {
        if (stream != "stdout" || state.stop.load(std::memory_order_relaxed))
            return;

        state.line_buf += chunk;
        std::size_t pos = 0;
        while (true) {
            const std::size_t nl = state.line_buf.find('\n', pos);
            if (nl == std::string::npos)
                break;
            const std::string raw = state.line_buf.substr(pos, nl - pos);
            pos = nl + 1;
            if (raw.empty() || raw == "\r")
                continue;

            try {
                // Parse NDJSON event.
                const nlohmann::json ev =
                    nlohmann::json::parse(raw, nullptr, /*exceptions=*/false);
                if (ev.is_discarded() || !ev.is_object())
                    continue;

                const std::string type = ev.value("type", "");
                if (!ev.contains("data") || !ev["data"].is_object())
                    continue; // all rg events have a data object

                const nlohmann::json& data = ev["data"];

                if (type == "context") {
                    const std::uint64_t ctx_line =
                        data.value("line_number", std::uint64_t{0});
                    std::string ctx_text;
                    if (data.contains("lines") && data["lines"].is_object()) {
                        if (auto decoded = rg_text_value(data["lines"]))
                            ctx_text = std::move(*decoded);
                        rg_strip_eol(ctx_text);
                    }
                    if (last_hit_idx != std::string::npos
                        && last_hit_idx < state.hits->size()
                        && ctx_line > (*state.hits)[last_hit_idx].line) {
                        (*state.hits)[last_hit_idx]
                            .metadata["context_after"]
                            .push_back(ctx_text);
                    } else {
                        pending_ctx.push_back({ctx_line, std::move(ctx_text)});
                    }

                } else if (type == "match") {
                    std::string path;
                    if (data.contains("path"))
                        if (auto decoded = rg_text_value(data["path"]))
                            path = std::move(*decoded);
                    if (path.empty()) {
                        pending_ctx.clear();
                        continue;
                    }

                    const std::uint64_t line_no =
                        data.value("line_number", std::uint64_t{0});

                    std::string line_text;
                    if (data.contains("lines"))
                        if (auto decoded = rg_text_value(data["lines"]))
                            line_text = std::move(*decoded);
                    std::string snippet = line_text;
                    rg_strip_eol(snippet);

                    Hit h;
                    h.path    = std::move(path);
                    h.snippet = std::move(snippet);
                    h.source  = "rg-grep";
                    h.kind    = state.kind;
                    h.line    = line_no;
                    h.score   = 1.0;

                    if (data.contains("submatches") && data["submatches"].is_array()) {
                        for (const auto& submatch : data["submatches"]) {
                            if (!submatch.is_object()
                                || !submatch.contains("start") || !submatch["start"].is_number_unsigned()
                                || !submatch.contains("end") || !submatch["end"].is_number_unsigned()) {
                                continue;
                            }
                            const auto byte_start = submatch["start"].get<std::uint64_t>();
                            const auto byte_end = submatch["end"].get<std::uint64_t>();
                            if (h.column == 0 && byte_start < (std::numeric_limits<std::uint64_t>::max)())
                                h.column = byte_start + 1;
                            h.metadata["byte_ranges"].push_back({
                                {"start", byte_start},
                                {"end", byte_end},
                            });
                            if (byte_start > (std::numeric_limits<std::size_t>::max)()
                                || byte_end > (std::numeric_limits<std::size_t>::max)()) {
                                continue;
                            }
                            auto relative = host::utf16_range(
                                line_text,
                                static_cast<std::size_t>(byte_start),
                                static_cast<std::size_t>(byte_end));
                            if (!relative)
                                continue;
                            auto normalized = offset_range_lines(*relative, line_no);
                            if (!normalized)
                                continue;
                            h.ranges.push_back(*normalized);
                            if (!h.range)
                                set_primary_range(h, *normalized);
                        }
                        const auto& first = data["submatches"].empty()
                            ? nlohmann::json{} : data["submatches"].front();
                        if (first.is_object() && first.contains("match")) {
                            if (auto matched = rg_text_value(first["match"]);
                                matched && !matched->empty()
                                && matched->size() <= k_max_anchor_bytes) {
                                h.metadata["matched_text"] = std::move(*matched);
                            }
                        }
                    }

                    if (!pending_ctx.empty()) {
                        for (const auto& c : pending_ctx)
                            h.metadata["context_before"].push_back(c.text);
                        pending_ctx.clear();
                    }

                    last_hit_idx = state.hits->size();
                    if (!state.push(std::move(h)))
                        return;

                } else if (type == "end") {
                    if (data.contains("binary_offset")
                        && !data["binary_offset"].is_null())
                        ++rg_binary_files;
                    pending_ctx.clear();

                } else if (type == "summary") {
                    rg_summary = data; // data IS the summary object
                }
                // "begin" — ignored.

            } catch (const std::exception& ex) {
                host::log_trace(std::string("rg_grep chunk: exception: ") + ex.what());
                // Don't crash — skip the offending line and continue.
            } catch (...) {
                host::log_trace("rg_grep chunk: unknown exception");
            }
        }
        if (pos <= state.line_buf.size())
            state.line_buf = state.line_buf.substr(pos);
        else
            state.line_buf.clear();
    };

    const host::ProcessResult rr = host::run_process(opts);

    out.ok = rr.ok || rr.exit_code == 0 || rr.exit_code == 1;
    if (!out.ok)
        out.error = rr.error.empty()
            ? "rg grep failed (exit " + std::to_string(rr.exit_code) + ")"
            : rr.error;
    out.stats = {
        {"matches",      static_cast<int>(out.hits.size())},
        {"timed_out",    rr.timed_out},
        {"cancelled",    rr.cancelled},
        {"exit_code",    rr.exit_code},
        {"binary_files", rg_binary_files},
    };
    if (!rg_summary.empty())
        out.stats["rg_summary"] = rg_summary;

    host::log_trace("rg_grep: " + std::to_string(out.hits.size())
                  + " hits exit=" + std::to_string(rr.exit_code)
                  + " timed_out=" + (rr.timed_out ? "1" : "0")
                  + " cancelled=" + (rr.cancelled  ? "1" : "0")
                  + (rr.error.empty() ? "" : " err=" + rr.error)
                  + (out.error.empty() ? "" : " out_err=" + out.error));
    return out;
}

std::int64_t file_mtime_sort_tick(const std::string& path) noexcept
{
    if (path.empty())
        return 0;
    try {
        std::error_code ec;
        const auto ft = std::filesystem::last_write_time(media::path::path_from_utf8(path), ec);
        if (ec)
            return 0;
        return static_cast<std::int64_t>(ft.time_since_epoch().count());
    } catch (...) {
        return 0;
    }
}

Backend effective_backend(const Request& req)
{
    if (req.mode == Mode::Fingerprint)
        return Backend::Fingerprint;
    if (req.backend != Backend::Auto)
        return req.backend;

    if (req.content == Content::Catalog)
        return Backend::Catalog;
    if (req.content == Content::Image && req.mode == Mode::Metadata)
        return Backend::ImageMeta;
    if (req.mode == Mode::Semantic)
        return Backend::Vector;
    return Backend::Exact;
}

Mode effective_mode(const Request& req, Backend backend)
{
    if (req.mode != Mode::Auto)
        return req.mode;
    if (backend == Backend::Vector)
        return Mode::Semantic;
    if (backend == Backend::ImageMeta)
        return Mode::Metadata;
    if (backend == Backend::Fingerprint)
        return Mode::Fingerprint;
    return Mode::Name;
}

host::OwnOutput exact_output_mode(const std::string& mode)
{
    const std::string m = lower_ascii(mode);
    if (m == "files_with_matches")
        return host::OwnOutput::FilesWithMatches;
    if (m == "count")
        return host::OwnOutput::Count;
    return host::OwnOutput::Content;
}

bool is_word_char(char ch)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_';
}

bool is_whole_word_match(const std::string& text, std::size_t pos, std::size_t len)
{
    const bool left_ok = pos == 0 || !is_word_char(text[pos - 1]);
    const std::size_t end = pos + len;
    const bool right_ok = end >= text.size() || !is_word_char(text[end]);
    return left_ok && right_ok;
}

struct AdapterGrepMatcher {
    bool use_regex = false;
    bool case_sensitive = false;
    bool whole_word = false;
    std::string needle;
    std::regex re;

    bool init(const Request& req)
    {
        use_regex = req.regex;
        case_sensitive = req.case_sensitive;
        whole_word = req.whole_word;
        if (use_regex) {
            auto flags = std::regex_constants::ECMAScript | std::regex_constants::optimize;
            if (!case_sensitive)
                flags |= std::regex_constants::icase;
            try {
                re = std::regex(req.query, flags);
            } catch (const std::regex_error&) {
                return false;
            }
        } else {
            needle = case_sensitive ? req.query : lower_ascii(req.query);
        }
        return true;
    }

    bool find_in_line(const std::string& line, std::size_t& pos, std::size_t& len) const
    {
        if (use_regex) {
            std::smatch match;
            if (!std::regex_search(line, match, re))
                return false;
            pos = static_cast<std::size_t>(match.position(0));
            len = static_cast<std::size_t>(match.length(0));
            return !whole_word || is_whole_word_match(line, pos, len);
        }

        const std::string hay = case_sensitive ? line : lower_ascii(line);
        if (needle.empty()) {
            pos = 0;
            len = 0;
            return true;
        }
        std::size_t found = 0;
        while ((found = hay.find(needle, found)) != std::string::npos) {
            if (!whole_word || is_whole_word_match(hay, found, needle.size())) {
                pos = found;
                len = needle.size();
                return true;
            }
            ++found;
        }
        return false;
    }
};

bool should_use_adapter_exact_grep(const Request& req, Mode mode)
{
    if (mode != Mode::Grep)
        return false;
    if (!req.adapter.empty() && req.adapter != "auto")
        return true;
    return req.content == Content::Document
        || req.content == Content::Office
        || req.content == Content::Pdf
        || req.content == Content::Image;
}

ISourceAdapter* adapter_for_exact_path(SourceAdapterRegistry& registry,
                                       const Request& req,
                                       const std::filesystem::path& path)
{
    if (!req.adapter.empty() && req.adapter != "auto") {
        for (ISourceAdapter* candidate : registry.adapters_for(path)) {
            if (candidate && req.adapter == candidate->id())
                return candidate;
        }
        return nullptr;
    }
    return registry.adapter_for(path);
}

std::string clamp_snippet(std::string text)
{
    constexpr std::size_t kMax = 500;
    if (text.size() <= kMax)
        return text;
    return text.substr(0, kMax) + " [...]";
}

Result unsupported(const Request& req, Backend backend, Mode mode, const char* what)
{
    Result out;
    out.ok = false;
    out.error = std::string("unified search backend not implemented yet: ") + what;
    out.backend = backend;
    out.mode = mode;
    out.content = req.content;
    return out;
}

Result run_adapter_exact_grep(const Request& req, Backend backend, Mode mode)
{
    Result out;
    out.ok = true;
    out.backend = backend;
    out.mode = mode;
    out.content = req.content;

    AdapterGrepMatcher matcher;
    if (!matcher.init(req)) {
        out.ok = false;
        out.error = "invalid regex";
        return out;
    }

    const TextFileInventory inventory = collect_text_files(req);
    SourceAdapterRegistry& registry = source_adapter_registry();
    const int offset = std::max(0, req.offset);
    const int limit = req.limit > 0 ? req.limit : 0;
    int total_matches = 0;
    int scanned_files = 0;
    int skipped_files = 0;

    for (const std::filesystem::path& path : inventory.files) {
        if (req.cancel_requested && req.cancel_requested()) {
            out.ok = false;
            out.error = "cancelled";
            break;
        }

        ISourceAdapter* adapter = adapter_for_exact_path(registry, req, path);
        if (!adapter) {
            ++skipped_files;
            continue;
        }

        set_source_adapter_options_for_thread(req.adapter_options);
        std::vector<SourceUnit> units = adapter->extract(path, req.max_file_size_bytes);
        clear_source_adapter_options_for_thread();
        if (units.empty()) {
            ++skipped_files;
            continue;
        }
        ++scanned_files;

        std::uint32_t file_matches = 0;
        for (const SourceUnit& unit : units) {
            std::istringstream lines(unit.text);
            std::string line;
            std::uint64_t line_no = 0;
            while (std::getline(lines, line)) {
                ++line_no;
                std::size_t pos = 0;
                std::size_t len = 0;
                if (!matcher.find_in_line(line, pos, len))
                    continue;

                ++total_matches;
                ++file_matches;
                if (total_matches <= offset)
                    continue;
                if (limit > 0 && static_cast<int>(out.hits.size()) >= limit)
                    continue;

                Hit h;
                h.path = unit.uri.empty() ? unit.path : unit.uri;
                h.title = unit.title;
                h.snippet = clamp_snippet(line);
                h.source = "adapter-grep";
                h.kind = unit.type;
                h.line = line_no;
                h.column = static_cast<std::uint64_t>(pos + 1);
                h.score = 1.0;
                h.metadata = {
                    {"adapter", adapter->id()},
                    {"facts", unit.facts},
                    {"locator", unit.locator},
                    {"tags", unit.tags},
                };
                if (auto range = host::utf16_range(
                        line, pos, pos + len)) {
                    if (auto normalized = offset_range_lines(*range, line_no)) {
                        set_primary_range(h, *normalized);
                        h.ranges.push_back(*normalized);
                        if (len > 0 && len <= k_max_anchor_bytes)
                            h.metadata["matched_text"] = line.substr(pos, len);
                    }
                }
                if (!req.include_detail.empty())
                    h.metadata["__indexed_detail_text"] = unit.text;
                out.hits.push_back(std::move(h));

                if (req.max_matches_per_file > 0 && file_matches >= req.max_matches_per_file)
                    break;
            }
            if (req.max_matches_per_file > 0 && file_matches >= req.max_matches_per_file)
                break;
        }
    }

    out.stats = {
        {"files_visited", static_cast<int>(inventory.files.size())},
        {"files_scanned", scanned_files},
        {"files_skipped", skipped_files},
        {"files_binary", 0},
        {"files_sensitive", 0},
        {"matches", static_cast<int>(out.hits.size())},
        {"total_matches", total_matches},
        {"applied_limit", limit},
        {"applied_offset", offset},
        {"direct_adapter", true},
    };
    return out;
}

bool name_matches_leaf(std::string_view name, const Request& req)
{
    if (name.empty() || req.query.empty())
        return false;
    if (req.regex) {
        try {
            const auto flags = req.case_sensitive
                ? std::regex::ECMAScript
                : (std::regex::ECMAScript | std::regex::icase);
            const std::regex re(req.query, flags);
            return std::regex_search(std::string(name), re);
        } catch (...) {
            return false;
        }
    }
    auto fold = [](std::string_view text, bool sensitive) {
        std::string out(text);
        if (!sensitive) {
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    const std::string hay = fold(name, req.case_sensitive);
    const std::string needle = fold(req.query, req.case_sensitive);
    if (req.whole_word) {
        if (hay.size() < needle.size())
            return false;
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            if (hay.compare(i, needle.size(), needle) != 0)
                continue;
            const bool left = i == 0
                || !std::isalnum(static_cast<unsigned char>(hay[i - 1]));
            const bool right = i + needle.size() == hay.size()
                || !std::isalnum(static_cast<unsigned char>(
                    hay[i + needle.size()]));
            if (left && right)
                return true;
        }
        return false;
    }
    return hay.find(needle) != std::string::npos;
}

Result run_remote_name(const Request& req, Backend backend, Mode mode)
{
    Result out;
    out.backend = backend;
    out.mode = mode;
    out.content = req.content;
    const int limit = req.head_limit > 0
        ? req.head_limit
        : (req.limit > 0 ? req.limit : 200);

    for (const std::string& input : req.inputs) {
        if (!host::is_remote_uri(input))
            continue;
        const auto collected = host::collect_remote(input);
        if (!collected.error.empty() && collected.nodes.empty()) {
            if (out.error.empty())
                out.error = collected.error;
            continue;
        }
        for (const auto& node : collected.nodes) {
            if (req.cancel_requested && req.cancel_requested())
                break;
            const auto slash = node.src.find_last_of("/\\");
            const std::string leaf = slash == std::string::npos ? node.src : node.src.substr(slash + 1);
            if (leaf.empty() || leaf == "." || leaf == "..")
                continue;
            if (name_matches_leaf(leaf, req)) {
                if (node.src == input)
                    continue;
                Hit hit;
                hit.path = node.src;
                hit.title = leaf;
                hit.source = "remote-name";
                hit.kind = node.is_dir ? "folder" : "file";
                hit.metadata["isDir"] = node.is_dir;
                out.hits.push_back(std::move(hit));
                if (limit > 0 && static_cast<int>(out.hits.size()) >= limit)
                    break;
            }
        }
        if (limit > 0 && static_cast<int>(out.hits.size()) >= limit)
            break;
    }
    out.ok = true;
    out.stats = {
        {"matches", static_cast<int>(out.hits.size())},
        {"remote", true},
    };
    return out;
}

bool request_has_remote_input(const Request& req)
{
    for (const std::string& input : req.inputs) {
        if (host::is_remote_uri(input))
            return true;
    }
    return false;
}

Result run_exact(const Request& req, Backend backend, Mode mode)
{
    if (request_has_remote_input(req)) {
        if (mode == Mode::Grep) {
            Result skipped;
            skipped.ok = true;
            skipped.backend = backend;
            skipped.mode = mode;
            skipped.content = req.content;
            skipped.stats = {{"remote", true}, {"skipped", "grep"}};
            return skipped;
        }
        host::log_info("search: remote name walk query=" + req.query);
        return run_remote_name(req, backend, mode);
    }

    // rg-backed exact search.
    // When host::rg_file_enabled() is true, rg is used automatically for name/grep
    // unless the caller explicitly opted out with exact_indexer="own" or "os".
    // Passing exact_indexer="rg" forces rg regardless of host::rg_file_enabled().
    // exact_indexer="" means auto: use rg when host::rg_file_enabled(), else own.
    // exact_indexer="rg" forces rg regardless of host::rg_file_enabled().
    // exact_indexer="own"/"os" explicit opt-out — never use rg.
    const bool query_has_glob = req.query.find('*') != std::string::npos
                             || req.query.find('?') != std::string::npos;
    // Fuzzy name scoring reads every filename the own walker emits. rg --files
    // never emits a file under an ignored directory, and a positive -g does
    // not walk into that directory. A wildcard query stays on the exact path.
    const bool use_name_fuzzy = req.fuzzy_names
                             && mode == Mode::Name
                             && !req.regex
                             && !query_has_glob;
    const bool want_rg = !use_name_fuzzy
                      && (eq(req.exact_indexer, "rg")
                          || (host::rg_file_enabled() && req.exact_indexer.empty()));
    if (want_rg) {
        if (mode == Mode::Name) {
            host::log_trace("rg: name search query=" + req.query);
            return run_rg_file_name(req, backend, mode);
        }
        if (mode == Mode::Grep) {
            host::log_trace("rg: grep search query=" + req.query);
            return run_rg_grep(req, backend, mode);
        }
    }

    host::log_trace("own: exact search mode=" + std::string(to_string(mode))
                  + " query=" + req.query);

    if (should_use_adapter_exact_grep(req, mode))
        return run_adapter_exact_grep(req, backend, mode);

    host::OwnQuery opts;
    opts.query = req.query;
    opts.is_regex = req.regex;
    opts.case_sensitive = req.case_sensitive;
    opts.whole_word = req.whole_word;
    opts.image_only = req.content == Content::Image;
    opts.grep = mode == Mode::Grep;
    opts.names_only = req.names_only;
    opts.name_fuzzy = use_name_fuzzy;
    opts.name_fuzzy_limit = req.fuzzy_limit;
    opts.recursive = req.recursive;
    opts.include_hidden = req.include_hidden;
    opts.follow_symlinks = req.follow_symlinks;
    opts.respect_gitignore = req.respect_gitignore;
    opts.include_globs = req.include_globs;
    opts.exclude_globs = req.exclude_globs;
    opts.exclude_dirs = req.exclude_dirs;
    opts.skip_binary = req.skip_binary;
    if (req.max_file_size_bytes > 0)
        opts.max_file_size_bytes = req.max_file_size_bytes;
    if (req.max_matches_per_file > 0)
        opts.max_matches_per_file = req.max_matches_per_file;
    opts.context_lines = req.context_lines;
    opts.context_before_lines = req.context_before;
    opts.context_after_lines = req.context_after;
    opts.multiline = req.multiline;
    opts.type_filter = req.type_filter;
    opts.use_os_indexer = !use_name_fuzzy && eq(req.exact_indexer, "os");
    opts.exclude_sensitive = true;
    opts.output_mode = exact_output_mode(req.output_mode);
    opts.max_results = req.limit;
    opts.head_limit = req.head_limit;
    opts.offset = req.offset;
    opts.dry_run = req.dry_run;
    opts.cancel_requested = req.cancel_requested;

    host::OwnResult sr = host::search_own(req.inputs, opts);

    Result out;
    out.ok = sr.ok;
    out.error = sr.error;
    out.backend = backend;
    out.mode = mode;
    out.content = req.content;
    out.stats = {
        {"files_visited", sr.stats.files_visited},
        {"files_scanned", sr.stats.files_scanned},
        {"files_skipped", sr.stats.files_skipped},
        {"files_binary", sr.stats.files_binary},
        {"files_sensitive", sr.stats.files_sensitive},
        {"matches", sr.stats.matches},
        {"truncated", sr.truncated},
        {"name_fuzzy", opts.name_fuzzy},
        {"total_matches", sr.total_matches},
        {"applied_limit", sr.applied_limit},
        {"applied_offset", sr.applied_offset},
    };

    out.hits.reserve(sr.matches.size());
    for (const auto& m : sr.matches) {
        Hit h;
        h.path = m.path;
        h.snippet = m.preview;
        h.source = m.source;
        h.kind = to_string(req.content);
        h.line = m.line;
        h.column = m.column;
        h.score = m.score;
        if (m.line > 0 && m.column > 0
            && m.column - 1 <= (std::numeric_limits<std::size_t>::max)()
            && m.match_length <= (std::numeric_limits<std::size_t>::max)()) {
            const auto byte_start = static_cast<std::size_t>(m.column - 1);
            const auto byte_length = static_cast<std::size_t>(m.match_length);
            if (byte_start <= m.matched_line.size()
                && byte_length <= m.matched_line.size() - byte_start) {
                if (auto range = host::utf16_range(
                        m.matched_line, byte_start, byte_start + byte_length)) {
                    if (auto normalized = offset_range_lines(*range, m.line)) {
                        set_primary_range(h, *normalized);
                        h.ranges.push_back(*normalized);
                        if (byte_length > 0 && byte_length <= k_max_anchor_bytes)
                            h.metadata["matched_text"] = m.matched_line.substr(byte_start, byte_length);
                    }
                }
            }
        }
        if (!m.context_before.empty())
            h.metadata["context_before"] = m.context_before;
        if (!m.context_after.empty())
            h.metadata["context_after"] = m.context_after;
        out.hits.push_back(std::move(h));
    }
    return out;
}

std::string first_office_row_path_from_snippet(const std::string& snippet)
{
    std::size_t pos = 0;
    while ((pos = snippet.find("[/", pos)) != std::string::npos) {
        const std::size_t end = snippet.find("]]", pos + 2);
        if (end == std::string::npos)
            return {};
        std::string candidate = snippet.substr(pos + 1, end - pos);
        if (candidate.find("/row[") != std::string::npos)
            return candidate;
        pos = end + 2;
    }
    return {};
}

void attach_details_to_hits(const Request& req, Result& result)
{
    if (req.include_detail.empty())
        return;

    int detail_index = 0;
    int expanded_count = 0;
    for (Hit& hit : result.hits) {
        ++detail_index;
        if (req.detail_max > 0 && expanded_count >= req.detail_max) {
            hit.metadata["detail_skipped"] = "detail-max";
            continue;
        }
        DetailRequest detail_req;
        detail_req.include_detail = req.include_detail;
        detail_req.context_before = req.context_before;
        detail_req.context_after = req.context_after;

        const std::string row_path = first_office_row_path_from_snippet(hit.snippet);
        if (!row_path.empty())
            detail_req.office_path = row_path;

        std::string detail_path = hit.path;
        std::string adapter_id;
        if (hit.metadata.contains("adapter") && hit.metadata["adapter"].is_string())
            adapter_id = hit.metadata["adapter"].get<std::string>();
        if (adapter_id.empty()
            && hit.metadata.contains("facts") && hit.metadata["facts"].is_object()
            && hit.metadata["facts"].contains("adapter") && hit.metadata["facts"]["adapter"].is_string()) {
            adapter_id = hit.metadata["facts"]["adapter"].get<std::string>();
        }
        if (hit.metadata.contains("locator") && hit.metadata["locator"].is_object()) {
            const auto& locator = hit.metadata["locator"];
            if (adapter_id.empty() && locator.contains("adapter") && locator["adapter"].is_string())
                adapter_id = locator["adapter"].get<std::string>();
            if (locator.contains("path") && locator["path"].is_string())
                detail_path = locator["path"].get<std::string>();
        }

        if (hit.metadata.contains("__indexed_detail_text")
            && hit.metadata["__indexed_detail_text"].is_string()) {
            const std::string detail_text = hit.metadata["__indexed_detail_text"].get<std::string>();
            hit.metadata.erase("__indexed_detail_text");
            nlohmann::json locator = hit.metadata.value("locator", nlohmann::json::object());
            if (!locator.is_object())
                locator = nlohmann::json::object();
            if (!locator.contains("path"))
                locator["path"] = detail_path;
            if (!adapter_id.empty() && !locator.contains("adapter"))
                locator["adapter"] = adapter_id;
            locator["includeDetail"] = detail_req.include_detail;

            nlohmann::json detail_json = {
                {"adapter", adapter_id},
                {"text", detail_text},
                {"locator", std::move(locator)},
                {"source", "index"},
            };
            if (req.include_detail == "metadata"
                && hit.metadata.contains("facts") && hit.metadata["facts"].is_object())
                detail_json["data"] = hit.metadata["facts"];
            hit.metadata["detail"] = std::move(detail_json);
            ++expanded_count;
            host::log_debug("search detail inline: indexed hit=" + std::to_string(detail_index)
                          + " text_bytes=" + std::to_string(detail_text.size()));
            continue;
        }

        host::log_debug("search detail inline: begin hit=" + std::to_string(detail_index)
                      + " path=" + detail_path
                      + " adapter=" + adapter_id
                      + " include=" + detail_req.include_detail
                      + " office_path=" + detail_req.office_path);
        SourceDetail detail = unified_detail(detail_path, adapter_id, detail_req);
        if (!detail.ok) {
            host::log_debug("search detail inline: error hit=" + std::to_string(detail_index)
                          + " error=" + detail.error);
            hit.metadata["detail_error"] = detail.error;
            continue;
        }
        nlohmann::json detail_json = {
            {"adapter", detail.adapter_id},
            {"text", detail.text},
            {"locator", detail.locator},
        };
        if (req.include_detail == "metadata" && !detail.data.empty())
            detail_json["data"] = detail.data;
        hit.metadata["detail"] = std::move(detail_json);
        ++expanded_count;
        host::log_debug("search detail inline: done hit=" + std::to_string(detail_index)
                      + " text_bytes=" + std::to_string(detail.text.size()));
    }
}

void sort_hits(std::vector<Hit>& hits, SortOrder sort)
{
    switch (sort) {
    case SortOrder::Score:
        std::stable_sort(hits.begin(), hits.end(),
                         [](const Hit& a, const Hit& b) { return a.score > b.score; });
        break;
    case SortOrder::Path:
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            if (a.path != b.path)
                return a.path < b.path;
            if (a.line != b.line)
                return a.line < b.line;
            return a.column < b.column;
        });
        break;
    case SortOrder::MTimeDesc:
    case SortOrder::MTimeAsc:
    {
        std::unordered_map<std::string, std::int64_t> mtimes;
        mtimes.reserve(hits.size());
        for (const Hit& hit : hits)
            mtimes.try_emplace(hit.path, file_mtime_sort_tick(hit.path));

        const bool ascending = sort == SortOrder::MTimeAsc;
        std::stable_sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
            const auto at = mtimes.find(a.path);
            const auto bt = mtimes.find(b.path);
            const std::int64_t av = at != mtimes.end() ? at->second : 0;
            const std::int64_t bv = bt != mtimes.end() ? bt->second : 0;
            if (av != bv)
                return ascending ? av < bv : av > bv;
            if (a.path != b.path)
                return a.path < b.path;
            if (a.line != b.line)
                return a.line < b.line;
            return a.column < b.column;
        });
        break;
    }
    case SortOrder::Auto:
    case SortOrder::None:
    default:
        break;
    }
}

// Runs exactly one (content, mode, backend) task. The request's single-valued
// content/mode/backend describe the task; multi-valued vectors are ignored here.
// Detail expansion and result sorting are applied by the caller so they compose
// correctly across a fan-out.
Result run_single_search(const Request& req)
{
    const Backend backend = effective_backend(req);
    const Mode mode = effective_mode(req, backend);

    if (backend == Backend::Exact) {
        if (mode != Mode::Name && mode != Mode::Grep)
            return unsupported(req, backend, mode, "exact supports name/grep only");
        return run_exact(req, backend, mode);
    }
    if (backend == Backend::ImageMeta)
        return unsupported(req, backend, mode, "image-meta");
    if (backend == Backend::Vector) {
        if (mode != Mode::Semantic)
            return unsupported(req, backend, mode, "vector supports semantic only");
        return search_vector(req, backend, mode);
    }
    if (backend == Backend::Fingerprint) {
        if (mode != Mode::Fingerprint)
            return unsupported(req, backend, mode, "fingerprint supports fingerprint mode only");
        return search_fingerprints(req);
    }
    if (backend == Backend::Catalog)
        return unsupported(req, backend, mode, "catalog");
    return unsupported(req, backend, mode, "auto");
}

} // namespace

FileGlobCount count_file_globs(
    const std::string& folder,
    const std::vector<std::string>& globs,
    bool recursive,
    const std::function<bool()>& cancel_requested)
{
    FileGlobCount out;
    const std::filesystem::path rg_bin = resolve_rg();
    if (rg_bin.empty()) {
        out.error = "rg not found";
        return out;
    }
    if (folder.empty()) {
        out.error = "folder is empty";
        return out;
    }

    auto strip_starstar = [](std::string g) {
        while (g.size() >= 3
            && (g.compare(0, 3, "**/") == 0 || g.compare(0, 3, "**\\") == 0)) {
            g.erase(0, 3);
        }
        return g.empty() ? std::string("*.*") : g;
    };

    bool walk = recursive;
    for (const auto& glob : globs) {
        if (glob.find("**") != std::string::npos)
            walk = true;
    }

    std::vector<std::string> args;
    args.push_back("--files");
    // File-panel count must see on-disk files, including gitignored
    // folders such as `test/` in this repo's .gitignore.
    args.push_back("--no-ignore");
    args.push_back("--no-ignore-vcs");
    args.push_back("--no-ignore-parent");
    if (!walk)
        args.push_back("--max-depth=1");
    args.push_back("--color=never");
    if (globs.empty()) {
        args.push_back("-g");
        args.push_back("*.*");
    } else {
        for (const auto& glob : globs) {
            if (glob.empty())
                continue;
            args.push_back("-g");
            args.push_back(strip_starstar(glob));
        }
    }
    args.push_back(folder);

    std::atomic<int> matches{0};
    std::atomic<bool> stop{false};
    std::string line_buf;

    host::ProcessRun opts;
    opts.executable = media::path::path_to_utf8(rg_bin);
    opts.args = std::move(args);
    opts.timeout_ms = 8'000;
    opts.bind_to_parent = true;
    opts.capture_max_bytes = 4 * 1024 * 1024;
    opts.on_cancel_check = [&]() -> bool {
        return stop.load(std::memory_order_relaxed)
            || (cancel_requested && cancel_requested());
    };
    opts.on_output_chunk = [&](const std::string& stream, const std::string& chunk) {
        if (stream != "stdout" || stop.load(std::memory_order_relaxed))
            return;
        line_buf += chunk;
        std::size_t pos = 0;
        while (true) {
            const std::size_t nl = line_buf.find('\n', pos);
            if (nl == std::string::npos)
                break;
            std::string line = line_buf.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            pos = nl + 1;
            if (!line.empty())
                matches.fetch_add(1, std::memory_order_relaxed);
        }
        if (pos > 0)
            line_buf.erase(0, pos);
    };

    const host::ProcessResult rr = host::run_process(opts);
    if (!line_buf.empty()) {
        while (!line_buf.empty()
            && (line_buf.back() == '\n' || line_buf.back() == '\r')) {
            line_buf.pop_back();
        }
        if (!line_buf.empty())
            matches.fetch_add(1, std::memory_order_relaxed);
    }

    out.used_rg = true;
    out.matches = matches.load(std::memory_order_relaxed);
    out.ok = rr.ok || rr.exit_code == 0 || rr.exit_code == 1;
    if (!out.ok) {
        out.error = rr.error.empty()
            ? "rg failed (exit " + std::to_string(rr.exit_code) + ")"
            : rr.error;
    }
    return out;
}

const char* to_string(Content value) noexcept
{
    switch (value) {
    case Content::Any: return "any";
    case Content::Text: return "text";
    case Content::Code: return "code";
    case Content::Image: return "image";
    case Content::Document: return "document";
    case Content::Office: return "office";
    case Content::Pdf: return "pdf";
    case Content::Catalog: return "catalog";
    }
    return "any";
}

const char* to_string(Mode value) noexcept
{
    switch (value) {
    case Mode::Auto: return "auto";
    case Mode::Name: return "name";
    case Mode::Grep: return "grep";
    case Mode::Semantic: return "semantic";
    case Mode::Metadata: return "metadata";
    case Mode::Fingerprint: return "fingerprint";
    }
    return "auto";
}

const char* to_string(Backend value) noexcept
{
    switch (value) {
    case Backend::Auto: return "auto";
    case Backend::Exact: return "exact";
    case Backend::ImageMeta: return "image-meta";
    case Backend::Vector: return "vector";
    case Backend::Fingerprint: return "fingerprint";
    case Backend::Catalog: return "catalog";
    }
    return "auto";
}

const char* to_string(IndexPolicy value) noexcept
{
    switch (value) {
    case IndexPolicy::SameDirDefault: return "same-dir";
    case IndexPolicy::SpecificDir: return "specific-dir";
    case IndexPolicy::Disabled: return "disabled";
    }
    return "same-dir";
}

const char* to_string(SortOrder value) noexcept
{
    switch (value) {
    case SortOrder::Auto: return "auto";
    case SortOrder::None: return "none";
    case SortOrder::Score: return "score";
    case SortOrder::Path: return "path";
    case SortOrder::MTimeDesc: return "mtime_desc";
    case SortOrder::MTimeAsc: return "mtime_asc";
    }
    return "auto";
}

const char* to_string(ActionKind value) noexcept
{
    switch (value) {
    case ActionKind::None: return "none";
    case ActionKind::Replace: return "replace";
    }
    return "none";
}

bool parse_content(const std::string& text, Content& out) noexcept
{
    const std::string v = lower_ascii(text);
    if (v == "any") { out = Content::Any; return true; }
    if (v == "text") { out = Content::Text; return true; }
    if (v == "code") { out = Content::Code; return true; }
    if (v == "image") { out = Content::Image; return true; }
    if (v == "document" || v == "doc" || v == "docs") { out = Content::Document; return true; }
    if (v == "office") { out = Content::Office; return true; }
    if (v == "pdf") { out = Content::Pdf; return true; }
    if (v == "catalog") { out = Content::Catalog; return true; }
    return false;
}

bool parse_mode(const std::string& text, Mode& out) noexcept
{
    const std::string v = lower_ascii(text);
    if (v == "auto") { out = Mode::Auto; return true; }
    if (v == "name") { out = Mode::Name; return true; }
    if (v == "grep") { out = Mode::Grep; return true; }
    if (v == "semantic") { out = Mode::Semantic; return true; }
    if (v == "metadata") { out = Mode::Metadata; return true; }
    if (v == "fingerprint" || v == "fingerprints") { out = Mode::Fingerprint; return true; }
    return false;
}

bool parse_backend(const std::string& text, Backend& out) noexcept
{
    const std::string v = lower_ascii(text);
    if (v == "auto" || v == "any") { out = Backend::Auto; return true; }
    if (v == "exact" || v == "own" || v == "os") { out = Backend::Exact; return true; }
    if (v == "image-meta" || v == "image_meta") { out = Backend::ImageMeta; return true; }
    if (v == "vector") { out = Backend::Vector; return true; }
    if (v == "fingerprint" || v == "fingerprints") { out = Backend::Fingerprint; return true; }
    if (v == "catalog") { out = Backend::Catalog; return true; }
    return false;
}

bool parse_index_policy(const std::string& text, IndexPolicy& out) noexcept
{
    const std::string v = lower_ascii(text);
    if (v == "same-dir" || v == "same_dir" || v == "default") { out = IndexPolicy::SameDirDefault; return true; }
    if (v == "specific-dir" || v == "specific_dir" || v == "dir") { out = IndexPolicy::SpecificDir; return true; }
    if (v == "disabled" || v == "none" || v == "off") { out = IndexPolicy::Disabled; return true; }
    return false;
}

bool parse_sort(const std::string& text, SortOrder& out) noexcept
{
    const std::string v = lower_ascii(text);
    if (v.empty() || v == "auto") { out = SortOrder::Auto; return true; }
    if (v == "none" || v == "off" || v == "order") { out = SortOrder::None; return true; }
    if (v == "score" || v == "relevance" || v == "rank") { out = SortOrder::Score; return true; }
    if (v == "path" || v == "name") { out = SortOrder::Path; return true; }
    if (v == "mtime_desc" || v == "mtime-desc" || v == "modified" || v == "modified_desc" || v == "newest") {
        out = SortOrder::MTimeDesc;
        return true;
    }
    if (v == "mtime_asc" || v == "mtime-asc" || v == "modified_asc" || v == "oldest") {
        out = SortOrder::MTimeAsc;
        return true;
    }
    return false;
}

bool parse_action_kind(const std::string& text, ActionKind& out) noexcept
{
    const std::string v = lower_ascii(text);
    if (v.empty() || v == "none") { out = ActionKind::None; return true; }
    if (v == "replace") { out = ActionKind::Replace; return true; }
    return false;
}

namespace {

std::string json_string_any(const nlohmann::json& j, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        if (j.contains(key) && j[key].is_string())
            return j[key].get<std::string>();
    }
    return {};
}

bool json_bool_any(const nlohmann::json& j, std::initializer_list<const char*> keys, bool fallback)
{
    for (const char* key : keys) {
        if (j.contains(key) && j[key].is_boolean())
            return j[key].get<bool>();
    }
    return fallback;
}

template <typename T>
bool json_int_any(const nlohmann::json& j, std::initializer_list<const char*> keys, T& out)
{
    for (const char* key : keys) {
        if (j.contains(key) && j[key].is_number_integer()) {
            out = static_cast<T>(j[key].get<long long>());
            return true;
        }
    }
    return false;
}

void append_string_values(const nlohmann::json& j, std::initializer_list<const char*> keys, std::vector<std::string>& out)
{
    for (const char* key : keys) {
        if (!j.contains(key))
            continue;
        const auto& value = j[key];
        if (value.is_string()) {
            out.push_back(value.get<std::string>());
        } else if (value.is_array()) {
            for (const auto& item : value) {
                if (item.is_string())
                    out.push_back(item.get<std::string>());
            }
        }
    }
}

template <typename Enum, typename ParseFn>
bool parse_enum_list(const nlohmann::json& j,
                     std::initializer_list<const char*> keys,
                     ParseFn&& parse_fn,
                     std::vector<Enum>& out,
                     std::string* error)
{
    std::vector<std::string> raw;
    append_string_values(j, keys, raw);
    for (const std::string& value : raw) {
        Enum e{};
        if (!parse_fn(value, e)) {
            if (error)
                *error = std::string("invalid value for ") + *keys.begin() + ": " + value;
            return false;
        }
        out.push_back(e);
    }
    return true;
}

} // namespace

bool request_from_json(const nlohmann::json& j, Request& out, std::string* error) noexcept
{
    try {
        out = Request{};
        if (error)
            error->clear();

        if (!j.is_object()) {
            if (error)
                *error = "request must be a JSON object";
            return false;
        }

        out.run_name = json_string_any(j, {"runName", "searchName", "indexName", "name"});
        if (j.contains("user") && j["user"].is_object())
            out.user = j["user"];
        if (!out.run_name.empty() && !out.user.contains("runName"))
            out.user["runName"] = out.run_name;

        out.query = json_string_any(j, {"query", "q"});
        append_string_values(j, {"inputs", "input", "paths"}, out.inputs);

        {
            const std::string content = json_string_any(j, {"content"});
            if (!content.empty() && !parse_content(content, out.content)) {
                if (error) *error = "invalid content: " + content;
                return false;
            }
        }
        {
            const std::string mode = json_string_any(j, {"mode"});
            if (!mode.empty() && !parse_mode(mode, out.mode)) {
                if (error) *error = "invalid mode: " + mode;
                return false;
            }
        }
        {
            const std::string backend = json_string_any(j, {"backend"});
            if (!backend.empty() && !parse_backend(backend, out.backend)) {
                if (error) *error = "invalid backend: " + backend;
                return false;
            }
        }
        {
            const std::string sort = json_string_any(j, {"sort"});
            if (!sort.empty() && !parse_sort(sort, out.sort)) {
                if (error) *error = "invalid sort: " + sort;
                return false;
            }
        }
        {
            const std::string policy = json_string_any(j, {"indexPolicy", "index_policy"});
            if (!policy.empty() && !parse_index_policy(policy, out.index_policy)) {
                if (error) *error = "invalid index policy: " + policy;
                return false;
            }
        }
        {
            const std::string action = json_string_any(j, {"action"});
            if (!action.empty() && !parse_action_kind(action, out.action)) {
                if (error) *error = "invalid action: " + action;
                return false;
            }
        }

        if (!parse_enum_list<Content>(j, {"contents"},
                                      [](const std::string& s, Content& c) { return parse_content(s, c); },
                                      out.contents, error))
            return false;
        if (!parse_enum_list<Mode>(j, {"modes"},
                                   [](const std::string& s, Mode& m) { return parse_mode(s, m); },
                                   out.modes, error))
            return false;
        if (!parse_enum_list<Backend>(j, {"backends"},
                                      [](const std::string& s, Backend& b) { return parse_backend(s, b); },
                                      out.backends, error))
            return false;

        append_string_values(j, {"includeGlobs", "include_globs", "include"}, out.include_globs);
        append_string_values(j, {"excludeGlobs", "exclude_globs", "exclude"}, out.exclude_globs);
        append_string_values(j, {"excludeDirs", "exclude_dirs"}, out.exclude_dirs);
        append_string_values(j, {"indexDirs", "index_dirs", "indexes"}, out.index_dirs);
        append_string_values(j, {"referenceImages", "reference_images"}, out.reference_images);

        out.type_filter = json_string_any(j, {"typeFilter", "type_filter", "type"});
        out.exact_indexer = json_string_any(j, {"exactIndexer", "exact_indexer", "indexer"});
        // Leave empty when not specified — run_exact treats empty as "auto"
        // (uses rg when host::rg_file_enabled(), else own). Only force "own" when the
        // caller explicitly sent that value.
        out.output_mode = json_string_any(j, {"outputMode", "output_mode"});
        if (out.output_mode.empty())
            out.output_mode = "content";
        out.index_dir = json_string_any(j, {"indexDir", "index_dir", "index"});
        out.model = json_string_any(j, {"model"});
        out.extract = json_string_any(j, {"extract"});
        if (out.extract.empty())
            out.extract = "auto";
        out.adapter = json_string_any(j, {"adapter"});
        out.fingerprint = json_string_any(j, {"fingerprint"});
        if (out.fingerprint.empty())
            out.fingerprint = "off";
        out.include_detail = json_string_any(j, {"includeDetail", "include_detail"});
        out.replacement = json_string_any(j, {"replacement"});

        out.recursive = json_bool_any(j, {"recursive"}, true);
        out.include_hidden = json_bool_any(j, {"includeHidden", "include_hidden"}, false);
        out.respect_gitignore = json_bool_any(
            j, {"respectGitignore", "respect_gitignore"}, true);
        out.follow_symlinks = json_bool_any(j, {"followSymlinks", "follow_symlinks"}, false);
        out.case_sensitive = json_bool_any(j, {"caseSensitive", "case_sensitive"}, false);
        out.regex = json_bool_any(j, {"regex"}, false);
        out.whole_word = json_bool_any(j, {"wholeWord", "whole_word"}, false);
        out.names_only = json_bool_any(j, {"namesOnly", "names_only"}, false);
        out.fuzzy_names = json_bool_any(j, {"fuzzyNames", "fuzzy_names"}, false);
        if (j.contains("fuzzyLimit") && j["fuzzyLimit"].is_number_integer())
            out.fuzzy_limit = j["fuzzyLimit"].get<int>();
        else if (j.contains("fuzzy_limit") && j["fuzzy_limit"].is_number_integer())
            out.fuzzy_limit = j["fuzzy_limit"].get<int>();
        out.skip_binary = json_bool_any(j, {"skipBinary", "skip_binary"}, true);
        out.multiline = json_bool_any(j, {"multiline"}, false);
        out.dry_run = json_bool_any(j, {"dryRun", "dry_run"}, false);
        out.reindex = json_bool_any(j, {"reindex"}, false);
        out.create_index = json_bool_any(j, {"createIndex", "create_index"}, true);

        (void)json_int_any(j, {"limit", "maxResults", "max_results"}, out.limit);
        (void)json_int_any(j, {"headLimit", "head_limit"}, out.head_limit);
        (void)json_int_any(j, {"offset"}, out.offset);
        (void)json_int_any(j, {"contextLines", "context_lines", "context"}, out.context_lines);
        (void)json_int_any(j, {"contextBefore", "context_before"}, out.context_before);
        (void)json_int_any(j, {"contextAfter", "context_after"}, out.context_after);
        (void)json_int_any(j, {"threads"}, out.threads);
        if (out.threads <= 0)
            out.threads = 4;
        (void)json_int_any(j, {"gpuLayers", "gpu_layers"}, out.gpu_layers);
        (void)json_int_any(j, {"chunkSize", "chunk_size"}, out.chunk_size);
        (void)json_int_any(j, {"chunkOverlap", "chunk_overlap"}, out.chunk_overlap);
        (void)json_int_any(j, {"detailMax", "detail_max"}, out.detail_max);

        long long i64 = 0;
        if (json_int_any(j, {"maxFileSize", "max_file_size", "max_file_size_bytes"}, i64) && i64 > 0)
            out.max_file_size_bytes = static_cast<std::uint64_t>(i64);
        if (json_int_any(j, {"maxMatchesPerFile", "max_matches_per_file", "max_per_file"}, i64) && i64 > 0)
            out.max_matches_per_file = static_cast<std::uint32_t>(i64);

        if (j.contains("adapterOptions") && j["adapterOptions"].is_object())
            out.adapter_options = j["adapterOptions"];
        else if (j.contains("adapter_options") && j["adapter_options"].is_object())
            out.adapter_options = j["adapter_options"];

        if (out.index_dirs.empty() && !out.index_dir.empty())
            out.index_dirs.push_back(out.index_dir);

        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return false;
    } catch (...) {
        if (error)
            *error = "unknown error while parsing request json";
        return false;
    }
}

Result unified_search(const Request& req)
{
    const std::vector<Content> contents =
        req.contents.empty() ? std::vector<Content>{req.content} : req.contents;
    const std::vector<Mode> modes =
        req.modes.empty() ? std::vector<Mode>{req.mode} : req.modes;
    const std::vector<Backend> backends =
        req.backends.empty() ? std::vector<Backend>{req.backend} : req.backends;

    const bool single_task =
        contents.size() == 1 && modes.size() == 1 && backends.size() == 1;

    // Fast path: one task. Behaviour is identical to the pre-fan-out facade.
    if (single_task) {
        Request one = req;
        one.content = contents.front();
        one.mode = modes.front();
        one.backend = backends.front();
        one.contents.clear();
        one.modes.clear();
        one.backends.clear();
        Result result = run_single_search(one);
        if (result.ok) {
            sort_hits(result.hits, req.sort);
            // Stream compact hits (pre-detail) before attaching inline detail to
            // the returned Result, so streaming consumers stay fast and lazy.
            if (req.on_hit) {
                for (const Hit& h : result.hits) {
                    if (!req.on_hit(h))
                        break;
                }
            }
            if (req.on_event) {
                req.on_event(nlohmann::json{
                    {"event", "task"},
                    {"content", to_string(result.content)},
                    {"mode", to_string(result.mode)},
                    {"backend", to_string(result.backend)},
                    {"matches", static_cast<int>(result.hits.size())},
                    {"ok", true},
                });
            }
            attach_details_to_hits(req, result);
        }
        if (!req.run_name.empty())
            result.stats["run_name"] = req.run_name;
        if (!req.user.empty())
            result.stats["user"] = req.user;
        return result;
    }

    // Fan-out: run the cartesian product in explicit user order, union the hits.
    Result merged;
    merged.content = contents.front();
    merged.mode = modes.front();
    merged.backend = backends.front();

    nlohmann::json tasks = nlohmann::json::array();
    std::set<std::string> ran_task_keys;
    std::set<std::string> seen_hit_keys;
    long files_visited = 0, files_scanned = 0, files_skipped = 0, files_binary = 0;
    long total_matches = 0;
    bool any_ok = false;
    bool stop_streaming = false;
    std::string last_error;

    for (Content content : contents) {
        if (stop_streaming) break;
        for (Mode mode : modes) {
            if (stop_streaming) break;
            for (Backend backend : backends) {
                if (stop_streaming) break;
                if (req.cancel_requested && req.cancel_requested()) {
                    last_error = "cancelled";
                    content = contents.back();
                    break;
                }

                Request one = req;
                one.content = content;
                one.mode = mode;
                one.backend = backend;
                one.contents.clear();
                one.modes.clear();
                one.backends.clear();

                const Backend eb = effective_backend(one);
                const Mode em = effective_mode(one, eb);
                const std::string task_key = std::string(to_string(content)) + "/"
                    + to_string(em) + "/" + to_string(eb);
                if (!ran_task_keys.insert(task_key).second)
                    continue; // identical resolved task already executed

                Result r = run_single_search(one);
                nlohmann::json info = {
                    {"content", to_string(content)},
                    {"mode", to_string(em)},
                    {"backend", to_string(eb)},
                    {"ok", r.ok},
                };
                if (!r.ok) {
                    info["error"] = r.error;
                    last_error = r.error;
                    tasks.push_back(std::move(info));
                    continue; // skip incompatible/empty task; do not fail the union
                }

                any_ok = true;
                files_visited += r.stats.value("files_visited", 0);
                files_scanned += r.stats.value("files_scanned", 0);
                files_skipped += r.stats.value("files_skipped", 0);
                files_binary += r.stats.value("files_binary", 0);
                total_matches += r.stats.value("total_matches", static_cast<int>(r.hits.size()));

                int task_added = 0;
                for (Hit& h : r.hits) {
                    std::string key = h.path + "\x1f" + std::to_string(h.line)
                        + "\x1f" + std::to_string(h.column);
                    if (h.range) {
                        key += "\x1f" + std::to_string(h.range->end.line)
                            + "\x1f" + std::to_string(h.range->end.column);
                    } else {
                        key += "\x1f" + std::to_string(h.line)
                            + "\x1f" + std::to_string(h.column);
                    }
                    if (!seen_hit_keys.insert(key).second)
                        continue;
                    if (h.content_type.empty())
                        h.content_type = to_string(content);
                    h.metadata["task"] = {
                        {"content", to_string(content)},
                        {"mode", to_string(em)},
                        {"backend", to_string(eb)},
                    };
                    // Stream in task order as hits are unioned. The final
                    // Result::hits is still re-sorted per req.sort below.
                    Hit& stored = merged.hits.emplace_back(std::move(h));
                    ++task_added;
                    if (req.on_hit && !req.on_hit(stored)) {
                        stop_streaming = true;
                        break;
                    }
                }
                info["matches"] = static_cast<int>(r.hits.size());
                info["added"] = task_added;
                if (req.on_event) {
                    nlohmann::json ev = info;
                    ev["event"] = "task";
                    req.on_event(std::move(ev));
                }
                tasks.push_back(std::move(info));
            }
        }
    }

    merged.ok = any_ok;
    if (!any_ok)
        merged.error = last_error.empty() ? "no search task produced results" : last_error;

    sort_hits(merged.hits, req.sort);
    if (req.limit > 0 && static_cast<int>(merged.hits.size()) > req.limit)
        merged.hits.resize(static_cast<std::size_t>(req.limit));

    merged.stats = {
        {"files_visited", files_visited},
        {"files_scanned", files_scanned},
        {"files_skipped", files_skipped},
        {"files_binary", files_binary},
        {"matches", static_cast<int>(merged.hits.size())},
        {"total_matches", total_matches},
        {"multi", true},
        {"tasks", std::move(tasks)},
        {"sort", to_string(req.sort)},
    };

    if (merged.ok)
        attach_details_to_hits(req, merged);
    if (!req.run_name.empty())
        merged.stats["run_name"] = req.run_name;
    if (!req.user.empty())
        merged.stats["user"] = req.user;
    return merged;
}

Result index_image_tags(const Request& req)
{
    Result out;
    out.ok = false;
    out.backend = Backend::Fingerprint;
    out.mode = Mode::Fingerprint;
    out.content = Content::Image;

    if (req.index_policy == IndexPolicy::SpecificDir && req.index_dir.empty()) {
        out.error = "--index-policy specific-dir requires --index";
        return out;
    }

    Request ingest = req;
    ingest.adapter = "image-tags";
    ingest.content = Content::Image;
    const std::filesystem::path index_dir = default_vector_index_dir(ingest);
    const TextFileInventory inventory = collect_text_files(ingest);

    ISourceAdapter* adapter = nullptr;
    if (!inventory.files.empty()) {
        SourceAdapterRegistry& registry = source_adapter_registry();
        for (ISourceAdapter* candidate : registry.adapters_for(inventory.files.front())) {
            if (candidate && std::string(candidate->id()) == "image-tags") {
                adapter = candidate;
                break;
            }
        }
        if (!adapter) {
            out.error = "image-tags adapter is not registered";
            return out;
        }
    }

    std::vector<nlohmann::json> records;
    int indexed_files = 0;
    int skipped_files = 0;
    for (const std::filesystem::path& path : inventory.files) {
        if (req.cancel_requested && req.cancel_requested()) {
            out.error = "cancelled";
            return out;
        }
        const std::vector<SourceUnit> units = adapter->extract(path, 0);
        if (units.empty()) {
            ++skipped_files;
            continue;
        }
        const std::vector<nlohmann::json> fingerprints =
            cheap_fingerprints_for_units("image-tags", units);
        records.insert(records.end(), fingerprints.begin(), fingerprints.end());
        ++indexed_files;
    }

    if (!save_fingerprints(index_dir, records)) {
        out.error = "failed to write tag fingerprint index";
        return out;
    }

    out.ok = true;
    out.stats = {
        {"indexed_files", indexed_files},
        {"indexed_chunks", static_cast<int>(records.size())},
        {"files_skipped", skipped_files},
        {"index_path", media::path::path_to_utf8(index_dir)},
        {"fingerprint_path", media::path::path_to_utf8(fingerprint_path(index_dir))},
        {"adapter", "image-tags"},
    };
    return out;
}

Result unified_index(const Request& req)
{
    if (req.adapter == "image-tags")
        return index_image_tags(req);

    const Backend backend = effective_backend(req);
    if (backend != Backend::Vector)
        return unsupported(req, backend, Mode::Semantic, "index supports vector backend only");

    Result out = index_vector(req);
    if (!req.run_name.empty())
        out.stats["run_name"] = req.run_name;
    if (!req.user.empty())
        out.stats["user"] = req.user;
    return out;
}

SourceDetail unified_detail(const std::filesystem::path& path,
                            const std::string& adapter_id,
                            const DetailRequest& req)
{
    SourceAdapterRegistry& registry = source_adapter_registry();
    ISourceAdapter* adapter = nullptr;
    if (!adapter_id.empty()) {
        for (ISourceAdapter* candidate : registry.adapters_for(path)) {
            if (candidate && adapter_id == candidate->id()) {
                adapter = candidate;
                break;
            }
        }
        if (!adapter) {
            SourceDetail out;
            out.adapter_id = adapter_id;
            out.path = media::path::path_to_utf8(path);
            out.error = "source adapter cannot handle path: " + adapter_id;
            return out;
        }
    } else {
        adapter = registry.adapter_for(path);
        if (!adapter) {
            SourceDetail out;
            out.path = media::path::path_to_utf8(path);
            out.error = "no source adapter for path";
            return out;
        }
    }
    return adapter->detail(path, req);
}

} // namespace media::search
