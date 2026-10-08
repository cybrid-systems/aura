#pragma once

// Issue #4377: store_define_v2 inside a mutation boundary stamps defuse
// before the exit bump. Remember those defines so the outermost success
// exit can write the post-exit defuse. Thread-local so Evaluator and
// CompilerService layouts stay put.

#include <string>
#include <string_view>
#include <vector>

namespace aura::compiler {

struct IrCacheBoundaryRestampHook {
    void (*fn)(void*) noexcept = nullptr;
    void* ctx = nullptr;
};

[[nodiscard]] inline std::vector<std::string>& ir_cache_boundary_stored_names() noexcept {
    thread_local std::vector<std::string> names;
    return names;
}

inline void note_ir_cache_stored_inside_boundary(std::string_view name) {
    auto& names = ir_cache_boundary_stored_names();
    for (const auto& note : names) {
        if (note == name)
            return;
    }
    names.emplace_back(name);
}

inline void clear_ir_cache_boundary_stored_names() noexcept {
    ir_cache_boundary_stored_names().clear();
}

[[nodiscard]] inline std::vector<IrCacheBoundaryRestampHook>&
ir_cache_boundary_restamp_stack() noexcept {
    thread_local std::vector<IrCacheBoundaryRestampHook> stack;
    return stack;
}

inline void push_ir_cache_boundary_restamp(void (*fn)(void*) noexcept, void* ctx) {
    ir_cache_boundary_restamp_stack().push_back(IrCacheBoundaryRestampHook{fn, ctx});
}

inline void pop_ir_cache_boundary_restamp(void* ctx) noexcept {
    auto& stack = ir_cache_boundary_restamp_stack();
    if (!stack.empty() && stack.back().ctx == ctx)
        stack.pop_back();
}

[[nodiscard]] inline IrCacheBoundaryRestampHook* current_ir_cache_boundary_restamp() noexcept {
    auto& stack = ir_cache_boundary_restamp_stack();
    return stack.empty() ? nullptr : &stack.back();
}

} // namespace aura::compiler
