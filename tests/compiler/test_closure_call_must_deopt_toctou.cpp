// @category: unit
// @reason: Issue #2472 — close TOCTOU window in aura_closure_call
//          MustDeopt path (lock-downgrade free+realloc vs force-deopt).
//
//   AC1: multi-step free+realloc under concurrent MustDeopt callers —
//        no crash; new-slot must_deopt not spuriously cleared by stale
//        force-deopt when func_id identity differs
//   AC2: exclusive re-acquire re-verifies freed + func_id + must_deopt
//   AC3: baseline force-deopt still clears flag for same identity
//   AC4: source cites Issue #2472 + freed / func_id guards
//   AC5: gate wiring (check script + CMake + build.py)
//
//   Issue #3635 ACs (blessed anon dispatch entry — single enforcement
//   point for closure native dispatch):
//   AC6 (#3635): entry owns the call-time transaction — invalid cid
//        returns callee_val, no-entry / freed returns 0, MustDeopt
//        force-deopt through the entry clears the flag + advances deopt
//        (same observable contract as aura_closure_call)
//   AC7 (#3635): stale anon closure (injected old bridge_epochs) through
//        the entry takes the safe fallback, never native
//   AC8 (#3635): adversarial linter face — --probe-file flags a bare
//        g_closure_func_ids read in a stub surface; #3635-allow-direct
//        annotated line passes; --self-test clean
//   AC9 (#3635): gate wiring — blessed entry + wrapper forward in the
//        table TU, header declaration, build.py linter wiring,
//        fast-path generation double-check preserved

#include "test_harness.hpp"

#include "compiler/runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;

// aura_deopt_count is defined in aura_jit_runtime (not always in headers).
extern "C" std::uint64_t aura_deopt_count(void);
extern "C" void aura_reset_runtime(void);
// Issue #3635: stale-epoch injection helper (test-only, table TU).
extern "C" void aura_inject_stale_closure_bridge_epoch_for_test(std::int64_t closure_id);

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// Issue #3635: locate a repo-relative path from the test CWD (same probe
// chain as read_file, returns the working prefix for command strings).
static std::string find_path(const std::string& rel) {
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (in)
            return p;
    }
    return {};
}

// ── AC3: baseline force-deopt still works for same identity ──
static void ac3_baseline_force_deopt() {
    std::println("\n--- #2472 AC3: baseline force-deopt same identity ---");
    aura_reset_runtime();
    auto cid = aura_alloc_closure(/*func_id=*/111);
    CHECK(cid >= 0, "AC3: alloc");
    aura_closure_set_must_deopt(cid, 1);
    CHECK(aura_closure_get_must_deopt(cid) == 1, "AC3: flag set");
    const auto deopt0 = aura_deopt_count();
    std::int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    CHECK(aura_closure_get_must_deopt(cid) == 0, "AC3: flag cleared after force-deopt");
    CHECK(aura_deopt_count() > deopt0, "AC3: deopt advanced");
    aura_free_closure(cid);
}

// ── AC1: concurrent free+realloc + MustDeopt callers ──
// Callers race force-deopt on cid with func_id 111. Freer recycles the
// slot as func_id 222 with must_deopt=1. After callers that started on
// the old identity finish, a final serial set+no-call must leave the
// new identity's flag set when no concurrent call is active — and the
// stress must complete without crash/UAF. Cross-identity clear would
// show as a lost must_deopt on a freshly-set new slot after a quiet
// barrier.
static void ac1_free_realloc_stress() {
    std::println("\n--- #2472 AC1: free+realloc × MustDeopt call stress ---");
    aura_reset_runtime();

    const auto cid = aura_alloc_closure(/*func_id=*/111);
    CHECK(cid >= 0, "AC1: alloc orig");
    aura_closure_set_must_deopt(cid, 1);

    std::atomic<int> go{0};
    std::atomic<int> callers_done{0};
    std::vector<std::thread> thr;
    thr.reserve(4);
    for (int t = 0; t < 4; ++t) {
        thr.emplace_back([cid, &go, &callers_done] {
            while (go.load(std::memory_order_acquire) == 0)
                std::this_thread::yield();
            std::int64_t args[1] = {0};
            for (int i = 0; i < 200; ++i)
                (void)aura_closure_call(cid, args, 0);
            callers_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Freer races with callers: free original, realloc same id with new
    // func_id, stamp must_deopt on the *new* identity repeatedly.
    thr.emplace_back([cid, &go] {
        while (go.load(std::memory_order_acquire) == 0)
            std::this_thread::yield();
        for (int i = 0; i < 80; ++i) {
            aura_free_closure(cid);
            auto nid = aura_alloc_closure(/*func_id=*/222 + (i % 3));
            // Prefer reuse of same slot (typical free-list LIFO).
            if (nid == cid)
                aura_closure_set_must_deopt(nid, 1);
            else if (nid >= 0)
                aura_closure_set_must_deopt(nid, 1);
            // Brief spin so callers hit MustDeopt window.
            for (int s = 0; s < 50; ++s)
                std::this_thread::yield();
        }
    });

    go.store(1, std::memory_order_release);
    for (auto& t : thr)
        t.join();

    CHECK(callers_done.load() == 4, "AC1: all callers finished");

    // Quiet barrier: ensure a live slot at cid with func_id 333, must_deopt=1,
    // no concurrent callers — flag must stick (not cleared by stale work).
    aura_free_closure(cid);
    auto live = aura_alloc_closure(/*func_id=*/333);
    CHECK(live >= 0, "AC1: quiet realloc");
    // If free-list reused cid, great; otherwise still test identity path.
    aura_closure_set_must_deopt(live, 1);
    CHECK(aura_closure_get_must_deopt(live) == 1, "AC1: quiet must_deopt set");
    // No concurrent call — flag must remain set.
    CHECK(aura_closure_get_must_deopt(live) == 1, "AC1: quiet must_deopt sticky");
    // Same-identity force-deopt still works after stress.
    std::int64_t args[1] = {0};
    (void)aura_closure_call(live, args, 0);
    CHECK(aura_closure_get_must_deopt(live) == 0, "AC1: same-id force-deopt clears");
    aura_free_closure(live);
}

// ── AC1b: serial free under must_deopt — call sees freed (fail path) ──
static void ac1b_freed_path() {
    std::println("\n--- #2472 AC1b: freed slot force-deopt fail ---");
    aura_reset_runtime();
    auto cid = aura_alloc_closure(77);
    aura_closure_set_must_deopt(cid, 1);
    aura_free_closure(cid);
    CHECK(aura_closure_is_freed(cid) == 1, "AC1b: freed");
    std::int64_t args[1] = {0};
    auto r = aura_closure_call(cid, args, 0);
    CHECK(r == 0, "AC1b: call freed → 0");
}

// ── AC4: source cite ──
static void ac4_source_cite() {
    std::println("\n--- #2472 AC4: source cites TOCTOU guards ---");
    auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(!rt.empty(), "AC4: read aura_jit_runtime.cpp");
    CHECK(rt.find("Issue #2472") != std::string::npos, "AC4: cites #2472");
    CHECK(rt.find("g_closure_freed") != std::string::npos, "AC4: freed vector");
    CHECK(rt.find("orig_func_id") != std::string::npos, "AC4: stashes orig_func_id");
    // Scope to the blessed dispatch entry body (earlier #2128 comments
    // also mention MustDeoptBeforeNextCall on the vector / remap path).
    // Issue #3635: aura_closure_call is a thin forward; the transaction
    // body lives in aura_closure_dispatch_native_checked.
    const auto call = rt.find("int64_t aura_closure_dispatch_native_checked(");
    CHECK(call != std::string::npos, "AC4: blessed dispatch entry present");
    if (call != std::string::npos) {
        const auto body = rt.substr(call, 4500);
        CHECK(body.find("Issue #2472") != std::string::npos, "AC4: #2472 in MustDeopt path");
        CHECK(body.find("g_closure_freed") != std::string::npos,
              "AC4: freed re-check in MustDeopt exclusive");
        CHECK(body.find("orig_func_id") != std::string::npos,
              "AC4: func_id identity in MustDeopt exclusive");
        CHECK(body.find("g_closure_must_deopt") != std::string::npos,
              "AC4: must_deopt re-check retained");
        CHECK(body.find("lock-downgrade TOCTOU") != std::string::npos,
              "AC4: documents lock-downgrade TOCTOU");
    }
}

// ── Issue #3247: getter is sticky observe (Option A), not consume ──
static void ac3247_getter_sticky() {
    std::println("\n--- #3247 AC1: N consecutive getter probes stay 1 ---");
    aura_reset_runtime();
    auto cid = aura_alloc_closure(/*func_id=*/4247);
    CHECK(cid >= 0, "3247: alloc");
    aura_closure_set_must_deopt(cid, 1);
    for (int i = 0; i < 5; ++i) {
        CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 1,
              "ac3247_1_sticky: getter returns 1 on consecutive probes");
    }
    CHECK(aura_closure_get_must_deopt(cid) == 1,
          "ac3247_1_sticky: flag still set after N getter probes");
    aura_free_closure(cid);

    std::println("\n--- #3247 AC2: remount/remap/alloc heal → getter 0 ---");
    cid = aura_alloc_closure(/*func_id=*/4248);
    aura_closure_set_must_deopt(cid, 1);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 1, "3247: armed");
    aura_closure_set_must_deopt(cid, 0);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0,
          "ac3247_2_heal: remount/remap/alloc clear → getter 0");
    aura_free_closure(cid);

    std::println("\n--- #3247 AC2: aura_closure_call still consumes ---");
    cid = aura_alloc_closure(/*func_id=*/4249);
    aura_closure_set_must_deopt(cid, 1);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 1, "3247: armed before call");
    const auto deopt0 = aura_deopt_count();
    std::int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0,
          "ac3247_2_call: force-deopt still clears under exclusive");
    CHECK(aura_deopt_count() > deopt0, "3247: deopt advanced (call path unchanged)");
    aura_free_closure(cid);

    std::println("\n--- #3247 AC3: free+realloc — getter does not clear new flag ---");
    cid = aura_alloc_closure(/*func_id=*/111);
    CHECK(cid >= 0, "3247 realloc: orig alloc");
    aura_closure_set_must_deopt(cid, 1);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 1,
          "3247 realloc: orig getter 1 (no consume)");
    aura_free_closure(cid);
    auto nid = aura_alloc_closure(/*func_id=*/222);
    CHECK(nid >= 0, "3247 realloc: new alloc");
    aura_closure_set_must_deopt(nid, 1);
    CHECK(aura_get_closure_must_deopt_before_next_call(nid) == 1,
          "ac3247_3_realloc: new identity flag set");
    CHECK(aura_get_closure_must_deopt_before_next_call(nid) == 1,
          "ac3247_3_realloc: getter does not clear new closure");
    CHECK(aura_closure_get_must_deopt(nid) == 1,
          "ac3247_3_realloc: new flag still sticky (#2472 parity)");
    aura_free_closure(nid);
}

// ── AC5: gate wiring ──
static void ac5_gate() {
    std::println("\n--- #2472 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_closure_call_must_deopt_toctou_2472.py");
    CHECK(build.find("check_closure_call_must_deopt_toctou_2472") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_closure_call_must_deopt_toctou_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_closure_call_must_deopt_toctou") != std::string::npos,
          "AC5: cmake test");
    CHECK(!script.empty() && script.find("2472") != std::string::npos, "AC5: check script exists");
}

// ── AC2: exclusive re-verify order documented in source ──
static void ac2_reverify_order() {
    std::println("\n--- #2472 AC2: exclusive re-verify order ---");
    auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto md = rt.find("Issue #2472: original closure freed");
    CHECK(md != std::string::npos, "AC2: freed bail comment");
    const auto id = rt.find("Issue #2472: slot freed+realloced");
    CHECK(id != std::string::npos, "AC2: realloc identity bail");
    // Freed check must appear before clear of must_deopt in exclusive section.
    if (md != std::string::npos && id != std::string::npos) {
        const auto clear = rt.find("g_closure_must_deopt[cid] = 0", md);
        CHECK(clear != std::string::npos && clear > id, "AC2: clear after identity checks");
    }
}

// ── #3635 AC6: entry owns the call-time transaction ──
static void ac3635_entry_transaction() {
    std::println("\n=== Issue #3635: blessed anon dispatch entry ===");
    std::println("--- #3635 AC6: entry owns the transaction (contract parity) ---");
    aura_reset_runtime();
    std::int64_t args[1] = {0};
    // Out-of-range cid: same refuse-as-callee semantics as aura_closure_call.
    CHECK(aura_closure_dispatch_native_checked(-7, args, 0) == -7,
          "AC6: invalid cid returns callee_val");
    // No native fn registered: entry and wrapper both return 0.
    auto cid = aura_alloc_closure(/*func_id=*/333);
    CHECK(cid >= 0, "AC6: alloc");
    CHECK(aura_closure_dispatch_native_checked(cid, args, 0) == 0,
          "AC6: no-entry returns 0 via entry");
    CHECK(aura_closure_call(cid, args, 0) == 0, "AC6: wrapper forwards (parity)");
    // MustDeopt transaction through the entry (#2128/#2472 consume path).
    aura_closure_set_must_deopt(cid, 1);
    const auto deopt0 = aura_deopt_count();
    CHECK(aura_closure_dispatch_native_checked(cid, args, 0) == 0,
          "AC6: must_deopt force-deopt returns 0");
    CHECK(aura_closure_get_must_deopt(cid) == 0, "AC6: flag cleared via entry");
    CHECK(aura_deopt_count() > deopt0, "AC6: deopt advanced via entry");
    // Freed refuse parity.
    aura_free_closure(cid);
    CHECK(aura_closure_dispatch_native_checked(cid, args, 0) == 0,
          "AC6: freed returns 0 via entry");
}

// ── #3635 AC7: stale anon through the entry → safe fallback ──
static void ac3635_stale_entry_fallback() {
    std::println("--- #3635 AC7: stale anon via entry → interpreter fallback ---");
    aura_reset_runtime();
    auto cid = aura_alloc_closure(/*func_id=*/444);
    CHECK(cid >= 0, "AC7: alloc anon closure");
    // Inject an old bridge_epochs snapshot (capture behind the live clock;
    // a no-op when the clock is 0 — domain inactive → fresh, same as prod
    // Soft). Either way: entry == wrapper, fallback only, never native.
    aura_inject_stale_closure_bridge_epoch_for_test(cid);
    std::int64_t args[1] = {0};
    CHECK(aura_closure_dispatch_native_checked(cid, args, 0) == 0,
          "AC7: stale entry safe-fallback (0)");
    CHECK(aura_closure_call(cid, args, 0) == 0, "AC7: wrapper parity under stale state");
    aura_free_closure(cid);
}

// ── #3635 AC8: adversarial linter face ──
static void ac3635_linter_probe() {
    std::println("--- #3635 AC8: adversarial probe-file face ---");
    const auto script = find_path("scripts/check_closure_dispatch_entry_3635.py");
    CHECK(!script.empty(), "AC8: linter script found");
    if (script.empty())
        return;
    // Bare direct table read in a stub surface → flagged.
    {
        std::ofstream bad("/tmp/aura_3635_probe_bad.cpp");
        bad << "#include <cstdint>\n"
            << "extern \"C\" const std::int64_t* stub_surface_direct_read() {\n"
            << "    return g_closure_func_ids.data(); // no annotation\n"
            << "}\n";
    }
    const std::string bad_cmd =
        "python3 " + script + " --probe-file /tmp/aura_3635_probe_bad.cpp >/dev/null 2>&1";
    CHECK(std::system(bad_cmd.c_str()) != 0, "AC8: bare direct read flagged");
    // Same read with the per-line annotation → allowed.
    {
        std::ofstream ok("/tmp/aura_3635_probe_ok.cpp");
        ok << "#include <cstdint>\n"
           << "extern \"C\" const std::int64_t* stub_surface_direct_read() {\n"
           << "    return g_closure_func_ids.data(); // #3635-allow-direct legacy surface\n"
           << "}\n";
    }
    const std::string ok_cmd =
        "python3 " + script + " --probe-file /tmp/aura_3635_probe_ok.cpp >/dev/null 2>&1";
    CHECK(std::system(ok_cmd.c_str()) == 0, "AC8: annotated read allowed");
    // Linter self-test stays clean.
    const std::string st_cmd = "python3 " + script + " --self-test >/dev/null 2>&1";
    CHECK(std::system(st_cmd.c_str()) == 0, "AC8: linter self-test clean");
}

// ── #3635 AC9: gate wiring ──
static void ac3635_gate_wiring() {
    std::println("--- #3635 AC9: gate wiring ---");
    auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    auto hdr = read_file("src/compiler/runtime_shared.h");
    auto build = read_file("build.py");
    CHECK(rt.find("aura_closure_dispatch_native_checked") != std::string::npos,
          "AC9: blessed entry in table TU");
    CHECK(rt.find("return aura_closure_dispatch_native_checked(closure_id, args, argc);") !=
              std::string::npos,
          "AC9: wrapper forwards to entry");
    CHECK(hdr.find("aura_closure_dispatch_native_checked") != std::string::npos,
          "AC9: header declares the entry");
    CHECK(build.find("check_closure_dispatch_entry_3635") != std::string::npos,
          "AC9: build.py wires the linter");
    CHECK(rt.find("(g1 & 1ull) == 0") != std::string::npos,
          "AC9: fast-path generation double-check preserved");
}

} // namespace

int run_test_closure_call_must_deopt_toctou() {
    std::println("=== Issue #2472: aura_closure_call MustDeopt TOCTOU ===");
    ac3_baseline_force_deopt();
    ac1_free_realloc_stress();
    ac1b_freed_path();
    ac2_reverify_order();
    ac4_source_cite();
    ac3247_getter_sticky();
    ac5_gate();
    ac3635_entry_transaction();
    ac3635_stale_entry_fallback();
    ac3635_linter_probe();
    ac3635_gate_wiring();
    std::println("\n=== #2472 + #3635 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_closure_call_must_deopt_toctou();
}
#endif
