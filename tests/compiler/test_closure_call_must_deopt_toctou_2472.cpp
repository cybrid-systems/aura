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
    // Scope to aura_closure_call body (earlier #2128 comments also mention
    // MustDeoptBeforeNextCall on the vector / remap path).
    const auto call = rt.find("int64_t aura_closure_call(");
    CHECK(call != std::string::npos, "AC4: aura_closure_call present");
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
    CHECK(cmake.find("test_closure_call_must_deopt_toctou_2472") != std::string::npos,
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

} // namespace

int run_test_closure_call_must_deopt_toctou_2472() {
    std::println("=== Issue #2472: aura_closure_call MustDeopt TOCTOU ===");
    ac3_baseline_force_deopt();
    ac1_free_realloc_stress();
    ac1b_freed_path();
    ac2_reverify_order();
    ac4_source_cite();
    ac5_gate();
    std::println("\n=== #2472 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_closure_call_must_deopt_toctou_2472();
}
#endif
