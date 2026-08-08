// test_json_io_cap_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_channel_rendezvous();
extern int run_test_command_line_cap_io_read();
extern int run_test_eval_current_no_auto_fix();
extern int run_test_json_parse_number_exception();
extern int run_test_json_parse_object_grow();
extern int run_test_list_end_of_list_void();
extern int run_test_load_cap_io_read();
extern int run_test_regex_redos_timeout();
extern int run_test_sys_open_path_harden();
extern int run_test_write_string_escape();
extern int run_test_tcp_listen_accept();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_json_io_cap_batch (11 members) ===");

    std::println("\n──── test_channel_rendezvous ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_channel_rendezvous() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_channel_rendezvous ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_channel_rendezvous ({} checks)", g_passed);
    }

    std::println("\n──── test_command_line_cap_io_read ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_command_line_cap_io_read() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_command_line_cap_io_read ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_command_line_cap_io_read ({} checks)", g_passed);
    }

    std::println("\n──── test_eval_current_no_auto_fix ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_eval_current_no_auto_fix() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_eval_current_no_auto_fix ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_eval_current_no_auto_fix ({} checks)", g_passed);
    }

    std::println("\n──── test_json_parse_number_exception ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_json_parse_number_exception() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_json_parse_number_exception ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_json_parse_number_exception ({} checks)", g_passed);
    }

    std::println("\n──── test_json_parse_object_grow ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_json_parse_object_grow() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_json_parse_object_grow ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_json_parse_object_grow ({} checks)", g_passed);
    }

    std::println("\n──── test_list_end_of_list_void ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_list_end_of_list_void() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_list_end_of_list_void ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_list_end_of_list_void ({} checks)", g_passed);
    }

    std::println("\n──── test_load_cap_io_read ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_load_cap_io_read() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_load_cap_io_read ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_load_cap_io_read ({} checks)", g_passed);
    }

    std::println("\n──── test_regex_redos_timeout ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_regex_redos_timeout() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_regex_redos_timeout ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_regex_redos_timeout ({} checks)", g_passed);
    }

    std::println("\n──── test_sys_open_path_harden ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_sys_open_path_harden() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_sys_open_path_harden ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_sys_open_path_harden ({} checks)", g_passed);
    }

    std::println("\n──── test_write_string_escape ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_write_string_escape() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_write_string_escape ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_write_string_escape ({} checks)", g_passed);
    }

    std::println("\n──── test_tcp_listen_accept ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tcp_listen_accept() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tcp_listen_accept ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tcp_listen_accept ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
