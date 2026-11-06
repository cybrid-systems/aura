// @category: unit
// @reason: Issue #4936 — PrimFn owns the callable, arith names are constexpr,
//          and the fiber overlay lock still serializes write/commit.
//
//   AC1: a small lambda and a function pointer are PrimFn, not std::function
//   AC2: a capture bigger than the SBO is one heap box, still not std::function
//   AC3: + / <= are the constexpr tables; other names are not
//   AC4: two fibers writing the same name: the first commit applies, the second conflicts

#include "compiler/arith_prim_names.hh"
#include "compiler/fiber_cow_overlay.hh"

import std;
import aura.compiler.evaluator_pure;
import aura.compiler.value;

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

aura::compiler::types::EvalValue forty(std::span<const aura::compiler::types::EvalValue>) {
    return aura::compiler::types::make_int(40);
}

} // namespace

int main() {
    static_assert(aura::compiler::is_arith_prim_name("+"));
    static_assert(aura::compiler::is_arith_prim_name("/"));
    static_assert(!aura::compiler::is_arith_prim_name("="));
    static_assert(aura::compiler::is_numeric_cmp_prim_name("<="));
    static_assert(!aura::compiler::is_numeric_cmp_prim_name("+"));
    static_assert(!aura::compiler::is_arith_prim_name(""));
    expect(aura::compiler::is_arith_prim_name("-") && aura::compiler::is_numeric_cmp_prim_name(">"),
           "constexpr arith and comparison names match at runtime");
    expect(!aura::compiler::is_arith_prim_name("display") &&
               !aura::compiler::is_numeric_cmp_prim_name("display"),
           "display is neither table");

    const int three = 3;
    aura::compiler::PrimFn fn([three](std::span<const aura::compiler::types::EvalValue>) {
        return aura::compiler::types::make_int(three);
    });
    auto got = fn({});
    expect(static_cast<bool>(fn) && !fn.uses_std_function() && fn.uses_sbo_trampoline() &&
               aura::compiler::types::is_int(got) && aura::compiler::types::as_int(got) == 3,
           "small capture is an SBO PrimFn and returns 3");

    aura::compiler::PrimFn ptr = forty;
    auto got40 = ptr({});
    expect(ptr.is_function_pointer() && !ptr.uses_std_function() &&
               aura::compiler::types::as_int(got40) == 40,
           "function pointer PrimFn does not use std::function");

    std::array<char, 80> big{};
    big[0] = 9;
    aura::compiler::PrimFn wide([big](std::span<const aura::compiler::types::EvalValue>) {
        return aura::compiler::types::make_int(big[0]);
    });
    auto got9 = wide({});
    expect(wide.uses_heap_box() && !wide.uses_std_function() &&
               aura::compiler::types::as_int(got9) == 9,
           "a capture past the SBO is one heap box");

    int owner = 0;
    auto& reg = aura::compiler::cow::registry<int>();
    reg.begin(&owner, 1, 10);
    reg.begin(&owner, 2, 10);
    expect(reg.write(&owner, 1, "x", 7, 10), "fiber 1 writes x");
    expect(reg.write(&owner, 2, "x", 9, 10), "fiber 2 writes x");
    int seen = 0;
    expect(reg.read(&owner, 1, "x", seen) && seen == 7, "fiber 1 reads its own overlay");
    auto first = reg.commit(&owner, 1, 10, [](const std::string& name, const int& value) {
        expect(name == "x" && value == 7, "first commit applies x=7");
    });
    expect(first.ok && first.applied == 1 && first.conflict.empty(), "first commit wins");
    auto second = reg.commit(&owner, 2, 10, [](const std::string&, const int&) {
        expect(false, "losing commit must not apply");
    });
    expect(!second.ok && second.conflict == "x" && second.applied == 0,
           "second commit conflicts on x and applies nothing");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
