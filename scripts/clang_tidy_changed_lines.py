#!/usr/bin/env python3
"""Issue #4936: clang-tidy on changed lines only (C++ Core Guidelines / C++26 rules).

The baseline gate (scripts/clang_tidy_gate.py) is a per-(file, check) ratchet:
it keeps existing debt from growing but cannot stop "fix one, add one". This
gate runs a fixed check set over the lines a commit range touches and fails
on any finding from the HARD set. SOFT checks are reported, not enforced; a
check moves from SOFT to HARD once its remediation batch in #4936 lands.

Rules and rationale: docs/cpp-style.md.

Usage (needs the Clang module build db, see ci.yml #4837):
  python3 scripts/clang_tidy_changed_lines.py --db build_clang/compile_commands.json [--base REV]

--base defaults to merge-base(HEAD, origin/main), or HEAD~1 when HEAD is
already on origin/main (CI push). Changed headers are checked through one TU
that includes them.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import clang_tidy_gate as base_gate  # noqa: E402

# Enforced on changed lines. Keep in sync with docs/cpp-style.md ([gate]).
HARD = (
    "bugprone-use-after-move",
    "bugprone-unchecked-optional-access",
    "bugprone-dangling-handle",
    "bugprone-empty-catch",  # B1: [SILENCE-PRIM-#N] marks intentional swallows
    "bugprone-exception-escape",  # B1: eprintln_nothrow; format_error ignored (.clang-tidy)
    "bugprone-narrowing-conversions",  # B1: explicit static_cast at the conversion
    "bugprone-implicit-widening-of-multiplication-result",  # B1: 2uz * 1024 ...
    "cppcoreguidelines-owning-memory",
    "cppcoreguidelines-no-malloc",
    "cppcoreguidelines-pro-type-cstyle-cast",
    "cppcoreguidelines-pro-type-const-cast",
    "cppcoreguidelines-avoid-goto",
    "modernize-avoid-c-style-cast",
    "modernize-use-nullptr",
    "modernize-use-override",
    "modernize-use-using",
)
# Reported only; promoted to HARD batch by batch (#4936 B1-B8).
SOFT = (
    "bugprone-nondeterministic-pointer-iteration-order",  # B1: views::keys/values when order-free
    "cppcoreguidelines-init-variables",
    "cppcoreguidelines-special-member-functions",
    "cppcoreguidelines-missing-std-forward",
    "performance-unnecessary-value-param",
    "performance-unnecessary-copy-initialization",
    "performance-for-range-copy",
    "performance-move-const-arg",
    "performance-noexcept-move-constructor",
    "performance-noexcept-swap",
    "performance-inefficient-string-concatenation",
    "modernize-use-scoped-lock",
)
SRC_EXT = (".cpp", ".ixx", ".cppm")
HDR_EXT = (".h", ".hh", ".hpp", ".inc")
EXCLUDE = ("src/linenoise/",)
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def git(*args: str) -> str:
    # `safe.directory=*` keeps the gate working in container jobs where
    # actions/checkout's temporary global config does not reach the job
    # container (git then refuses with "dubious ownership", rc=128).
    return subprocess.run(
        ["git", "-c", "safe.directory=*", *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()


def pick_base(explicit: str | None) -> str:
    if explicit:
        return explicit
    head = git("rev-parse", "HEAD")
    try:
        mb = git("merge-base", "HEAD", "origin/main")
    except subprocess.CalledProcessError:
        mb = ""
    if mb and mb != head:
        return mb
    return "HEAD~1"


def changed_lines(base: str) -> dict[str, list[list[int]]]:
    diff = git("diff", "-U0", "--no-color", f"{base}..HEAD", "--", "src")
    out: dict[str, list[list[int]]] = {}
    cur: str | None = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            path = line[4:]
            cur = path[2:] if path.startswith("b/") else None
            if cur and (not cur.endswith(SRC_EXT + HDR_EXT) or cur.startswith(EXCLUDE)):
                cur = None
            continue
        m = HUNK_RE.match(line)
        if m and cur:
            start, count = int(m.group(1)), int(m.group(2) or "1")
            if count:
                out.setdefault(cur, []).append([start, start + count - 1])
    return out


def load_entries(db: Path) -> dict[str, dict]:
    entries: dict[str, dict] = {}
    raw = json.loads(db.read_text())
    # Prefer the main aura target: its modmap files exist after the BMI build.
    raw.sort(key=lambda e: 0 if "/aura.dir/" in (e.get("command") or "") else 1)
    for e in raw:
        try:
            rel = str(Path(e["file"]).resolve().relative_to(ROOT))
        except ValueError:
            continue
        if rel in entries or not rel.endswith(SRC_EXT):
            continue
        args = base_gate.clean_args(e.get("command", ""), e.get("directory", ""))
        if args:
            entries[rel] = {"directory": e["directory"], "file": str(ROOT / rel), "arguments": args}
    return entries


def includer_for(header: str, entries: dict[str, dict]) -> str | None:
    name = Path(header).name
    pat = re.compile(r'#\s*include\s*["<](?:[^">]*/)?' + re.escape(name) + r'[">]')
    for rel in sorted(entries):
        try:
            if pat.search((ROOT / rel).read_text(errors="ignore")):
                return rel
        except OSError:
            continue
    return None


def build_all_bmis(build: Path, jobs: int) -> None:
    """clang_module_targets.py covers src/core and src/compiler only; main.cpp and
    friends also import repl/parser/serve modules. Failures (e.g. -freflection
    units Clang cannot compile) only make the affected TUs unparseable."""
    proc = subprocess.run(["ninja", "-C", str(build), "-t", "targets", "all"], capture_output=True, text=True)
    pat = re.compile(r"^(CMakeFiles/aura\.dir/src/[^:]+\.(?:ixx|cppm)\.o):")
    targets = sorted({m.group(1) for line in proc.stdout.splitlines() if (m := pat.match(line))})
    if targets:
        subprocess.run(
            ["ninja", "-C", str(build), "-k", "0", f"-j{max(1, jobs)}", *targets],
            capture_output=True,
            text=True,
        )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="build_clang/compile_commands.json")
    ap.add_argument("--base")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument(
        "--build-bmis",
        action="store_true",
        help="best-effort build of every aura module BMI (repl/parser/serve too) so .cpp TUs parse",
    )
    args = ap.parse_args()

    base = pick_base(args.base)
    try:
        changes = changed_lines(base)
    except subprocess.CalledProcessError as exc:
        print(f"clang-tidy changed-lines: cannot diff against {base} ({exc.stderr.strip()}); skipped")
        return 0
    if not changes:
        print(f"clang-tidy changed-lines: no C++ changes under src/ since {base}")
        return 0
    db = Path(args.db)
    if not db.is_absolute():
        db = ROOT / db
    if not db.is_file():
        sys.exit(f"error: {db} not found (configure the Clang module build first)")
    if args.build_bmis:
        build_all_bmis(db.parent, args.jobs)
    entries = load_entries(db)

    tus: dict[str, None] = {}
    for path in changes:
        if path.endswith(SRC_EXT):
            if path in entries:
                tus[path] = None
        else:
            inc = includer_for(path, entries)
            if inc:
                tus[inc] = None
            else:
                print(f"clang-tidy changed-lines: no TU includes {path}; skipped")
    line_filter = json.dumps([{"name": str(ROOT / p), "lines": r} for p, r in changes.items()])
    checks = ",".join(["-*", *HARD, *SOFT])

    with tempfile.TemporaryDirectory(prefix="aura-tidy-diff-") as tmp:
        tmp_db = Path(tmp) / "compile_commands.json"
        tmp_db.write_text(json.dumps([entries[t] for t in tus]))

        def run(rel: str) -> tuple[str, str]:
            cmd = [
                "clang-tidy",
                "-p",
                tmp,
                "--quiet",
                f"--checks={checks}",
                f"--line-filter={line_filter}",
                "--header-filter=.*/src/.*",
                str(ROOT / rel),
            ]
            try:
                p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=args.timeout)
                return rel, p.stdout + p.stderr
            except subprocess.TimeoutExpired:
                return rel, "timed out"
            except FileNotFoundError:
                sys.exit("error: clang-tidy not found on PATH")

        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            outputs = list(pool.map(run, tus))

    seen: set[tuple[str, str, str, str]] = set()
    hard: list[str] = []
    soft: list[str] = []
    unparsed: list[str] = []
    for rel, out in outputs:
        # A TU Clang cannot parse (GCC-only feature, missing BMI) yields
        # unreliable findings: report them as notes and never block on them.
        broken = out == "timed out" or "[clang-diagnostic-error]" in out
        if broken:
            unparsed.append(rel)
        for line in out.splitlines():
            m = base_gate.DIAG_RE.match(line.strip())
            if not m:
                continue
            try:
                path = str(Path(m.group("path")).resolve().relative_to(ROOT))
            except ValueError:
                continue
            for check in m.group("check").split(","):
                key = (path, m.group("line"), m.group("col"), check)
                if key in seen or check.startswith("clang-diagnostic"):
                    continue
                seen.add(key)
                msg = f"{path}:{m.group('line')}:{m.group('col')}: {m.group('msg')} [{check}]"
                (hard if check in HARD and not broken else soft).append(msg)

    print(f"clang-tidy changed-lines: base {base}, {len(changes)} file(s), {len(tus)} TU(s)")
    for rel in unparsed:
        print(f"  note: {rel} did not parse (or timed out) under Clang; findings there are advisory")
    for msg in soft:
        print(f"  note: {msg}")
    if hard:
        print(f"clang-tidy changed-lines: FAIL - {len(hard)} finding(s) on changed lines (docs/cpp-style.md)")
        for msg in hard:
            print(f"  {msg}")
        return 1
    print(f"clang-tidy changed-lines: OK ({len(soft)} soft note(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
