"""Regression test fixtures for tests/test_regression.py."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "regression_tests.json"


@dataclass
class RegressionCase:
    name: str
    code: str
    expect_out: str
    expect_err: str


def load_regression_cases() -> list[RegressionCase]:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    return [
        RegressionCase(
            name=item["name"],
            code=item["code"],
            expect_out=item.get("expect_out", ""),
            expect_err=item.get("expect_err", ""),
        )
        for item in raw
    ]
