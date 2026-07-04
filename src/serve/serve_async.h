// serve/serve_async.h — Async serve mode (fiber-based multi-session)
#ifndef AURA_SERVE_SERVE_ASYNC_H
#define AURA_SERVE_SERVE_ASYNC_H

#include <cstdint>
#include <string>
#include <string_view>

namespace aura::serve {

// Entry point for --serve-async mode.
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

} // namespace detail

} // namespace aura::serve

#endif // AURA_SERVE_SERVE_ASYNC_H
