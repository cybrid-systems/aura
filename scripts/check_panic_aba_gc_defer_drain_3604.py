#!/usr/bin/env python3
# scripts/check_panic_aba_gc_defer_drain_3604.py -- Issue #3604 source-cite gate.
#
# Verifies the PanicCheckpoint ABA arm drains gc_defer for the saved
# evaluator identity (#3570/#1727 residual):
#
#  AC1: raii.ixx ABA arm cites #3604 and, after the gen-mismatch skip,
#       calls clear_gc_defer_for_evaluator + reconcile_gc_defer_bits_after_
#       clear — gated on defer_armed_at_save_ AND a wired has_panic_checkpoint
#       probe AND !probe(ctx). host.clear(ctx) must NOT run on this arm
#       (#3570 AC: the address names the NEW occupant).
#  AC2: Guard snapshots defer_armed_at_save_ at construction from
#       gc_deferred_for_evaluator after the save attempt.
#  AC3: counter discipline — bump restores_discriminator_cleared only when
#       the drain actually ran (drained > 0); NO steal-named total reuse;
#       NO new PanicCheckpointStats mid-field; no extra mutex (Soft /
#       no-pending-panic keeps the single atomic loads).
#  AC4: evaluator.ixx factory wires the has_panic_checkpoint probe shim.
#  AC5: test faces — test_panic_checkpoint_aba.cpp drives live defer
#       arm/drain (AC6/AC7/AC8); steal ACs (test_steal_complete_gc_defer.cpp)
#       stay in place, untouched.
#  AC6: no docs/design/3604-* (#1655); no tests/**/test_issue_3604.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

RAII = "src/core/panic_checkpoint_raii.ixx"
EVAL = "src/compiler/evaluator.ixx"
ABA = "tests/core/test_panic_checkpoint_aba.cpp"
STEAL = "tests/serve/test_steal_complete_gc_defer.cpp"
BUILD = "build.py"

REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1/AC2: drain arm + snapshot in raii.ixx.
    (RAII, r"Issue\s+#3604", "3604 AC1: raii.ixx cites #3604"),
    (
        RAII,
        r"clear_gc_defer_for_evaluator\(",
        "3604 AC1: ABA arm calls clear_gc_defer_for_evaluator",
    ),
    (
        RAII,
        r"reconcile_gc_defer_bits_after_clear\(\)",
        "3604 AC1: ABA arm reconciles bits after clear",
    ),
    (
        RAII,
        r"gc_deferred_for_evaluator\(host_\.ctx\)",
        "3604 AC2: ctor snapshots defer_armed_at_save_ via gc_deferred_for_evaluator",
    ),
    (
        RAII,
        r"bool\s+\(\*has_panic_checkpoint\)\(void\* ctx\)\s*=\s*nullptr;",
        "3604 AC1: nullable host probe (legacy hosts keep #3570 exact)",
    ),
    # AC3: counter discipline.
    (
        RAII,
        r"restores_discriminator_cleared",
        "3604 AC3: sanctioned clears counter bumped on drain",
    ),
    # AC4: factory wiring.
    (
        EVAL,
        r"/\*has_panic_checkpoint=\*/",
        "3604 AC4: factory wires the has_panic_checkpoint shim",
    ),
    # AC5: test faces.
    (ABA, r"#3604", "3604 AC5: aba test hosts #3604 ACs"),
    (ABA, r"gc_deferred_for_evaluator", "3604 AC5: aba test asserts live defer state"),
    (ABA, r"arm_gc_defer_pending_panic_for", "3604 AC5: aba test arms defer like the real save"),
    (STEAL, r"clear_gc_defer_for_evaluator|steal_complete", "3604 AC5: steal ACs stay in place"),
    (BUILD, r"check_panic_aba_gc_defer_drain_3604", "3604 AC6: build.py wires the linter"),
)

FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        RAII,
        r"g_gc_defer_orphan_cleared_on_steal_total",
        "3604 AC3: steal-named total must not be reused (ABA is not steal)",
    ),
    (
        RAII,
        r"std::mutex",
        "3604 AC5: no extra mutex (Soft keeps single atomic loads)",
    ),
    (
        RAII,
        r"std::uint64_t\s+\w*gc_defer\w*\s*;",
        "3604 AC3: no new PanicCheckpointStats gc_defer mid-field",
    ),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    for path, pattern, label in FORBIDDEN:
        if re.search(pattern, body(path)) is not None:
            failures.append(label)

    flat = body(RAII)

    # AC1: the ABA arm window — from the gen-mismatch bump to its return —
    # must contain the drain + gates and must NOT call host_.clear.
    bump = flat.find("++g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total;")
    if bump == -1:
        failures.append("3604 AC1: ABA bump site not found")
    else:
        ret = flat.find("return;", bump)
        window = flat[bump:ret] if ret != -1 else flat[bump : bump + 3000]
        for needle, lbl in (
            ("clear_gc_defer_for_evaluator", "3604 AC1: drain call inside the ABA window"),
            ("reconcile_gc_defer_bits_after_clear", "3604 AC1: reconcile inside the ABA window"),
            ("defer_armed_at_save_", "3604 AC1: drain gated on defer_armed_at_save_"),
            ("has_panic_checkpoint", "3604 AC1: drain gated on the wired probe"),
            (
                "restores_discriminator_cleared",
                "3604 AC3: sanctioned counter bump inside the ABA window",
            ),
        ):
            if needle not in window:
                failures.append(lbl)
        if "host_.clear" in window:
            failures.append("3604 AC1: ABA arm must not host.clear (new occupant's checkpoint)")

    # AC1: the drain must be guarded on the probe being wired + returning
    # false (new occupant clean). Look for the negated probe call.
    if re.search(r"!\s*host_\.has_panic_checkpoint\(", flat) is None:
        failures.append("3604 AC1: drain requires !has_panic_checkpoint(ctx) (new occupant clean)")

    # AC6: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3604-"):
                failures.append(f"3604 AC6: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3604.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3604.cpp",
    ):
        if probe.exists():
            failures.append(f"3604 AC6: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3604 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any missing row")
    ap.parse_args()
    failures = run_checks()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"check_panic_aba_gc_defer_drain_3604: {len(failures)} failure(s)")
        return 1
    print("check_panic_aba_gc_defer_drain_3604: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
