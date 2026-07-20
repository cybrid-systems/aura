"""Integration test case fixtures for build.py integ suite (#1962 shards)."""

from __future__ import annotations

from dataclasses import dataclass

from fixture_store import load_case_array


@dataclass
class IntegCase:
    name: str
    code: str
    pipeline: str  # eval | ir | typecheck | serve
    expected: str = ""
    expected_err: str = ""
    expected_status: int = 0


def load_integ_cases() -> list[IntegCase]:
    return [
        IntegCase(
            name=item["name"],
            code=item["code"],
            pipeline=item["pipeline"],
            expected=item.get("expected", ""),
            expected_err=item.get("expected_err", ""),
            expected_status=item.get("expected_status", 0),
        )
        for item in load_case_array("integ")
    ]
