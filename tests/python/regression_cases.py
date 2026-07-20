"""Regression fixture cases (tests/fixtures/regression/*.json) — #1962."""

from __future__ import annotations

from dataclasses import dataclass

from fixture_store import load_case_array


@dataclass
class RegressionCase:
    name: str
    code: str
    expect_out: str = ""
    expect_err: str = ""


def load_regression_cases() -> list[RegressionCase]:
    return [
        RegressionCase(
            name=item["name"],
            code=item["code"],
            expect_out=item.get("expect_out", ""),
            expect_err=item.get("expect_err", ""),
        )
        for item in load_case_array("regression")
    ]
