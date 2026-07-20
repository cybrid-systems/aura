"""Load Aura test case fixtures from shards or legacy mono JSON (#1962).

Layout (preferred):
  tests/fixtures/<kind>/*.json   # hand-maintained shards (arrays of objects)
  tests/fixtures/<kind>_tests.json  # optional legacy mono (still supported)

`load_case_array("regression")` merges every `fixtures/regression/*.json`
(sorted by name). If the shard directory is missing, falls back to
`fixtures/regression_tests.json`.

Kinds used today: regression, integ, benchmark, smoke.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def shard_dir(kind: str) -> Path:
    return FIXTURES / kind


def mono_path(kind: str) -> Path:
    # smoke uses smoke_tests.json; others use <kind>_tests.json
    return FIXTURES / f"{kind}_tests.json"


def list_shard_files(kind: str) -> list[Path]:
    d = shard_dir(kind)
    if not d.is_dir():
        return []
    return sorted(p for p in d.glob("*.json") if p.is_file() and not p.name.startswith("_"))


def load_case_array(kind: str) -> list[dict[str, Any]]:
    """Return the merged list of case objects for *kind*."""
    shards = list_shard_files(kind)
    if shards:
        cases: list[dict[str, Any]] = []
        for path in shards:
            data = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(data, list):
                raise ValueError(f"{path.relative_to(FIXTURES)}: root must be a JSON array")
            for i, item in enumerate(data):
                if not isinstance(item, dict):
                    raise ValueError(f"{path.name}[{i}]: entry must be an object")
                cases.append(item)
        return cases

    mono = mono_path(kind)
    if mono.is_file():
        data = json.loads(mono.read_text(encoding="utf-8"))
        if not isinstance(data, list):
            raise ValueError(f"{mono.name}: root must be a JSON array")
        return list(data)

    raise FileNotFoundError(f"no fixtures for kind={kind!r}: expected {shard_dir(kind)}/*.json or {mono_path(kind)}")


def fixture_status() -> list[dict[str, Any]]:
    """Summary rows for tooling / docs regeneration."""
    rows: list[dict[str, Any]] = []
    for kind in ("regression", "integ", "benchmark", "smoke"):
        shards = list_shard_files(kind)
        mono = mono_path(kind)
        if shards:
            counts = []
            total = 0
            bytes_ = 0
            for p in shards:
                data = json.loads(p.read_text(encoding="utf-8"))
                n = len(data) if isinstance(data, list) else 0
                counts.append((p.name, n, p.stat().st_size))
                total += n
                bytes_ += p.stat().st_size
            rows.append(
                {
                    "kind": kind,
                    "mode": "shards",
                    "files": len(shards),
                    "cases": total,
                    "bytes": bytes_,
                    "shards": counts,
                }
            )
        elif mono.is_file():
            data = json.loads(mono.read_text(encoding="utf-8"))
            n = len(data) if isinstance(data, list) else 0
            rows.append(
                {
                    "kind": kind,
                    "mode": "mono",
                    "files": 1,
                    "cases": n,
                    "bytes": mono.stat().st_size,
                    "path": mono.name,
                }
            )
        else:
            rows.append({"kind": kind, "mode": "missing", "files": 0, "cases": 0, "bytes": 0})
    return rows
