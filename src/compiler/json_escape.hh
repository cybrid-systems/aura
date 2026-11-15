// Shared JSON string escaping (RFC 8259 section 7).
//
// #5234: main --serve, serve-async, reflect, the evo log, the LSP writer and
// the LLM payload builder each carried their own escaper. Three of them let
// raw control characters through, which makes the emitted JSON invalid.
// Every caller now routes through this header.
#ifndef AURA_COMPILER_JSON_ESCAPE_HH
#define AURA_COMPILER_JSON_ESCAPE_HH

#include <string>
#include <string_view>

namespace aura::json {

// Append the escaped body of a JSON string (no surrounding quotes).
inline void escape_into(std::string& out, std::string_view text) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    for (const unsigned char code : text) {
        switch (code) {
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
                if (code < 0x20) {
                    const auto byte = static_cast<unsigned>(code);
                    out += "\\u00";
                    out += kHex[(byte >> 4u) & 0xfu];
                    out += kHex[byte & 0xfu];
                } else {
                    out.push_back(static_cast<char>(code));
                }
        }
    }
}

inline std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    escape_into(out, text);
    return out;
}

} // namespace aura::json

#endif // AURA_COMPILER_JSON_ESCAPE_HH
