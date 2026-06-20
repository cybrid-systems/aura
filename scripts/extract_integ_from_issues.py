#!/usr/bin/env python3
"""Extract candidate integ_tests.json entries from integration issue tests.

Scans tests/test_issue_*.cpp files tagged:
  // @category: integration

Finds eval strings passed to run_on(cs, ...), run_int(cs, ...), or
run_bool(cs, ...) and emits JSON candidates suitable for
tests/fixtures/integ_tests.json when the assertion is a simple
single-expression check.

Also reports integration issue files that look eval-only (no direct
C++ IR / snapshot / subprocess infrastructure) and therefore may be
safe to delete once their eval cases are covered in integ_tests.json.

Usage:
  python3 scripts/extract_integ_from_issues.py
  python3 scripts/extract_integ_from_issues.py --json
  python3 scripts/extract_integ_from_issues.py --safe-only
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = ROOT / "tests"

CATEGORY_RE = re.compile(r"//\s*@category:\s*integration\b")
ISSUE_FILE_RE = re.compile(r"^test_issue_\d")

# run_on/run_int/run_bool with string-literal second argument
EVAL_CALL_RE = re.compile(
    r"(?:run_on|run_int|run_bool)\(\s*cs\s*,\s*"
    r"(\"(?:\\.|[^\"\\])*\"|R\"\((?:.|\n)*?\)\")"
    r"\s*\)",
    re.MULTILINE,
)

# CHECK patterns that reveal an expected scalar
CHECK_INT_RE = re.compile(r"CHECK\(\s*(\w+)\s*==\s*(-?\d+)\s*,")
CHECK_BOOL_RE = re.compile(r"CHECK\(\s*!(\w+)\s*,|CHECK\(\s*(\w+)\s*,\s*\"[^\"]*\"\s*\)")
CHECK_GE_RE = re.compile(r"CHECK\(\s*(\w+)\s*>=\s*(-?\d+)\s*,")

# Signals that the file exercises C++ infrastructure beyond eval strings.
CPP_INFRA_PATTERNS = [
    re.compile(p)
    for p in (
        r"\b(snapshot|typed_mutate|LoweringState|FlatAST|IRFunction|IRModule)\b",
        r"\b(fork|execv|execve|posix_spawn|popen|std::process|subprocess)\b",
        r"\b(std::filesystem|filesystem::|emit-binary|ELF)\b",
        r"\bmutate:rebind\b.*\+\s*std::to_string",
        r"cs\.eval\(\s*std::string",
        r"for\s*\([^)]*\)\s*\{[^}]*cs\.eval",
        r"static\s+int\s+g_passed\s*=",  # custom counters, not harness
        r"#include\s+\"(?!test_harness\.hpp)",
        r"\bpair_car\b|\bpair_cdr\b",
        r"\bcompile_flat\b|\btype_check_flat\b|\binfer_flat\b",
        r"\bget_ir\b|\blower\b|\bIR::",
    )
]


@dataclass
class Candidate:
    source_file: str
    issue: str
    name: str
    code: str
    pipeline: str = "eval"
    expected: str | None = None
    notes: list[str] = field(default_factory=list)


def decode_cxx_string(literal: str) -> str:
    if literal.startswith('R"('):
        inner = literal[3:-2]
        return inner
    text = literal[1:-1]
    out: list[str] = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        i += 1
        if i >= len(text):
            break
        esc = text[i]
        mapping = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}
        if esc in mapping:
            out.append(mapping[esc])
        elif esc == "x" and i + 2 < len(text):
            out.append(chr(int(text[i + 1 : i + 3], 16)))
            i += 2
        else:
            out.append(esc)
        i += 1
    return "".join(out)


def issue_number(path: Path) -> str:
    m = re.search(r"test_issue_(\d+)", path.stem)
    return m.group(1) if m else path.stem


def infer_expected(run_var: str, context: str) -> str | None:
    for rx, fmt in (
        (CHECK_INT_RE, lambda m: m.group(2) if m.group(1) == run_var else None),
        (CHECK_GE_RE, lambda m: f">={m.group(2)}" if m.group(1) == run_var else None),
    ):
        m = rx.search(context)
        if m:
            val = fmt(m)
            if val:
                return val

    m = CHECK_BOOL_RE.search(context)
    if m:
        var = m.group(1) or m.group(2)
        if var == run_var:
            if m.group(1):
                return "#f"
            return "#t"
    return None


def has_cpp_infra(text: str) -> list[str]:
    hits: list[str] = []
    for rx in CPP_INFRA_PATTERNS:
        if rx.search(text):
            hits.append(rx.pattern)
    return hits


def extract_from_file(path: Path) -> tuple[list[Candidate], dict]:
    text = path.read_text(encoding="utf-8")
    if not CATEGORY_RE.search(text):
        return [], {"category": None}

    infra = has_cpp_infra(text)
    issue = issue_number(path)

    candidates: list[Candidate] = []
    seen_codes: set[str] = set()

    for m in EVAL_CALL_RE.finditer(text):
        literal = m.group(1)
        code = decode_cxx_string(literal)
        if not code.strip() or code in seen_codes:
            continue
        seen_codes.add(code)

        # Focus on simple single-expression / single-eval-string cases.
        if "\n" in code and not code.strip().startswith("("):
            continue

        start = m.start()
        end = min(len(text), m.end() + 400)
        context = text[start:end]

        fn = (
            "run_int"
            if "run_int" in text[m.start() - 20 : m.start() + 8]
            else ("run_bool" if "run_bool" in text[m.start() - 20 : m.start() + 8] else "run_on")
        )

        # Try to bind the run result variable name from the surrounding line.
        line_start = text.rfind("\n", 0, start) + 1
        line_end = text.find("\n", m.end())
        if line_end == -1:
            line_end = len(text)
        line = text[line_start:line_end]
        var_m = re.search(r"(?:auto\s+(\w+)|(?:int64_t|bool)\s+(\w+))\s*=\s*(?:run_)", line)
        run_var = (var_m.group(1) or var_m.group(2)) if var_m else ""

        expected = None
        notes: list[str] = []
        if fn == "run_int":
            if run_var:
                expected = infer_expected(run_var, context)
            if expected is None:
                notes.append("expected int not inferred from nearby CHECK")
        elif fn == "run_bool":
            if run_var:
                expected = infer_expected(run_var, context)
            if expected is None:
                notes.append("expected bool not inferred from nearby CHECK")
        else:
            notes.append("run_on result — verify expected manually")

        # Registration primitive checks use val==11 void sentinel; skip those.
        if "got void" in context and "val == 11" in context:
            notes.append("registration sentinel check — not a pure eval assertion")
            continue

        idx = len(candidates) + 1
        slug = re.sub(r"[^a-zA-Z0-9]+", "_", code[:40]).strip("_").lower()
        if not slug:
            slug = f"case{idx}"
        name = f"issue_{issue}_{slug[:48]}"

        candidates.append(
            Candidate(
                source_file=path.name,
                issue=issue,
                name=name,
                code=code,
                expected=expected,
                notes=notes,
            )
        )

    meta = {
        "category": "integration",
        "infra_hits": infra,
        "eval_only": len(infra) == 0,
        "candidate_count": len(candidates),
    }
    return candidates, meta


def collect() -> tuple[list[Candidate], dict[str, dict]]:
    all_candidates: list[Candidate] = []
    per_file: dict[str, dict] = {}

    for path in sorted(TESTS_DIR.glob("test_issue_*.cpp")):
        cands, meta = extract_from_file(path)
        per_file[path.name] = meta
        all_candidates.extend(cands)

    return all_candidates, per_file


def file_blocks_integ_migration(text: str) -> list[str]:
    """Reasons a file still needs its C++ issue test harness."""
    reasons: list[str] = []
    if re.search(r"\bv\.val\s*==\s*11\b", text):
        reasons.append("registration sentinel checks (void val=11)")
    if re.search(r"\brun_on\s*\(", text):
        reasons.append("uses run_on() / non-scalar EvalValue assertions")
    if re.search(r"\bcs\.eval\(\s*std::string", text):
        reasons.append("builds eval source dynamically")
    if re.search(r"\b(snapshot|typed_mutate|LoweringState|FlatAST)\b", text):
        reasons.append("uses direct compiler C++ APIs")
    if re.search(r"\bstring_value\s*\(|\brun_ok\s*\(", text):
        reasons.append("inspects EvalValue / string heap in C++")
    direct_evals = len(re.findall(r"\bcs\.eval\s*\(", text))
    helper_evals = len(EVAL_CALL_RE.findall(text))
    if direct_evals > helper_evals:
        reasons.append("has cs.eval() calls outside run_int/run_bool/run_on helpers")
    return reasons


def safe_to_remove(per_file: dict[str, dict], candidates: list[Candidate], tests_dir: Path) -> list[str]:
    safe: list[str] = []
    for fname, meta in sorted(per_file.items()):
        if meta.get("category") != "integration":
            continue
        if not meta.get("eval_only"):
            continue

        path = tests_dir / fname
        text = path.read_text(encoding="utf-8")
        if file_blocks_integ_migration(text):
            continue

        file_cands = [c for c in candidates if c.source_file == fname]
        if not file_cands:
            continue

        # Every extracted eval must have an inferred expected value.
        if any(c.expected is None for c in file_cands):
            continue

        # All literal eval calls in the file should be accounted for.
        literal_evals = EVAL_CALL_RE.findall(text)
        if len(literal_evals) != len(file_cands):
            continue

        safe.append(fname)
    return safe


def to_integ_json(candidates: list[Candidate]) -> list[dict]:
    out: list[dict] = []
    for c in candidates:
        if c.expected is None:
            continue
        entry = {
            "name": c.name,
            "code": c.code,
            "pipeline": c.pipeline,
            "expected": c.expected,
        }
        out.append(entry)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--json",
        action="store_true",
        help="print candidate integ_tests.json entries (expected inferred only)",
    )
    parser.add_argument(
        "--safe-only",
        action="store_true",
        help="print only integration files safe to fully remove",
    )
    args = parser.parse_args()

    candidates, per_file = collect()
    removable = safe_to_remove(per_file, candidates, TESTS_DIR)

    if args.json:
        json.dump(to_integ_json(candidates), sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    if args.safe_only:
        for fname in removable:
            print(fname)
        return 0

    print("Function naming convention (bundle members):")
    print("  test_issue_<N>[_suffix] -> aura_issue_<N>[_suffix]_run")
    print("  test_<name>             -> aura_issue_<name>_run")
    print()

    print(f"Scanned {len(per_file)} issue test files; found {len(candidates)} eval candidates.")
    print()

    print("Candidate integ_tests.json entries (expected inferred):")
    for c in candidates:
        if c.expected is None:
            continue
        print(f"  - {c.name}: expected={c.expected!r}  ({c.source_file})")
    print()

    print("Integration files likely safe to fully remove (eval-only, no C++ IR tests):")
    if removable:
        for fname in removable:
            n = per_file[fname]["candidate_count"]
            print(f"  - {fname}  ({n} eval candidate(s))")
    else:
        print("  (none)")
    print()

    skipped = [c for c in candidates if c.expected is None and "registration sentinel" not in " ".join(c.notes)]
    if skipped:
        print("Manual review suggested (eval found, expected not inferred):")
        for c in skipped[:20]:
            print(f"  - {c.source_file}: {c.code[:72]!r}")
        if len(skipped) > 20:
            print(f"  ... and {len(skipped) - 20} more")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
