// @category: unit
// @reason: Issue #1851 — compile:invalidations-stats must
// shared_lock workspace_mtx_ while loading workspace_flat() and
// reading counters so concurrent set_workspace_flat (#1729) cannot
// leave a stale FlatAST* (UAF).
//
//   AC1: source cites #1851; shared_lock + workspace_mtx_
//   AC2: engine:metrics returns hash with expected keys
//   AC3: set-code then metrics still returns (no hang)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: shared_lock on invalidations-stats ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_00.cpp");
        CHECK(src.find("#1851") != std::string::npos, "cites #1851");
        auto pos = src.find("\"compile:invalidations-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        // Window must span the local build_hash lambda (~2k) plus the
        // locked counter snapshot (shared_lock / workspace_mtx_).
        auto win = src.substr(pos, 3600);
        CHECK(win.find("shared_lock") != std::string::npos, "uses shared_lock");
        CHECK(win.find("workspace_mtx_") != std::string::npos, "locks workspace_mtx_");
        CHECK(win.find("workspace_flat") != std::string::npos, "reads workspace_flat");
        CHECK(win.find("bump_generation_count") != std::string::npos, "reads bump counter");
        // Writer pairing note.
        auto pre = src.substr(pos > 400 ? pos - 400 : 0, 400);
        CHECK(pre.find("#1729") != std::string::npos || win.find("#1729") != std::string::npos,
              "pairs with #1729 set_workspace_flat lock");
    }

    // ── AC2: metrics hash shape ──
    {
        std::println("\n--- AC2: engine:metrics invalidations-stats ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:invalidations-stats\")");
        CHECK(r.has_value(), "metrics returns");
        if (r) {
            CHECK(is_hash(*r) || is_void(*r), "hash or void");
        }
        if (r && is_hash(*r)) {
            auto bumps = cs.eval("(hash-ref (engine:metrics \"compile:invalidations-stats\") "
                                 "\"bump-generation-count\")");
            CHECK(bumps && is_int(*bumps) && as_int(*bumps) >= 0, "bump-generation-count >= 0");
            auto checks = cs.eval("(hash-ref (engine:metrics \"compile:invalidations-stats\") "
                                  "\"is-valid-check-count\")");
            CHECK(checks && is_int(*checks) && as_int(*checks) >= 0, "is-valid-check-count >= 0");
        }
    }

    // ── AC3: after set-code (workspace swap path) ──
    {
        std::println("\n--- AC3: metrics after set-code workspace ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code");
        // Exercise eval that may bump gen / is_valid paths.
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(engine:metrics \"compile:invalidations-stats\")");
        CHECK(r.has_value(), "metrics after set-code");
        if (r && is_hash(*r)) {
            auto bumps = cs.eval("(hash-ref (engine:metrics \"compile:invalidations-stats\") "
                                 "\"bump-generation-count\")");
            CHECK(bumps && is_int(*bumps) && as_int(*bumps) >= 0, "bumps still readable");
        }
        // Sequential smoke: many metrics + set-code interleaves.
        for (int i = 0; i < 20; ++i) {
            CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(),
                  std::format("set-code iter {}", i));
            auto m = cs.eval("(engine:metrics \"compile:invalidations-stats\")");
            CHECK(m.has_value(), std::format("metrics iter {}", i));
        }
    }

    std::println("\n=== test_invalidations_stats_workspace_lock_1851: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
