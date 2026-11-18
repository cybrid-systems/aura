// Position encoding negotiation (LSP 3.17 positionEncodings).
//
// #5258: the server reports byte columns and the client reads UTF-16 code
// units. One codec converts every Position in both directions; diagnostics,
// definition ranges and incoming positions all go through it. With
// "utf-8" negotiated the byte column is the wire column, so the conversion
// is the identity — no per-client special cases.
#ifndef AURA_LSP_POSITION_INC
#define AURA_LSP_POSITION_INC

namespace lsp_4911 {

enum class Encoding { Utf16, Utf8 };

// Length in bytes of the UTF-8 sequence starting at s[i] (1 on a bad lead).
inline std::size_t utf8_sequence(std::string_view s, std::size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if (c >= 0xF0)
        len = 4;
    else if (c >= 0xE0)
        len = 3;
    else if (c >= 0xC0)
        len = 2;
    return std::min(s.size(), i + len) - i;
}

// A code point above the BMP is one surrogate pair: two UTF-16 code units.
inline int utf16_units_for(unsigned char lead) {
    return lead >= 0xF0 ? 2 : 1;
}

inline std::string_view line_at(std::string_view text, std::size_t line0) {
    std::size_t start = 0;
    for (std::size_t k = 0; k < line0; ++k) {
        const auto nl = text.find('\n', start);
        if (nl == std::string_view::npos)
            return {};
        start = nl + 1;
    }
    const auto nl = text.find('\n', start);
    return text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
}

struct PositionCodec {
    Encoding enc = Encoding::Utf16;

    // Internal (0-based byte column) → wire character.
    int from_byte(std::string_view line, std::size_t byte_col) const {
        if (enc == Encoding::Utf8)
            return static_cast<int>(std::min(byte_col, line.size()));
        byte_col = std::min(byte_col, line.size());
        int units = 0;
        std::size_t i = 0;
        while (i < byte_col) {
            const std::size_t start = i;
            i += utf8_sequence(line, i);
            if (i > byte_col)
                break; // landed inside a character: clamp to its first unit
            units += utf16_units_for(static_cast<unsigned char>(line[start]));
        }
        return units;
    }

    // Wire character → internal (0-based byte column).
    std::size_t to_byte(std::string_view line, long long character) const {
        if (character <= 0)
            return 0;
        if (enc == Encoding::Utf8)
            return std::min<std::size_t>(static_cast<std::size_t>(character), line.size());
        long long units = 0;
        std::size_t i = 0;
        while (i < line.size() && units < character) {
            const std::size_t start = i;
            i += utf8_sequence(line, i);
            units += utf16_units_for(static_cast<unsigned char>(line[start]));
            if (units > character)
                return start; // inside a surrogate pair
        }
        return i;
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_POSITION_INC
