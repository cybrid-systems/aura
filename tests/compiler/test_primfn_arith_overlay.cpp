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
#include "compiler/mutate_type_gate.hh"
#include "compiler/typed_mutation_audit.h"

import std;
import aura.compiler.adt_runtime;
import aura.compiler.evaluator_pure;
import aura.compiler.ffi_primitives;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;
import aura.diag;

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

    static_assert(!std::is_nothrow_move_constructible_v<aura::compiler::PrimFn>,
                  "SBO move copies the payload and is not noexcept");
    struct MutableCall {
        int x = 1;
        aura::compiler::types::EvalValue operator()(std::span<const aura::compiler::types::EvalValue>) {
            return aura::compiler::types::make_int(x);
        }
    };
    struct ReturnsInt {
        int operator()(std::span<const aura::compiler::types::EvalValue>) const { return 1; }
    };
    struct MoveOnlyCall {
        MoveOnlyCall() = default;
        MoveOnlyCall(const MoveOnlyCall&) = delete;
        MoveOnlyCall(MoveOnlyCall&&) = default;
        aura::compiler::types::EvalValue
        operator()(std::span<const aura::compiler::types::EvalValue>) const {
            return aura::compiler::types::make_int(1);
        }
    };
    static_assert(!std::is_constructible_v<aura::compiler::PrimFn, MutableCall>);
    static_assert(!std::is_constructible_v<aura::compiler::PrimFn, ReturnsInt>);
    static_assert(!std::is_constructible_v<aura::compiler::PrimFn, MoveOnlyCall>);
    expect(true, "mutable, non-EvalValue, and move-only callables are rejected");

    struct ThrowCopy {
        int value;
        int* left;
        ThrowCopy(int v, int* budget) : value(v), left(budget) {}
        ThrowCopy(const ThrowCopy& o) : value(o.value), left(o.left) {
            if (*left <= 0)
                throw std::runtime_error("copy");
            --*left;
        }
        aura::compiler::types::EvalValue
        operator()(std::span<const aura::compiler::types::EvalValue>) const {
            return aura::compiler::types::make_int(value);
        }
    };
    {
        int left = 1;
        ThrowCopy body{4, &left};
        aura::compiler::PrimFn src = body;
        bool threw = false;
        try {
            aura::compiler::PrimFn copy(src);
            (void)copy;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        auto kept = src({});
        expect(threw && aura::compiler::types::as_int(kept) == 4,
               "throwing SBO copy leaves the source callable");
        left = 1;
        ThrowCopy body2{4, &left};
        aura::compiler::PrimFn src2 = body2;
        threw = false;
        try {
            aura::compiler::PrimFn moved(std::move(src2));
            (void)moved;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        auto kept2 = src2({});
        expect(threw && aura::compiler::types::as_int(kept2) == 4,
               "throwing SBO move leaves the source callable");
    }
    struct HeapThrow {
        int value;
        int* left;
        char pad[80]{};
        HeapThrow(int v, int* budget) : value(v), left(budget) {}
        HeapThrow(const HeapThrow& o) : value(o.value), left(o.left) {
            if (*left <= 0)
                throw std::runtime_error("heap-copy");
            --*left;
        }
        aura::compiler::types::EvalValue
        operator()(std::span<const aura::compiler::types::EvalValue>) const {
            return aura::compiler::types::make_int(value);
        }
    };
    {
        int left = 1;
        HeapThrow body{8, &left};
        aura::compiler::PrimFn src = body;
        expect(src.uses_heap_box(), "oversized payload is a heap box");
        bool threw = false;
        try {
            aura::compiler::PrimFn copy(src);
            (void)copy;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        auto kept = src({});
        expect(threw && aura::compiler::types::as_int(kept) == 8,
               "throwing heap copy leaves the source callable");
    }
    struct alignas(64) Over {
        char pad[96]{};
        int value;
        explicit Over(int v) : value(v) {}
        aura::compiler::types::EvalValue
        operator()(std::span<const aura::compiler::types::EvalValue>) const {
            const auto addr = reinterpret_cast<std::uintptr_t>(this);
            return aura::compiler::types::make_int(value + (addr % 64 == 0 ? 0 : 1000));
        }
    };
    {
        aura::compiler::PrimFn wide(Over{5});
        auto aligned = wide({});
        expect(wide.uses_heap_box() && aura::compiler::types::as_int(aligned) == 5,
               "overaligned heap payload is 64-byte aligned");
        aura::compiler::PrimFn copy(wide);
        auto aligned_copy = copy({});
        expect(aura::compiler::types::as_int(aligned_copy) == 5, "copied overaligned payload stays aligned");
    }

    {
        const int captured = 11;
        aura::compiler::PrimFn body([captured](std::span<const aura::compiler::types::EvalValue>) {
            return aura::compiler::types::make_int(captured);
        });
        aura::compiler::AdtRuntime adt;
        int called = 0;
        adt.register_dynamic_ctor(
            [&](std::string name, aura::compiler::PrimFn fn) {
                expect(name == "Pair" && fn.uses_sbo_trampoline() && !fn.uses_std_function(),
                       "ADT registration receives an SBO PrimFn");
                called = static_cast<int>(aura::compiler::types::as_int(fn({})));
            },
            "Pair", std::move(body), 2, 1);
        expect(called == 11 && adt.find_ctor("Pair").value_or(0) == 1,
               "ADT ctor is registered and the body ran");
    }
    {
        aura::compiler::FFIRuntime ffi;
        std::optional<aura::compiler::PrimFn> load;
        ffi.register_primitives(
            [&](std::string name, aura::compiler::PrimFn fn) {
                if (name == "c-load")
                    load = std::move(fn);
            },
            nullptr, nullptr, nullptr);
        expect(load.has_value() && load->uses_sbo_trampoline() && !load->uses_std_function(),
               "c-load is registered as an SBO PrimFn");
        if (load)
            expect(aura::compiler::types::as_int((*load)({})) == 0,
                   "c-load with no arguments returns 0 before dlopen");
    }

    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::mutate_type_gate::set_mode(aura::compiler::mutate_type_gate::MutateTypeGate::Soft);
    {
        aura::core::TypeRegistry reg;
        aura::diag::DiagnosticCollector diag;
        aura::compiler::InferenceEngine engine(reg, diag);
        aura::ast::FlatAST flat;
        aura::ast::StringPool pool;
        const auto plus = flat.add_variable(pool.intern("+"));
        const auto text = flat.add_literalstring(pool.intern("hi"));
        const auto one = flat.add_literal(1);
        const aura::ast::NodeId args[] = {text, one};
        const auto call = flat.add_call(plus, std::span<const aura::ast::NodeId>{args});
        auto ty = engine.synthesize_flat_call_arith(flat, pool, flat.get(call), plus);
        expect(ty.has_value() && diag.has_errors(), "arith string literal is a type error");
        bool said = false;
        for (const auto& d : diag.diagnostics())
            said = said || d.message.find("expected a number, got String literal") != std::string::npos;
        expect(said, "arith diagnostic names the string literal");
        diag.clear();
        aura::ast::FlatAST flat_eq;
        aura::ast::StringPool pool_eq;
        const auto eq = flat_eq.add_variable(pool_eq.intern("="));
        const auto text_eq = flat_eq.add_literalstring(pool_eq.intern("hi"));
        const auto one_eq = flat_eq.add_literal(1);
        const aura::ast::NodeId eq_args[] = {text_eq, one_eq};
        const auto eq_call = flat_eq.add_call(eq, std::span<const aura::ast::NodeId>{eq_args});
        auto eq_ty = engine.synthesize_flat_call_arith(flat_eq, pool_eq, flat_eq.get(eq_call), eq);
        expect(eq_ty.has_value() && diag.has_errors(), "comparison string literal is a type error");
    }

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
