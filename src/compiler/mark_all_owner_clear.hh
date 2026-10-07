#pragma once

// Issue #4360: mark_all_defines_dirty clears ir_define_closure_owner_ only
// for a full workspace replace (set-code / snapshot restore). An add or a
// rebind flush still dirties the IR cache. Sibling closure ids stay
// dispatchable. Thread-local so Evaluator / CompilerService layout stays put.

namespace aura::compiler {

[[nodiscard]] inline bool& mark_all_clears_ir_owners_flag() noexcept {
    thread_local bool flag = false;
    return flag;
}

[[nodiscard]] inline bool mark_all_clears_ir_owners() noexcept {
    return mark_all_clears_ir_owners_flag();
}

struct MarkAllClearsIrOwners {
    bool prev_;
    MarkAllClearsIrOwners() noexcept
        : prev_(mark_all_clears_ir_owners_flag()) {
        mark_all_clears_ir_owners_flag() = true;
    }
    ~MarkAllClearsIrOwners() noexcept { mark_all_clears_ir_owners_flag() = prev_; }
    MarkAllClearsIrOwners(const MarkAllClearsIrOwners&) = delete;
    MarkAllClearsIrOwners& operator=(const MarkAllClearsIrOwners&) = delete;
};

} // namespace aura::compiler
