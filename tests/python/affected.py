#!/usr/bin/env python3
"""Fast loop: git diff or explicit paths → one L1 home, plus a small core (#5232).

The map is the genealogy in homes.py (the same rows as tests/HOMES.md).
This command does not compile the issue-farm and does not invent a second
suite list. An unmapped diff exits non-zero.
"""

from __future__ import annotations

import ast
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from _aura_harness import AURA_BIN, ROOT
from aura_file_runner import (
    AuraFileCase,
    file_process_directives,
    first_expect,
    format_failure_detail,
    invoke_aura_load,
    judge_aura_file,
)
from homes import HOMES, Home, match_homes
from smoke_cases import load_smoke_cases

# Editing the harness or the smoke fixture is itself a core run.
CORE_PATHS = (
    "lib/std/test.aura",
    "lib/std/edsl-test-harness.aura",
    "tests/fixtures/smoke.json",
    "tests/python/smoke_cases.py",
)

NEXT_HOME = 'python3 tests/run.py home "<query>"'
NEXT_PR = "./build.py test pr"

_CORE_SNIPPET = """\
(require "std/test" all:)
(test-file "affected-core.aura")
(test-case "std-test-loads" (hash "tags" (list "core"))
  (check #t))
(test-finish)
"""

_PROCESS_DIR = "tests/python/process"


@dataclass(frozen=True)
class CaseRow:
    file: str
    name: str
    issue: int | None
    ok: bool
    kind: str


@dataclass(frozen=True)
class Hit:
    surface: str
    path: str
    source: str


@dataclass(frozen=True)
class Plan:
    paths: tuple[str, ...]
    aura: tuple[str, ...]
    python: tuple[str, ...]
    cpp: tuple[Hit, ...]
    l2: tuple[Hit, ...]
    unmapped: tuple[str, ...]
    core: bool

    @property
    def runnable(self) -> bool:
        return self.core


def _norm(path: str) -> str:
    rel = path.replace("\\", "/").strip()
    if rel.startswith("./"):
        rel = rel[2:]
    return rel


def _assigned(path: Path, names: set[str]) -> dict[str, object]:
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found: dict[str, object] = {}
    for node in tree.body:
        targets: list[ast.expr] = []
        value: ast.expr | None = None
        if isinstance(node, ast.Assign):
            targets = list(node.targets)
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
            value = node.value
        if value is None:
            continue
        for target in targets:
            if isinstance(target, ast.Name) and target.id in names:
                found[target.id] = ast.literal_eval(value)
    return found


def _python_part_index() -> dict[str, Home]:
    """PARTS modules of genealogy Python homes. Read from the theme file."""
    index: dict[str, Home] = {}
    for home in HOMES:
        if home.language != "python":
            continue
        meta = _assigned(ROOT / home.path, {"PARTS"})
        parts = meta.get("PARTS", ())
        if not isinstance(parts, tuple):
            continue
        for part in parts:
            if isinstance(part, str):
                index[f"{_PROCESS_DIR}/{part}.py"] = home
    return index


def _is_suite_file(rel: str) -> bool:
    prefix = "tests/suite/"
    return rel.startswith(prefix) and rel.endswith(".aura") and "/" not in rel[len(prefix) :]


def workspace_paths(root: Path | None = None) -> list[str]:
    """Merge-base...HEAD, the index, unstaged edits, and untracked files."""
    root = root or ROOT
    files: set[str] = set()
    base = os.environ.get("AURA_GIT_BASE", "origin/main").strip() or "origin/main"
    try:
        mb = subprocess.run(
            ["git", "merge-base", base, "HEAD"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if mb.returncode == 0 and mb.stdout.strip():
            diff = subprocess.run(
                ["git", "diff", "--name-only", f"{mb.stdout.strip()}...HEAD"],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            if diff.returncode == 0:
                files.update(line.strip() for line in diff.stdout.splitlines() if line.strip())
        for args in (
            ["git", "diff", "--name-only", "--cached"],
            ["git", "diff", "--name-only"],
            ["git", "ls-files", "--others", "--exclude-standard"],
        ):
            listed = subprocess.run(args, cwd=root, capture_output=True, text=True, check=False)
            if listed.returncode == 0:
                files.update(line.strip() for line in listed.stdout.splitlines() if line.strip())
    except (OSError, subprocess.SubprocessError):
        return []
    return sorted(files)


def plan_paths(paths: list[str]) -> Plan:
    """Select L1 files and Python themes. Does not run anything."""
    part_index = _python_part_index()
    ordered: list[str] = []
    seen: set[str] = set()
    for raw in paths:
        rel = _norm(raw)
        if rel and rel not in seen:
            seen.add(rel)
            ordered.append(rel)

    aura: list[str] = []
    aura_seen: set[str] = set()
    python: list[str] = []
    python_seen: set[str] = set()
    cpp: list[Hit] = []
    cpp_seen: set[tuple[str, str]] = set()
    l2: list[Hit] = []
    l2_seen: set[tuple[str, str]] = set()
    unmapped: list[str] = []
    core_hit = False

    def add_aura(rel: str) -> None:
        if rel not in aura_seen:
            aura_seen.add(rel)
            aura.append(rel)

    def add_python(rel: str) -> None:
        if rel not in python_seen:
            python_seen.add(rel)
            python.append(rel)

    for rel in ordered:
        if rel in CORE_PATHS:
            core_hit = True
        homes = list(match_homes(rel))
        extra = part_index.get(rel)
        if extra is not None and all(home.path != extra.path for home in homes):
            homes.append(extra)
        if _is_suite_file(rel):
            add_aura(rel)
        if not homes and rel not in CORE_PATHS and not _is_suite_file(rel):
            unmapped.append(rel)
            continue
        for home in homes:
            if home.language == "aura":
                add_aura(home.path)
            elif home.language == "python":
                meta = _assigned(ROOT / home.path, {"PROCESS_TIER"})
                if meta.get("PROCESS_TIER") == "l2":
                    key = (home.surface, home.path)
                    if key not in l2_seen:
                        l2_seen.add(key)
                        l2.append(Hit(home.surface, home.path, rel))
                else:
                    add_python(home.path)
            else:
                key = (home.surface, home.path)
                if key not in cpp_seen:
                    cpp_seen.add(key)
                    cpp.append(Hit(home.surface, home.path, rel))

    return Plan(
        paths=tuple(ordered),
        aura=tuple(aura),
        python=tuple(python),
        cpp=tuple(cpp),
        l2=tuple(l2),
        unmapped=tuple(unmapped),
        core=bool(aura or python or core_hit),
    )


def _aura_env() -> dict[str, str]:
    env = os.environ.copy()
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
    if not str(env.get("AURA_PIPELINE_STRICT", "")).strip():
        env["AURA_PIPELINE_STRICT"] = "0"
    return env


def _emit(row: CaseRow) -> None:
    issue = "-" if row.issue is None else f"#{row.issue}"
    mark = "PASS" if row.ok else "FAIL"
    print(f"case {mark} {row.file} | {row.name} | {issue}", flush=True)


def _json_cases(text: str) -> list[tuple[str, int | None, bool]]:
    rows: list[tuple[str, int | None, bool]] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("{"):
            continue
        try:
            obj = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        cases = obj.get("cases") if isinstance(obj, dict) else None
        if not isinstance(cases, list):
            continue
        for case in cases:
            if not isinstance(case, dict):
                continue
            issue = case.get("issue")
            if not isinstance(issue, int):
                issue = None
            rows.append((str(case.get("name", "")), issue, bool(case.get("ok"))))
    return rows


def _file_case(rel: str) -> AuraFileCase:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    status, err, err_regex = file_process_directives(text)
    return AuraFileCase(
        path=path,
        expect=first_expect(text),
        rel=rel,
        expect_status=status,
        expect_err=err,
        err_regex=err_regex,
    )


def _run_aura_file(rel: str, *, env: dict[str, str], timeout_s: float, kind: str) -> list[CaseRow]:
    path = ROOT / rel
    print(f"run {rel}", flush=True)
    if not path.is_file():
        row = CaseRow(rel, "(file)", None, False, kind)
        _emit(row)
        print(f"affected: missing {rel}", flush=True)
        return [row]
    case = _file_case(rel)
    try:
        proc = invoke_aura_load(path, aura_bin=AURA_BIN, env=env, timeout_s=timeout_s, cwd=ROOT)
    except subprocess.TimeoutExpired:
        row = CaseRow(rel, "(file)", None, False, kind)
        _emit(row)
        print(f"affected: timeout {timeout_s:g}s {rel}", flush=True)
        return [row]
    stdout = proc.stdout or ""
    stderr = proc.stderr or ""
    ok_file, detail = judge_aura_file(proc.returncode, stdout, stderr, case)
    parsed = _json_cases(stdout)
    if not parsed:
        row = CaseRow(rel, "(file)", None, ok_file, kind)
        _emit(row)
        if not ok_file:
            print(format_failure_detail(detail, stdout=stdout, stderr=stderr), flush=True)
        return [row]
    rows = [CaseRow(rel, name, issue, ok, kind) for name, issue, ok in parsed]
    for row in rows:
        _emit(row)
    if not ok_file and all(row.ok for row in rows):
        extra = CaseRow(rel, "(file)", None, False, kind)
        _emit(extra)
        print(format_failure_detail(detail, stdout=stdout, stderr=stderr), flush=True)
        rows.append(extra)
    return rows


def _run_core_snippet(env: dict[str, str]) -> list[CaseRow]:
    print("run lib/std/test.aura", flush=True)
    path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(prefix="affected-core-", suffix=".aura", delete=False) as handle:
            path = Path(handle.name)
            handle.write(_CORE_SNIPPET.encode())
            handle.flush()
            try:
                proc = invoke_aura_load(path, aura_bin=AURA_BIN, env=env, timeout_s=30, cwd=ROOT)
            except subprocess.TimeoutExpired:
                row = CaseRow("lib/std/test.aura", "std-test-loads", None, False, "core")
                _emit(row)
                return [row]
    finally:
        if path is not None:
            path.unlink(missing_ok=True)
    stdout = proc.stdout or ""
    parsed = _json_cases(stdout)
    if not parsed:
        ok_file = proc.returncode == 0 and "std-test-loads" in stdout
        row = CaseRow("lib/std/test.aura", "std-test-loads", None, ok_file, "core")
        _emit(row)
        if not ok_file:
            tail = ((proc.stderr or "") + stdout).strip()
            print(tail[-400:], flush=True)
        return [row]
    rows = [CaseRow("lib/std/test.aura", name, issue, ok, "core") for name, issue, ok in parsed]
    for row in rows:
        _emit(row)
    return rows


def _run_smoke(env: dict[str, str]) -> list[CaseRow]:
    rows: list[CaseRow] = []
    for spec in load_smoke_cases():
        print(f"run tests/fixtures/smoke.json {spec.name}", flush=True)
        try:
            proc = subprocess.run(
                ["bash", "-c", spec.command],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
        except subprocess.TimeoutExpired:
            row = CaseRow("tests/fixtures/smoke.json", spec.name, None, False, "smoke")
            _emit(row)
            rows.append(row)
            continue
        combined = (proc.stdout or "") + (proc.stderr or "")
        ok = spec.expected in combined
        row = CaseRow("tests/fixtures/smoke.json", spec.name, None, ok, "smoke")
        _emit(row)
        if not ok:
            got = combined.strip().replace("\n", " ")[:120]
            print(f"affected: smoke {spec.name} expected {spec.expected!r}, got {got!r}", flush=True)
        rows.append(row)
    return rows


def _python_cases(theme: Path) -> list[tuple[int | None, str, str]]:
    meta = _assigned(theme, {"CASES"})
    rows = meta.get("CASES", ())
    found: list[tuple[int | None, str, str]] = []
    if not isinstance(rows, tuple):
        return found
    for row in rows:
        if isinstance(row, tuple) and len(row) == 3 and isinstance(row[1], str) and isinstance(row[2], str):
            issue = row[0] if isinstance(row[0], int) else None
            found.append((issue, row[1], row[2]))
    return found


def _run_python(rel: str) -> list[CaseRow]:
    path = ROOT / rel
    print(f"run {rel}", flush=True)
    meta = _assigned(path, {"PROCESS_TIMEOUT"})
    timeout = meta.get("PROCESS_TIMEOUT")
    timeout_s = float(timeout) if isinstance(timeout, int) and timeout > 0 else 180.0
    catalog = _python_cases(path)
    by_spec = {spec: (issue, name) for issue, name, spec in catalog}
    try:
        proc = subprocess.run(
            [sys.executable, str(path), "-v"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
        )
    except subprocess.TimeoutExpired:
        row = CaseRow(rel, "(theme)", None, False, "python")
        _emit(row)
        print(f"affected: timeout {timeout_s:g}s {rel}", flush=True)
        return [row]
    text = (proc.stdout or "") + "\n" + (proc.stderr or "")
    parsed: list[CaseRow] = []
    for line in text.splitlines():
        if " ... " not in line or not line.startswith("test_"):
            continue
        head, status = line.split(" ... ", 1)
        status = status.strip().split()[0] if status.strip() else ""
        if "(" not in head or not head.endswith(")"):
            continue
        method, _, spec = head.partition(" (")
        spec = spec[:-1]
        issue, name = by_spec.get(spec, (None, method))
        if status not in {"ok", "FAIL", "ERROR", "skipped"}:
            continue
        parsed.append(CaseRow(rel, name, issue, status == "ok", "python"))
    if not parsed and proc.returncode == 0 and catalog:
        parsed = [CaseRow(rel, name, issue, True, "python") for issue, name, _spec in catalog]
    if not parsed:
        parsed = [CaseRow(rel, "(theme)", None, proc.returncode == 0, "python")]
    if proc.returncode != 0 and all(row.ok for row in parsed):
        parsed.append(CaseRow(rel, "(theme)", None, False, "python"))
    for row in parsed:
        _emit(row)
    if proc.returncode != 0:
        tail = text.strip().replace("\n", " ")[-400:]
        print(tail, flush=True)
    return parsed


def _next_steps() -> None:
    print(f"Next: {NEXT_HOME}", flush=True)
    print(f"Next: {NEXT_PR}", flush=True)


def _header(plan: Plan) -> None:
    if not plan.paths:
        print("affected: no changed files.", flush=True)
        print("Do not guess a file.", flush=True)
        _next_steps()
    part_index = _python_part_index()
    for rel in plan.paths:
        homes = list(match_homes(rel))
        extra = part_index.get(rel)
        if extra is not None and all(home.path != extra.path for home in homes):
            homes.append(extra)
        for home in homes:
            print(f"affected: {rel} -> {home.surface} {home.path}", flush=True)
        if _is_suite_file(rel) and not homes:
            print(f"affected: {rel} -> suite {rel}", flush=True)
        if rel in CORE_PATHS and not homes and not _is_suite_file(rel):
            print(f"affected: {rel} -> core", flush=True)
    for rel in plan.aura:
        print(f"suite {rel}", flush=True)
    for rel in plan.python:
        print(f"python {rel}", flush=True)
    if plan.core:
        print("core: std/test, smoke", flush=True)
    for hit in plan.cpp:
        print(
            f"affected: {hit.source} matches the C++ home {hit.surface} ({hit.path}).",
            flush=True,
        )
        print("This loop does not compile the issue-farm.", flush=True)
        print(f"Next: {NEXT_PR}", flush=True)
    for hit in plan.l2:
        print(
            f"affected: {hit.source} matches the l2 theme {hit.surface} ({hit.path}).",
            flush=True,
        )
        print("Next: python3 tests/run.py process --tier l2", flush=True)
    for rel in plan.unmapped:
        print(f'affected: not found: no home for "{rel}".', flush=True)
        print("Do not guess a file.", flush=True)
        _next_steps()


def _payload(plan: Plan, rows: list[CaseRow], *, ran: bool, tests_ok: bool) -> dict[str, object]:
    passed = sum(1 for row in rows if row.ok)
    failed = len(rows) - passed
    ok = ran and tests_ok and not plan.unmapped and plan.runnable
    if not ran:
        ok = plan.runnable and not plan.unmapped
    return {
        "command": "affected",
        "ran": ran,
        "ok": ok,
        "passed": passed,
        "failed": failed,
        "aura": list(plan.aura),
        "python": list(plan.python),
        "cpp_skipped": [{"surface": hit.surface, "path": hit.path, "source": hit.source} for hit in plan.cpp],
        "l2_skipped": [{"surface": hit.surface, "path": hit.path, "source": hit.source} for hit in plan.l2],
        "unmapped": list(plan.unmapped),
        "cases": [
            {"file": row.file, "name": row.name, "issue": row.issue, "ok": row.ok, "kind": row.kind} for row in rows
        ],
    }


def command_exit(plan: Plan, *, ran: bool, tests_ok: bool) -> int:
    if not plan.runnable:
        return 1
    if plan.unmapped:
        return 1
    if not ran:
        return 0
    return 0 if tests_ok else 1


def execute(plan: Plan, *, plan_only: bool) -> int:
    """Print the selection and, unless plan_only, run it. Return the exit code."""
    _header(plan)
    if not plan.runnable:
        payload = _payload(plan, [], ran=False, tests_ok=False)
        print(json.dumps(payload, ensure_ascii=False), flush=True)
        return command_exit(plan, ran=False, tests_ok=False)
    if plan_only:
        payload = _payload(plan, [], ran=False, tests_ok=True)
        print(json.dumps(payload, ensure_ascii=False), flush=True)
        return command_exit(plan, ran=False, tests_ok=True)
    if not AURA_BIN.is_file():
        print(f"affected: {AURA_BIN} not found.", flush=True)
        print("Next: ./build.py build", flush=True)
        payload = _payload(plan, [], ran=False, tests_ok=False)
        payload["ok"] = False
        print(json.dumps(payload, ensure_ascii=False), flush=True)
        return 1
    env = _aura_env()
    rows: list[CaseRow] = []
    for rel in plan.aura:
        rows.extend(_run_aura_file(rel, env=env, timeout_s=180, kind="aura"))
    for rel in plan.python:
        rows.extend(_run_python(rel))
    rows.extend(_run_core_snippet(env))
    rows.extend(_run_smoke(env))
    tests_ok = all(row.ok for row in rows)
    code = command_exit(plan, ran=True, tests_ok=tests_ok)
    payload = _payload(plan, rows, ran=True, tests_ok=tests_ok)
    print(json.dumps(payload, ensure_ascii=False), flush=True)
    return code


def main_affected(paths: list[str] | None, *, plan_only: bool) -> int:
    selected = list(paths) if paths else workspace_paths()
    plan = plan_paths(selected)
    return execute(plan, plan_only=plan_only)
