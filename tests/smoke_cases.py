"""Smoke test fixtures for build.py."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "smoke_tests.json"


@dataclass
class SmokeCase:
    name: str
    command: str
    expected: str


def load_smoke_cases() -> list[SmokeCase]:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    return [SmokeCase(name=item["name"], command=item["command"], expected=item["expected"]) for item in raw]
