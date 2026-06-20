"""Integration test case fixtures for build.py integ suite."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "integ_tests.json"


@dataclass
class IntegCase:
    name: str
    code: str
    pipeline: str  # eval | ir | typecheck | serve
    expected: str = ""
    expected_err: str = ""
    expected_status: int = 0


def load_integ_cases() -> list[IntegCase]:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    return [
        IntegCase(
            name=item["name"],
            code=item["code"],
            pipeline=item["pipeline"],
            expected=item.get("expected", ""),
            expected_err=item.get("expected_err", ""),
            expected_status=item.get("expected_status", 0),
        )
        for item in raw
    ]
