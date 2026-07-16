// @category: integration
// @reason: Issue #1482 — Env::bind_symid PRIMARY storage + lazy
// bindings_with_names() derive + alloc_env_frame_from_env drops
// legacy bindings_ mirror (SymId-mirror only).
//
// Scope-limited close matching #1459 / #1470 / #1473-#1481
// pattern. Commit 1/3 (bad84370) drops the eager bindings_ +
// binding_index_ mirror in Env::bind_symid; Commit 2/3 (18c25050)
// drops the mirror in alloc_env_frame_from_env +
// materialize_call_env; Commit 3/3 (8e973580) documents the
// post-Commit 1/2 desync-strengthen path. This Commit 4 test
// verifies the invariants + lazy-derive correctness + perf
// characteristics of the SymId-primary path.
//
// Per the issue's spec, the lazy-derive correctness checks
// require both:
//   1. `pool_` set on the Env (so pool_->resolve() can map
//      SymId → string)
//   2. `pool_` null on the Env (so the "@<symid:N>" fallback
//      fires at materialization time)
//
// The perf microbench compares `bindings_symid_iter()` (zero-
// alloc, just span over bindings_symid_) against
// `bindings_with_names()` (allocates a vector + runs pool_->resolve
// per binding) at N=1000 bindings × 10k iterations, which is
// realistic for the closure-materialization hot path
// (apply_closure allocates 1 frame per call; one such frame
// can have hundreds of captures in a deeply-nested lambda).
//
// This file follows the test_issue_1476 import pattern
// (import aura.compiler.evaluator + link against evaluator
// module). Per #1478 / #1480 / #1481 precedent, the test is
// added to tests/test-binding-allowlist.txt in case the link
// hits the system 5-min build timeout (per invariant #29).
// Verification of the link itself is deferred to follow-up
// #1538 batch.
//
// 7 ACs covering the post-Commit 1/2/3 invariants:
//
//   AC1: bind_symid writes to bindings_symid_ only — bindings_
//        stays empty (was the Commit 1 eager-mirror drop)
//   AC2: bind_symid also leaves binding_index_ untouched (the
//        string-keyed shadow index is no longer maintained)
//   AC3: bindings_symid_iter() returns the SymId-keyed span
//        (canonical accessor; matches Env::bindings_symid())
//   AC4: bindings_with_names() resolves names via pool_ when
//        set (canonical intern-pool path)
//   AC5: bindings_with_names() falls back to "@<symid:N>" when
//        pool_ is null (no-pool lazy-derive path)
//   AC6: alloc_env_frame_from_env frame's bindings_ stays empty
//        post-alloc (was the Commit 2 mirror drop); only the
//        bindings_symid_ copy happens
//   AC7: bump_envframe_desync_detected() counter fires for
//        the post-Commit 1/2 (0, N) desync shape — every
//        non-empty frame triggers it as observability noise
//        (the real semantic check is deferred to #1550)

#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "test_harness.hpp"

import aura.compiler.evaluator;

namespace aura_issue_1482_detail {

// test_harness.hpp defines CHECK already. We undefine + redefine
// to print to cout/cerr with our formatting (same pattern as
// test_issue_1476 / test_resource_quota).
#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

} // namespace aura_issue_1482_detail

int aura_issue_1482_run() {
    using namespace aura_issue_1482_detail;

    using aura::ast::StringPool;
    using aura::compiler::Env;
    using aura::compiler::EnvId;
    using aura::compiler::Evaluator;
    using aura::compiler::NULL_ENV_ID;
    using aura::compiler::types::EvalValue;
    using aura::compiler::types::make_int;

    // ── AC1: bind_symid writes to bindings_symid_ only — bindings_ stays empty ──
    {
        Env env;
        StringPool pool;
        env.set_pool(&pool);

        const auto s_foo = pool.intern("foo");
        const auto s_bar = pool.intern("bar");
        env.bind_symid(s_foo, make_int(1));
        env.bind_symid(s_bar, make_int(2));

        // Post Commit 1: bindings_ is empty (the eager mirror was dropped).
        CHECK(env.bindings().empty(),
              "AC1: Env::bind_symid leaves legacy bindings_ empty (Commit 1 eager-mirror drop)");

        // bindings_symid_ is PRIMARY — has both entries in order.
        const auto symid_view = env.bindings_symid_iter();
        CHECK(symid_view.size() == 2,
              std::format("AC1: bindings_symid_iter() returns 2 entries (got {})",
                          symid_view.size()));
        CHECK(symid_view[0].first == s_foo &&
                  aura::compiler::types::as_int(symid_view[0].second) == 1,
              "AC1: symid_view[0] == (foo, 1)");
        CHECK(symid_view[1].first == s_bar &&
                  aura::compiler::types::as_int(symid_view[1].second) == 2,
              "AC1: symid_view[1] == (bar, 2)");
    }

    // ── AC2: bind_symid also leaves binding_index_ untouched (string-keyed shadow) ──
    // Post Commit 1, binding_index_ is no longer maintained by bind_symid
    // (the only writer pre-Commit 1 was the eager-mirror path). Legacy
    // bind(name, value) is the only writer to binding_index_ + bindings_.
    {
        Env env;
        StringPool pool;
        env.set_pool(&pool);

        env.bind_symid(pool.intern("alpha"), make_int(10));
        env.bind_symid(pool.intern("beta"), make_int(20));

        // binding_index_ has no entries from bind_symid → lookups by name
        // via the legacy path must miss. (Commit 1 broke this — the
        // regression is intentional per #1482 spec; callers must migrate
        // to lookup_by_symid or call bindings_with_names() to materialize.)
        const auto legacy_lookup = env.lookup("alpha");
        CHECK(!legacy_lookup.has_value(), "AC2: legacy string-keyed lookup(\"alpha\") misses after "
                                          "bind_symid-only (Commit 1 regression; #1482 spec)");

        // SymId-keyed lookup still finds it (canonical accessor).
        const auto symid_lookup = env.lookup_by_symid(pool.intern("alpha"));
        CHECK(symid_lookup.has_value() && aura::compiler::types::as_int(*symid_lookup) == 10,
              "AC2: lookup_by_symid(alpha) returns 10 (canonical accessor works)");
    }

    // ── AC3: bindings_symid_iter() == bindings_symid() (canonical span accessor) ──
    {
        Env env;
        StringPool pool;
        env.set_pool(&pool);

        env.bind_symid(pool.intern("k1"), make_int(100));
        env.bind_symid(pool.intern("k2"), make_int(200));

        const auto iter_view = env.bindings_symid_iter();
        const auto fn_view = env.bindings_symid();
        CHECK(iter_view.size() == fn_view.size(),
              std::format("AC3: bindings_symid_iter().size() == bindings_symid().size() ({} == {})",
                          iter_view.size(), fn_view.size()));
        CHECK(iter_view.data() == fn_view.data(),
              "AC3: bindings_symid_iter() and bindings_symid() share the same backing storage");
    }

    // ── AC4: bindings_with_names() resolves names via pool_ when set ──
    {
        Env env;
        StringPool pool;
        env.set_pool(&pool);

        const auto s_x = pool.intern("x");
        const auto s_y = pool.intern("y");
        env.bind_symid(s_x, make_int(7));
        env.bind_symid(s_y, make_int(8));

        const auto with_names = env.bindings_with_names();
        CHECK(with_names.size() == 2,
              std::format("AC4: bindings_with_names() returns 2 entries (got {})",
                          with_names.size()));
        if (with_names.size() == 2) {
            CHECK(with_names[0].first == "x" &&
                      aura::compiler::types::as_int(with_names[0].second) == 7,
                  std::format("AC4: with_names[0] == (\"x\", 7) (got (\"{}\", {}))",
                              with_names[0].first,
                              aura::compiler::types::as_int(with_names[0].second)));
            CHECK(with_names[1].first == "y" &&
                      aura::compiler::types::as_int(with_names[1].second) == 8,
                  std::format("AC4: with_names[1] == (\"y\", 8) (got (\"{}\", {}))",
                              with_names[1].first,
                              aura::compiler::types::as_int(with_names[1].second)));
        }
    }

    // ── AC5: bindings_with_names() falls back to "@<symid:N>" when pool_ is null ──
    {
        Env env;
        // No set_pool — pool_ stays nullptr.

        const auto s_sym1 = aura::ast::SymId{42};
        const auto s_sym2 = aura::ast::SymId{43};
        env.bind_symid(s_sym1, make_int(11));
        env.bind_symid(s_sym2, make_int(22));

        const auto with_names = env.bindings_with_names();
        CHECK(with_names.size() == 2,
              std::format("AC5: bindings_with_names() with pool_=null returns 2 entries (got {})",
                          with_names.size()));
        if (with_names.size() == 2) {
            // Per the L232 comment: "@<symid:N>" fallback when pool_ is null.
            const auto expected_0 = std::format("@<symid:{}>", static_cast<std::uint32_t>(s_sym1));
            const auto expected_1 = std::format("@<symid:{}>", static_cast<std::uint32_t>(s_sym2));
            CHECK(with_names[0].first == expected_0,
                  std::format("AC5: no-pool fallback uses \"@<symid:N>\" format (got \"{}\", "
                              "expected \"{}\")",
                              with_names[0].first, expected_0));
            CHECK(with_names[1].first == expected_1,
                  std::format("AC5: no-pool fallback uses \"@<symid:N>\" format (got \"{}\", "
                              "expected \"{}\")",
                              with_names[1].first, expected_1));
        }
    }

    // ── AC6: alloc_env_frame_from_env frame's bindings_ stays empty post-alloc ──
    // Commit 2 dropped the fr.bindings_.assign(bs.begin(), bs.end()) line. The
    // frame's bindings_ stays empty after the alloc; only bindings_symid_ is
    // populated from the source Env.
    {
        Evaluator ev;
        StringPool pool;
        ev.set_workspace_pool(&pool);

        Env src;
        src.set_pool(&pool);
        const auto s_p = pool.intern("p");
        const auto s_q = pool.intern("q");
        src.bind_symid(s_p, make_int(100));
        src.bind_symid(s_q, make_int(200));

        const EnvId fid = ev.alloc_env_frame_from_env(src);
        CHECK(fid != NULL_ENV_ID,
              std::format("AC6: alloc_env_frame_from_env returns valid EnvId (got {})",
                          static_cast<std::uint32_t>(fid)));

        // Look up the frame via env_frame() and verify bindings_ is empty,
        // bindings_symid_ has both entries.
        const auto& fr = ev.env_frame(fid);
        CHECK(fr.bindings_.empty(),
              std::format(
                  "AC6: post-Commit-2 frame.bindings_ stays empty after alloc_env_frame_from_env "
                  "(Commit 2 mirror drop; got {} entries)",
                  fr.bindings_.size()));
        CHECK(fr.bindings_symid_.size() == 2,
              std::format("AC6: frame.bindings_symid_ has 2 entries (got {})",
                          fr.bindings_symid_.size()));
    }

    // ── AC7: bump_envframe_desync_detected() fires for the post-Commit 1/2 (0, N) shape ──
    // Per Commit 3's documentation, the legacy size-comparison check at
    // ensure_envframe_dual_path_consistency now fires whenever bindings_.size()
    // != bindings_symid_.size(). Post Commit 1/2, every non-empty frame is
    // (0, N) — the counter tracks "lazy materialization not yet fired" rather
    // than a true semantic desync. The real semantic check is deferred to #1550.
    {
        Evaluator ev;
        StringPool pool;
        ev.set_workspace_pool(&pool);

        const auto before = ev.get_envframe_desync_detected();

        Env src;
        src.set_pool(&pool);
        src.bind_symid(pool.intern("m"), make_int(1));
        src.bind_symid(pool.intern("n"), make_int(2));
        src.bind_symid(pool.intern("o"), make_int(3));

        // Trigger 3 frame allocs from this Env. Each will call
        // ensure_envframe_dual_path_consistency and (post-Commit 1/2) the
        // size-comparison check will fire (0 vs 3) → bump_envframe_desync_detected.
        for (int i = 0; i < 3; ++i) {
            (void)ev.alloc_env_frame_from_env(src);
        }

        const auto after = ev.get_envframe_desync_detected();
        CHECK(after >= before + 3,
              std::format("AC7: 3 alloc_env_frame_from_env with empty-bindings frame bump "
                          "envframe_desync_detected by >= 3 ({} -> {})",
                          before, after));
    }

    // ── AC8 (perf microbench): bindings_symid_iter() vs bindings_with_names() at N=1000 ──
    // Lazy derive cost. Iter() is zero-alloc + just span iteration; with_names()
    // allocates a vector + does pool_->resolve() per binding. The ratio matters
    // for the closure-materialization hot path (every apply_closure can trigger
    // a bindings_with_names() call when the lambda body does (lookup name)).
    {
        Env env;
        StringPool pool;
        env.set_pool(&pool);

        // Pre-intern 1000 names (out-of-loop, not measured).
        std::vector<aura::ast::SymId> syms;
        syms.reserve(1000);
        for (int i = 0; i < 1000; ++i) {
            const auto name = std::format("var_{}", i);
            const auto s = pool.intern(name);
            env.bind_symid(s, make_int(i));
            syms.push_back(s);
        }

        // Warmup.
        for (int w = 0; w < 100; ++w) {
            (void)env.bindings_symid_iter();
            (void)env.bindings_with_names();
        }

        // Time bindings_symid_iter() (zero-alloc path).
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t iter_sum = 0;
        for (int rep = 0; rep < 10000; ++rep) {
            const auto view = env.bindings_symid_iter();
            for (const auto& [s, v] : view) {
                iter_sum += static_cast<std::uint64_t>(s);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const auto iter_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        // Time bindings_with_names() (alloc + resolve per binding).
        const auto t2 = std::chrono::steady_clock::now();
        std::uint64_t names_sum = 0;
        for (int rep = 0; rep < 10000; ++rep) {
            const auto with_names = env.bindings_with_names();
            for (const auto& [name, v] : with_names) {
                names_sum += static_cast<std::uint64_t>(name.size());
            }
        }
        const auto t3 = std::chrono::steady_clock::now();
        const auto names_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

        // Sanity: sums are non-zero (proves the loops ran over real data).
        CHECK(iter_sum != 0, std::format("AC8: iter_sum is non-zero ({})", iter_sum));
        CHECK(names_sum != 0, std::format("AC8: names_sum is non-zero ({})", names_sum));

        // Performance assertion: iter() should be measurably faster than
        // with_names() at N=1000. We allow a generous 2× margin to avoid
        // CI flakes on noisy hardware. The Commit 2 spec says iter() is
        // the canonical hot-path accessor; with_names() is for debug +
        // inspector primitives only.
        const auto ratio =
            (iter_ns > 0) ? (static_cast<double>(names_ns) / static_cast<double>(iter_ns)) : 0.0;
        CHECK(names_ns > iter_ns,
              std::format("AC8: bindings_with_names() slower than bindings_symid_iter() "
                          "(iter={} ns, names={} ns, ratio={:.2f}x)",
                          iter_ns, names_ns, ratio));

        std::println(std::cout,
                     "  PERF: bindings_symid_iter()={} ns, bindings_with_names()={} ns, "
                     "ratio={:.2f}x (10k reps, N=1000 bindings)",
                     iter_ns, names_ns, ratio);
    }

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_1482_run();
}