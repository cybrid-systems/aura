// test_shape_soa_storm_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <execinfo.h>
#include <unistd.h>

#include <cxxabi.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <print>
#include <string>

import std;

// CI 的 issue-farm 在收尾时偶发 std::system_error: Resource deadlock
// avoided (rc=134)，本地与单独重跑都不复现；terminate 默认不打印栈，
// 无法定位。这个 handler 只在 abort 时多打一段：tid、异常文本和
// backtrace（PIE 下是 二进制(+0x…) 形式，可用 addr2line 映射）。
static void aura_terminate_trace() {
    std::fprintf(stderr, "[terminate] tid=%zu\n",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[terminate] what=%s\n", ex.what());
        } catch (...) {
            std::fprintf(stderr, "[terminate] non-std exception\n");
        }
    }
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, 2);
    // The runner keeps the tail of stderr and cuts long lines: print the
    // innermost frames LAST, function name only, each short.
    if (char** syms = ::backtrace_symbols(frames, n)) {
        std::string names[6];
        for (int i = 0; i < n && i < 6; ++i) {
            std::string name = syms[i] ? syms[i] : "?";
            const auto lp = name.rfind('(');
            const auto plus = name.rfind('+');
            const auto rp = name.rfind(')');
            if (lp != std::string::npos && plus != std::string::npos && rp != std::string::npos &&
                lp < plus && plus < rp) {
                const std::string mangled = name.substr(lp + 1, plus - lp - 1);
                int status = 0;
                if (char* dm = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status)) {
                    name = dm;
                    std::free(dm);
                }
            }
            if (const auto args = name.find('('); args != std::string::npos)
                name.resize(args);
            if (name.size() > 42)
                name = name.substr(name.size() - 42);
            names[i] = name;
        }
        for (const int base : {3, 0}) {
            std::string line = base == 3 ? "[ts-out]" : "[ts-in]";
            for (int i = base; i < base + 3 && i < 6; ++i)
                line += "|" + std::to_string(i) + "=" + names[i];
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        std::free(syms);
    }
    std::abort();
}

extern int run_test_dirty_aware_shape_linear_passes();
extern int run_test_hot_pass_dirty_soa();
extern int run_test_shape_compact_storm_isolation();
extern int run_test_shape_high_mutation_storm();
extern int run_test_shape_storm_adaptive();
extern int run_test_soa_ban_residual_aos_bridge();
extern int run_test_soa_dirty_aware_pipeline();
extern int run_test_soa_generation_fence();
extern int run_test_soa_partial_desync_gate();
extern int run_test_soa_residual_production_smoke();
extern int run_test_soa_single_entry_dirty_sync();
extern int run_test_storm_isolation();
extern int run_test_hot_children_columnar();
extern int run_test_validate_post_restore_soa();

int main() {
    std::set_terminate(aura_terminate_trace);
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_shape_soa_storm_batch (14 members) ===");

    std::println("\n──── test_dirty_aware_shape_linear_passes ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dirty_aware_shape_linear_passes() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dirty_aware_shape_linear_passes ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dirty_aware_shape_linear_passes ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_dirty_soa ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_dirty_soa() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_dirty_soa ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_dirty_soa ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_compact_storm_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_compact_storm_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_compact_storm_isolation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_compact_storm_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_high_mutation_storm ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_high_mutation_storm() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_high_mutation_storm ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_high_mutation_storm ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_storm_adaptive ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_storm_adaptive() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_storm_adaptive ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_storm_adaptive ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_ban_residual_aos_bridge ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_ban_residual_aos_bridge() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_ban_residual_aos_bridge ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_ban_residual_aos_bridge ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_dirty_aware_pipeline ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_dirty_aware_pipeline() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_dirty_aware_pipeline ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_dirty_aware_pipeline ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_generation_fence ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_generation_fence() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_generation_fence ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_generation_fence ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_partial_desync_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_partial_desync_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_partial_desync_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_partial_desync_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_residual_production_smoke ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_residual_production_smoke() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_residual_production_smoke ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_residual_production_smoke ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_single_entry_dirty_sync ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_single_entry_dirty_sync() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_single_entry_dirty_sync ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_single_entry_dirty_sync ({} checks)", g_passed);
    }

    std::println("\n──── test_storm_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_storm_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_storm_isolation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_storm_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_children_columnar ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_children_columnar() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_children_columnar ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_children_columnar ({} checks)", g_passed);
    }

    std::println("\n──── test_validate_post_restore_soa ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_validate_post_restore_soa() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_validate_post_restore_soa ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_validate_post_restore_soa ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
