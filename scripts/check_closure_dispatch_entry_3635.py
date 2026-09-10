#!/usr/bin/env python3
# scripts/check_closure_dispatch_entry_3635.py -- Issue #3635 source-cite gate.
#
# Verifies anonymous (sid==0 / unnamed) closure native dispatch has a single
# enforcement point: the blessed entry aura_closure_dispatch_native_checked
# in the table-internal TU (src/compiler/aura_jit_runtime.cpp). Every
# dispatch surface (prim dispatch, closure_bridge_, FFI return, unnamed
# call path) must route through it; direct closure-table reads / native
# fn-ptr invocation outside the table TU fail the gate.
#
#  AC1: blessed entry exists (extern "C" definition citing #3635) and owns
#       the full call-time transaction (MustDeopt #2472, dual-fresh
#       aura_is_jit_closure_fresh, deopt_pending consult
#       closure_call_deopt_pending_leave_native_); aura_closure_call is a
#       thin forward to it; runtime_shared.h declares the entry.
#  AC2: g_closure_func_ids / g_jit_fns[ / JitFnEntry appear ONLY in
#       src/compiler/aura_jit_runtime.cpp among src/** - any other TU
#       referencing them without a per-line "#3635-allow-direct"
#       annotation fails. (--probe-file PATH scans one extra file as an
#       outside-TU stub - the test's adversarial face.)
#  AC3: test face in test_closure_call_must_deopt_toctou.cpp (entry
#       transaction parity, stale-epoch fallback through the entry,
#       adversarial --probe-file stub, self-test clean).
#  AC4: no docs/design/3635-* (#1655); no tests/**/test_issue_3635.cpp
#       (#81934); build.py wires this linter; the two-load fast-path
#       generation double-check (#1707) stays in the entry.

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

RUNTIME = "src/compiler/aura_jit_runtime.cpp"
HDR = "src/compiler/runtime_shared.h"
TEST = "tests/compiler/test_closure_call_must_deopt_toctou.cpp"

ALLOW_ANNOTATION = "#3635-allow-direct"
TABLE_TOKENS: tuple[str, ...] = ("g_closure_func_ids", "g_jit_fns[", "JitFnEntry")
SCAN_SUFFIXES = {".cpp", ".cc", ".cxx", ".hh", ".hpp", ".h", ".ixx"}

# Entry-body window: transaction tokens that must sit between the blessed
# entry definition and the aura_closure_call forward wrapper.
ENTRY_DEF = 'extern "C" int64_t aura_closure_dispatch_native_checked('
WRAPPER_DEF = 'extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc) {'
WINDOW_TOKENS: tuple[str, ...] = (
    "g_closure_must_deopt",  # #2128/#2472 MustDeopt consume TOCTOU
    "aura_is_jit_closure_fresh",  # #1508 dual-fresh
    "closure_call_deopt_pending_leave_native_",  # #3441 deopt_pending consult
    "Issue #2472",  # TOCTOU transaction marker
)

REQUIRED: tuple[tuple[str, str, str], ...] = (
    (RUNTIME, re.escape(ENTRY_DEF), "3635 AC1: blessed entry defined (extern C) in the table TU"),
    (RUNTIME, r"Issue\s+#3635", "3635 AC1: entry cites #3635"),
    (RUNTIME, re.escape(WRAPPER_DEF), "3635 AC1: aura_closure_call forward wrapper present"),
    (HDR, r"aura_closure_dispatch_native_checked", "3635 AC1: header declares the entry"),
    (TEST, r"Issue\s+#3635", "3635 AC3: test hosts #3635 ACs"),
    (TEST, r"aura_closure_dispatch_native_checked", "3635 AC3: test dispatches through the entry"),
    (TEST, r"--probe-file", "3635 AC3: test drives the adversarial probe-file face"),
    ("build.py", r"check_closure_dispatch_entry_3635", "3635 AC4: build.py wires the linter"),
    (RUNTIME, r"\(g1 & 1ull\) == 0", "3635 AC4: two-load fast-path generation double-check preserved"),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def scan_text_lines(text: str, origin: str) -> list[str]:
    """Flag table tokens on lines lacking the per-line allow annotation."""
    failures: list[str] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if ALLOW_ANNOTATION in line:
            continue
        for token in TABLE_TOKENS:
            if token in line:
                failures.append(
                    f"3635 AC2: direct closure-table access '{token}' outside the table TU "
                    f"at {origin}:{lineno} (annotate the line with {ALLOW_ANNOTATION} if blessed)"
                )
                break
    return failures


def scan_outside_tu(root: Path, table_rel: str = RUNTIME) -> list[str]:
    """Scan root/src/** for table tokens outside the table-internal TU."""
    failures: list[str] = []
    src = root / "src"
    if not src.is_dir():
        return [f"3635 AC2: missing src/ under {root}"]
    table_abs = (root / table_rel).resolve()
    for path in sorted(src.rglob("*")):
        if not path.is_file() or path.suffix not in SCAN_SUFFIXES:
            continue
        if path.resolve() == table_abs:
            continue
        try:
            text = read(path)
        except OSError:
            continue
        failures.extend(scan_text_lines(text, str(path.relative_to(root))))
    return failures


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    # AC2: table tokens stay table-TU-only across src/**.
    failures.extend(scan_outside_tu(REPO_ROOT))

    # AC1: the entry body owns the full transaction (window between the
    # blessed entry definition and the forward wrapper).
    rt = body(RUNTIME)
    def_pos = rt.find(ENTRY_DEF)
    wrap_pos = rt.find(WRAPPER_DEF)
    if def_pos == -1 or wrap_pos == -1 or wrap_pos <= def_pos:
        failures.append("3635 AC1: entry/wrapper anchors not found in order")
    else:
        window = rt[def_pos:wrap_pos]
        for token in WINDOW_TOKENS:
            if token not in window:
                failures.append(f"3635 AC1: entry body missing transaction token '{token}'")

    # AC4: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3635-"):
                failures.append(f"3635 AC4: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3635.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3635.cpp",
    ):
        if probe.exists():
            failures.append(f"3635 AC4: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def probe_file(path: str) -> int:
    """Treat one file as an outside-TU stub (test adversarial face)."""
    target = Path(path)
    if not target.is_file():
        print(f"check_closure_dispatch_entry_3635: probe-file not found: {path}")
        return 2
    failures = scan_text_lines(read(target), str(target))
    if failures:
        print(f"check_closure_dispatch_entry_3635: probe-file {len(failures)} violation(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1
    print("check_closure_dispatch_entry_3635: probe-file clean")
    return 0


def self_test() -> int:
    """Regexes compile + anchor targets exist + synthetic scan probe."""
    ok = True
    for _, pattern, label in REQUIRED:
        try:
            re.compile(pattern)
        except re.error as e:
            print(f"SELF-TEST FAIL (regex compile): {label}: {e}")
            ok = False
    for path, _, _ in REQUIRED:
        if not (REPO_ROOT / path).exists():
            print(f"SELF-TEST FAIL (missing target): {path}")
            ok = False

    # Synthetic scan probe: table TU clean, bare outside read flagged,
    # annotated outside read clean.
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        table = root / "src" / "compiler"
        table.mkdir(parents=True)
        (table / "aura_jit_runtime.cpp").write_text("static std::vector<int64_t> g_closure_func_ids;\n")
        other = root / "src" / "other"
        other.mkdir()
        (other / "bad.cpp").write_text("const int64_t* p = g_closure_func_ids.data();\n")
        (other / "ok.cpp").write_text("const int64_t* p = g_closure_func_ids.data(); // #3635-allow-direct\n")
        found = scan_outside_tu(root)
        if len(found) != 1 or "bad.cpp" not in found[0]:
            print(f"SELF-TEST FAIL (scan probe): expected exactly bad.cpp flagged, got {found}")
            ok = False

    if ok:
        print("self-test: regexes compile + targets present + scan probe clean")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3635 blessed closure dispatch entry gate")
    ap.add_argument("--self-test", action="store_true", help="compile regexes + probe targets + scan probe")
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    ap.add_argument("--probe-file", metavar="PATH", help="scan one file as an outside-TU stub")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.probe_file:
        return probe_file(args.probe_file)
    failures = run_checks()
    if failures:
        print(f"check_closure_dispatch_entry_3635: {len(failures)} failure(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1 if args.strict else 0
    print("check_closure_dispatch_entry_3635: clean (blessed entry owns anon closure native dispatch)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
