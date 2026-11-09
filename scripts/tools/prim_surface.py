#!/usr/bin/env python3
"""Single source of truth for the primitive registration scan (Issue #5008).

Aura registers primitives through two independent channels, and both are part
of the public catalog:

1. add("name", ...) / register_prim(add, ev, "name", ...) in src/**/*.cpp
   (the #1552 / #2915 scan).
2. The Evaluator ctor hot table table_["name"] = ... (plus set_meta) in
   src/compiler/evaluator_primitives_builtins.cpp.  + - * / < <= = > >= not
   and or eq? live *only* there — a name-level inventory that misses this
   channel reports 503 names where the engine actually exposes 516.

scripts/tools/gen_docs.py and
scripts/coverage/checks/check_primitive_tiers_5008.py both import this module,
so the generated docs and the CI tier gate can never see different catalogs.

The scan is regex based on purpose: every registration form in the tree is one
of the four shapes below, and a full C++ parse would buy nothing.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
STD = ROOT / "lib" / "std"

# Channel 2: the ctor hot table (Issue #5008).
HOT_TABLE_TU = "src/compiler/evaluator_primitives_builtins.cpp"

# Channel 1: historical registrar forms.
PRIMITIVE_RE = re.compile(r'primitives_\.add\s*\(\s*"([^"]+)"')
FFI_ADD_RE = re.compile(r'^\s+add\s*\(\s*"([^"]+)"', re.MULTILINE)
REGISTER_PRIM_RE = re.compile(r'register_prim\s*\(\s*add\s*,\s*\w+\s*,\s*"([^"]+)"', re.MULTILINE)

# Channel 2: table_["name"] = ... plus the set_meta("name", ...) that gives it
# a PrimMeta. Names are deduped, so double counting is harmless.
TABLE_RE = re.compile(r'table_\["([^"]+)"\]\s*=')
SET_META_RE = re.compile(r'set_meta\s*\(\s*"([^"]+)"')

_REGISTRAR_FILES = {"ffi_primitives_impl.cpp"}

# Comment stripper for lib/std token scans (keeps string contents).
_COMMENT_RE = re.compile(r";[^\n]*")


@dataclass(frozen=True, order=True)
class Registration:
    """One registration site: the name, the TU that registers it, channel."""

    name: str
    file: str
    kind: str  # add | register_prim | table


def _registrar_add_tu(path: Path) -> bool:
    return path.name in _REGISTRAR_FILES or path.name.startswith("evaluator_primitives_")


def scan_registrations() -> list[Registration]:
    """Every registration site under src/**/*.cpp, sorted by name/file."""
    found: list[Registration] = []
    for path in sorted(SRC.rglob("*.cpp")):
        rel = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        if _registrar_add_tu(path):
            found += [Registration(m.group(1), rel, "add") for m in FFI_ADD_RE.finditer(text)]
            found += [Registration(m.group(1), rel, "register_prim") for m in REGISTER_PRIM_RE.finditer(text)]
        else:
            found += [Registration(m.group(1), rel, "add") for m in PRIMITIVE_RE.finditer(text)]
        if rel == HOT_TABLE_TU:
            found += [Registration(m.group(1), rel, "table") for m in TABLE_RE.finditer(text)]
            found += [Registration(m.group(1), rel, "table") for m in SET_META_RE.finditer(text)]
    return sorted(set(found))


def scan_names() -> list[str]:
    """Sorted unique catalog names across both channels."""
    return sorted({r.name for r in scan_registrations()})


def scan_sources() -> dict[str, list[str]]:
    """name -> sorted source TU list (both channels)."""
    out: dict[str, set[str]] = {}
    for r in scan_registrations():
        out.setdefault(r.name, set()).add(r.file)
    return {n: sorted(v) for n, v in sorted(out.items())}


def stdlib_texts() -> dict[str, str]:
    """lib/std/**/*.aura sources keyed by repo-relative path."""
    return {
        p.relative_to(ROOT).as_posix(): p.read_text(encoding="utf-8", errors="replace")
        for p in sorted(STD.rglob("*.aura"))
    }


def _tokens(text: str) -> list[str]:
    """Call-site tokens: drop ; comments, keep quoted names as bare tokens."""
    body = _COMMENT_RE.sub(" ", text)
    body = body.replace('"', " ")
    return re.findall(r"[^\s()\[\]{},']+", body)


def stdlib_references(names: list[str] | None = None) -> dict[str, list[str]]:
    """name -> ["lib/std/x.aura:LINE", ...] for names referenced by lib/std.

    String literals are scanned too: (stats:get "query:foo") reaches the
    primitive by name, so it counts as a reference.
    """
    wanted = set(names) if names is not None else None
    out: dict[str, list[str]] = {}
    for rel, text in stdlib_texts().items():
        for lineno, line in enumerate(text.splitlines(), 1):
            for tok in _tokens(line):
                if wanted is not None and tok not in wanted:
                    continue
                out.setdefault(tok, []).append(f"{rel}:{lineno}")
    if wanted is not None:
        return {n: sorted(v) for n, v in sorted(out.items()) if n in wanted}
    return {n: sorted(v) for n, v in sorted(out.items())}
