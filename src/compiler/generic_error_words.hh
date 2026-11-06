// generic_error_words.hh — Issue #4936 B4.
// The five short tokens that must not count as a keyword hit inside an
// error-like capture. Previously a function-local unordered_set built on
// first use. Membership is exact, not a prefix.
#ifndef AURA_COMPILER_GENERIC_ERROR_WORDS_HH
#define AURA_COMPILER_GENERIC_ERROR_WORDS_HH

#include <array>
#include <string_view>

namespace aura::compiler {

inline constexpr std::array<std::string_view, 5> kGenericErrorWords = {
    "error", "kind", "parse", "syntax", "type",
};

[[nodiscard]] inline bool is_generic_error_word(std::string_view word) noexcept {
    for (const auto entry : kGenericErrorWords) {
        if (entry == word)
            return true;
    }
    return false;
}

} // namespace aura::compiler

#endif
