// serve/serve_async.h — Async serve mode (fiber-based multi-session)
#ifndef AURA_SERVE_SERVE_ASYNC_H
#define AURA_SERVE_SERVE_ASYNC_H

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace aura::serve {

// Entry point for --serve-async mode.
// Issue #4047: under Soft (AURA_SANDBOX=off / !production_defaults_active)
// main enters this path with a Soft Ready profile — same JSON-line
// protocol as --serve / production async; stderr logs Soft Ready once.
// Soft Ready async shares workspace_tree across named sessions.
// Soft (#4047 B) — both Soft sync --serve and Soft Ready --serve-async —
// aliases named sessions onto one CompilerService + shared_workspace_tree
// so orch→project bindings are visible. Production keeps per-session
// isolation. Stamp serve_cross_session_shared_ast only when measured.
// Named-session stdin routing uses json_field (spaced or tight JSON);
// stdin push wakes Waiting session fibers (fiber-wake residual fix).
// Reads JSON-line protocol from stdin in non-blocking mode,
// dispatches to session fibers, returns JSON-line results on stdout.
// num_workers: number of worker threads (0 = auto-detect).
void run_serve_async(int num_workers = 0);

// Entry point for --serve-async-bench mode.
// Loads an Aura source file and executes it in serve-async runtime,
// enabling fiber parallelism for benchmarks.
// num_workers: number of worker threads (0 = auto-detect).
void run_serve_async_bench(const std::string& file_path, int num_workers = 0);

// Issue #677: Prometheus text from active scheduler (empty if none).
std::string prometheus_scheduler_metrics();

// ── Detail helpers — exposed for unit testing (Issue #473) ────────────
//
// `detail::json_escape` follows RFC 8259 §7: every control character
// (U+0000–U+001F) is escaped as \uXXXX, in addition to the four common
// escapes (\", \\, \n, \r, \t). Without this, an unescaped control char
// in a string value lets a malicious client smuggle raw bytes that
// confuse downstream JSON consumers (JSON injection / parser ambiguity).
//
// `detail::json_field` is a minimal hand-rolled extractor for the
// flat JSON-line protocol. It is NOT a general JSON parser — it
// assumes the protocol's encoding conventions (no nested objects in
// field values, properly-escaped strings). The matcher accepts both
// `"k": "v"` and `"k":"v"` variants.
namespace detail {

    // Issue #473 §5: escape all control chars (RFC 8259 §7).
    inline std::string json_escape(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 16);
        for (auto c : s) {
            const auto u = static_cast<unsigned char>(c);
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (u < 0x20) {
                        // RFC 8259 §7: escape all U+0000–U+001F as \uXXXX
                        static constexpr char hex[] = "0123456789abcdef";
                        out += "\\u00";
                        out += hex[(u >> 4) & 0xF];
                        out += hex[u & 0xF];
                    } else {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    }

    // Extract a top-level string field from a flat JSON object.
    // Returns empty string if the field is missing.
    inline std::string json_field(std::string_view json, std::string_view field) {
        // Try with space after colon, then without
        std::string spaced = "\"" + std::string(field) + "\": \"";
        auto pos = json.find(spaced);
        std::size_t key_len = spaced.size();
        if (pos == std::string_view::npos) {
            std::string tight = "\"" + std::string(field) + "\":\"";
            pos = json.find(tight);
            key_len = tight.size();
        }
        if (pos == std::string_view::npos)
            return {};
        pos += key_len;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                auto next = json[pos + 1];
                std::size_t advance = 2; // default: skip the escape pair
                if (next == 'n')
                    val += '\n';
                else if (next == 't')
                    val += '\t';
                else if (next == 'r')
                    val += '\r';
                else if (next == '"')
                    val += '"';
                else if (next == '\\')
                    val += '\\';
                else if (next == 'u' && pos + 5 < json.size()) {
                    // \uXXXX — decode 4 hex digits (RFC 8259 §7).
                    // For BMP characters this is sufficient; surrogate pairs
                    // (U+D800–U+DFFF) are left as separate codepoints because
                    // the protocol only encodes BMP controls here.
                    auto hexv = [](char c) -> int {
                        if (c >= '0' && c <= '9')
                            return c - '0';
                        if (c >= 'a' && c <= 'f')
                            return 10 + c - 'a';
                        if (c >= 'A' && c <= 'F')
                            return 10 + c - 'A';
                        return -1;
                    };
                    int hi = hexv(json[pos + 2]);
                    int lo_h = hexv(json[pos + 3]);
                    int lo_l = hexv(json[pos + 4]);
                    int lo_r = hexv(json[pos + 5]);
                    if (hi < 0 || lo_h < 0 || lo_l < 0 || lo_r < 0) {
                        // malformed — emit literal 'u' and continue (graceful)
                        val += next;
                    } else {
                        char32_t cp = (char32_t(hi) << 12) | (char32_t(lo_h) << 8) |
                                      (char32_t(lo_l) << 4) | char32_t(lo_r);
                        // Issue #473 §5: this round-trip is the inverse of
                        // json_escape's \u00XX control-char emission. We emit
                        // the UTF-8 of the codepoint for BMP. Values <0x80 are
                        // single-byte; others are 2/3-byte UTF-8.
                        if (cp < 0x80) {
                            val.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            val.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            val.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            val.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            val.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            val.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        advance = 6; // skip \uXXXX (1 + 1 + 4)
                    }
                } else {
                    // unknown escape — emit literal 'next' char
                    val += next;
                }
                pos += advance;
            } else {
                val += json[pos++];
            }
        }
        return val;
    }

    // Issue #473 §1: 1 MiB hard cap on per-line stdin input. Lines
    // exceeding this are rejected (serve_async emits an error response
    // and discards the partial line). Compiled-in default; future work
    // could make this configurable via env or flag.
    inline constexpr std::size_t kMaxServeAsyncLineBytes = 1u << 20;

    // ── Issue #4702: exec chunk splitting ────────────────────────────────
    //
    // The serve `exec` command takes a whole chunk of Aura source. A
    // chunk with SEVERAL top-level forms used to be evaluated as ONE
    // program: the compiler lowers a multi-form program (Begin root) to
    // IR, where the top-level defines become IR-local cells that die with
    // the IR module. So `(define a1 1)\n(define b1 2)` bound nothing in
    // the session env and the NEXT exec failed with
    // `unbound variable: a1` — only function defines survived (the
    // compiler re-registers Lambda defines in the env separately).
    // Evaluating the forms one at a time makes every top-level define a
    // single-form program, which the compiler binds in the session env.
    //
    // The splitter also REJECTS unbalanced input. An unclosed `(` used to
    // be accepted silently (the parser recovered and dropped the rest of
    // the chunk), so the defines after it were swallowed with no error.

    // Length of a character literal (#\a, #\space, #\x41, …) starting at
    // `i`, or 0 when `i` does not start one. A delimiter inside a literal
    // (#\( #\) #\" #\\; #\[ #\]) is not a delimiter (Issue #4622).
    inline std::size_t character_literal_length(std::string_view src, std::size_t i) {
        if (i + 1 >= src.size() || src[i] != '#' || src[i + 1] != '\\')
            return 0;
        std::size_t p = i + 2; // past the '#' and the '\\'
        if (p >= src.size())
            return 0; // bare #\ at EOF — malformed, the lexer reports it
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F')
                return 10 + (c - 'A');
            return -1;
        };
        const char c = src[p];
        if ((c == 'x' || c == 'X') && p + 1 < src.size() && hex_val(src[p + 1]) >= 0) {
            ++p; // the 'x'
            int digits = 0;
            while (p < src.size() && digits < 6 && hex_val(src[p]) >= 0) {
                ++p;
                ++digits;
            }
            return p - i;
        }
        if (!std::isalpha(static_cast<unsigned char>(c)))
            return p + 1 - i; // single-character form
        while (p < src.size() &&
               (std::isalnum(static_cast<unsigned char>(src[p])) || src[p] == '-'))
            ++p;
        return p - i;
    }

    // Result of splitting an exec chunk into its top-level forms.
    struct FormSplit {
        std::vector<std::string> forms;
        // Empty ⇔ the chunk is balanced and every form was recovered.
        // Otherwise it is the ready-to-report diagnostic (line:col prefix
        // included) and `forms` holds nothing that may be evaluated.
        std::string error;
        bool ok() const noexcept { return error.empty(); }
    };

    // Split a chunk of Aura source into its top-level forms.
    //
    // The scan tracks delimiter depth ('(' / '['), skips string literals,
    // character literals and ';' line comments, and keeps the leading text
    // of every form so a diagnostic raised on a form keeps the line
    // number it has inside the chunk. An unmatched closer or an unclosed
    // opener is reported as an error instead of being recovered from.
    inline FormSplit split_top_level_forms(std::string_view src) {
        FormSplit out;
        std::string current;
        int depth = 0;
        bool in_string = false;
        std::uint32_t line = 1;
        std::uint32_t col = 1;
        std::uint32_t open_line = 1;
        std::uint32_t open_col = 1;

        // Advance the line/column cursor over the "count" characters that
        // start at "from" in the source.
        auto advance = [&](std::size_t from, std::size_t count) {
            for (std::size_t k = 0; k < count; ++k) {
                if (src[from + k] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
        };
        // A rejected chunk yields NO form to evaluate: the forms recovered
        // before the error are dropped so no caller can run a truncated
        // chunk by accident.
        auto at = [&](std::uint32_t l, std::uint32_t c, const char* what) {
            out.forms.clear();
            out.error = std::to_string(l) + ":" + std::to_string(c) + ": parse error: " + what;
        };
        auto push_form = [&]() {
            // Keep the leading text (line numbers stay absolute inside the
            // chunk); trim the trailing whitespace only.
            std::size_t end = current.size();
            while (end > 0 && std::isspace(static_cast<unsigned char>(current[end - 1])))
                --end;
            current.resize(end);
            bool blank = true;
            for (char c : current) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    blank = false;
                    break;
                }
            }
            if (!blank)
                out.forms.push_back(current);
            current.clear();
        };

        for (std::size_t i = 0; i < src.size();) {
            const char c = src[i];
            if (in_string) {
                current += c;
                if (c == '\\' && i + 1 < src.size()) {
                    current += src[i + 1];
                    advance(i, 2);
                    i += 2;
                    continue;
                }
                if (c == '"')
                    in_string = false;
                advance(i, 1);
                ++i;
                continue;
            }
            if (c == '#') {
                const std::size_t lit = character_literal_length(src, i);
                if (lit > 0) {
                    current.append(src, i, lit);
                    advance(i, lit);
                    i += lit;
                    continue;
                }
            }
            if (c == ';') {
                // Line comment: the text is dropped, the newline stays so
                // the line numbers of the following forms are unchanged.
                if (depth > 0)
                    current += ';';
                std::size_t j = i + 1;
                while (j < src.size() && src[j] != '\n')
                    ++j;
                advance(i, j - i);
                i = j;
                continue;
            }
            if (c == '"') {
                current += c;
                in_string = true;
                advance(i, 1);
                ++i;
                continue;
            }
            if (c == '(' || c == '[') {
                if (depth == 0 && !current.empty()) {
                    // A new form starts here. A leftover that is only a
                    // prefix char (' ` ,) belongs to the new form.
                    bool is_prefix = false;
                    bool only_prefix = true;
                    for (char pc : current) {
                        if (pc == '\'' || pc == '`' || pc == ',') {
                            is_prefix = true;
                            continue;
                        }
                        if (!std::isspace(static_cast<unsigned char>(pc))) {
                            only_prefix = false;
                            break;
                        }
                    }
                    if (!only_prefix || !is_prefix)
                        push_form();
                }
                if (depth == 0) {
                    open_line = line;
                    open_col = col;
                }
                current += c;
                ++depth;
                advance(i, 1);
                ++i;
                continue;
            }
            if (c == ')' || c == ']') {
                if (depth == 0) {
                    at(line, col, c == ')' ? "unexpected ')'" : "unexpected ']'");
                    return out;
                }
                current += c;
                --depth;
                advance(i, 1);
                ++i;
                if (depth == 0)
                    push_form();
                continue;
            }
            current += c;
            advance(i, 1);
            ++i;
        }
        if (in_string) {
            at(line, col, "unterminated string literal");
            return out;
        }
        if (depth > 0) {
            at(open_line, open_col, "unclosed '(' (missing ')')");
            return out;
        }
        // Trailing text that is not a parenthesised form (a bare atom).
        if (!current.empty())
            push_form();
        return out;
    }

    // Evaluate the top-level forms of an exec chunk in order.
    //
    // `eval_one(form)` must return a result type R that converts to bool
    // (std::expected-like): false means the form failed and R carries the
    // error. The FIRST failure aborts the sequence and its error is
    // returned; otherwise the LAST form's result is returned (the chunk
    // result). An empty form list returns std::nullopt — the caller
    // reports "no complete form".
    template <typename EvalOne>
    auto eval_top_level_forms(const std::vector<std::string>& forms, EvalOne&& eval_one)
        -> std::optional<std::invoke_result_t<EvalOne, std::string_view>> {
        using R = std::invoke_result_t<EvalOne, std::string_view>;
        std::optional<R> last;
        for (const auto& form : forms) {
            R r = eval_one(std::string_view(form));
            if (!r)
                return std::optional<R>(std::move(r));
            last = std::move(r);
        }
        return last;
    }

} // namespace detail

} // namespace aura::serve

#endif // AURA_SERVE_SERVE_ASYNC_H
