"""Smoke fixture cases (tests/fixtures/smoke/*.json) — #1962."""

from __future__ import annotations

from dataclasses import dataclass

from fixture_store import load_case_array


@dataclass
class SmokeCase:
    name: str
    command: str
    expected: str


def load_smoke_cases() -> list[SmokeCase]:
    return [
        SmokeCase(
            name=item["name"],
            command=item["command"],
            expected=item["expected"],
        )
        for item in load_case_array("smoke")
    ]
