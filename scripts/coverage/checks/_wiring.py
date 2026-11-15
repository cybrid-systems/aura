#!/usr/bin/env python3
"""Wiring assertions: is a coverage check actually gated? (Issue #5246)

Before #5246 an issue contract proved it was gated by grepping build.py for
its own linter name: the check was hand-wired into build.py's 10k-line
`cmd_lint` chain, so "the name appears in build.py" meant "the gate runs it".
That chain is gone. Gate coverage is now scripts/coverage/run_checks.py
discovery plus the declarative manifests under scripts/coverage/manifests/,
so this module renders the same fact from the runner itself:

  * checks/check_*.py that run_checks.discover_checks() returns (thin
    manifest shims excepted — run_checks drives runner.py for those), and
  * manifest rows for a check script or an issue.

WIRED_TEXT is that rendering. A contract row

    must("check_foo", "ACn", wired_text)

therefore still fails when the check file is deleted, when it stops being
discovered (for example it degrades into a thin wrapper), or when its
manifest row goes away. It no longer fails merely because build.py stopped
being a list of 1054 check names.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
COVERAGE = Path(__file__).resolve().parents[1]
CHECKS = COVERAGE / "checks"
MANIFESTS = COVERAGE / "manifests"


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        return None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_run_checks = _load_module("coverage_run_checks", COVERAGE / "run_checks.py")


def discovered() -> list[str]:
    """Check scripts run_checks.py actually runs, as repo-relative paths."""
    if _run_checks is None:  # pragma: no cover - defensive
        return sorted(str(p.relative_to(ROOT)) for p in CHECKS.glob("check_*.py"))
    out: list[str] = []
    for p in _run_checks.discover_checks():
        try:
            out.append(str(p.resolve().relative_to(ROOT)))
        except ValueError:  # pragma: no cover - defensive
            continue
    return sorted(out)


def manifest_rows() -> list[tuple[str, str]]:
    """(issue-or-name, declared path) pairs from every manifest."""
    rows: list[tuple[str, str]] = []
    if not MANIFESTS.is_dir():
        return rows
    for path in sorted(MANIFESTS.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError):  # pragma: no cover - defensive
            rows.append((path.stem, str(path.relative_to(ROOT))))
            continue
        issue = str(data.get("issue") or path.stem)
        rows.append((issue, str(path.relative_to(ROOT))))
        for row in data.get("checks") or ():
            rel = str(row.get("path") or "")
            if rel:
                rows.append((issue, rel))
    return rows


def _render() -> str:
    lines: list[str] = []
    for rel in discovered():
        lines.append(f"{Path(rel).name} {rel}")
    for issue, rel in manifest_rows():
        lines.append(f"manifest {issue} {rel}")
    return "\n".join(lines) + "\n"


WIRED_TEXT = _render()
_WIRED_NAMES = frozenset(rel.rsplit("/", 1)[-1] for rel in discovered())
_ROWS = manifest_rows()
# Thin manifest shims are not discovered by run_checks (it drives runner.py
# for them); the manifest row that names the shim is their wiring.
_MANIFEST_PATHS = frozenset(Path(rel).name for _, rel in _ROWS)
_MANIFEST_OWNERS = frozenset(issue for issue, _ in _ROWS)


def is_wired(name: str) -> bool:
    """True when `name` is a check the gate runs (script or manifest row)."""
    base = Path(name).name
    if not base.endswith(".py"):
        base += ".py"
    return base in _WIRED_NAMES or base in _MANIFEST_PATHS or Path(name).stem in _MANIFEST_OWNERS


def require(fails: list[str], name: str, label: str) -> None:
    """Append a failure row when `name` is not gated by the runner."""
    if not is_wired(name):
        fails.append(
            f"{label}: {name} is not wired — no scripts/coverage/checks/{name} "
            "discovered by run_checks.py and no manifest row (Issue #5246)"
        )
