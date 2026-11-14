"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "evo"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "evo_log",
    "evo_fork",
    "evo_lineage",
    "evo_moead",
    "evo_trial_rebind",
    "evo_trial_heap",
    "evo_recursive",
)
CASES = (
    (
        4910,
        "test_record_tip_skips_auto_logged_rebinds",
        "evo_log.EvoLog.test_record_tip_skips_auto_logged_rebinds",
    ),
    (
        4910,
        "test_log_records_and_cli",
        "evo_log.EvoLog.test_log_records_and_cli",
    ),
    (
        4910,
        "test_restore_appends_rollback_under_preset_dev",
        "evo_log.EvoLog.test_restore_appends_rollback_under_preset_dev",
    ),
    (
        4910,
        "test_v2_records_and_deterministic_reruns",
        "evo_log.EvoLog.test_v2_records_and_deterministic_reruns",
    ),
    (
        4910,
        "test_off_by_default_outside_projects",
        "evo_log.EvoLog.test_off_by_default_outside_projects",
    ),
    (
        4910,
        "test_project_run_defaults_log",
        "evo_log.EvoLog.test_project_run_defaults_log",
    ),
    (
        4910,
        "test_qd_heatmap_and_lineage_svg",
        "evo_log.EvoReportQD.test_qd_heatmap_and_lineage_svg",
    ),
    (
        4910,
        "test_no_qd_records_means_no_empty_svg",
        "evo_log.EvoReportQD.test_no_qd_records_means_no_empty_svg",
    ),
    (
        4910,
        "test_report_pareto_hv_folds_and_surrogate",
        "evo_log.EvoReportMultiObjective.test_report_pareto_hv_folds_and_surrogate",
    ),
    (
        4910,
        "test_log_front_and_lineage",
        "evo_log.EvoReportMultiObjective.test_log_front_and_lineage",
    ),
    (
        4910,
        "test_multiparent_edges_and_unresolved",
        "evo_log.EvoLineageIds.test_multiparent_edges_and_unresolved",
    ),
    (
        4910,
        "test_unknown_lineage_id_is_an_error",
        "evo_log.EvoLineageIds.test_unknown_lineage_id_is_an_error",
    ),
    (
        4910,
        "test_header_counts_every_decision",
        "evo_log.EvoLineageIds.test_header_counts_every_decision",
    ),
    (
        4910,
        "test_parent_is_record_id_and_survives_restart",
        "evo_log.EvoCoreParentChain.test_parent_is_record_id_and_survives_restart",
    ),
    (
        5088,
        "test_child_rebind_is_fork_not_keep",
        "evo_fork.ForkEvoLog.test_child_rebind_is_fork_not_keep",
    ),
    (
        5098,
        "test_report_chain_is_linear_and_keeps_the_tree",
        "evo_lineage.EvoLineageLinear.test_report_chain_is_linear_and_keeps_the_tree",
    ),
    (
        5098,
        "test_log_lineage_chain_is_the_full_ancestor_list",
        "evo_lineage.EvoLineageLinear.test_log_lineage_chain_is_the_full_ancestor_list",
    ),
    (
        5098,
        "test_small_chain_lineage_matches_the_old_layout",
        "evo_lineage.EvoLineageLinear.test_small_chain_lineage_matches_the_old_layout",
    ),
    (
        5072,
        "test_rand_is_recorded_as_random",
        "evo_moead.MoeadOperatorAudit.test_rand_is_recorded_as_random",
    ),
    (
        5162,
        "test_preset_dev_parent_rebind_after_trial",
        "evo_trial_rebind.TrialParentRebind.test_preset_dev_parent_rebind_after_trial",
    ),
    (
        5163,
        "test_preset_dev_child_slot_updates",
        "evo_trial_heap.TrialHeapUpdate.test_preset_dev_child_slot_updates",
    ),
    (
        5052,
        "test_recursive_rebind_is_accepted",
        "evo_recursive.RecursiveRebind.test_recursive_rebind_is_accepted",
    ),
    (
        5052,
        "test_non_recursive_rebind_still_accepted",
        "evo_recursive.RecursiveRebind.test_non_recursive_rebind_still_accepted",
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
