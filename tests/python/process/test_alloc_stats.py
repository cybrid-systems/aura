"""Issue #5636: allocation statistics that track process RSS."""

from __future__ import annotations

import unittest

PROCESS_THEME = "alloc-stats"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ("alloc_stats",)
CASES = (
    (5636, "default cli reads arena and alloc stats", "alloc_stats.AllocStats.test_default_cli_reads_stats"),
    (
        5636,
        "std load capacity within 10 percent of rss",
        "alloc_stats.AllocStats.test_capacity_tracks_rss_after_std_load",
    ),
    (5636, "ast arena grows", "alloc_stats.AllocStats.test_ast_arena_grows"),
    (5636, "flat ast grows", "alloc_stats.AllocStats.test_flat_ast_grows"),
    (5636, "pcv grows", "alloc_stats.AllocStats.test_pcv_grows"),
    (5636, "ir slab grows", "alloc_stats.AllocStats.test_ir_grows"),
    (5636, "pairs grow", "alloc_stats.AllocStats.test_pairs_grow"),
    (5636, "cells grow", "alloc_stats.AllocStats.test_cells_grow"),
    (5636, "strings grow", "alloc_stats.AllocStats.test_strings_grow"),
    (5636, "fiber nursery grows", "alloc_stats.AllocStats.test_fiber_grows"),
    (5636, "name res grows", "alloc_stats.AllocStats.test_name_res_grows"),
)


def load_tests(loader, tests, pattern):
    import importlib

    suite = unittest.TestSuite()
    for _issue, _title, spec in CASES:
        bits = spec.split(".")
        mod = importlib.import_module(bits[0])
        suite.addTest(getattr(mod, bits[1])(bits[2]))
    return suite


if __name__ == "__main__":
    unittest.main()
