#!/usr/bin/env python3
"""Issue #3796: occurrence_coercion_batch CI ACs after #3698/#3699.

Re-anchors (not production holes):
  - #3618 AC1 re-establishes IR cache via set-code + eval-current before inject
  - #3618 AC1 force-full after unmatched persist + non-empty map
  - #3689 soak mutated body remains

Also requires build/ WORKING_DIRECTORY linter self-tests to use repo root
(aura_python_repo_script / aura_repo_file_exists) so the suite stays green.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    cone = (ROOT / "tests/compiler/test_dead_coercion_dirty_cone.cpp").read_text()
    harness = (ROOT / "tests/test_harness.hpp").read_text()
    fails: list[str] = []
    for needle, label in [
        ("3618 AC1: map non-empty (inject)", "AC1 inject CHECK"),
        ("3618 AC1: unmatched persist + non-empty map → force full", "AC1 force-full CHECK"),
        ("3689 soak: mutated body", "3689 soak CHECK"),
        ("re-establish f entry post-relower", "AC1 re-establish after #3698"),
        ("inject_source_to_ir_map_desync_for_test", "inject helper call"),
    ]:
        if needle not in cone:
            fails.append(f"FAIL: {label}")
    for needle, label in [
        ("aura_python_repo_script", "harness python cwd helper"),
        ("aura_repo_file_exists", "harness exists helper"),
        ("Issue #3796", "harness cites #3796"),
    ]:
        if needle not in harness:
            fails.append(f"FAIL: {label}")
    if fails:
        print("FAIL #3796:")
        for f in fails:
            print(" ", f)
        return 1
    print("OK: #3796 coercion CI AC anchors + build-cwd harness helpers present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
