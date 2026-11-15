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
inline void escape_into(std::string& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char c : s) {
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
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xf];
                    out += kHex[c & 0xf];
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
}

inline std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    escape_into(out, s);
    return out;
}

} // namespace aura::json

#endif // AURA_COMPILER_JSON_ESCAPE_HH
