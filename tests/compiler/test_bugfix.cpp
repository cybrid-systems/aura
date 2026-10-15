// Issue #957/#968/#982/#984 (#1978 renamed): issue# moved from filename to header.
// test_bugfix_968_984.cpp — Issues #968–#984 bugfix regression

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ffi_primitives;
import aura.compiler.evaluator_pure;

using aura::compiler::CompilerService;
using aura::compiler::FFIRuntime;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

// Issue #4531 fixtures: local C functions standing in for c-func targets.
static double ffi4531_si(const char* p, std::int64_t n) {
    return static_cast<double>(p[0]) + static_cast<double>(n);
}
static double ffi4531_sf(const char* p, double k) {
    return static_cast<double>(p[0]) * k;
}
static double ffi4531_i(std::int64_t a) {
    return static_cast<double>(a) * 0.5;
}
static std::int64_t ffi4531_if(std::int64_t a, double k) {
    return a + static_cast<std::int64_t>(k);
}
static double ffi4531_z() {
    return 2.5;
}
static std::int64_t ffi4531_len(const char* p) {
    return static_cast<std::int64_t>(p[0]);
}

int run_test_bugfix() {
    // #982: parse_ffi_sig rejects missing ')'
    {
        int ret = 0;
        std::vector<int> args;
        std::string err;
        CHECK(!FFIRuntime::parse_ffi_sig("(Int -> Int", ret, args, &err), "missing ) rejected");
        CHECK(FFIRuntime::parse_ffi_sig("(Int) -> Int", ret, args, &err),
              "well-formed sig accepted");
        CHECK(ret == 1, "ret type Int");
    }

    // #968: snapshot populates function metrics fields (no crash)
    {
        CompilerService cs;
        auto snap = cs.snapshot();
        // functions may be empty without JIT activity; just ensure path works
        for (auto& f : snap.functions) {
            CHECK(!f.name.empty(), "fn metrics name set");
            (void)f.total_calls;
            (void)f.hit_rate;
            (void)f.specialized_for;
        }
        CHECK(true, "snapshot() ok");
    }

    // #957/basic: defuse_version still works
    {
        CompilerService cs;
        auto v = cs.evaluator().defuse_version();
        CHECK(v >= 0, "defuse_version readable");
        (void)v;
    }

    // Smoke: arithmetic still works after typechecker changes
    {
        CompilerService cs;
        auto r = cs.eval("(+ 40 2)");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "(+ 40 2) = 42");
    }

    // Issue #4531: the class-split FFI dispatcher. SysV assigns integer args
    // (rdi..r9) and SSE args (xmm0..7) independently, so the int-first
    // prototype ffi_call_by_class calls through is register-compatible with
    // any declared interleaving of the same (ni, nd) shape, and a Float
    // result comes from xmm0 whatever the argument classes are. The old
    // any_float switch read Float returns from rax and sent String/Int
    // slots through xmm.
    {
        using aura::compiler::pure::ffi_call_by_class;
        const std::int64_t p_s = reinterpret_cast<std::int64_t>("A"); // p[0] == 'A' == 65
        // (String Int) -> Float: both classes, Float return from xmm0.
        {
            const std::int64_t ic[2] = {p_s, 3};
            CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_si), ic, nullptr, 2,
                                            0) == 68.0,
                  "4531: (String Int) -> Float reads xmm0");
        }
        // (String Float) -> Float: mixed classes, each in its own register
        // class (the old double trampoline sent the pointer through xmm).
        {
            const std::int64_t ic[1] = {p_s};
            const double dc[1] = {2.0};
            CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_sf), ic, dc, 1, 1) ==
                      130.0,
                  "4531: (String Float) -> Float keeps the pointer in rdi");
        }
        // (Int) -> Float: Float return without a Float argument (the old
        // int trampoline read rax).
        {
            const std::int64_t ic[1] = {3};
            CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_i), ic, nullptr, 1,
                                            0) == 1.5,
                  "4531: (Int) -> Float reads xmm0");
        }
        // (Int Float) -> Int: integer return from rax with a Float slot.
        {
            const std::int64_t ic[1] = {2};
            const double dc[1] = {4.0};
            CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4531_if), ic, dc, 1,
                                                  1) == 6,
                  "4531: (Int Float) -> Int reads rax");
        }
        // () -> Float: no args at all.
        CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_z), nullptr, nullptr, 0,
                                        0) == 2.5,
              "4531: () -> Float reads xmm0");
        // String slot lands in rdi (the callee dereferences it).
        {
            const std::int64_t ic[1] = {p_s};
            CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4531_len), ic, nullptr,
                                                  1, 0) == 65,
                  "4531: String slot passes a live pointer");
        }
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("bugfix #968–#984: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_bugfix();
}
#endif
