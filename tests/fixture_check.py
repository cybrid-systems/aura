#!/usr/bin/env python3
"""Validate tests/fixtures shards and cross-file invariants (#1962).

Prefer: python3 tests/run.py fixtures  (#1961)
Also:   python3 scripts/fixtures_tool.py validate

Direct invocation of this script remains supported.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from fixture_store import (  # noqa: E402
    FIXTURES,
    fixture_status,
    list_shard_files,
    load_case_array,
    mono_path,
    shard_dir,
)

BENCH_PIPELINES = frozenset({"eval", "ir", "typecheck"})
INTEG_PIPELINES = frozenset({"eval", "ir", "typecheck", "serve"})

# Soft budgets — warn (error) if a single hand-edited shard grows too large
# so PRs stay reviewable (#1962).
MAX_SHARD_BYTES = 12_000
MAX_SHARD_CASES = 50


def _fail(errors: list[str], msg: str) -> None:
    errors.append(msg)


def _require_str(errors: list[str], ctx: str, obj: dict, key: str) -> str | None:
    val = obj.get(key)
    if not isinstance(val, str) or not val.strip():
        _fail(errors, f"{ctx}: '{key}' must be a non-empty string")
        return None
    return val


def _check_unique_names(errors: list[str], label: str, names: list[str]) -> None:
    seen: set[str] = set()
    for name in names:
        if name in seen:
            _fail(errors, f"{label}: duplicate name '{name}'")
        seen.add(name)


def check_layout(errors: list[str]) -> None:
    """Prefer shards; mono *_tests.json is legacy-only if shards missing."""
    for kind in ("regression", "integ", "benchmark", "smoke"):
        shards = list_shard_files(kind)
        mono = mono_path(kind)
        if shards and mono.is_file():
            _fail(
                errors,
                f"{kind}: both shards ({shard_dir(kind).name}/) and legacy {mono.name} present — remove the mono file",
            )
        if not shards and not mono.is_file():
            _fail(errors, f"{kind}: missing shards under {shard_dir(kind)}/ and no {mono.name}")


def check_shard_budgets(errors: list[str]) -> None:
    for kind in ("regression", "integ", "benchmark", "smoke"):
        for path in list_shard_files(kind):
            data = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(data, list):
                _fail(errors, f"{path.relative_to(FIXTURES)}: root must be array")
                continue
            rel = path.relative_to(FIXTURES).as_posix()
            size = path.stat().st_size
            n = len(data)
            if size > MAX_SHARD_BYTES:
                _fail(
                    errors,
                    f"{rel}: {size} bytes exceeds soft budget {MAX_SHARD_BYTES} "
                    f"— split further (see fixtures/README.md)",
                )
            if n > MAX_SHARD_CASES:
                _fail(
                    errors,
                    f"{rel}: {n} cases exceeds soft budget {MAX_SHARD_CASES} — split further",
                )


def check_issues_fast(errors: list[str]) -> None:
    path = FIXTURES / "issues_fast.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    targets = data.get("targets")
    if not isinstance(targets, list) or not targets:
        _fail(errors, f"{path.name}: 'targets' must be a non-empty array")
        return
    names: list[str] = []
    for i, target in enumerate(targets):
        if not isinstance(target, str) or not (
            target.startswith("test_issue_")
            or target.startswith("test_issues_")
            or target.startswith("test_obs_")
            or target.startswith("test_domain_")
            or target.startswith("test_aura_result_")
            or target.startswith("test_primitives_")
            or target.startswith("test_aura_pets_")
            or target.startswith("test_cyber_cat_")
            or target.startswith("test_arena_")
            or target.startswith("test_compact_")
            or target == "test_gc_batch"
        ):
            _fail(
                errors,
                f"issues_fast.json[{i}]: target must be test_issue_*/test_issues_*/domain suite name",
            )
            continue
        names.append(target)
    _check_unique_names(errors, "issues_fast.json", names)


def check_benchmark(errors: list[str]) -> set[str]:
    items = load_case_array("benchmark")
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"benchmark[{i}]"
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "code")
        pipeline = _require_str(errors, ctx, item, "pipeline")
        if pipeline and pipeline not in BENCH_PIPELINES:
            _fail(errors, f"{ctx}: pipeline must be one of {sorted(BENCH_PIPELINES)}")
        if item.get("typecheck_suite") and not item.get("expected_type"):
            _fail(errors, f"{ctx}: typecheck_suite entries require expected_type")
        if name:
            names.append(name)
    _check_unique_names(errors, "benchmark", names)
    return set(names)


def check_integ(errors: list[str]) -> None:
    items = load_case_array("integ")
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"integ[{i}]"
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "code")
        pipeline = _require_str(errors, ctx, item, "pipeline")
        if pipeline and pipeline not in INTEG_PIPELINES:
            _fail(errors, f"{ctx}: pipeline must be one of {sorted(INTEG_PIPELINES)}")
        if "expected_status" in item and not isinstance(item["expected_status"], int):
            _fail(errors, f"{ctx}: expected_status must be an integer")
        if name:
            names.append(name)
    _check_unique_names(errors, "integ", names)


def check_regression(errors: list[str]) -> None:
    items = load_case_array("regression")
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"regression[{i}]"
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "code")
        for key in ("expect_out", "expect_err"):
            if key in item and not isinstance(item[key], str):
                _fail(errors, f"{ctx}: '{key}' must be a string")
        if name:
            names.append(name)
    _check_unique_names(errors, "regression", names)


def check_smoke(errors: list[str]) -> None:
    items = load_case_array("smoke")
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"smoke[{i}]"
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "command")
        _require_str(errors, ctx, item, "expected")
        if name:
            names.append(name)
    _check_unique_names(errors, "smoke", names)


def check_benchmark_baseline_sync(errors: list[str], fixture_names: set[str]) -> None:
    baseline_path = _SCRIPT_DIR / "benchmark_baseline.json"
    if not baseline_path.exists():
        _fail(errors, "benchmark_baseline.json: missing")
        return
    data: dict[str, Any] = json.loads(baseline_path.read_text(encoding="utf-8"))
    cases = data.get("cases")
    if not isinstance(cases, list):
        _fail(errors, "benchmark_baseline.json: 'cases' must be an array")
        return
    baseline_names = {c["name"] for c in cases if isinstance(c, dict) and isinstance(c.get("name"), str)}
    missing = sorted(fixture_names - baseline_names)
    extra = sorted(baseline_names - fixture_names)
    if missing:
        _fail(
            errors,
            f"benchmark_baseline.json: missing {len(missing)} fixture case(s): {', '.join(missing)}",
        )
    if extra:
        _fail(
            errors,
            f"benchmark_baseline.json: {len(extra)} stale case(s) not in fixture: {', '.join(extra)}",
        )
    if data.get("total_cases") != len(fixture_names):
        _fail(
            errors,
            f"benchmark_baseline.json: total_cases={data.get('total_cases')} "
            f"but benchmark fixtures have {len(fixture_names)} cases",
        )


def run_check() -> int:
    errors: list[str] = []
    check_layout(errors)
    check_shard_budgets(errors)
    fixture_names = check_benchmark(errors)
    check_issues_fast(errors)
    check_integ(errors)
    check_regression(errors)
    check_smoke(errors)
    check_benchmark_baseline_sync(errors, fixture_names)

    if errors:
        print("Fixture validation failed:")
        for err in errors:
            print(f"  - {err}")
        return 1

    rows = fixture_status()
    parts = []
    for r in rows:
        if r["mode"] == "shards":
            parts.append(f"{r['kind']}={r['cases']}@{r['files']}shards")
        elif r["mode"] == "mono":
            parts.append(f"{r['kind']}={r['cases']}@mono")
    print(f"Fixture validation OK ({', '.join(parts)})")
    return 0


def main() -> None:
    sys.exit(run_check())


if __name__ == "__main__":
    main()
