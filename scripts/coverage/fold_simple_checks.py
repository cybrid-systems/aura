#!/usr/bin/env python3
"""Fold substring-only coverage check_*.py files into JSON manifests.

A script folds only when every assertion is one of:

- needle in a whole file (`contains` / `contains_any`)
- needle absent from a whole file (`absent`)
- a path that must not exist (`forbid`)
- a glob that must match nothing (`forbid_glob`)

Order checks, fixed character windows, regex, and subprocess stays in
checks/. A folded row never copies a `[start:start+N]` window.

Usage:
  python3 scripts/coverage/fold_simple_checks.py --dry-run
  python3 scripts/coverage/fold_simple_checks.py --apply
"""

from __future__ import annotations

import argparse
import ast
import importlib.util
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKS = ROOT / "scripts" / "coverage" / "checks"
MANIFESTS = ROOT / "scripts" / "coverage" / "manifests"
GRANDFATHER = ROOT / "scripts" / "coverage" / "simple_check_grandfather.txt"
QUERY_PATH = "src/compiler/evaluator_primitives_query.cpp"

_ISSUE_IN_NAME = re.compile(r"_(\d{3,5})\.py$")
_ISSUE_IN_DOC = re.compile(r"Issue\s+#(\d{3,5})")
_SKIP_FUNCS = {"_read", "read", "must", "_must", "must_not", "_must_not", "must_key"}
_MUST_NAMES = {"must", "_must", "must_key"}
_MUST_NOT_NAMES = {"must_not", "_must_not"}

Row = tuple[str, str, str | list[str]]


def _issue_id(path: Path, text: str) -> int | None:
    match = _ISSUE_IN_NAME.search(path.name)
    if match:
        return int(match.group(1))
    match = _ISSUE_IN_DOC.search(text)
    return int(match.group(1)) if match else None


def classify(path: Path, text: str) -> str:
    if (
        "runner.py" in text
        and "--issue" in text
        and text.count("def ") <= 3
        and "must(" not in text.replace("_must(", "_X(")
    ):
        return "wrapper"
    if any(m in text for m in ("re.compile", "re.search", "re.findall", "re.match", "ast.parse")):
        return "custom"
    if "subprocess." in text:
        return "custom"
    # Windowed source-cite (find + slice) is custom logic, not a manifest row.
    if ".find(" in text and re.search(r"\[[^\]]+:[^\]]+\]", text):
        return "custom"
    if "_source_scan" in text or "source_block_after(" in text or "lambda_body_after(" in text:
        return "custom"
    if text.count("must(") >= 2:
        return "simple"
    return "other"


def _const_str(node: ast.AST) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _name(node: ast.AST) -> str | None:
    return node.id if isinstance(node, ast.Name) else None


class _Env:
    def __init__(self) -> None:
        self.strs: dict[str, str] = {}
        self.reads: dict[str, str] = {}
        self.paths: dict[str, str] = {}
        self.lists: dict[str, tuple[str, ...]] = {}


def _segment(node: ast.AST, env: _Env) -> str | None:
    literal = _const_str(node)
    if literal is not None:
        return literal
    if isinstance(node, ast.Name) and node.id in env.strs:
        return env.strs[node.id]
    return None


def _rel_path(node: ast.AST, env: _Env) -> str | None:
    if isinstance(node, ast.Name) and node.id == "ROOT":
        return ""
    if isinstance(node, ast.Name) and node.id in env.paths:
        return env.paths[node.id]
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        left = _rel_path(node.left, env)
        right = _segment(node.right, env)
        if left is None or right is None:
            return None
        return f"{left}/{right}" if left else right
    return None


def _read_paths(value: ast.AST, env: _Env) -> tuple[str, ...] | None:
    """Files a read expression concatenates, in order. None if it is not a read."""
    if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Add):
        left = _read_paths(value.left, env)
        right = _read_paths(value.right, env)
        if left is None or right is None:
            return None
        return left + right
    if isinstance(value, ast.Name) and value.id in env.reads:
        return tuple(part for part in env.reads[value.id].split("\n") if part)
    if isinstance(value, ast.Call):
        fn = _name(value.func)
        if fn in {"_read", "read"} and value.args:
            rel = _segment(value.args[0], env)
            return (rel,) if rel else None
        if fn == "read_query_prims":
            return (QUERY_PATH,)
    return None


def _note_assign(tgt: str, value: ast.AST, env: _Env) -> None:
    env.strs.pop(tgt, None)
    env.reads.pop(tgt, None)
    env.paths.pop(tgt, None)
    env.lists.pop(tgt, None)
    if isinstance(value, (ast.List, ast.Tuple)) and all(_const_str(elt) is not None for elt in value.elts):
        env.lists[tgt] = tuple(_const_str(elt) or "" for elt in value.elts)
        return
    literal = _const_str(value)
    if literal is not None:
        env.strs[tgt] = literal
        return
    paths = _read_paths(value, env)
    if paths:
        env.reads[tgt] = "\n".join(paths)
        return
    rel = _rel_path(value, env)
    if rel is not None:
        env.paths[tgt] = rel


def _needle(node: ast.AST, env: _Env) -> str | None:
    literal = _const_str(node)
    if literal is not None:
        return literal
    if isinstance(node, ast.Name) and node.id in env.strs:
        return env.strs[node.id]
    if isinstance(node, ast.JoinedStr):
        parts: list[str] = []
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                parts.append(value.value)
            elif (
                isinstance(value, ast.FormattedValue)
                and isinstance(value.value, ast.Name)
                and value.value.id in env.strs
                and value.format_spec is None
                and value.conversion == -1
            ):
                parts.append(env.strs[value.value.id])
            else:
                return None
        return "".join(parts)
    return None


def _hay_path(node: ast.AST, env: _Env) -> str | None:
    ident = _name(node)
    if ident and ident in env.reads:
        return env.reads[ident]
    return None


def _one(path: str, kind: str, payload: str | list[str]) -> list[Row]:
    return [(path, kind, payload)]


def _success_cond(cond: ast.AST, env: _Env) -> list[Row] | None:
    """Rows that hold when `cond` is true. None if the shape is not a substring."""
    if isinstance(cond, ast.Compare) and len(cond.ops) == 1:
        op = cond.ops[0]
        needle = _needle(cond.left, env)
        hay = _hay_path(cond.comparators[0], env) if cond.comparators else None
        if needle is None or hay is None:
            return None
        if isinstance(op, ast.In):
            return _one(hay, "contains", needle)
        if isinstance(op, ast.NotIn):
            return _one(hay, "absent", needle)
        return None
    if isinstance(cond, ast.UnaryOp) and isinstance(cond.op, ast.Not):
        inner = _success_cond(cond.operand, env)
        return _invert(inner) if inner is not None else None
    if isinstance(cond, ast.BoolOp) and isinstance(cond.op, ast.And):
        out: list[Row] = []
        for value in cond.values:
            rows = _success_cond(value, env)
            if rows is None:
                return None
            out.extend(rows)
        return out
    if isinstance(cond, ast.BoolOp) and isinstance(cond.op, ast.Or):
        parts: list[Row] = []
        for value in cond.values:
            rows = _success_cond(value, env)
            if not rows or len(rows) != 1 or rows[0][1] != "contains" or not isinstance(rows[0][2], str):
                return None
            parts.append(rows[0])
        paths = {row[0] for row in parts}
        needles = [row[2] for row in parts]
        assert all(isinstance(needle, str) for needle in needles)
        if len(paths) == 1:
            return _one(parts[0][0], "contains_any", [str(needle) for needle in needles])
        # Same needle in any of several files is the concatenation.
        # Different needles across files would match the wrong file.
        if len(set(needles)) == 1:
            joined = "\n".join(row[0] for row in parts)
            return _one(joined, "contains", str(needles[0]))
        return None
    return None


def _invert(rows: list[Row]) -> list[Row] | None:
    out: list[Row] = []
    for path, kind, payload in rows:
        if kind == "contains" and isinstance(payload, str):
            out.append((path, "absent", payload))
        elif kind == "absent" and isinstance(payload, str):
            out.append((path, "contains", payload))
        else:
            return None
    return out


def _parse_must(node: ast.Call, env: _Env) -> list[Row] | None:
    fn = _name(node.func)
    if fn is None or node.keywords:
        return None
    args = node.args
    absent = fn in _MUST_NOT_NAMES
    if fn not in _MUST_NAMES and not absent:
        return None
    if len(args) >= 3 and _needle(args[0], env) is not None and _name(args[2]) == "fails":
        return _success_cond(args[0], env)
    if len(args) >= 3 and _needle(args[0], env) is not None:
        hay = _hay_path(args[2], env)
        needle = _needle(args[0], env)
        if hay is None or needle is None:
            return None
        return _one(hay, "absent" if absent else "contains", needle)
    if len(args) >= 1 and not absent:
        return _success_cond(args[0], env)
    if len(args) >= 1 and absent:
        rows = _success_cond(args[0], env)
        return _invert(rows) if rows is not None else None
    return None


def _is_fails_report(stmt: ast.stmt) -> bool:
    if isinstance(stmt, ast.If) and _name(stmt.test) == "fails":
        return True
    return bool(isinstance(stmt, ast.For) and _name(stmt.iter) == "fails")


def _call_name(node: ast.AST) -> str | None:
    if not isinstance(node, ast.Call):
        return None
    if isinstance(node.func, ast.Name):
        return node.func.id
    if isinstance(node.func, ast.Attribute):
        return node.func.attr
    return None


def _append_is_fails(stmt: ast.stmt) -> bool:
    if not isinstance(stmt, ast.Expr) or not isinstance(stmt.value, ast.Call):
        return False
    call = stmt.value
    return isinstance(call.func, ast.Attribute) and call.func.attr == "append" and _name(call.func.value) == "fails"


def _forbid_isfile(test: ast.AST, env: _Env) -> str | None:
    """`(ROOT / "a").is_file()` → relative path. None if it is a different test."""
    if not isinstance(test, ast.Call) or not isinstance(test.func, ast.Attribute):
        return None
    if test.func.attr != "is_file":
        return None
    return _rel_path(test.func.value, env)


def _glob_call(node: ast.AST, env: _Env) -> str | None:
    if isinstance(node, ast.Call) and _call_name(node) == "sorted" and node.args:
        return _glob_call(node.args[0], env)
    if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
        return None
    if node.func.attr != "glob" or not node.args:
        return None
    base = _rel_path(node.func.value, env)
    pat = _const_str(node.args[0])
    if base is None or pat is None:
        return None
    return f"{base}/{pat}" if base else pat


def _walk(stmts: list[ast.stmt], env: _Env, rows: list[Row]) -> str | None:
    """Return a reject reason, or None when every statement was kept."""
    for stmt in stmts:
        reason = _stmt(stmt, env, rows)
        if reason:
            return reason
    return None


def _stmt(stmt: ast.stmt, env: _Env, rows: list[Row]) -> str | None:
    if isinstance(stmt, (ast.Import, ast.ImportFrom, ast.Pass)):
        return None
    if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant):
        return None
    if isinstance(stmt, ast.FunctionDef):
        if stmt.name in _SKIP_FUNCS:
            return None
        return _walk(stmt.body, env, rows)
    if isinstance(stmt, ast.Assign) and len(stmt.targets) == 1 and isinstance(stmt.targets[0], ast.Name):
        _note_assign(stmt.targets[0].id, stmt.value, env)
        return None
    if isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name) and stmt.value is not None:
        _note_assign(stmt.target.id, stmt.value, env)
        return None
    # `if __name__ == "__main__":` is not a contract.
    if (
        isinstance(stmt, ast.If)
        and isinstance(stmt.test, ast.Compare)
        and isinstance(stmt.test.left, ast.Name)
        and stmt.test.left.id == "__name__"
    ):
        return None
    if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
        call = stmt.value
        if _call_name(call) in {"insert", "print", "_read", "read", "read_query_prims", "add_argument"}:
            return None
        if _name(call.func) in _MUST_NAMES | _MUST_NOT_NAMES:
            parsed = _parse_must(call, env)
            if parsed is None:
                return "must-shape"
            rows.extend(parsed)
            return None
        return "expr-call"
    if isinstance(stmt, ast.Return):
        return None
    if _is_fails_report(stmt):
        return None
    if isinstance(stmt, ast.If):
        return _walk_if(stmt, env, rows)
    if isinstance(stmt, ast.For):
        return _walk_for(stmt, env, rows)
    return f"stmt-{type(stmt).__name__}"


def _walk_if(stmt: ast.If, env: _Env, rows: list[Row]) -> str | None:
    if stmt.orelse:
        return "if-else"
    forbid = _forbid_isfile(stmt.test, env)
    if forbid is not None and all(_append_is_fails(s) or _is_fails_report(s) for s in stmt.body):
        rows.append(("", "forbid", forbid))
        return None
    if (
        isinstance(stmt.test, ast.Call)
        and isinstance(stmt.test.func, ast.Attribute)
        and stmt.test.func.attr == "is_dir"
    ):
        return _walk(stmt.body, env, rows)
    # `if cond: fails.append` — cond true is a failure.
    if stmt.body and all(_append_is_fails(s) for s in stmt.body):
        held = _success_cond(stmt.test, env)
        if held is None:
            return "if-cond"
        flipped = _invert(held)
        if flipped is None:
            return "if-cond"
        rows.extend(flipped)
        return None
    return "if-body"


def _const_iter(node: ast.AST, env: _Env) -> list[str] | None:
    if isinstance(node, ast.Name) and node.id in env.lists:
        return list(env.lists[node.id])
    if isinstance(node, ast.Call) and _call_name(node) == "sorted" and node.args:
        return _const_iter(node.args[0], env)
    if isinstance(node, (ast.Tuple, ast.List)) and all(_const_str(elt) is not None for elt in node.elts):
        return [text for elt in node.elts if (text := _const_str(elt)) is not None]
    return None


def _walk_for(stmt: ast.For, env: _Env, rows: list[Row]) -> str | None:
    if stmt.orelse:
        return "for-else"
    pattern = _glob_call(stmt.iter, env)
    if pattern is not None and all(_append_is_fails(s) or _is_fails_report(s) for s in stmt.body):
        rows.append(("", "forbid_glob", pattern))
        return None
    values = _const_iter(stmt.iter, env)
    target = _name(stmt.target)
    if values is not None and target is not None:
        # Unroll `for name in ("a", "b"): must(name in hay)` so each needle
        # stays a literal. The loop variable is not a second contract.
        saved = env.strs.get(target)
        try:
            for value in values:
                env.strs[target] = value
                reason = _walk(stmt.body, env, rows)
                if reason:
                    return reason
        finally:
            if saved is None:
                env.strs.pop(target, None)
            else:
                env.strs[target] = saved
        return None
    return "for-shape"


def _is_self_wiring(rel: str, payload: str | list[str], path: Path) -> bool:
    needles = payload if isinstance(payload, list) else [payload]
    return rel.endswith("build.py") and any(path.stem in needle for needle in needles)


def extract_simple(path: Path, text: str) -> tuple[dict | None, str]:
    """Return (manifest, reason). reason is 'ok' or why the script stayed."""
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return None, "syntax"
    env = _Env()
    rows: list[Row] = []
    reason = _walk(tree.body, env, rows)
    if reason:
        return None, reason
    kept = [row for row in rows if not _is_self_wiring(row[0], row[2], path)]
    if not kept:
        return None, "no-contract"
    issue = _issue_id(path, text)
    if issue is None:
        return None, "no-issue"
    manifest = _manifest(path, text, issue, kept)
    if manifest is None:
        return None, "empty"
    return manifest, "ok"


def _manifest(path: Path, text: str, issue: int, rows: list[Row]) -> dict | None:
    grouped: dict[tuple[str, str], list] = defaultdict(list)
    order: list[tuple[str, str]] = []
    for rel, kind, payload in rows:
        key = (rel, kind)
        if key not in grouped:
            order.append(key)
        bucket = grouped[key]
        if kind == "contains_any":
            assert isinstance(payload, list)
            if payload not in bucket:
                bucket.append(payload)
        elif payload not in bucket:
            bucket.append(payload)
    checks = []
    ac_i = 1
    for rel, kind in order:
        bucket = grouped[(rel, kind)]
        if kind == "contains_any":
            for alt in bucket:
                row: dict = {"ac": f"AC{ac_i}", "contains_any": alt}
                _set_path(row, rel)
                checks.append(row)
                ac_i += 1
            continue
        if kind == "forbid":
            checks.append({"ac": f"AC{ac_i}", "forbid": bucket})
            ac_i += 1
            continue
        if kind == "forbid_glob":
            checks.append({"ac": f"AC{ac_i}", "forbid_glob": bucket})
            ac_i += 1
            continue
        row = {"ac": f"AC{ac_i}"}
        _set_path(row, rel)
        row[kind] = bucket
        checks.append(row)
        ac_i += 1
    if not checks:
        return None
    title = _doc_title(path, text)
    return {
        "issue": issue,
        "title": title,
        "ok": f"OK: Issue #{issue} folded from {path.name} — all AC rows satisfied",
        "checks": checks,
    }


def _doc_title(path: Path, text: str) -> str:
    """First docstring line. The file's first line is the shebang."""
    try:
        doc = ast.get_docstring(ast.parse(text)) or ""
    except SyntaxError:
        doc = ""
    for raw in doc.splitlines():
        line = raw.strip()
        if line:
            return line[:200]
    return path.stem


def _set_path(row: dict, rel: str) -> None:
    parts = [part for part in rel.split("\n") if part] if rel else []
    if len(parts) == 1:
        row["path"] = parts[0]
    elif len(parts) > 1:
        row["paths"] = parts


def _path_key(check: dict) -> str:
    if check.get("paths"):
        return "\n".join(check["paths"])
    return check.get("path", "")


def _row_needles(check: dict) -> set[tuple[str, str, str]]:
    path = _path_key(check)
    have: set[tuple[str, str, str]] = set()
    for needle in check.get("contains") or []:
        have.add((path, "contains", needle))
    for needle in check.get("contains_any") or []:
        have.add((path, "any", needle))
    for needle in check.get("absent") or []:
        have.add((path, "absent", needle))
    for needle in check.get("forbid") or []:
        have.add(("", "forbid", needle))
    for needle in check.get("forbid_glob") or []:
        have.add(("", "glob", needle))
    return have


def merge_manifest(existing: dict, extra: dict) -> dict:
    have: set[tuple[str, str, str]] = set()
    for check in existing.get("checks") or []:
        have |= _row_needles(check)
    for check in extra.get("checks") or []:
        path = _path_key(check)
        fresh = {item for item in _row_needles(check) if item not in have}
        if not fresh:
            continue
        have |= fresh
        row: dict = {"ac": check.get("ac", "folded")}
        if "forbid" not in check and "forbid_glob" not in check:
            _set_path(row, path)
        contains = [n for n in (check.get("contains") or []) if (path, "contains", n) in fresh]
        any_needles = [n for n in (check.get("contains_any") or []) if (path, "any", n) in fresh]
        absent = [n for n in (check.get("absent") or []) if (path, "absent", n) in fresh]
        forbid = [n for n in (check.get("forbid") or []) if ("", "forbid", n) in fresh]
        globs = [n for n in (check.get("forbid_glob") or []) if ("", "glob", n) in fresh]
        if contains:
            row["contains"] = contains
        if any_needles:
            row["contains_any"] = any_needles
        if absent:
            row["absent"] = absent
        if forbid:
            row["forbid"] = forbid
        if globs:
            row["forbid_glob"] = globs
        if len(row) > 1:
            existing.setdefault("checks", []).append(row)
    return existing


def _load_runner():
    spec = importlib.util.spec_from_file_location("coverage_runner", ROOT / "scripts" / "coverage" / "runner.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _validate(runner, manifest: dict) -> list[str]:
    try:
        return list(runner.run_manifest(manifest))
    except Exception as exc:  # a bad row must not delete the script
        return [f"runner raised {type(exc).__name__}: {exc}"]


def _shrink_grandfather(deleted: set[str]) -> int:
    if not GRANDFATHER.is_file():
        return 0
    kept: list[str] = []
    removed = 0
    for line in GRANDFATHER.read_text(encoding="utf-8").splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            kept.append(line)
            continue
        # Lines are either a basename or a repo-relative path. A full path
        # is not a child of CHECKS/, so join the basename before deciding
        # the script is gone.
        base = Path(name).name
        if base in deleted or name in deleted:
            removed += 1
            continue
        if not (CHECKS / base).is_file() and not (ROOT / name).is_file():
            removed += 1
            continue
        kept.append(line)
    GRANDFATHER.write_text("\n".join(kept) + "\n", encoding="utf-8")
    return removed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="Write manifests and delete folded scripts")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan (default)")
    parser.add_argument(
        "--wrappers-only",
        action="store_true",
        help="Only delete thin runner.py wrappers (do not fold simple scripts)",
    )
    args = parser.parse_args()

    wrappers: list[Path] = []
    folded: list[tuple[Path, dict]] = []
    reasons: Counter[str] = Counter()

    for path in sorted(CHECKS.glob("check_*.py")):
        if path.name == "check_coverage_policy.py":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        kind = classify(path, text)
        if kind == "wrapper":
            wrappers.append(path)
            reasons["wrapper"] += 1
            continue
        if args.wrappers_only or kind == "custom":
            reasons[kind] += 1
            continue
        manifest, why = extract_simple(path, text)
        if manifest is None:
            reasons[f"kept-{why}"] += 1
            continue
        folded.append((path, manifest))
        reasons["folded"] += 1

    print(f"wrappers (delete): {len(wrappers)}")
    print(f"simple folded:     {len(folded)}")
    print(f"kept:              {sum(n for k, n in reasons.items() if k not in {'folded', 'wrapper'})}")
    for key, count in reasons.most_common():
        if key in {"folded", "wrapper"}:
            continue
        print(f"  {key}: {count}")

    if not args.apply:
        print("dry-run; pass --apply to write")
        return 0

    if args.wrappers_only:
        for path in wrappers:
            path.unlink()
        print(f"deleted {len(wrappers)} wrapper script(s)")
        return 0

    runner = _load_runner()
    by_issue: dict[int, list[tuple[Path, dict]]] = defaultdict(list)
    for path, manifest in folded:
        by_issue[int(manifest["issue"])].append((path, manifest))

    written = 0
    deleted_names: set[str] = set()
    failed_issues = 0
    for issue, items in sorted(by_issue.items()):
        dest = MANIFESTS / f"{issue}.json"
        if dest.is_file():
            merged = json.loads(dest.read_text(encoding="utf-8"))
        else:
            merged = {"issue": issue, "title": items[0][1]["title"], "ok": items[0][1]["ok"], "checks": []}
        accepted: list[Path] = []
        for path, manifest in items:
            trial = json.loads(json.dumps(merged))
            trial = merge_manifest(trial, manifest)
            fails = _validate(runner, trial)
            if fails:
                failed_issues += 1
                print(f"KEEP {path.name}: manifest #{issue} would fail", file=sys.stderr)
                for line in fails[:6]:
                    print(f"  {line}", file=sys.stderr)
                continue
            merged = trial
            accepted.append(path)
        if not accepted:
            continue
        dest.write_text(
            json.dumps(merged, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        written += 1
        for path in accepted:
            path.unlink()
            deleted_names.add(path.name)

    for path in wrappers:
        if path.is_file():
            path.unlink()
            deleted_names.add(path.name)
    shrunk = _shrink_grandfather(deleted_names)
    print(
        f"wrote {written} manifest(s); deleted {len(deleted_names)} check script(s); "
        f"grandfather -{shrunk}; rejected {failed_issues} script(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
