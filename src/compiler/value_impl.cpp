// value_impl.cpp — Implementation for aura.compiler.value exported functions
module;
#include <cstdint>
module aura.compiler.value;

namespace aura::compiler::types {

// Truthiness: #f (val=3) and integer 0 (val=0) are both falsy.
bool is_truthy(const EvalValue& v) noexcept {
    if (v.val == 3) return false;           // #f
    if (is_int(v) && as_int(v) == 0) return false;  // integer 0
    return true;
}

} // namespace aura::compiler::types
