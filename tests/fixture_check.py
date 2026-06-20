#!/usr/bin/env python3
"""Validate tests/fixtures/*.json schema and cross-file invariants."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

_SCRIPT_DIR = Path(__file__).resolve().parent
FIXTURES = _SCRIPT_DIR / "fixtures"

BENCH_PIPELINES = frozenset({"eval", "ir", "typecheck"})
INTEG_PIPELINES = frozenset({"eval", "ir", "typecheck", "serve"})


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


def _load_array(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ValueError(f"{path.name}: root must be a JSON array")
    return data


def check_issues_fast(errors: list[str]) -> None:
    path = FIXTURES / "issues_fast.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    targets = data.get("targets")
    if not isinstance(targets, list) or not targets:
        _fail(errors, f"{path.name}: 'targets' must be a non-empty array")
        return
    names: list[str] = []
    for i, target in enumerate(targets):
        if not isinstance(target, str) or not target.startswith("test_issue_"):
            _fail(errors, f"issues_fast.json[{i}]: target must be test_issue_* name")
            continue
        names.append(target)
    _check_unique_names(errors, "issues_fast.json", names)


def check_benchmark(errors: list[str]) -> set[str]:
    path = FIXTURES / "benchmark_tests.json"
    items = _load_array(path)
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"benchmark_tests.json[{i}]"
        if not isinstance(item, dict):
            _fail(errors, f"{ctx}: entry must be an object")
            continue
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "code")
        pipeline = _require_str(errors, ctx, item, "pipeline")
        if pipeline and pipeline not in BENCH_PIPELINES:
            _fail(errors, f"{ctx}: pipeline must be one of {sorted(BENCH_PIPELINES)}")
        if item.get("typecheck_suite") and not item.get("expected_type"):
            _fail(errors, f"{ctx}: typecheck_suite entries require expected_type")
        if name:
            names.append(name)
    _check_unique_names(errors, "benchmark_tests.json", names)
    return set(names)


def check_integ(errors: list[str]) -> None:
    path = FIXTURES / "integ_tests.json"
    items = _load_array(path)
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"integ_tests.json[{i}]"
        if not isinstance(item, dict):
            _fail(errors, f"{ctx}: entry must be an object")
            continue
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "code")
        pipeline = _require_str(errors, ctx, item, "pipeline")
        if pipeline and pipeline not in INTEG_PIPELINES:
            _fail(errors, f"{ctx}: pipeline must be one of {sorted(INTEG_PIPELINES)}")
        if "expected_status" in item and not isinstance(item["expected_status"], int):
            _fail(errors, f"{ctx}: expected_status must be an integer")
        if name:
            names.append(name)
    _check_unique_names(errors, "integ_tests.json", names)


def check_regression(errors: list[str]) -> None:
    path = FIXTURES / "regression_tests.json"
    items = _load_array(path)
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"regression_tests.json[{i}]"
        if not isinstance(item, dict):
            _fail(errors, f"{ctx}: entry must be an object")
            continue
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "code")
        for key in ("expect_out", "expect_err"):
            if key in item and not isinstance(item[key], str):
                _fail(errors, f"{ctx}: '{key}' must be a string")
        if name:
            names.append(name)
    _check_unique_names(errors, "regression_tests.json", names)


def check_smoke(errors: list[str]) -> None:
    path = FIXTURES / "smoke_tests.json"
    items = _load_array(path)
    names: list[str] = []
    for i, item in enumerate(items):
        ctx = f"smoke_tests.json[{i}]"
        if not isinstance(item, dict):
            _fail(errors, f"{ctx}: entry must be an object")
            continue
        name = _require_str(errors, ctx, item, "name")
        _require_str(errors, ctx, item, "command")
        _require_str(errors, ctx, item, "expected")
        if name:
            names.append(name)
    _check_unique_names(errors, "smoke_tests.json", names)


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
            f"but benchmark_tests.json has {len(fixture_names)} cases",
        )


def run_check() -> int:
    errors: list[str] = []
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

    print(f"Fixture validation OK (benchmark={len(fixture_names)}, integ/regression/smoke checked)")
    return 0


def main() -> None:
    sys.exit(run_check())


if __name__ == "__main__":
    main()
