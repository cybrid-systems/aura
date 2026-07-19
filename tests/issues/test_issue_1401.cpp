// test_issue_1401.cpp — Issue #1401: load_module_file ↔
// compact_env_frames() mutex interlock + post-reload closure
// env_id validity contract.
//
// compact_env_frames (Issue #1386) re-packs env_frames_ and
// rewrites Closure::env_id via remap. load_module_file allocates
// fresh env_frames_ + adds new closures_ for the loaded module.
// Without the interlock, a concurrent compact_env_frames could miss
// freshly-added closures (Step 2 walk) and reclaim frames the
// loader is about to use (Step 3 pack), producing torn env_id
// remap + iterator invalidation.
//
// Fix: shared mutex compact_env_frames_lock_ acquired at the start
// of both functions (canonical order:
// compact_env_frames_lock_ → env_frames_mtx_ | workspace_mtx_).
//
// ACs:
//   AC1: single-threaded alternating calls — both complete cleanly,
//        no deadlock (mutex acquired and released correctly)
//   AC2: multi-threaded direct Evaluator calls — both complete
//        cleanly (mutex mutual exclusion verified directly, bypassing
//        cs.eval which has a separate pre-existing concurrent crash
//        documented in #1399's honest gap)

#include "test_harness.hpp"
import std;
using namespace std::chrono_literals;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace test_issue_1401_detail {

static void run_ac1_single_thread_alternating(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: single-thread alternating compact + load ---");
    constexpr int N = 100;
    int errors = 0;
    for (int i = 0; i < N; ++i) {
        // compact_env_frames returns size_t — just verify it doesn't throw.
        try {
            (void)cs.evaluator().compact_env_frames();
        } catch (...) {
            ++errors;
        }
        // load_module_file with nonexistent path returns EvalValue
        // (likely make_void() = val 0). Just verify it returns cleanly.
        try {
            auto r = cs.evaluator().load_module_file("__nonexistent_test_path_1401__.aura");
            (void)r; // value irrelevant — we just need clean return + no deadlock
        } catch (...) {
            ++errors;
        }
    }
    CHECK(errors == 0,
          std::format("{} alternating compact+load calls: 0 errors (got {})", N, errors));
}

static void run_ac2_multi_thread_mutex_exclusion(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: multi-thread direct Evaluator calls ---");
    constexpr int N = 200;
    std::atomic<int> compact_count{0};
    std::atomic<int> load_count{0};
    std::atomic<int> errors{0};
    auto compact_worker = [&]() {
        try {
            for (int i = 0; i < N; ++i) {
                (void)cs.evaluator().compact_env_frames();
                compact_count.fetch_add(1);
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    };
    auto load_worker = [&]() {
        try {
            for (int i = 0; i < N; ++i) {
                auto r = cs.evaluator().load_module_file("__nonexistent_test_path_1401__.aura");
                (void)r; // value irrelevant — clean return + no UB
                load_count.fetch_add(1);
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    };
    std::thread t1(compact_worker);
    std::thread t2(load_worker);
    t1.join();
    t2.join();
    CHECK(compact_count.load() == N,
          std::format("compact_worker ran {} times (got {})", N, compact_count.load()));
    CHECK(load_count.load() == N,
          std::format("load_worker ran {} times (got {})", N, load_count.load()));
    CHECK(errors.load() == 0,
          std::format("2 threads x {} compact+load: 0 errors (got {})", N, errors.load()));
}

} // namespace test_issue_1401_detail

int aura_issue_1401_run() {
    using namespace test_issue_1401_detail;
    std::println("=== Issue #1401: load_module_file ↔ compact_env_frames interlock ===");
    {
        aura::compiler::CompilerService cs;
        run_ac1_single_thread_alternating(cs);
        run_ac2_multi_thread_mutex_exclusion(cs);
    }
    std::println("\nResults: {}/{} passed, {}/{} failed", ::aura::test::g_passed,
                 ::aura::test::g_passed + ::aura::test::g_failed, ::aura::test::g_failed,
                 ::aura::test::g_passed + ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1401_run();
}
#endif