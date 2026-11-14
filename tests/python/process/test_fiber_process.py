"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "fiber"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 900
PARTS = (
    "fiber_fastpath",
    "fiber_named_let",
    "fiber_workers",
    "fiber_blocking_recv",
    "fiber_rejoin",
    "fiber_recv_yield",
    "fiber_slab",
    "fiber_never_joined",
    "fiber_closure_stamps",
    "fiber_env_frames",
    "fiber_scope_join",
    "fiber_nested_define",
    "fiber_stats",
)
CASES = (
    (
        5066,
        "test_worker_cap_keeps_checksum",
        "fiber_fastpath.FiberFastPath5066.test_worker_cap_keeps_checksum",
    ),
    (
        5116,
        "test_serve_async_sees_each_capture",
        "fiber_named_let.NamedLetReentrant5116.test_serve_async_sees_each_capture",
    ),
    (
        5063,
        "test_flag_before_load_runs_the_file",
        "fiber_workers.WorkerThreadsBeforeLoad.test_flag_before_load_runs_the_file",
    ),
    (
        4871,
        "test_worker_exit_is_clean_and_prompt",
        "fiber_blocking_recv.TestBlockingRecvExit4871.test_worker_exit_is_clean_and_prompt",
    ),
    (
        4872,
        "test_rejoin_after_cache_eviction_top_level",
        "fiber_rejoin.TestIssue4872RejoinEvicted.test_rejoin_after_cache_eviction_top_level",
    ),
    (
        4872,
        "test_rejoin_after_cache_eviction_inside_fiber",
        "fiber_rejoin.TestIssue4872RejoinEvicted.test_rejoin_after_cache_eviction_inside_fiber",
    ),
    (
        4872,
        "test_join_unknown_fid_returns",
        "fiber_rejoin.TestIssue4872RejoinEvicted.test_join_unknown_fid_returns",
    ),
    (
        4872,
        "test_cached_error_still_reraises",
        "fiber_rejoin.TestIssue4872RejoinEvicted.test_cached_error_still_reraises",
    ),
    (
        4873,
        "test_second_agent_starts_before_recv_timeout",
        "fiber_recv_yield.TestBlockingRecvYields4873.test_second_agent_starts_before_recv_timeout",
    ),
    (
        4873,
        "test_ask_to_second_blocking_worker_succeeds",
        "fiber_recv_yield.TestBlockingRecvYields4873.test_ask_to_second_blocking_worker_succeeds",
    ),
    (
        4854,
        "test_agent_and_scope_bodies_release_their_slab",
        "fiber_slab.TestIssue4854FiberSlabRelease.test_agent_and_scope_bodies_release_their_slab",
    ),
    (
        4890,
        "test_done_agents_return_checkpoint_stacks",
        "fiber_never_joined.TestNeverJoinedAgentStacks4890.test_done_agents_return_checkpoint_stacks",
    ),
    (
        4890,
        "test_joined_agent_still_reports",
        "fiber_never_joined.TestNeverJoinedAgentStacks4890.test_joined_agent_still_reports",
    ),
    (
        4891,
        "test_off_face_writes_no_stamps",
        "fiber_closure_stamps.TestClosureTenantStamps4891.test_off_face_writes_no_stamps",
    ),
    (
        4891,
        "test_prod_face_stamps_are_reclaimed",
        "fiber_closure_stamps.TestClosureTenantStamps4891.test_prod_face_stamps_are_reclaimed",
    ),
    (
        5099,
        "test_joined_fibers_do_not_fill_the_arena",
        "fiber_env_frames.EnvFrameFiberChurn.test_joined_fibers_do_not_fill_the_arena",
    ),
    (
        5099,
        "test_exhaustion_diagnostic_names_allocated_and_live",
        "fiber_env_frames.EnvFrameFiberChurn.test_exhaustion_diagnostic_names_allocated_and_live",
    ),
    (
        5103,
        "test_second_join_and_watch_do_not_raise",
        "fiber_scope_join.ScopeJoinKeepsScope.test_second_join_and_watch_do_not_raise",
    ),
    (
        4764,
        "test_inner_define_loop_runs",
        "fiber_nested_define.TestNestedDefine4764.test_inner_define_loop_runs",
    ),
    (
        4860,
        "test_concurrent_stats_reads_do_not_corrupt_the_heap",
        "fiber_stats.TestIssue4860.test_concurrent_stats_reads_do_not_corrupt_the_heap",
    ),
)


def load_tests(loader, tests, pattern):
    import importlib
    import sys

    suite = unittest.TestSuite()
    for _issue, _title, spec in CASES:
        bits = spec.split(".")
        if len(bits) == 3:
            mod = importlib.import_module(bits[0])
            suite.addTest(getattr(mod, bits[1])(bits[2]))
        else:
            suite.addTest(getattr(sys.modules[__name__], bits[0])(bits[1]))
    return suite


if __name__ == "__main__":
    unittest.main()
