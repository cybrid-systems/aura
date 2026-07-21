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

using aura::compiler::CompilerService;
using aura::compiler::FFIRuntime;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

int main() {
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

    if (::aura::test::g_failed)
        return 1;
    std::println("bugfix #968–#984: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
