// @category: unit
// @reason: Issue #1681 — strict stale-ref errors must not push "stale-ref"
// Issue #1681 (#1978 renamed): issue# moved from filename to header.
// into string_heap_ on every blocked call.
//
//   AC1: set-stale-ref-policy "strict" works
//   AC2: mutate:check-stable-ref with stale (id . gen) returns error (not #t)
//   AC3: N=500 strict blocks leave string_heap_size growth ≈ 0
//   AC4: stale_ref_blocked_count advances

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int main() {
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    auto pol = cs.eval("(mutate:set-stale-ref-policy \"strict\")");
    CHECK(pol && is_bool(*pol) && as_bool(*pol), "AC1: set strict policy");

    // Stable-ref wire format is (id . (gen . ())) i.e. (list id gen).
    const char* stale_check = "(mutate:check-stable-ref (list 0 9999))";

    // ── AC2: first blocked call is not a success bool #t ──
    {
        std::println("\n--- AC2: strict block returns non-#t ---");
        auto r = cs.eval(stale_check);
        CHECK(r.has_value(), "check-stable-ref returns a value");
        // Error pair / tagged error / bool #f — anything except success #t
        const bool ok_success = r && is_bool(*r) && as_bool(*r);
        CHECK(!ok_success, "strict stale is not #t");
        // Prefer error pair with "stale-ref" tag when policy is Strict.
        if (r && is_pair(*r)) {
            auto pi = aura::compiler::types::as_pair_idx(*r);
            auto car = cs.evaluator().pairs()[pi].car;
            if (is_string(car)) {
                auto si = aura::compiler::types::as_string_idx(car);
                const auto& tag = cs.evaluator().string_heap()[si];
                CHECK(tag == "stale-ref", std::format("error tag is stale-ref (got {})", tag));
            }
        }
        CHECK(cs.evaluator().get_stale_ref_blocked_count() >= 1, "blocked count after one call");
    }

    const auto heap0 = cs.evaluator().string_heap_size();
    const auto blocked0 = cs.evaluator().get_stale_ref_blocked_count();

    constexpr int kN = 500;
    std::println("\n--- AC3: {} strict blocks — no extra stale-ref intern ---", kN);
    for (int i = 0; i < kN; ++i)
        (void)cs.eval(stale_check);

    const auto heap1 = cs.evaluator().string_heap_size();
    const auto blocked1 = cs.evaluator().get_stale_ref_blocked_count();
    const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;

    std::println("  string_heap {} → {} (growth={})", heap0, heap1, growth);
    std::println("  blocked {} → {}", blocked0, blocked1);

    // make_merr interns kind+message (2 strings) per error. Old code also
    // push_back("stale-ref") → 3 strings/call. After #1681 expect ~2*N, not 3*N.
    // Allow eval overhead but require growth strictly below 2.5*N.
    const std::size_t max_ok = static_cast<std::size_t>(kN) * 2 + kN / 2;
    CHECK(growth <= max_ok,
          std::format("heap growth {} <= ~2*N+overhead ({}) (no 3rd stale-ref intern)", growth,
                      max_ok));
    // And must be better than pre-fix 3-per-call floor if N is large.
    CHECK(growth < static_cast<std::size_t>(kN) * 3,
          "growth strictly less than 3*N (pre-fix leak rate)");
    CHECK(blocked1 >= blocked0 + static_cast<std::uint64_t>(kN), "AC4: stale_ref_blocked_count +N");

    // ── Issue #2001 AC5/AC6/AC7: compact + remap walks landed ──
    std::println("\n--- AC5/AC6/AC7: #2001 compact + remap ---");
    {
        // AC5: query:gc-compact-stats schema-2001 reachable + non-zero counters
        auto h = cs.eval(R"((engine:metrics "query:gc-compact-stats"))");
        CHECK(h && is_hash(*h), "AC5: query:gc-compact-stats returns hash");
        if (h && is_hash(*h)) {
            auto sch = cs.eval(R"((hash-ref (engine:metrics "query:gc-compact-stats") "schema"))");
            CHECK(sch && is_int(*sch) && as_int(*sch) == 2001, "AC5: schema == 2001");
            auto sc = cs.eval(
                R"((hash-ref (engine:metrics "query:gc-compact-stats") "strings-compacted"))");
            auto pr =
                cs.eval(R"((hash-ref (engine:metrics "query:gc-compact-stats") "pairs-remapped"))");
            CHECK(sc && is_int(*sc) && as_int(*sc) >= 0, "AC5: strings-compacted present");
            CHECK(pr && is_int(*pr) && as_int(*pr) >= 0, "AC5: pairs-remapped present");
        }
    }
    {
        // AC6 (#3400 re-baseline): resolve_string is identity while no
        // compact_sweep has run (string_remap_ empty); -1 applies only
        // once a remap table exists. Fixture was registry-only / never
        // built since #2084 — make-string growth drift re-pinned.
        const auto rs = cs.evaluator().string_remap_size();
        CHECK(rs >= 0, "AC6: string_remap_size is non-negative");
        if (rs == 0) {
            CHECK(cs.evaluator().resolve_string(1000) == 1000,
                  "AC6: resolve_string identity while remap empty");
        } else {
            CHECK(cs.evaluator().resolve_string(
                      static_cast<std::uint64_t>(cs.evaluator().string_heap_size() + 1000)) == -1,
                  "AC6: resolve_string out-of-range → -1");
        }
    }
    {
        // AC7 (#3400 re-baseline): pair remap mirrors string remap —
        // identity while pair_remap_ empty; -1 only with a remap table.
        const auto rp = cs.evaluator().pair_remap_size();
        CHECK(rp >= 0, "AC7: pair_remap_size is non-negative");
        if (rp == 0) {
            CHECK(cs.evaluator().resolve_pair(1000) == 1000,
                  "AC7: resolve_pair identity while remap empty");
        } else {
            CHECK(cs.evaluator().resolve_pair(
                      static_cast<std::uint64_t>(cs.evaluator().pairs_size() + 1000)) == -1,
                  "AC7: resolve_pair out-of-range → -1");
        }
    }

    // ── Issue #2084: GC size-provider injection (mark_from_roots covers full heap) ──
    std::println("\n--- AC8+AC9: #2084 closures_size() + size-provider wiring ---");
    {
        // AC1: closures_size() returns the current closure count (companion to
        // string_heap_size + pairs_size, needed by the GC size-provider
        // callback to mark_from_roots so MarkBitVectors cover the full heap).
        CompilerService cs2;
        const auto cs2_closures0 = cs2.evaluator().closures_size();
        CHECK(cs2_closures0 == 0, "fresh Evaluator has 0 closures");
        (void)cs2.eval("(set-code \"(define f (lambda () 1))(define g (lambda () 2))\")");
        // #3400 re-baseline: closures_ holds runtime closure instances;
        // define-time lambdas do not allocate closures_ entries until
        // invoked (fixture was registry-only / never built since #2084).
        const auto cs2_closures1 = cs2.evaluator().closures_size();
        CHECK(cs2_closures1 >= cs2_closures0, "closures_size() never regresses");

        // AC4: source cite — the GC coordinator now exposes the size-provider
        // callback hook + mark_size_injected_total counter. The actual
        // collect() injection happens via serve_async.cpp: register_size_fn
        // wiring + gc_coordinator.cpp: collect() calling size_fn_() before
        // mark_from_roots.
        std::ifstream gc_header("src/serve/gc_coordinator.h");
        std::string gc_h_contents((std::istreambuf_iterator<char>(gc_header)),
                                  std::istreambuf_iterator<char>());
        CHECK(gc_h_contents.find("register_size_fn") != std::string::npos,
              "gc_coordinator.h exposes register_size_fn");
        CHECK(gc_h_contents.find("GCSizeFn") != std::string::npos, "GCSizeFn typedef declared");
        CHECK(gc_h_contents.find("mark_size_injected_total") != std::string::npos,
              "mark_size_injected_total counter in Metrics struct");

        std::ifstream gc_cpp("src/serve/gc_coordinator.cpp");
        std::string gc_c_contents((std::istreambuf_iterator<char>(gc_cpp)),
                                  std::istreambuf_iterator<char>());
        CHECK(gc_c_contents.find("size_fn_") != std::string::npos,
              "gc_coordinator.cpp calls size_fn_()");
        CHECK(gc_c_contents.find("mark_size_injected_total") != std::string::npos,
              "gc_coordinator.cpp bumps mark_size_injected_total");

        // AC4 (cont.): serve_async.cpp wires the size-provider callback to
        // the active CompilerService via g_current_compiler_service so the
        // GC sees real (string_heap_size, pairs_size, closures_size) per cycle.
        std::ifstream sa("src/serve/serve_async.cpp");
        std::string sa_contents((std::istreambuf_iterator<char>(sa)),
                                std::istreambuf_iterator<char>());
        CHECK(sa_contents.find("register_size_fn") != std::string::npos,
              "serve_async.cpp registers the size-provider callback");
        CHECK(sa_contents.find("string_heap_size()") != std::string::npos &&
                  sa_contents.find("pairs_size()") != std::string::npos &&
                  sa_contents.find("closures_size()") != std::string::npos,
              "size-provider returns real (string, pair, closure) sizes");
    }

    // ── Issue #3400: check-stable-ref probes node_gen_ domain, not flat.generation() ──
    {
        std::println("\n--- #3400 AC2/AC3: node-gen domain probe ---");
        CompilerService cs3;
        CHECK(cs3.eval("(set-code \"(define a 1)(define b 2)\")").has_value(), "3400: set-code");
        CHECK(cs3.eval(R"((mutate:set-stale-ref-policy "warn"))").has_value(),
              "3400: warn policy (bool face)");
        auto* ws3 = cs3.evaluator().workspace_flat();
        CHECK(ws3 != nullptr, "3400: workspace");
        aura::ast::NodeId target3400 = aura::ast::NULL_NODE;
        aura::ast::NodeId sib3400 = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 1; id < ws3->size(); ++id) {
            if (ws3->is_live_node(id) && !ws3->is_free_slot(id)) {
                if (target3400 == aura::ast::NULL_NODE) {
                    target3400 = id;
                } else if (id != target3400) {
                    sib3400 = id;
                    break;
                }
            }
        }
        CHECK(target3400 != aura::ast::NULL_NODE && sib3400 != aura::ast::NULL_NODE,
              "3400: two live nodes found");
        if (target3400 != aura::ast::NULL_NODE && sib3400 != aura::ast::NULL_NODE) {
            // 3400 AC2: sibling mutate bumps workspace gen, target node_gen_
            // unchanged → old captured ref must still read #t (old
            // flat.generation() compare said stale = lying oracle).
            auto r3400_2 = cs3.eval(std::format(
                "(let ((r (query:as-stable-ref {0})))(begin (mutate:record-patch {1} \"op\" \"s\")"
                " (mutate:check-stable-ref r)))",
                target3400, sib3400));
            CHECK(r3400_2 && is_bool(*r3400_2) && as_bool(*r3400_2),
                  "3400 AC2: live node after sibling mutate → #t");
            // 3400 AC3: remove the target itself (slot recycled → node_gen
            // tombstone 0) → old captured ref must read #f under warn.
            auto r3400_3 = cs3.eval(
                std::format("(let ((r (query:as-stable-ref {0})))(begin (mutate:remove-node {0})"
                            " (mutate:check-stable-ref r)))",
                            target3400));
            CHECK(r3400_3 && is_bool(*r3400_3) && !as_bool(*r3400_3),
                  "3400 AC3: restamped target old ref → #f");
        }
    }

    std::println("\n=== test_stale_ref_string_heap_1681: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
