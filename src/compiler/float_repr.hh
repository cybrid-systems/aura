// Issue #4468: shortest decimal that reads back as the same double.
// std::to_chars (general, no precision) is the round-trip spelling.
// An integer-valued float gains ".0" so string->number keeps it inexact.
#pragma once

#include <charconv>
#include <string>
#include <system_error>

namespace aura::compiler {

inline std::string format_float_shortest(double v) {
    char buf[64];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec != std::errc{})
        return std::to_string(v);
    std::string s(buf, static_cast<std::size_t>(ptr - buf));
    const bool special = s.find("inf") != std::string::npos || s.find("INF") != std::string::npos ||
                         s.find("nan") != std::string::npos || s.find("NAN") != std::string::npos;
    if (!special && s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos)
        s += ".0";
    return s;
}

} // namespace aura::compiler
