"""Typecheck-focused test fixtures for build.py."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "typecheck_tests.json"


@dataclass
class TypecheckCase:
    name: str
    code: str
    expected_type: str


def load_typecheck_cases() -> list[TypecheckCase]:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    return [
        TypecheckCase(
            name=item["name"],
            code=item["code"],
            expected_type=item["expected_type"],
        )
        for item in raw
    ]
