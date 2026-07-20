"""Benchmark and typecheck-suite fixtures (#1962 shards)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from fixture_store import load_case_array


@dataclass
class BenchCase:
    name: str
    code: str
    pipeline: str  # eval | ir | typecheck
    expected_val: Any = None
    expected_type: str | None = None
    expected_err: str | None = None


@dataclass
class TypecheckCase:
    name: str
    code: str
    expected_type: str


def _load_raw() -> list[dict]:
    return load_case_array("benchmark")


def load_benchmark_cases() -> list[BenchCase]:
    return [
        BenchCase(
            name=item["name"],
            code=item["code"],
            pipeline=item["pipeline"],
            expected_val=item.get("expected_val"),
            expected_type=item.get("expected_type"),
            expected_err=item.get("expected_err"),
        )
        for item in _load_raw()
    ]


def load_typecheck_cases() -> list[TypecheckCase]:
    return [
        TypecheckCase(
            name=item["name"],
            code=item["code"],
            expected_type=item["expected_type"],
        )
        for item in _load_raw()
        if item.get("typecheck_suite") and item.get("expected_type")
    ]
