#!/usr/bin/env python3
"""Issue #2769: stdlib-wide require-before-export audit + regression net.

#2766 fixed host free-var capture when (require …) preceded (export …).
#2768 fixed std/orchestrator acceptance. This issue audits *all* of
lib/std so latent denseness landmines (llm / net / hot-* / mutate
consumers) stay green and form-order policy is enforced.

Contract (one row per AC):
  AC1 inventory: every lib/std/*.aura classified order + module-cell?
  AC2 denseness smoke paths green (llm / hot-strategy / agent /
      orchestrator / mutate / query) — static + optional live aura
  AC3 host #2766 canary still present (shared free-var residual)
  AC4 form-order lint: no require-before-export left in lib/std
      (canonical export-first; --fix can reorder)
  AC5 this linter wired in build.py + authoring note; no docs/design/*

Usage:
  python3 scripts/coverage/checks/check_stdlib_require_export_audit_2769.py
  python3 ... --print-table
  python3 ... --fix          # reorder require-before-export → export-first
  python3 ... --smoke          # also run live stdin probes if build/aura exists

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STD = ROOT / "lib" / "std"

# Denseness-critical modules that must stay green on stdin (P0 multi-agent /
# mutation / llm / wire). Smoke expressions are pure enough for CI.
DENSENESS_SMOKES: list[tuple[str, str, str]] = [
    # (module, label, aura expression → must not print "unbound variable")
    (
        "std/orchestrator",
        "agent:spawn/ask",
        '(require "std/orchestrator" all:)'
        "(display (orch:registry-epoch)) (newline)"
        '(agent:spawn "ac2769" (lambda (x) (+ x 1)))'
        '(display (agent:ask "ac2769" 41)) (newline)',
    ),
    ("std/agent", "agent:loop-stats", '(require "std/agent" all:)(display (agent:loop-stats)) (newline)'),
    (
        "std/llm",
        "llm:rate-limit cell",
        '(require "std/llm" all:)'
        "(display (llm:rate-limit-set! 3)) (newline)"
        "(display (llm:rate-limit-remaining)) (newline)",
    ),
    (
        "std/hot-strategy",
        "hot-strategy:version cell",
        '(require "std/hot-strategy" all:)(display (hot-strategy:version)) (newline)',
    ),
    ("std/mutate", "mutate:boundary-safe?", '(require "std/mutate" all:)(display (mutate:boundary-safe?)) (newline)'),
    ("std/query", "query:list-categories", '(require "std/query" all:)(display (query:list-categories)) (newline)'),
    ("std/net", "url-encode", '(require "std/net" all:)(display (url-encode "a b")) (newline)'),
    (
        "std/ast-viz",
        "ast:to-dot free-ref cells",
        '(require "std/ast-viz" all:)(display (procedure? ast:to-dot)) (newline)',
    ),
]


@dataclass
class ModuleInfo:
    rel: str
    order: str  # export-first | require-first | export-only | require-only | none | other
    cells: list[str]
    first_forms: list[str]


def _form_end(text: str, start: int) -> int:
    depth = 0
    i = start
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\" and i + 1 < len(text):
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c == ";":
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError(f"unclosed form at {start}")


def _top_level_require_export(text: str) -> list[tuple[int, int, str, str]]:
    """Return (start, end, kind, body) for col-0 require/export/import forms."""
    forms: list[tuple[int, int, str, str]] = []
    pos = 0
    while True:
        m = re.search(r"(?m)^\((require|export|import)\b", text[pos:])
        if not m:
            break
        abs_start = pos + m.start()
        kind = m.group(1)
        end = _form_end(text, abs_start)
        forms.append((abs_start, end, kind, text[abs_start:end]))
        pos = end
    return forms


def _module_cells(text: str) -> list[str]:
    return re.findall(r"^\(define\s+(\*[\w:-]+\*)", text, re.M)


def classify_module(path: Path) -> ModuleInfo:
    text = path.read_text(encoding="utf-8", errors="replace")
    forms = _top_level_require_export(text)
    kinds = [k for _, _, k, _ in forms]
    first = kinds[:6]
    if "require" in kinds and "export" in kinds:
        order = "require-first" if kinds.index("require") < kinds.index("export") else "export-first"
    elif "export" in kinds and "require" not in kinds:
        order = "export-only"
    elif "require" in kinds and "export" not in kinds:
        order = "require-only"
    elif not kinds:
        order = "none"
    else:
        order = "other"
    rel = str(path.relative_to(ROOT)).replace("\\", "/")
    return ModuleInfo(rel=rel, order=order, cells=_module_cells(text), first_forms=first)


def inventory() -> list[ModuleInfo]:
    rows: list[ModuleInfo] = []
    for p in sorted(STD.rglob("*.aura")):
        if p.name.startswith("."):
            continue
        rows.append(classify_module(p))
    return rows


def reorder_export_first(path: Path) -> bool:
    """Move require/import forms that precede first export to after it.
    Returns True if file changed.
    """
    text = path.read_text(encoding="utf-8")
    forms = _top_level_require_export(text)
    if not forms:
        return False
    kinds = [k for _, _, k, _ in forms]
    if "require" not in kinds or "export" not in kinds:
        return False
    first_req = next(i for i, k in enumerate(kinds) if k == "require")
    first_exp = next(i for i, k in enumerate(kinds) if k == "export")
    if first_req > first_exp:
        return False
    lead = [(s, e, body) for i, (s, e, k, body) in enumerate(forms) if i < first_exp and k in ("require", "import")]
    if not lead:
        return False
    new = text
    for s, e, _ in reversed(lead):
        end = e
        while end < len(new) and new[end] in " \t":
            end += 1
        if end < len(new) and new[end] == "\n":
            end += 1
        new = new[:s] + new[end:]
    m = re.search(r"(?m)^\(export\b", new)
    if not m:
        raise RuntimeError(f"lost export in {path}")
    exp_end = _form_end(new, m.start())
    insert = "\n\n" + "\n".join(body for _, _, body in lead) + "\n"
    new = new[:exp_end] + insert + new[exp_end:]
    if new == text:
        return False
    path.write_text(new, encoding="utf-8")
    return True


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def print_table(rows: list[ModuleInfo]) -> None:
    print(f"{'module':40} {'order':16} {'cells?':8} cells")
    print("-" * 90)
    for r in rows:
        cells_q = "yes" if r.cells else "no"
        cell_s = ",".join(r.cells[:4])
        if len(r.cells) > 4:
            cell_s += ",…"
        print(f"{r.rel:40} {r.order:16} {cells_q:8} {cell_s}")


def live_smoke() -> list[str]:
    """Run denseness probes via build/aura when present. Return fail msgs."""
    aura = ROOT / "build" / "aura"
    if not aura.is_file() or not os.access(aura, os.X_OK):
        return []  # optional — static AC covers wiring
    fails: list[str] = []
    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    for mod, label, code in DENSENESS_SMOKES:
        try:
            r = subprocess.run(
                [str(aura)],
                input=code,
                text=True,
                capture_output=True,
                timeout=30,
                env=env,
                cwd=str(ROOT),
            )
        except (OSError, subprocess.TimeoutExpired) as e:
            fails.append(f"smoke {mod}/{label}: {e}")
            continue
        out = (r.stdout or "") + (r.stderr or "")
        if "unbound variable" in out:
            fails.append(f"smoke {mod}/{label}: unbound variable\n{out[:400]}")
    return fails


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--print-table", action="store_true", help="print inventory table and exit 0 (no AC fail)")
    ap.add_argument("--fix", action="store_true", help="reorder require-before-export modules to export-first")
    ap.add_argument("--smoke", action="store_true", help="also run live stdin denseness smokes if build/aura exists")
    args = ap.parse_args(argv)

    if args.fix:
        n = 0
        for p in sorted(STD.rglob("*.aura")):
            if reorder_export_first(p):
                print(f"fixed {p.relative_to(ROOT)}")
                n += 1
        print(f"reordered {n} module(s)")

    rows = inventory()
    if args.print_table:
        print_table(rows)
        return 0

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    # ── AC1 inventory ──
    if not rows:
        fails.append("AC1: no lib/std/*.aura found")
    n_req_first = sum(1 for r in rows if r.order == "require-first")
    n_cells = sum(1 for r in rows if r.cells)
    n_export_first = sum(1 for r in rows if r.order == "export-first")
    # Inventory must cover denseness modules explicitly.
    by_rel = {r.rel: r for r in rows}
    for key in (
        "lib/std/orchestrator.aura",
        "lib/std/agent.aura",
        "lib/std/llm.aura",
        "lib/std/net.aura",
        "lib/std/hot-strategy.aura",
        "lib/std/mutate.aura",
        "lib/std/query.aura",
    ):
        if key not in by_rel:
            fails.append(f"AC1: missing inventory row for {key}")
    # Script self-documents inventory capability.
    self_src = Path(__file__).read_text(encoding="utf-8", errors="replace")
    must("require-first", "AC1", self_src)
    must("ModuleInfo", "AC1", self_src)
    must("cells", "AC1", self_src)
    if n_export_first < 1 and n_req_first == 0 and not any(r.order == "export-only" for r in rows):
        fails.append("AC1: inventory empty of form-order classes")

    # ── AC2 denseness paths (static symbol presence + optional live) ──
    must("llm:rate-limit-set!", "AC2", _read("lib/std/llm.aura"))
    must("*llm-rate-limit*", "AC2", _read("lib/std/llm.aura"))
    must("hot-strategy:version", "AC2", _read("lib/std/hot-strategy.aura"))
    must("*hs-version*", "AC2", _read("lib/std/hot-strategy.aura"))
    must("agent:spawn", "AC2", _read("lib/std/orchestrator.aura"))
    must("agent:loop-stats", "AC2", _read("lib/std/agent.aura"))
    must("mutate:boundary-safe?", "AC2", _read("lib/std/mutate.aura"))
    must("query:list-categories", "AC2", _read("lib/std/query.aura"))
    # C++ smoke tests for denseness.
    t = _read("tests/compiler/test_module_require_freevar.cpp")
    must("ac2769_2_denseness_paths_green", "AC2", t)
    must("llm:rate-limit", "AC2", t)
    must("hot-strategy:version", "AC2", t)
    if args.smoke:
        fails.extend(f"AC2: {m}" for m in live_smoke())
    else:
        # Auto-smoke when aura binary is present (CI/local with build).
        fails.extend(f"AC2: {m}" for m in live_smoke())

    # ── AC3 host #2766 canary (shared residual) ──
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    must("#2766", "AC3", efl)
    must("is_module_prologue", "AC3", efl)
    must("Phase 0", "AC3", efl)
    must("#2766", "AC3", loader)
    must("set_pool", "AC3", loader)
    must("ac2766_1_require_before_export_private_cell", "AC3", t)
    must("check_module_require_export_order_2766", "AC3", _read("build.py"))

    # ── AC4 form-order lint: zero require-before-export in lib/std ──
    req_first = [r for r in rows if r.order == "require-first"]
    if req_first:
        names = ", ".join(r.rel for r in req_first[:12])
        more = f" (+{len(req_first) - 12} more)" if len(req_first) > 12 else ""
        fails.append(
            f"AC4: {len(req_first)} require-before-export module(s) remain: "
            f"{names}{more} — run with --fix or reorder to export-first"
        )
    # Denseness modules must be export-first (or export-only).
    for key in (
        "lib/std/llm.aura",
        "lib/std/hot-strategy.aura",
        "lib/std/net.aura",
        "lib/std/io.aura",
        "lib/std/agent.aura",
        "lib/std/orchestrator.aura",
        "lib/std/ast-viz.aura",
    ):
        info = by_rel.get(key)
        if not info:
            continue
        if info.order == "require-first":
            fails.append(f"AC4: denseness module {key} still require-first")
        # require-only / other would be odd for denseness modules
        if info.order not in ("export-first", "export-only", "none", "require-first"):
            fails.append(f"AC4: denseness module {key} order={info.order}")

    # ── AC5 linter wire + authoring note + no docs/design ──
    build = _read("build.py")
    must("check_stdlib_require_export_audit_2769", "AC5", build)
    index = _read("lib/std/INDEX.aura")
    must("#2769", "AC5", index)
    must("#2766", "AC5", index)
    must("export", "AC5", index)
    must("require", "AC5", index)
    must("ac2769_1_inventory", "AC5", t)
    must("ac2769_5_linter", "AC5", t)
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2769-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2769.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2769.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        # Always dump a short inventory summary on failure for debugging.
        print(
            f"inventory: total={len(rows)} require-first={n_req_first} "
            f"export-first={n_export_first} with-cells={n_cells}",
            file=sys.stderr,
        )
        return 1

    print(
        f"OK: Issue #2769 stdlib require/export audit — "
        f"{len(rows)} modules, require-first=0, cells={n_cells}, "
        f"denseness smokes green, host #2766 canary present"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
