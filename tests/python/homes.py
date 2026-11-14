#!/usr/bin/env python3
"""Query homes and coverage for Aura tests (#5225).

`home` answers from the table in this module (the same rows as tests/HOMES.md).
`cover` answers by parsing suite sources. Neither command builds or runs Aura.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

_SKIP_REL = {
    "tests/python/homes.py",
    "tests/python/test_run_home_cover.py",
}
_SOURCE_SUFFIXES = {".aura", ".py", ".cpp", ".h", ".hpp"}
_HEADER_LINES = 15
_BANNER_LINES = 25

CREATE_RULE = """\
when the surface has no home yet:
  Do not guess a file.
  Aura: create tests/suite/<module>_<feature>.aura
        (module + feature words, no issue number) and append:
        (require "std/test" all:)
        (test-file "<module>_<feature>.aura")
        (test-case "<feature>" (hash "tags" (list "<module>"))
          (check= expected actual))
        (test-finish)
        An unclosed program cannot sit inside test-case. Give it its own
        tests/suite/<module>_<feature>.aura with ;; expect-status: and
        ;; expect-err:.
  C++: create tests/<src-module>/test_<module>_<feature>.cpp
       (no issue digits) and register it once with aura_add_issue_test.
       Schema-only rows go in tests/compiler/obs_schema_cases.hpp.
  Python process checks stay on the CLI / help, diagnostic-location,
  cross-mode, and sandbox / Restricted homes. Behavior assertions do not
  go there.
  Do not create test_issue_N.py, *_N.aura, test_issue_N.cpp, or test_*_NNNN.cpp.
  lib/std/tests/ is not a home.
"""


@dataclass(frozen=True)
class Home:
    surface: str
    language: str
    path: str
    keywords: tuple[str, ...]
    tag: str
    requires: tuple[str, ...]
    body: str
    note: str


def _h(
    surface: str,
    language: str,
    path: str,
    keywords: tuple[str, ...],
    *,
    tag: str = "",
    requires: tuple[str, ...] = (),
    body: str = "(check= expected actual)",
    note: str,
) -> Home:
    return Home(surface, language, path, keywords, tag, requires, body, note)


# One file per surface. Keywords are exact queries or token phrases, not a
# fuzzy search: a miss stays a miss.
HOMES: tuple[Home, ...] = (
    _h(
        "std/string",
        "aura",
        "tests/suite/string_join.aura",
        ("string-join", "string join", "std/string", "string", "string-ref", "string-split"),
        tag="string",
        requires=("std/string",),
        body='(check= "a b" (string-join (list "a" "b") " "))',
        note="std/string behavior, including string-join.",
    ),
    _h(
        "std/list",
        "aura",
        "tests/suite/list_srfi_5133.aura",
        ("iota", "std/list", "std/iter", "list", "iter"),
        tag="list",
        requires=("std/list",),
        body="(check= (list 0 1 2) (iota 3))",
        note="std/list contract, including the std/iter re-export of the same names.",
    ),
    _h(
        "std/vector",
        "aura",
        "tests/suite/vector_srfi_5133.aura",
        ("std/vector", "vector-ref", "vector-map", "vector"),
        tag="vector",
        requires=("std/vector",),
        body="(check= 20 (vector-ref (vector 10 20 30) 1))",
        note="std/vector contract.",
    ),
    _h(
        "std/sort",
        "aura",
        "tests/suite/sort_srfi_5133.aura",
        ("std/sort", "std/algorithm", "sorted?", "list-sort", "sort", "algorithm"),
        tag="sort",
        requires=("std/sort",),
        body="(check= (list 1 2 3) (sort (list 3 1 2)))",
        note="std/sort contract, including the std/algorithm re-export of the same names.",
    ),
    _h(
        "std/json",
        "aura",
        "tests/suite/json_reg_split_5032.aura",
        ("std/json", "json-parse", "json-encode", "json"),
        tag="json",
        body='(check= #f (json-parse "false"))',
        note="std/json parse and encode. json-parse of the JSON false literal is #f.",
    ),
    _h(
        "std/extract",
        "aura",
        "tests/suite/export_contracts_5198.aura",
        ("extract-code", "llm:extract-code", "std/extract"),
        tag="extract",
        requires=("std/extract",),
        body='(check= "x" (extract-code "x"))',
        note=("Text extractor is extract-code. The agent extractor is llm:extract-code. Both belong in this home."),
    ),
    _h(
        "lang/module",
        "aura",
        "tests/suite/module.aura",
        ("lang/module", "module-get", "overwrite warning", "module"),
        tag="module",
        body='(check (module? (use "std/list")))',
        note="export, require, and the overwrite warning.",
    ),
    _h(
        "lang/parser",
        "aura",
        "tests/suite/paren_semi_comment_5199.aura",
        ("unclosed list", "unclosed-list", "(;", "lang/parser", "parser"),
        tag="parser",
        body="(check= expected actual)",
        note=(
            "Parser language face, including the unclosed-list diagnostic and "
            "the `(;` form. An unclosed program is its own file; it cannot sit "
            "inside test-case."
        ),
    ),
    _h(
        "runtime/fiber",
        "aura",
        "tests/suite/issue_5064_fiber_cancel_text.aura",
        ("fiber cancel", "fiber:cancel", "runtime/fiber"),
        tag="fiber",
        body=(
            '(define payload "fiber-cancelled: cancel requested via (fiber:cancel)")\n'
            "(define cf (fiber:spawn (lambda ()\n"
            "  (try (let loop ((i 0))\n"
            "         (fiber:yield)\n"
            "         (loop (+ i 1)))\n"
            '       (catch (e) (format "~a" e))))))\n'
            "(check= #t (fiber:cancel cf))\n"
            "(check= payload (fiber:join cf))"
        ),
        note="runtime/fiber cancel payload. Cancellation is not a type error.",
    ),
    _h(
        "runtime/orch",
        "aura",
        "tests/suite/orchestrator.aura",
        ("runtime/orch", "orchestrator", "orch"),
        tag="orch",
        requires=("std/orch",),
        note="runtime/orch behavior.",
    ),
    _h(
        "runtime/workspace",
        "aura",
        "tests/suite/workspace_source_ssot_2920.aura",
        ("runtime/workspace", "workspace", "current-source"),
        tag="workspace",
        note="runtime/workspace source and require behavior.",
    ),
    _h(
        "cli/help",
        "python",
        "tests/python/test_issue_4935_help.py",
        ("--help", "cli help", "help"),
        note="CLI / help process checks. Behavior assertions do not go in this file.",
    ),
    _h(
        "diagnostic-location",
        "python",
        "tests/python/test_issue_4864.py",
        ("--load", "oneshot", "file face", "diagnostic location"),
        note=("Process faces only: --load vs the file face vs oneshot. Behavior assertions do not go in this file."),
    ),
    _h(
        "cross-mode",
        "python",
        "tests/python/test_issue_4908_cross_mode.py",
        ("cross-mode", "cross mode"),
        note=("Cross-mode process gate (tests/cross_mode/). Behavior assertions do not go in this file."),
    ),
    _h(
        "sandbox",
        "python",
        "tests/python/test_issue_4870_production_sandbox.py",
        ("Restricted", "AURA_SANDBOX", "sandbox", "production sandbox"),
        note=("Production sandbox / Restricted process checks. Behavior assertions do not go in this file."),
    ),
    _h(
        "arena",
        "cpp",
        "tests/core/test_arena_batch.cpp",
        ("arena",),
        note="C++ arena theme batch. Extend this file.",
    ),
    _h(
        "FlatAST",
        "cpp",
        "tests/core/test_flatast_atomic_lock_batch.cpp",
        ("FlatAST",),
        note="C++ FlatAST lock / atomic batch. Extend this file.",
    ),
    _h(
        "mutation-boundary",
        "cpp",
        "tests/compiler/test_mutation_boundary_batch.cpp",
        ("mutation boundary", "mutation guard"),
        note="C++ mutation-boundary batch. Extend this file.",
    ),
    _h(
        "occurrence",
        "cpp",
        "tests/compiler/test_mutation_occurrence_dirty_batch.cpp",
        ("occurrence", "dirty-key"),
        note="C++ occurrence batch. Extend this file.",
    ),
    _h(
        "linear",
        "cpp",
        "tests/compiler/test_linear_cross_closure.cpp",
        ("linear ownership", "cross-closure", "linear"),
        note="C++ linear-ownership home. Extend this file.",
    ),
    _h(
        "dead-coercion",
        "cpp",
        "tests/compiler/test_dead_coercion_batch.cpp",
        ("dead coercion", "CastOp"),
        note="C++ dead-coercion batch. Extend this file.",
    ),
    _h(
        "aot-jit",
        "cpp",
        "tests/compiler/test_jit_aot_hot_update_unit_batch.cpp",
        ("SpecJIT", "relower", "aot"),
        note="C++ AOT / SpecJIT batch. Extend this file.",
    ),
    _h(
        "obs-schema",
        "cpp",
        "tests/compiler/obs_schema_cases.hpp",
        ("obs schema", "observability schema"),
        note="Schema-only rows go in this header. The matrix driver stays a separate TU.",
    ),
    _h(
        "capability",
        "cpp",
        "tests/core/test_capability_sandbox_batch.cpp",
        ("capability sandbox", "capability"),
        note="C++ capability batch. Process-level Restricted checks use the Python sandbox home.",
    ),
    _h(
        "hotpath",
        "cpp",
        "tests/core/test_hotpath_matrix_batch.cpp",
        ("hotpath", "hot-path"),
        note="C++ hot-path matrix. Extend this file.",
    ),
)


@dataclass(frozen=True)
class CaseRec:
    name: str
    kind: str
    hash_issues: tuple[int, ...]


@dataclass(frozen=True)
class FileRec:
    path: str
    filename_issues: tuple[int, ...]
    banner_issues: tuple[int, ...]
    text_issues: tuple[int, ...]
    cases: tuple[CaseRec, ...]
    header: str


def norm(text: str) -> str:
    folded = text.strip().casefold().replace("_", " ").replace(":", " ").replace("/", " ")
    folded = re.sub(r"(?<=[a-z0-9?])-(?=[a-z0-9])", " ", folded)
    folded = re.sub(r"[#]+", " ", folded)
    return re.sub(r"\s+", " ", folded).strip()


def tokens(text: str) -> tuple[str, ...]:
    folded = norm(text)
    if not folded:
        return ()
    if not re.search(r"[a-z0-9]", folded) or folded.startswith("--"):
        return (folded,)
    return tuple(folded.split())


def keyword_score(keyword: str, query: str) -> int:
    got = tokens(keyword)
    want = tokens(query)
    if not got or not want:
        return 0
    if got == want:
        return 1000 + len(got)
    if len(got) < len(want):
        span = len(got)
        for i in range(len(want) - span + 1):
            if want[i : i + span] == got:
                return 500 + span
    return 0


def resolve_homes(query: str) -> tuple[Home, ...]:
    best = 0
    winners: list[Home] = []
    for home in HOMES:
        score = 0
        for keyword in home.keywords:
            score = max(score, keyword_score(keyword, query))
        if score > best:
            best = score
            winners = [home]
        elif score == best and score > 0:
            winners.append(home)
    return tuple(winners)


def query_issue(query: str) -> int | None:
    match = re.search(r"(?:#|\bissue\s+)(\d{3,5})\b", query, re.I)
    if not match:
        return None
    return int(match.group(1))


def case_slug(query: str) -> str:
    folded = query.strip().casefold()
    if folded in {"(;", "( ;"}:
        return "paren-semi"
    if folded.startswith("--") or folded in {"help", "cli help"}:
        slug_src = "help" if "help" in folded else folded
    else:
        slug_src = folded
    slug = re.sub(r"[^a-z0-9]+", "-", slug_src).strip("-")
    slug = re.sub(r"(?:^|-)(\d{3,5})$", "", slug).strip("-")
    return slug or "case"


def _indent_body(body: str) -> str:
    lines = body.split("\n")
    indented = ["  " + line for line in lines]
    indented[-1] = indented[-1] + ")"
    return "\n".join(indented)


def aura_snippet(home: Home, query: str) -> str:
    issue = query_issue(query)
    if issue is None:
        meta = f'(hash "tags" (list "{home.tag}"))'
    else:
        meta = f'(hash "issue" {issue} "tags" (list "{home.tag}"))'
    lines = ['(require "std/test" all:)']
    for mod in home.requires:
        lines.append(f'(require "{mod}" all:)')
    lines.append(f'(test-file "{Path(home.path).name}")')
    lines.append(f'(test-case "{case_slug(query)}" {meta}')
    lines.append(_indent_body(home.body))
    lines.append("(test-finish)")
    return "\n".join(lines)


def harness_note(path: str) -> str:
    file = ROOT / path
    try:
        text = file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "Home file is not in the tree. Do not invent a different path."
    if "(test-case" in text:
        return (
            "This file already uses test-case. Append another case. "
            "Keep a single (test-file ...) and a single (test-finish)."
        )
    return (
        "This file still uses an older harness. Append the new case in the "
        "std/test shape above. Do not add another failure counter."
    )


def _python_snippet(home: Home, query: str) -> str:
    name = case_slug(query).replace("-", "_")
    return (
        f"# Append a process check to {home.path}.\n"
        "# Behavior assertions do not go in this file.\n"
        "class Case(unittest.TestCase):\n"
        f"    def test_{name}(self) -> None:\n"
        "        ...\n"
    )


def _cpp_snippet(home: Home) -> str:
    return (
        f"Append an AC function to {home.path} and call it from main / run_all.\n"
        "Put the issue number in a comment, not in the filename.\n"
        "Do not paste the Aura std/test form into this C++ file.\n"
    )


def _parser_diagnostic_snippet() -> str:
    return (
        "diagnostic file (the program itself is unclosed, so it is not inside test-case):\n"
        ";; expect-status: 1\n"
        ";; expect-err: unclosed list opened at line 1\n"
        "(;\n"
    )


def render_home(query: str) -> tuple[int, str]:
    query = query.strip()
    winners = resolve_homes(query)
    if not winners:
        text = f'not found: no home for "{query}".\nDo not guess a file.\n{CREATE_RULE}'
        return 1, text
    if len(winners) > 1:
        lines = [
            f'not found: "{query}" matches more than one home.',
            "Do not guess a file.",
        ]
        for home in winners:
            lines.append(f"- {home.surface} {home.path}")
        lines.append(CREATE_RULE.rstrip("\n"))
        return 1, "\n".join(lines) + "\n"
    home = winners[0]
    lines = [
        f"home: {home.path}",
        f"surface: {home.surface}",
        f"language: {home.language}",
        f"note: {home.note}",
        "",
        "append:",
    ]
    if home.language == "aura":
        lines.append(aura_snippet(home, query))
        lines.append(harness_note(home.path))
        if home.surface == "lang/parser":
            lines.append(_parser_diagnostic_snippet().rstrip("\n"))
    elif home.language == "python":
        lines.append(_python_snippet(home, query).rstrip("\n"))
    else:
        lines.append(_cpp_snippet(home).rstrip("\n"))
    lines.append(CREATE_RULE.rstrip("\n"))
    return 0, "\n".join(lines) + "\n"


_ISSUE_RE = re.compile(r"(?i)(?:issue\s*)?#(\d{3,5})\b")
_HASH_ISSUE_RE = re.compile(r'"issue"\s+(\d{3,5})\b')
_FILENAME_ISSUE_RE = re.compile(r"(?<![0-9])(\d{3,5})(?![0-9])")
_TEST_CASE_RE = re.compile(r'\(test-case\s+"([^"]+)"')
_TEST_SUITE_RE = re.compile(r'\(test-suite\s+"([^"]+)"')
_CHECK_RE = re.compile(r'\(check\s+"([^"]*)"')
_T_EQ_RE = re.compile(r'\(t=\s+"([^"]+)"')
_PY_TEST_RE = re.compile(r"^\s*def (test_[A-Za-z0-9_]+)\s*\(", re.M)


def _issues_in(text: str) -> tuple[int, ...]:
    return tuple(sorted({int(n) for n in _ISSUE_RE.findall(text)}))


def _hash_issues(text: str, open_at: int) -> tuple[int, ...]:
    depth = 0
    for i in range(open_at, len(text)):
        char = text[i]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                found = {int(n) for n in _HASH_ISSUE_RE.findall(text[open_at : i + 1])}
                return tuple(sorted(found))
    return ()


def _aura_cases(text: str) -> tuple[CaseRec, ...]:
    found: list[CaseRec] = []
    for match in _TEST_CASE_RE.finditer(text):
        cursor = match.end()
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        hashed: tuple[int, ...] = ()
        if text.startswith("(hash", cursor):
            hashed = _hash_issues(text, cursor)
        found.append(CaseRec(match.group(1), "test-case", hashed))
    for pattern, kind in ((_TEST_SUITE_RE, "test-suite"), (_CHECK_RE, "check"), (_T_EQ_RE, "t=")):
        for match in pattern.finditer(text):
            found.append(CaseRec(match.group(1), kind, ()))
    return tuple(found)


def _py_cases(text: str) -> tuple[CaseRec, ...]:
    return tuple(CaseRec(name, "py", ()) for name in _PY_TEST_RE.findall(text))


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def scan_tree(root: Path, *, rel_to: Path | None = None) -> tuple[FileRec, ...]:
    """Parse test sources under root. rel_to defaults to root."""
    root = root.resolve()
    base = (rel_to or root).resolve()
    found: list[FileRec] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name != "__pycache__" and not name.startswith(".")]
        for filename in filenames:
            path = Path(dirpath) / filename
            if path.suffix.lower() not in _SOURCE_SUFFIXES:
                continue
            rel = path.resolve().relative_to(base).as_posix()
            if rel in _SKIP_REL:
                continue
            text = _read(path)
            if not text:
                continue
            header_lines = text.splitlines()[:_HEADER_LINES]
            banner_lines = text.splitlines()[:_BANNER_LINES]
            suffix = path.suffix.lower()
            if suffix == ".aura":
                cases = _aura_cases(text)
            elif suffix == ".py":
                cases = _py_cases(text)
            else:
                cases = ()
            fname_issues = tuple(sorted({int(n) for n in _FILENAME_ISSUE_RE.findall(path.name)}))
            found.append(
                FileRec(
                    path=rel,
                    filename_issues=fname_issues,
                    banner_issues=_issues_in("\n".join(banner_lines)),
                    text_issues=_issues_in(text),
                    cases=cases,
                    header="\n".join(header_lines),
                )
            )
    found.sort(key=lambda rec: rec.path)
    return tuple(found)


_CACHE: tuple[FileRec, ...] | None = None


def default_files() -> tuple[FileRec, ...]:
    global _CACHE
    if _CACHE is None:
        _CACHE = scan_tree(ROOT / "tests", rel_to=ROOT)
    return _CACHE


def _fmt_issues(nums: set[int]) -> str:
    if not nums:
        return "-"
    return ",".join(f"#{n}" for n in sorted(nums))


def _strong_issues(rec: FileRec, case: CaseRec | None = None) -> set[int]:
    nums = set(rec.filename_issues) | set(rec.banner_issues)
    if case is not None:
        nums.update(case.hash_issues)
    return nums


def contains_symbol(text: str, query: str) -> bool:
    raw = query.strip()
    if not raw:
        return False
    if not re.search(r"[A-Za-z0-9]", raw) or raw.startswith("-"):
        return raw.casefold() in text.casefold()
    if re.search(r"[\s\-]", raw):
        parts = [part for part in re.split(r"[\s\-]+", raw) if part]
        pattern = r"[:\s\-]+".join(re.escape(part) for part in parts)
        return re.search(pattern, text, re.I) is not None
    ident = re.compile(
        r"(?<![A-Za-z0-9\-])" + re.escape(raw) + r"(?![A-Za-z0-9\-])",
        re.I,
    )
    for match in ident.finditer(text):
        before = text[max(0, match.start() - 5) : match.start()]
        if before.casefold() == "std::":
            continue
        return True
    return False


def _case_rows_for_issue(number: int, files: tuple[FileRec, ...]) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    for rec in files:
        strong = number in rec.filename_issues or number in rec.banner_issues
        hashed = [case for case in rec.cases if number in case.hash_issues]
        mentioned = number in rec.text_issues or number in rec.filename_issues
        if not mentioned and not hashed:
            continue
        if strong and rec.cases:
            for case in rec.cases:
                rows.append((rec.path, case.name, _fmt_issues(_strong_issues(rec, case))))
        elif hashed:
            for case in hashed:
                rows.append((rec.path, case.name, _fmt_issues(_strong_issues(rec, case))))
        else:
            rows.append((rec.path, "(file)", f"#{number}"))
    rows.sort(key=lambda row: (row[0], row[1]))
    return rows


def _case_rows_for_name(query: str, files: tuple[FileRec, ...]) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    seen: set[tuple[str, str]] = set()

    def add(path: str, case: str, issues: str) -> None:
        key = (path, case)
        if key in seen:
            return
        seen.add(key)
        rows.append((path, case, issues))

    by_path = {rec.path: rec for rec in files}
    for rec in files:
        for case in rec.cases:
            if contains_symbol(case.name, query):
                add(rec.path, case.name, _fmt_issues(_strong_issues(rec, case)))
    for rec in files:
        if any(path == rec.path for path, _, _ in rows):
            continue
        if contains_symbol(rec.header, query):
            add(rec.path, "(file)", _fmt_issues(_strong_issues(rec)))
    winners = resolve_homes(query)
    if len(winners) == 1:
        home = winners[0]
        if not any(path == home.path for path, _, _ in rows):
            rec = by_path.get(home.path)
            text = rec.header if rec is not None else ""
            if rec is None or not contains_symbol(text, query):
                text = _read(ROOT / home.path)
            if contains_symbol(text, query):
                issues = _fmt_issues(_strong_issues(rec)) if rec is not None else "-"
                add(home.path, f"({query})", issues)
    rows.sort(key=lambda row: (row[0], row[1]))
    return rows


def _format_rows(rows: list[tuple[str, str, str]], *, home_line: str = "") -> str:
    lines: list[str] = []
    if home_line:
        lines.append(home_line)
    for path, case, issues in rows:
        lines.append(f"{path}  {case}  {issues}")
    return "\n".join(lines) + ("\n" if lines else "")


def render_cover(
    *,
    issue: int | None = None,
    name: str | None = None,
    files: tuple[FileRec, ...] | None = None,
) -> tuple[int, str]:
    catalog = default_files() if files is None else files
    if issue is not None:
        rows = _case_rows_for_issue(issue, catalog)
        if not rows:
            return 1, f"no coverage for issue {issue}.\n"
        return 0, _format_rows(rows)
    query = (name or "").strip()
    if not query:
        return 1, 'no coverage for "".\n'
    winners = resolve_homes(query)
    home_line = ""
    if len(winners) == 1:
        home = winners[0]
        home_line = f"home  {home.surface}  {home.path}  {home.note}"
    rows = _case_rows_for_name(query, catalog)
    if not rows and not home_line:
        return 1, f'no coverage for "{query}".\n'
    return 0, _format_rows(rows, home_line=home_line)
