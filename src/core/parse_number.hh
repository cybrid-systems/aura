// parse_number.hh — Issue #4936: exception-free numeric parsing.
//
// Replaces `try { std::stoul(s) } catch (...) {}` (an empty catch that also
// accepts "-1" by wrapping and truncates when narrowed) with std::from_chars.
// Leading digits are parsed and trailing text is ignored, like stoul; a
// value outside T's range, a sign, or no digits at all yields nullopt.
#ifndef AURA_CORE_PARSE_NUMBER_HH
#define AURA_CORE_PARSE_NUMBER_HH

#include <charconv>
#include <concepts>
#include <memory> // std::to_address
#include <optional>
#include <string_view>
#include <system_error>

namespace aura::core {

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> parse_leading_int(std::string_view text) noexcept {
    T value{};
    const std::from_chars_result res =
        std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
    if (res.ec != std::errc{})
        return std::nullopt;
    return value;
}

static_assert(parse_leading_int<unsigned>("42.wal") == 42u);
static_assert(!parse_leading_int<unsigned>("-1").has_value());
static_assert(!parse_leading_int<unsigned char>("256").has_value());
static_assert(!parse_leading_int<int>("").has_value());

} // namespace aura::core

#endif // AURA_CORE_PARSE_NUMBER_HH
