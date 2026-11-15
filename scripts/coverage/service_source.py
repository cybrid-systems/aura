"""Read the aura.compiler.service module source (interface + implementation units).

#5242: member bodies live in service_*.cpp units; a source-cite check pins a
sentence, not a character window, so it reads the whole module surface.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_INTERFACE = "service.ixx"


def service_source() -> str:
    base = ROOT / "src" / "compiler"
    # Implementation units first: contract rows locate a definition with
    # find("Ret name("), which would otherwise hit the in-class declaration.
    units = sorted(p.name for p in base.glob("service_*.cpp"))
    units.append(_INTERFACE)
    parts = []
    for unit in units:
        path = base / unit
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if unit.endswith(".cpp"):
            # Re-indent one level so rows written for the old in-class bodies
            # (which end with "\n    }\n") keep locating the function end.
            text = "\n".join(("    " + line) if line.strip() else line for line in text.split("\n"))
        parts.append(text)
    # Moved bodies are out-of-class definitions (CompilerService::name); strip
    # the qualifier so existing contract rows that pin "Ret name(...)" keep
    # matching after a #5242 move.
    return re.sub(r"(?<![&:])CompilerService::", "", chr(10).join(parts))
