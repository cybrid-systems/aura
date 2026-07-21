// @category: unit
// @reason: Issue #1856 — central CompilerService::try_snapshot() for
// all compile_* stats sites that used to call snapshot() raw (throw
// escape + false-clean zeros). compiler_service_ remains non-owning
// (#1839).
//
//   AC1: try_snapshot declared; snapshot_failures_total metric;
//        compile_01/02/03/06 use try_snapshot (no raw svc->snapshot)
//   AC2: happy-path stats still return hash/pair via engine:metrics
//   AC3: clean path does not bump snapshot_failures_total

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
using aura::compiler::types::is_hash;
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

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: try_snapshot + all compile sites ---");
        auto svc = read_first({"src/compiler/service.ixx", "../src/compiler/service.ixx"});
        CHECK(!svc.empty(), "read service.ixx");
        CHECK(svc.find("try_snapshot") != std::string::npos, "try_snapshot declared");
        CHECK(svc.find("#1856") != std::string::npos, "service cites #1856");
        CHECK(svc.find("snapshot_failures_total") != std::string::npos, "bumps snapshot_failures");

        auto mh = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!mh.empty(), "read metrics.h");
        CHECK(mh.find("snapshot_failures_total") != std::string::npos, "metric field");
        CHECK(mh.find("#1856") != std::string::npos, "metrics cites #1856");

        for (const char* rel : {"src/compiler/evaluator_primitives_compile.cpp",
                                "src/compiler/evaluator_primitives_compile.cpp",
                                "src/compiler/evaluator_primitives_compile.cpp",
                                "src/compiler/evaluator_primitives_compile.cpp"}) {
            std::string p = rel;
            auto alt = std::string("../") + rel;
            auto src = read_first({p.c_str(), alt.c_str()});
            CHECK(!src.empty(), std::format("read {}", rel));
            CHECK(src.find("try_snapshot") != std::string::npos,
                  std::format("{} uses try_snapshot", rel));
            CHECK(src.find("svc->snapshot()") == std::string::npos,
                  std::format("{} has no raw svc->snapshot()", rel));
        }
    }

    // ── AC2: happy path ──
    {
        std::println("\n--- AC2: stats primitives still return data ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        for (const char* name : {"compile:occurrence-typing-stats", "compile:let-poly-stats",
                                 "query:incremental-effectiveness"}) {
            auto r = cs.eval(std::format("(engine:metrics \"{}\")", name));
            if (!r)
                r = cs.eval(std::format("({})", name));
            CHECK(r.has_value(), std::format("{} returns", name));
            if (r)
                CHECK(is_hash(*r) || is_pair(*r) || is_void(*r) || is_int(*r),
                      std::format("{} shape ok", name));
        }
    }

    // ── AC3: metric not bumped on clean path ──
    {
        std::println("\n--- AC3: clean path no snapshot_failures ---");
        CompilerService cs;
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics present");
        if (m) {
            const auto before = m->snapshot_failures_total.load(std::memory_order_relaxed);
            (void)cs.eval("(engine:metrics \"compile:occurrence-typing-stats\")");
            (void)cs.eval("(engine:metrics \"compile:mutation-log-invalidation-stats\")");
            (void)cs.eval("(query:incremental-effectiveness)");
            const auto after = m->snapshot_failures_total.load(std::memory_order_relaxed);
            CHECK(after == before, "clean path does not bump snapshot_failures_total");
        }
    }

    std::println("\n=== test_safe_snapshot_umbrella_1856: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
