"""Issue #2914: aggregate query primitive peel sources for coverage linters."""

from __future__ import annotations

import contextlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUERY_GLOB = "evaluator_primitives_query*.cpp"


def query_prims_paths(root: Path | None = None) -> list[Path]:
    base = (root or ROOT) / "src" / "compiler"
    return sorted(base.glob(QUERY_GLOB))


def read_query_prims(root: Path | None = None) -> str:
    parts: list[str] = []
    for p in query_prims_paths(root):
        with contextlib.suppress(OSError):
            parts.append(p.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)
