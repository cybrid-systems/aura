"""Read the aura.compiler.service module source (interface + implementation units).

#5242: member bodies live in service_*.cpp units; a source-cite check pins a
sentence, not a character window, so it reads the whole module surface.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_UNITS = (
    "service.ixx",
    "service_ctor.cpp",
    "service_dirty.cpp",
    "service_eval.cpp",
    "service_eval_ir.cpp",
    "service_exec_jit.cpp",
    "service_jit.cpp",
    "service_relower.cpp",
    "service_snapshot.cpp",
    "service_typed_mutate.cpp",
)


def service_source() -> str:
    base = ROOT / "src" / "compiler"
    parts = []
    for unit in _UNITS:
        path = base / unit
        if path.exists():
            parts.append(path.read_text(encoding="utf-8", errors="ignore"))
    return chr(10).join(parts)
