// test_json_io_cap_batch.cpp — thematic multi-TU batch
// JSON / IO / cap / regex / channel / string escape ACs
// Stream A5 of tests/CONSOLIDATION_PLAN.md — carved from misc_issue_fold.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_channel_rendezvous_2483();
extern int run_test_command_line_cap_io_read_2478();
extern int run_test_eval_current_no_auto_fix_2484();
extern int run_test_json_parse_number_exception_2480();
extern int run_test_json_parse_object_grow_2481();
extern int run_test_list_end_of_list_void_2482();
extern int run_test_load_cap_io_read_2485();
extern int run_test_regex_redos_timeout_2479();
extern int run_test_sys_open_path_harden_2487();
extern int run_test_write_string_escape_2574();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_json_io_cap_batch (10 members) ===");

    std::println("\n──── test_channel_rendezvous_2483 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_channel_rendezvous_2483() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_channel_rendezvous_2483 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_channel_rendezvous_2483 ({} checks)", g_passed);
    }

    std::println("\n──── test_command_line_cap_io_read_2478 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_command_line_cap_io_read_2478() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_command_line_cap_io_read_2478 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_command_line_cap_io_read_2478 ({} checks)", g_passed);
    }

    std::println("\n──── test_eval_current_no_auto_fix_2484 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_eval_current_no_auto_fix_2484() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_eval_current_no_auto_fix_2484 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_eval_current_no_auto_fix_2484 ({} checks)", g_passed);
    }

    std::println("\n──── test_json_parse_number_exception_2480 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_json_parse_number_exception_2480() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_json_parse_number_exception_2480 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_json_parse_number_exception_2480 ({} checks)", g_passed);
    }

    std::println("\n──── test_json_parse_object_grow_2481 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_json_parse_object_grow_2481() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_json_parse_object_grow_2481 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_json_parse_object_grow_2481 ({} checks)", g_passed);
    }

    std::println("\n──── test_list_end_of_list_void_2482 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_list_end_of_list_void_2482() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_list_end_of_list_void_2482 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_list_end_of_list_void_2482 ({} checks)", g_passed);
    }

    std::println("\n──── test_load_cap_io_read_2485 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_load_cap_io_read_2485() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_load_cap_io_read_2485 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_load_cap_io_read_2485 ({} checks)", g_passed);
    }

    std::println("\n──── test_regex_redos_timeout_2479 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_regex_redos_timeout_2479() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_regex_redos_timeout_2479 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_regex_redos_timeout_2479 ({} checks)", g_passed);
    }

    std::println("\n──── test_sys_open_path_harden_2487 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_sys_open_path_harden_2487() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_sys_open_path_harden_2487 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_sys_open_path_harden_2487 ({} checks)", g_passed);
    }

    std::println("\n──── test_write_string_escape_2574 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_write_string_escape_2574() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_write_string_escape_2574 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_write_string_escape_2574 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
