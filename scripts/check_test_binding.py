#!/usr/bin/env python3
"""Issue #1453: hard test-binding gate for primitive / surface changes.

When production primitive registration or evaluator core changes, require a
paired change under tests/ (or allowlisted paths). Optional stricter mode
requires that new public ``add("…")`` names appear in a test file.

Usage:
  python3 scripts/check_test_binding.py
  python3 scripts/check_test_binding.py --base origin/main
  python3 scripts/check_test_binding.py --files a.cpp b.cpp
  python3 scripts/check_test_binding.py --require-name-mention   # stricter

Exit 0 = OK, 1 = binding violation.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST_PATH = ROOT / "tests" / "test-binding-allowlist.txt"

# Production sources that force a test pairing.
PROD_GLOBS = (
    re.compile(r"^src/compiler/evaluator_primitives.*\.cpp$"),
    re.compile(r"^src/compiler/evaluator\.ixx$"),
    re.compile(r"^src/compiler/evaluator_ctor\.cpp$"),
    re.compile(r"^src/compiler/evaluator_eval_flat\.cpp$"),
    re.compile(r"^src/compiler/observability_prims_decl\.inc$"),
    re.compile(r"^src/compiler/primitives_detail\.h$"),
    re.compile(r"^src/compiler/primitives_meta\.h$"),
    re.compile(r"^src/compiler/security_capabilities\.h$"),
    re.compile(r"^src/compiler/service\.ixx$"),
    re.compile(r"^lib/std/edsl-test-harness\.aura$"),
    re.compile(r"^lib/std/stats\.aura$"),
    re.compile(r"^lib/std/engine-metrics\.aura$"),
    # Issue #1454: TUI protected surface (aura-pets)
    re.compile(r"^src/compiler/evaluator_primitives_tui\.cpp$"),
    re.compile(r"^src/tui/.*\.(hh|h|cpp|ixx)$"),
    re.compile(r"^lib/std/tui/.*\.aura$"),
    re.compile(r"^examples/(cyber_cat|snake|tetris)\.aura$"),
)

TEST_PREFIXES = (
    "tests/",
    "lib/std/edsl-test-harness.aura",
    "lib/std/test.aura",
    "lib/std/tests/",
    "scripts/run_pets_regression.py REMOVED per Anqi 2026-07-19 directive wave 11 (broken — referenced deleted demos).",
)

# New public registration patterns (for --require-name-mention).
ADD_RE = re.compile(r'\.add\(\s*"([^"]+)"')
REGISTER_ADD_RE = re.compile(r'(?:^|[^\w])add\(\s*"([^"]+)"')
STATS_IMPL_RE = re.compile(r'register_stats_impl\(\s*"([^"]+)"')


def is_prod(path: str) -> bool:
    path = path.replace("\\", "/")
    return any(g.match(path) for g in PROD_GLOBS)


def is_test(path: str) -> bool:
    path = path.replace("\\", "/")
    return any(path.startswith(p) or path == p for p in TEST_PREFIXES)


def load_allowlist() -> set[str]:
    if not ALLOWLIST_PATH.exists():
        return set()
    out: set[str] = set()
    for line in ALLOWLIST_PATH.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        out.add(line.replace("\\", "/"))
    return out


def git_diff_names(base: str) -> list[str]:
    cmds = [
        ["git", "diff", "--name-only", f"{base}...HEAD"],
        ["git", "diff", "--name-only", base],
        ["git", "diff", "--name-only", "HEAD"],
        ["git", "diff", "--name-only", "--cached"],
    ]
    names: set[str] = set()
    for cmd in cmds:
        try:
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
        except FileNotFoundError:
            return []
        if r.returncode != 0:
            continue
        for line in r.stdout.splitlines():
            line = line.strip()
            if line:
                names.add(line)
        if names:
            break
    return sorted(names)


def git_diff_text(base: str, path: str) -> str:
    """Unified diff for one path (working tree + staged vs base when possible)."""
    for args in (
        ["git", "diff", f"{base}...HEAD", "--", path],
        ["git", "diff", base, "--", path],
        ["git", "diff", "HEAD", "--", path],
        ["git", "diff", "--cached", "--", path],
    ):
        try:
            r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
        except FileNotFoundError:
            return ""
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout
    return ""


def added_public_names(diff_text: str) -> list[str]:
    """Extract newly added public prim names from a unified diff (+ lines only)."""
    names: set[str] = set()
    for line in diff_text.splitlines():
        if not line.startswith("+") or line.startswith("+++"):
            continue
        body = line[1:]
        for rx in (ADD_RE, REGISTER_ADD_RE):
            for m in rx.finditer(body):
                names.add(m.group(1))
        # register_stats_impl is internal-only — do not require name mention
    return sorted(names)


def test_corpus_mentions(name: str, test_files: list[str]) -> bool:
    """True if any changed test file mentions the primitive name."""
    needle = name
    for rel in test_files:
        path = ROOT / rel
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if needle in text:
            return True
    # Also search full tests/ tree for a dedicated issue test (relaxed:
    # name present anywhere under tests/ for this commit is enough when
    # a tests/ path is already in the change set — checked by caller).
    return False


def evaluate_binding(
    files: list[str],
    *,
    require_name_mention: bool = False,
    base: str = "origin/main",
) -> tuple[int, str]:
    """Return (exit_code, message). Pure-ish core for unit tests."""
    allow = load_allowlist()
    files_n = [f.replace("\\", "/") for f in files]
    prod = [f for f in files_n if is_prod(f) and f not in allow]
    tests = [f for f in files_n if is_test(f)]

    if not prod:
        return 0, f"OK: check_test_binding ({len(files_n)} files, no production prim sources)"

    if not tests:
        msg = (
            "FAIL: production primitive sources changed without tests/ updates "
            "(Issue #1453 test binding).\n"
            "Production files:\n"
            + "".join(f"  + {f}\n" for f in prod)
            + "Add or update tests under tests/ (C++ issue test or EDSL suite).\n"
            "Exceptional allowlist: tests/test-binding-allowlist.txt\n"
            "Policy: docs/design/testing-framework-v1.md"
        )
        return 1, msg

    lines = [f"OK: check_test_binding — {len(prod)} production file(s) paired with {len(tests)} test file(s)"]
    for f in prod[:12]:
        lines.append(f"  prod: {f}")
    for f in tests[:12]:
        lines.append(f"  test: {f}")

    if require_name_mention:
        missing: list[str] = []
        for pf in prod:
            if not pf.endswith(".cpp") and not pf.endswith(".ixx"):
                continue
            diff = git_diff_text(base, pf)
            for name in added_public_names(diff):
                # Skip very short / operator names that appear everywhere
                if len(name) < 2:
                    continue
                if not test_corpus_mentions(name, tests):
                    # Fall back: any tests/ file on disk (not just changed) —
                    # only for names longer than 4 to reduce noise
                    if len(name) >= 4 and _tree_mentions(name):
                        continue
                    missing.append(f'{pf}: add("{name}")')
        if missing:
            msg = (
                "FAIL: new public primitive name(s) not mentioned in changed tests "
                "(--require-name-mention):\n"
                + "".join(f"  + {m}\n" for m in missing[:20])
                + "Reference the name in a test_issue_*.cpp or edsl_self_test.aura.\n"
            )
            return 1, msg
        lines.append("OK: --require-name-mention (new add() names referenced in tests)")

    return 0, "\n".join(lines)


def _tree_mentions(name: str) -> bool:
    tests_root = ROOT / "tests"
    if not tests_root.is_dir():
        return False
    # Cheap scan of issue tests + edsl self-test only
    candidates = list(tests_root.glob("test_issue_*.cpp"))
    candidates += list(tests_root.glob("test_*.cpp"))
    candidates.append(tests_root / "edsl_self_test.aura")
    for path in candidates[:400]:
        try:
            if name in path.read_text(encoding="utf-8", errors="replace"):
                return True
        except OSError:
            continue
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default="origin/main", help="git base ref (default: origin/main)")
    ap.add_argument("--files", nargs="*", help="explicit file list (skips git)")
    ap.add_argument("--soft", action="store_true", help="warn but exit 0 on violation")
    ap.add_argument(
        "--require-name-mention",
        action="store_true",
        help='also require new add("name") strings appear in a test file',
    )
    args = ap.parse_args()

    files = list(args.files) if args.files else git_diff_names(args.base)
    if not files:
        print("OK: check_test_binding (no changed files detected)")
        return 0

    code, msg = evaluate_binding(
        files,
        require_name_mention=args.require_name_mention,
        base=args.base,
    )
    if code == 0:
        print(msg)
        return 0
    print(msg, file=sys.stderr)
    return 0 if args.soft else 1


if __name__ == "__main__":
    sys.exit(main())
