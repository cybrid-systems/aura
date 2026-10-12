// Issue #4472: code-point index over UTF-8 bytes.
// A well-formed Unicode scalar is one unit. An ill-formed byte is one
// unit, so a later well-formed sequence is never split and every byte
// stays addressable. Characters remain fixnums (#4129); this header
// does not introduce a second character value.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aura::compiler {

struct Utf8Unit {
    std::uint32_t cp = 0;
    std::size_t nbytes = 0;
};

// True for U+0000..U+10FFFF excluding UTF-16 surrogates.
[[nodiscard]] inline bool unicode_scalar(std::int64_t n) noexcept {
    return n >= 0 && n <= 0x10FFFF && !(n >= 0xD800 && n <= 0xDFFF);
}

// Decode one unit at byte offset i. Returns false at end of string.
[[nodiscard]] inline bool utf8_unit_at(std::string_view s, std::size_t i, Utf8Unit& out) noexcept {
    if (i >= s.size())
        return false;
    const auto c0 = static_cast<unsigned char>(s[i]);
    out.cp = c0;
    out.nbytes = 1;
    int need = 0;
    std::uint32_t cp = 0;
    std::uint32_t minc = 0;
    if (c0 < 0x80)
        return true;
    if (c0 >= 0xC2 && c0 <= 0xDF) {
        need = 2;
        cp = c0 & 0x1Fu;
        minc = 0x80;
    } else if (c0 >= 0xE0 && c0 <= 0xEF) {
        need = 3;
        cp = c0 & 0x0Fu;
        minc = 0x800;
    } else if (c0 >= 0xF0 && c0 <= 0xF4) {
        need = 4;
        cp = c0 & 0x07u;
        minc = 0x10000;
    } else {
        return true; // continuation or illegal lead: one byte
    }
    if (i + static_cast<std::size_t>(need) > s.size())
        return true;
    for (int k = 1; k < need; ++k) {
        const auto ck = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((ck & 0xC0u) != 0x80u)
            return true;
        cp = (cp << 6) | (ck & 0x3Fu);
    }
    if (cp < minc || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return true;
    out.cp = cp;
    out.nbytes = static_cast<std::size_t>(need);
    return true;
}

[[nodiscard]] inline std::size_t utf8_unit_count(std::string_view s) noexcept {
    std::size_t n = 0;
    std::size_t i = 0;
    Utf8Unit u;
    while (utf8_unit_at(s, i, u)) {
        i += u.nbytes;
        ++n;
    }
    return n;
}

// Byte offset of code-point index, or s.size() when the index is past the end.
[[nodiscard]] inline std::size_t utf8_byte_offset(std::string_view s,
                                                  std::size_t cp_index) noexcept {
    std::size_t i = 0;
    std::size_t n = 0;
    Utf8Unit u;
    while (n < cp_index && utf8_unit_at(s, i, u)) {
        i += u.nbytes;
        ++n;
    }
    return i;
}

// Code point at a character index, or -1 when the index is out of range.
[[nodiscard]] inline int utf8_code_point_at(std::string_view s, std::size_t cp_index) noexcept {
    std::size_t i = 0;
    std::size_t n = 0;
    Utf8Unit u;
    while (utf8_unit_at(s, i, u)) {
        if (n == cp_index)
            return static_cast<int>(u.cp);
        i += u.nbytes;
        ++n;
    }
    return -1;
}

// How many units start strictly before byte_pos. A position on a unit
// boundary is the code-point index of that unit.
[[nodiscard]] inline std::size_t utf8_units_before(std::string_view s,
                                                   std::size_t byte_pos) noexcept {
    if (byte_pos > s.size())
        byte_pos = s.size();
    std::size_t i = 0;
    std::size_t n = 0;
    Utf8Unit u;
    while (i < byte_pos && utf8_unit_at(s, i, u)) {
        if (i + u.nbytes > byte_pos)
            break;
        i += u.nbytes;
        ++n;
    }
    return n;
}

inline void utf8_append(std::string& out, std::int64_t cp) {
    if (!unicode_scalar(cp))
        return;
    const auto u = static_cast<std::uint32_t>(cp);
    if (u < 0x80u) {
        out.push_back(static_cast<char>(u));
    } else if (u < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (u >> 6)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    } else if (u < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (u >> 12)));
        out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (u >> 18)));
        out.push_back(static_cast<char>(0x80u | ((u >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    }
}

} // namespace aura::compiler
