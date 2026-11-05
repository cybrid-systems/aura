// diag_print.hh — Issue #4936: stderr diagnostics callable from noexcept code.
//
// std::println(std::cerr, ...) throws std::system_error when the write fails
// (closed or full stderr, EPIPE) and std::bad_alloc when formatting cannot
// allocate. Inside a noexcept function either one ends in std::terminate, and
// it also skips whatever cleanup follows the print (a std::thread still
// joinable when it is destroyed terminates too). A lost diagnostic is better
// than a terminated process, so this wrapper drops the line instead.
//
// Use it only for best-effort diagnostics in noexcept code
// (docs/cpp-style.md F5). Code that may throw keeps std::println.
#ifndef AURA_CORE_DIAG_PRINT_HH
#define AURA_CORE_DIAG_PRINT_HH

#include <format>
#include <iostream>
#include <print>
#include <utility>

namespace aura::core {

template <class... Args>
void eprintln_nothrow(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        std::println(std::cerr, fmt, std::forward<Args>(args)...);
    } catch (...) {
        // [SILENCE-PRIM-#4936] A best-effort diagnostic must not terminate its
        // noexcept caller; the line is dropped.
    }
}

} // namespace aura::core

#endif // AURA_CORE_DIAG_PRINT_HH
