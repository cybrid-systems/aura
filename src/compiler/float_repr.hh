// Issue #4468: shortest decimal that reads back as the same double.
// std::to_chars (general, no precision) is the round-trip spelling.
// An integer-valued float gains ".0" so string->number keeps it inexact.
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>

namespace aura::compiler {

inline std::string format_float_shortest(double v) {
    // NaN sign is not meaningful (0.0/0.0 yields a negative quiet NaN on
    // x86-64 and to_chars spells it "-nan"); print one canonical "nan".
    if (v != v)
        return "nan";
    std::array<char, 64> buf;
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if (ec != std::errc{})
        return std::to_string(v);
    std::string s(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
    const bool special = s.find("inf") != std::string::npos || s.find("INF") != std::string::npos ||
                         s.find("nan") != std::string::npos || s.find("NAN") != std::string::npos;
    if (!special && s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos)
        s += ".0";
    return s;
}

// Issue #4472: lowercase digits, radix 2..36. Radix 10 stays std::to_string.
inline std::string format_int_radix(std::int64_t n, int radix) {
    if (radix == 10)
        return std::to_string(n);
    const bool neg = n < 0;
    std::uint64_t mag = 0;
    if (neg)
        mag = static_cast<std::uint64_t>(-(n + 1)) + 1u;
    else
        mag = static_cast<std::uint64_t>(n);
    if (mag == 0)
        return "0";
    std::array<char, 72> buf;
    int i = 0;
    while (mag > 0 && i < static_cast<int>(buf.size())) {
        const auto d = static_cast<unsigned>(mag % static_cast<unsigned>(radix));
        buf[i++] = static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
        mag /= static_cast<unsigned>(radix);
    }
    std::string s;
    s.reserve(static_cast<std::size_t>(i) + (neg ? 1 : 0));
    if (neg)
        s.push_back('-');
    while (i > 0)
        s.push_back(buf[--i]);
    return s;
}

} // namespace aura::compiler
