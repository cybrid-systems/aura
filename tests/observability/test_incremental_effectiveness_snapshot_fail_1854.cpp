// @category: unit
// @reason: Issue #1854 — query:incremental-effectiveness must not
// silently return a false-clean 4-tuple of zeros when svc->snapshot()
// throws; return void + bump
// incremental_effectiveness_snapshot_failures (#1669 class A).
//
//   AC1: source cites #1854; typed catch + make_void + metric
//   AC2: happy path still returns pair 4-tuple (or void/int if no svc)
//   AC3: metric field present on CompilerMetrics (zero after clean call)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
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
        std::println("\n--- AC1: snapshot failure → void + metric ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_03.cpp");
        CHECK(src.find("#1854") != std::string::npos, "cites #1854");
        auto pos = src.find("\"query:incremental-effectiveness\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 2800);
        CHECK(win.find("snapshot()") != std::string::npos, "calls snapshot");
        CHECK(win.find("make_void()") != std::string::npos, "returns void on failure");
        CHECK(win.find("incremental_effectiveness_snapshot_failures") != std::string::npos,
              "bumps snapshot_failures metric");
        // #1856: centralized try_snapshot (catch lives in CompilerService).
        CHECK(win.find("try_snapshot") != std::string::npos, "uses try_snapshot (#1856)");
        CHECK(win.find("svc->snapshot()") == std::string::npos, "no raw snapshot()");
        // Must not only leave zeros and fall through to 4-tuple.
        CHECK(win.find("zeros are already initialized") == std::string::npos,
              "removed silent-zero comment");
    }

    // ── AC2: happy path ──
    {
        std::println("\n--- AC2: happy path returns 4-tuple ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        // Prefer public name; fall back to engine:metrics.
        auto r = cs.eval("(query:incremental-effectiveness)");
        if (!r)
            r = cs.eval("(engine:metrics \"query:incremental-effectiveness\")");
        CHECK(r.has_value(), "returns a result");
        if (r) {
            // Success: pair 4-tuple. No-svc path returns int 0.
            // Failure path would be void — must not be void on happy path.
            CHECK(is_pair(*r) || is_int(*r), "pair 4-tuple or int fallback");
            CHECK(!is_void(*r), "happy path is not void");
        }
    }

    // ── AC3: metric field zero after clean call ──
    {
        std::println("\n--- AC3: snapshot_failures metric field ---");
        CompilerService cs;
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics present");
        if (m) {
            const auto before =
                m->incremental_effectiveness_snapshot_failures.load(std::memory_order_relaxed);
            (void)cs.eval("(engine:metrics \"query:incremental-effectiveness\")");
            (void)cs.eval("(query:incremental-effectiveness)");
            const auto after =
                m->incremental_effectiveness_snapshot_failures.load(std::memory_order_relaxed);
            CHECK(after == before, "clean path does not bump snapshot_failures");
        }
        // Header documents the field.
        std::string h;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            h = read_file(p);
            if (!h.empty())
                break;
        }
        CHECK(!h.empty(), "read observability_metrics.h");
        CHECK(h.find("incremental_effectiveness_snapshot_failures") != std::string::npos,
              "metric declared in CompilerMetrics");
        CHECK(h.find("#1854") != std::string::npos, "header cites #1854");
    }

    std::println(
        "\n=== test_incremental_effectiveness_snapshot_fail_1854: {} passed, {} failed ===",
        g_passed, g_failed);
    return g_failed ? 1 : 0;
}
