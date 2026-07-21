// test_atomic_swap_stdlib.cpp — Issue #1380:
// Generic atomic-resource-swap stdlib (bind R → A, drain at sync).

#include "test_harness.hpp"

#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

namespace {

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool eval_bool(CompilerService& cs, const char* expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    // ── Files ──
    CHECK(file_exists("lib/std/atomic-swap.aura"), "atomic-swap.aura");
    CHECK(file_exists("lib/std/atomic-swap.aura-type"), "atomic-swap.aura-type");
    CHECK(file_exists("lib/std/tests/test_atomic_swap_basic.aura"), "basic aura test");
    CHECK(file_exists("lib/std/tests/test_atomic_swap_registry.aura"), "registry aura test");
    CHECK(file_exists("lib/std/tests/test_atomic_swap_fuzz.aura"), "fuzz aura test");

    {
        auto idx = read_file("lib/std/INDEX.aura");
        CHECK(idx.find("atomic-swap") != std::string::npos, "INDEX lists atomic-swap");
    }
    {
        auto main = read_file("lib/std/atomic-swap.aura");
        CHECK(main.find("swap-binding!") != std::string::npos, "export swap-binding!");
        CHECK(main.find("sync-bindings!") != std::string::npos, "export sync-bindings!");
        CHECK(main.find("make-binding") != std::string::npos, "export make-binding");
        CHECK(main.find("drain-pending-swaps!") != std::string::npos, "export drain");
    }

    // ── require + API ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/atomic-swap\" all:)").has_value(), "require atomic-swap");

        CHECK(eval_bool(cs, "(let* ((b (make-binding \"x\" 0 \"a0\"))"
                            "       (b2 (swap-binding! b 1 \"a1\"))"
                            "       (r (sync-bindings! b2)))"
                            "  (and (binding-dirty? b2)"
                            "       (cdr r)"
                            "       (= (binding-target-version (car r)) 1)"
                            "       (equal? (binding-artifact-id (car r)) \"a1\")"
                            "       (not (binding-dirty? (car r)))))"),
              "swap then sync commits once");

        CHECK(eval_bool(cs, "(let* ((b (make-binding \"y\" 0 \"a0\"))"
                            "       (r0 (sync-bindings! b))"
                            "       (b2 (swap-binding! b 2 \"a2\"))"
                            "       (r1 (sync-bindings! b2))"
                            "       (r2 (sync-bindings! (car r1))))"
                            "  (and (not (cdr r0))"
                            "       (cdr r1)"
                            "       (not (cdr r2))))"),
              "sync idempotent — second drain #f");

        CHECK(eval_bool(cs, "(let* ((reg (make-registry))"
                            "       (b (make-binding \"z\" 0 \"Z0\"))"
                            "       (reg2 (registry-put reg (swap-binding! b 9 \"Z9\")))"
                            "       (dr (drain-pending-swaps! reg2))"
                            "       (nb (registry-ref (car dr) \"z\")))"
                            "  (and (cdr dr)"
                            "       (= (binding-target-version nb) 9)"
                            "       (equal? (binding-artifact-id nb) \"Z9\")))"),
              "registry drain commits");

        // Rapid successive swaps — last writer wins after one sync
        CHECK(eval_bool(cs, "(let* ((b0 (make-binding \"lw\" 0 \"L0\"))"
                            "       (b1 (swap-binding! b0 1 \"L1\"))"
                            "       (b2 (swap-binding! b1 2 \"L2\"))"
                            "       (b3 (swap-binding! b2 3 \"L3\"))"
                            "       (r (sync-bindings! b3))"
                            "       (c (car r)))"
                            "  (and (cdr r)"
                            "       (= (binding-target-version c) 3)"
                            "       (equal? (binding-artifact-id c) \"L3\")))"),
              "last writer wins on sync");
    }

    // ── Aura unit tests ──
    {
        CompilerService cs;
        auto r = cs.eval("(load \"lib/std/tests/test_atomic_swap_basic.aura\")");
        CHECK(r.has_value(), "load basic aura");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "basic aura → #t");
    }
    {
        CompilerService cs;
        auto r = cs.eval("(load \"lib/std/tests/test_atomic_swap_registry.aura\")");
        CHECK(r.has_value(), "load registry aura");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "registry aura → #t");
    }
    {
        CompilerService cs;
        auto r = cs.eval("(load \"lib/std/tests/test_atomic_swap_fuzz.aura\")");
        CHECK(r.has_value(), "load fuzz aura");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "fuzz 50 cycles → #t");
    }

    // ── INDEX help ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/INDEX\" all:)").has_value(), "require INDEX");
        auto help = cs.eval("(stdlib:help \"atomic-swap\")");
        CHECK(help.has_value(), "stdlib:help atomic-swap");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("atomic-swap stdlib #1380: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
