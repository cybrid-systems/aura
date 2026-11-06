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

inline std::string format_float_shortest(double value) {
    // NaN sign is not meaningful (0.0/0.0 yields a negative quiet NaN on
    // x86-64 and to_chars spells it "-nan"); print one canonical "nan".
    if (value != value)
        return "nan";
    std::array<char, 64> buf;
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec != std::errc{})
        return std::to_string(value);
    std::string text(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
    const bool special =
        text.find("inf") != std::string::npos || text.find("INF") != std::string::npos ||
        text.find("nan") != std::string::npos || text.find("NAN") != std::string::npos;
    if (!special && text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos)
        text += ".0";
    return text;
}

// Issue #4472: lowercase digits, radix 2..36. Radix 10 stays std::to_string.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — value/base are distinct.
inline std::string format_int_radix(std::int64_t value, int radix) {
    if (radix == 10)
        return std::to_string(value);
    const bool neg = value < 0;
    std::uint64_t mag = 0;
    if (neg)
        mag = static_cast<std::uint64_t>(-(value + 1)) + 1u;
    else
        mag = static_cast<std::uint64_t>(value);
    if (mag == 0)
        return "0";
    std::array<char, 72> buf;
    int pos = 0;
    while (mag > 0 && pos < static_cast<int>(buf.size())) {
        const auto digit = static_cast<unsigned>(mag % static_cast<unsigned>(radix));
        buf.at(static_cast<std::size_t>(pos)) =
            static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        ++pos;
        mag /= static_cast<unsigned>(radix);
    }
    std::string text;
    text.reserve(static_cast<std::size_t>(pos) + (neg ? 1 : 0));
    if (neg)
        text.push_back('-');
    while (pos > 0) {
        --pos;
        text.push_back(buf.at(static_cast<std::size_t>(pos)));
    }
    return text;
}

} // namespace aura::compiler
