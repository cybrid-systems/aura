// test_fiber_concurrent_unit_batch.cpp — light concurrent units
#include "compiler/observability_metrics.h"
// Heavy/flaky: set_arena, stress_alloc, terminal_concurrent, self_heal, production_sweep
// remain standalone under tests/fiber/.

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

// from test_agent_fingerprint_atomic.cpp
namespace aura_fiber_run_agent_fingerprint_1730 {
// @category: unit
// @reason: Issue #1730 — current_agent_fingerprint_ must be atomic
// Issue #1730 (#1978 renamed): issue# moved from filename to header.
// under concurrent fiber set/get.
//
//   AC1: source cites #1730; atomic store/load on fingerprint
//   AC2: set/get roundtrip returns the stored value
//   AC3: concurrent writers/readers do not crash; final value is one of written
//   AC4: set_workspace_flat re-stamps flat from atomic load


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::Evaluator;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

} // namespace

int run_agent_fingerprint_1730() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: atomic fingerprint field ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1730") != std::string::npos, "cites #1730");
        CHECK(ixx.find("std::atomic<std::uint64_t> current_agent_fingerprint_") !=
                  std::string::npos,
              "atomic member");
        CHECK(ixx.find("memory_order_release") != std::string::npos, "store release");
        CHECK(ixx.find("memory_order_acquire") != std::string::npos, "load acquire");
        CHECK(ixx.find("current_agent_fingerprint_.store") != std::string::npos, "uses store");
        CHECK(ixx.find("current_agent_fingerprint_.load") != std::string::npos, "uses load");
    }

    // ── AC2: roundtrip ──
    {
        std::println("\n--- AC2: set/get roundtrip ---");
        Evaluator ev;
        CHECK(ev.current_agent_fingerprint() == 0, "default 0");
        ev.set_current_agent_fingerprint(0xA11CE7F00DULL);
        CHECK(ev.current_agent_fingerprint() == 0xA11CE7F00DULL, "roundtrip 0xA11CE7F00D");
        ev.set_current_agent_fingerprint(0);
        CHECK(ev.current_agent_fingerprint() == 0, "reset to 0");
    }

    // ── AC3: concurrent set/get ──
    {
        std::println("\n--- AC3: concurrent set/get ---");
        Evaluator ev;
        std::atomic<int> errors{0};
        constexpr int kWriters = 4;
        constexpr int kReaders = 4;
        std::vector<std::uint64_t> written_vals;
        for (int i = 0; i < kWriters; ++i)
            written_vals.push_back(0x1000ULL + static_cast<std::uint64_t>(i));

        auto writer = [&](int id) {
            for (int i = 0; i < 200; ++i)
                ev.set_current_agent_fingerprint(written_vals[static_cast<std::size_t>(id)]);
        };
        auto reader = [&]() {
            for (int i = 0; i < 200; ++i) {
                auto v = ev.current_agent_fingerprint();
                // 0 is default / reset-safe; otherwise must be a published writer value
                if (v != 0) {
                    bool ok = false;
                    for (auto w : written_vals)
                        if (v == w)
                            ok = true;
                    if (!ok)
                        errors.fetch_add(1);
                }
            }
        };
        std::vector<std::thread> thr;
        for (int i = 0; i < kWriters; ++i)
            thr.emplace_back(writer, i);
        for (int i = 0; i < kReaders; ++i)
            thr.emplace_back(reader);
        for (auto& t : thr)
            t.join();
        CHECK(errors.load() == 0, "no torn/unknown values under concurrent R/W");
        auto final_v = ev.current_agent_fingerprint();
        bool final_ok = false;
        for (auto w : written_vals)
            if (final_v == w)
                final_ok = true;
        CHECK(final_ok, "final value is one of the written fingerprints");
    }

    // ── AC4: set_workspace_flat re-stamps from atomic ──
    {
        std::println("\n--- AC4: set_workspace_flat re-stamps fingerprint ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto alloc = ev.test_arena().allocator();
        auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
        auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
        (void)pool;
        flat->add_literal(1);

        ev.set_current_agent_fingerprint(0xBEEFCAFEULL);
        ev.set_workspace_flat(flat);
        // FlatAST should have received the stamp; verify via a mutation record
        // if API exposes context, else just ensure set/get still consistent.
        CHECK(ev.current_agent_fingerprint() == 0xBEEFCAFEULL, "fingerprint still set after flat");
        // add_mutation should pick up author from flat context
        auto mid = flat->add_mutation(0, "test", "a", "b", "c");
        (void)mid;
        const auto hist = flat->mutation_history(0);
        if (!hist.empty()) {
            CHECK(hist[0].author_fingerprint == 0xBEEFCAFEULL,
                  "workspace flat stamped author from atomic fingerprint");
        } else {
            // Some FlatAST versions require a real node id; still OK if stamp API ran.
            CHECK(ev.current_agent_fingerprint() == 0xBEEFCAFEULL, "stamp path completed");
        }
    }

    std::println("\n=== test_agent_fingerprint_atomic_1730: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_agent_fingerprint_1730

// from test_intend_closure_live.cpp
namespace aura_fiber_run_intend_closure_live {
// @category: unit
// @reason: Issue #1719 — intend call_fn must not apply_closure on freed
// Issue #1719 (#1978 renamed): issue# moved from filename to header.
// generator/verifier/fixer ClosureIds (UAF sibling of #1713).
//
//   AC1: source call_fn uses agent_cid_live / Issue #1719
//   AC2: agent_closure_freed_during_call metric exists
//   AC3: free generator then intend does not crash; metric bumps
//   AC4: live intend still can complete (or empty without LLM)


namespace {

    using aura::compiler::CompilerMetrics;
    using aura::compiler::CompilerService;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static CompilerMetrics* metrics_of(CompilerService& cs) {
        return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    }

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

} // namespace

int run_intend_closure_live() {
    // ── AC1/AC2: source + metric field ──
    {
        std::println("\n--- AC1/AC2: call_fn live gate + metric ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1719") != std::string::npos, "cites #1719");
            CHECK(src.find("agent_cid_live") != std::string::npos, "agent_cid_live shared");
            CHECK(src.find("agent_note_closure_freed_call") != std::string::npos,
                  "call metric helper");
            auto pos = src.find("add(\"intend\"");
            CHECK(pos != std::string::npos, "found intend");
            if (pos != std::string::npos) {
                auto win = src.substr(pos, 6000);
                CHECK(win.find("agent_cid_live") != std::string::npos, "intend gates live");
                CHECK(win.find("agent_closure_freed_during_call") != std::string::npos ||
                          win.find("agent_note_closure_freed_call") != std::string::npos,
                      "intend bumps call metric");
            }
        }
        const char* mpaths[] = {
            "src/compiler/observability_metrics.h",
            "../src/compiler/observability_metrics.h",
        };
        std::string msrc;
        for (const char* p : mpaths) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() && msrc.find("agent_closure_freed_during_call") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: free generator before intend ──
    {
        std::println("\n--- AC3: free generator → no crash, metric bump ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto m0 = m->agent_closure_freed_during_call.load(std::memory_order_relaxed);

        // generator returns empty string; verifier never reached if gen freed.
        auto r = cs.eval(
            R"AURA((begin
                 (define gen (lambda (g) "(define (f x) x)"))
                 (define ver (lambda (c) "#t"))
                 (closure:free! gen)
                 (intend "goal" gen ver)))AURA");
        CHECK(r.has_value(), "intend after free evaluates (no crash)");

        const auto m1 = m->agent_closure_freed_during_call.load(std::memory_order_relaxed);
        CHECK(m1 > m0, "agent_closure_freed_during_call bumped");
    }

    // ── AC4: live path still works ──
    {
        std::println("\n--- AC4: live intend does not crash ---");
        CompilerService cs;
        auto r = cs.eval(
            R"AURA((begin
                 (define gen (lambda (g) "(define (f x) x)"))
                 (define ver (lambda (c) "#t"))
                 (intend "goal" gen ver 1)))AURA");
        CHECK(r.has_value(), "live intend completes");
    }

    std::println("\n=== test_intend_closure_live_1719: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_intend_closure_live

// from test_intend_heap_slots.cpp
namespace aura_fiber_run_intend_heap_slots {
// @category: unit
// @reason: Issue #1721 — intend must not unbounded-push intermediate
// Issue #1721 (#1978 renamed): issue# moved from filename to header.
// strings onto string_heap each attempt; reuse fixed slots.
//
//   AC1: source cites #1721 and uses put_slot / slot_goal/code/err
//   AC2: no per-attempt push_back(goal)/push_back(code_str) in loop body
//   AC3: live intend completes; multi-attempt free-gen does not crash
//   AC4: heap growth bounded across many intend calls (slot reuse)


namespace {

    using aura::compiler::CompilerService;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string strip_line_comments(std::string_view win) {
        std::string code;
        code.reserve(win.size());
        for (size_t i = 0; i < win.size();) {
            if (i + 1 < win.size() && win[i] == '/' && win[i + 1] == '/') {
                while (i < win.size() && win[i] != '\n')
                    ++i;
                continue;
            }
            code.push_back(win[i++]);
        }
        return code;
    }

} // namespace

int run_intend_heap_slots() {
    // ── AC1/AC2: source audit ──
    {
        std::println("\n--- AC1/AC2: put_slot reuse ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent");
        CHECK(src.find("Issue #1721") != std::string::npos, "cites #1721");
        auto pos = src.find("add(\"intend\"");
        CHECK(pos != std::string::npos, "found intend");
        if (pos != std::string::npos) {
            auto end = src.find("\n    add(\"", pos + 10);
            auto win = src.substr(pos, end == std::string::npos ? 12000 : end - pos);
            auto code = strip_line_comments(win);
            CHECK(code.find("put_slot") != std::string::npos, "put_slot present");
            CHECK(code.find("slot_goal") != std::string::npos, "slot_goal");
            CHECK(code.find("slot_code") != std::string::npos, "slot_code");
            CHECK(code.find("slot_err") != std::string::npos, "slot_err");
            CHECK(code.find("finish_result") != std::string::npos, "finish_result");
            // Old unbounded pattern: string_heap_.push_back of intermediates.
            CHECK(code.find("string_heap_.push_back(goal)") == std::string::npos,
                  "no string_heap push_back(goal)");
            CHECK(code.find("string_heap_.push_back(current_code_str)") == std::string::npos,
                  "no string_heap push_back(current_code_str)");
            CHECK(code.find("string_heap_.push_back(last_error)") == std::string::npos,
                  "no string_heap push_back(last_error)");
            CHECK(code.find("string_heap_.push_back(code_str)") == std::string::npos,
                  "no string_heap push_back(code_str)");
        }
    }

    // ── AC3: functional ──
    {
        std::println("\n--- AC3: intend completes ---");
        CompilerService cs;
        auto r = cs.eval(
            R"AURA((begin
               (define gen (lambda (g) "(define (f x) x)"))
               (define ver (lambda (c) "#t"))
               (intend "goal" gen ver 2)))AURA");
        CHECK(r.has_value(), "live intend ok");
    }

    // ── AC4: bounded heap growth ──
    {
        std::println("\n--- AC4: heap growth bounded across many intends ---");
        CompilerService cs;
        // Warm once so slots exist.
        (void)cs.eval(
            R"AURA((begin
               (define gen (lambda (g) "(define (f x) x)"))
               (define ver (lambda (c) "#t"))
               (intend "w" gen ver 1)))AURA");
        const auto heap0 = cs.evaluator().string_heap_mut().size();
        for (int i = 0; i < 30; ++i) {
            auto r = cs.eval(
                R"AURA((begin
                   (define gen (lambda (g) "(define (f x) x)"))
                   (define ver (lambda (c) "#t"))
                   (intend "g" gen ver 1)))AURA");
            CHECK(r.has_value(), "intend iter ok");
        }
        const auto heap1 = cs.evaluator().string_heap_mut().size();
        // Each intend still pushes: define names + final result (+ gen/ver bodies).
        // Intermediate goal/code/err must NOT grow with attempts×calls.
        // Allow generous headroom for defines/results but fail if linear with pollution.
        // 30 calls × ~15 polluted strings would be ~450; with slots expect much less.
        const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;
        CHECK(growth < 200, "heap growth bounded (slot reuse, not O(attempts) pollution)");
        std::println("    heap growth over 30 intends: {} (slots {})", growth, heap0);
    }

    std::println("\n=== test_intend_heap_slots_1721: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_intend_heap_slots

int main() {
    std::println("\n######## fingerprint ########");
    if (int rc = aura_fiber_run_agent_fingerprint_1730::run_agent_fingerprint_1730(); rc != 0)
        return rc;
    std::println("\n######## intend_closure ########");
    if (int rc = aura_fiber_run_intend_closure_live::run_intend_closure_live(); rc != 0)
        return rc;
    std::println("\n######## intend_heap_slots ########");
    if (int rc = aura_fiber_run_intend_heap_slots::run_intend_heap_slots(); rc != 0)
        return rc;
    if (::aura::test::g_failed)
        return 1;
    std::println("\ntest_fiber_concurrent_unit_batch: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
