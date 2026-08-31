#!/usr/bin/env python3
"""Fold substring-only coverage check_*.py files into JSON manifests.

Thin wrappers (`subprocess` → runner.py --issue N) are deleted: gate runs
manifests via run_checks.py. Simple `must("needle" in file)` scripts become
`scripts/coverage/manifests/<N>.json`. Custom-logic scripts are left alone.

Usage:
  python3 scripts/coverage/fold_simple_checks.py --dry-run
  python3 scripts/coverage/fold_simple_checks.py --apply
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKS = ROOT / "scripts" / "coverage" / "checks"
MANIFESTS = ROOT / "scripts" / "coverage" / "manifests"

_ISSUE_IN_NAME = re.compile(r"_(\d{3,5})\.py$")
_ISSUE_IN_DOC = re.compile(r"Issue\s+#(\d{3,5})")


def _issue_id(path: Path, text: str) -> int | None:
    m = _ISSUE_IN_NAME.search(path.name)
    if m:
        return int(m.group(1))
    m = _ISSUE_IN_DOC.search(text)
    return int(m.group(1)) if m else None


def classify(path: Path, text: str) -> str:
    if (
        "runner.py" in text
        and "--issue" in text
        and text.count("def ") <= 3
        and "must(" not in text.replace("_must(", "_X(")
    ):
        return "wrapper"
    if any(
        m in text
        for m in (
            "re.compile",
            "re.search",
            "re.findall",
            "re.match",
            "ast.parse",
        )
    ):
        return "custom"
    if "subprocess." in text:
        return "custom"
    # Windowed source-cite (find + slice) is custom logic, not a manifest row.
    if ".find(" in text and re.search(r"\[[^\]]+:[^\]]+\]", text):
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


def extract_simple(path: Path, text: str) -> dict | None:
    """Return a manifest dict, or None if the script is not a pure substring gate."""
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return None
    reads: dict[str, str] = {}
    must_calls = 0
    resolved = 0
    extracted: list[tuple[str, str, str | list[str]]] = []

    class Visitor(ast.NodeVisitor):
        def visit_Assign(self, node: ast.Assign) -> None:  # noqa: N802
            if len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
                val = node.value
                if isinstance(val, ast.Call) and _name(val.func) in {"_read", "read"} and val.args:
                    rel = _const_str(val.args[0])
                    if rel:
                        reads[node.targets[0].id] = rel
            self.generic_visit(node)

        def visit_Call(self, node: ast.Call) -> None:  # noqa: N802
            nonlocal must_calls, resolved
            fname = _name(node.func)
            if fname not in {"must", "_must"}:
                self.generic_visit(node)
                return
            must_calls += 1
            rows = _parse_must(node, reads)
            if rows is None:
                return
            resolved += 1
            for rel, kind, payload in rows:
                if _is_self_wiring(rel, payload, path):
                    continue
                extracted.append((rel, kind, payload))
            self.generic_visit(node)

    Visitor().visit(tree)
    if must_calls == 0 or resolved != must_calls or not extracted:
        return None
    issue = _issue_id(path, text)
    if issue is None:
        return None
    by_path: dict[str, dict[str, list[str]]] = defaultdict(lambda: {"contains": [], "contains_any": []})
    for rel, kind, payload in extracted:
        bucket = by_path[rel]
        if kind == "contains":
            assert isinstance(payload, str)
            if payload not in bucket["contains"]:
                bucket["contains"].append(payload)
        else:
            assert isinstance(payload, list)
            if payload not in bucket["contains_any"]:
                bucket["contains_any"].append(payload)  # type: ignore[arg-type]
    checks = []
    ac_i = 1
    for rel, bucket in by_path.items():
        row: dict = {"ac": f"AC{ac_i}", "path": rel}
        ac_i += 1
        if bucket["contains"]:
            row["contains"] = bucket["contains"]
        if bucket["contains_any"]:
            # Flatten one contains_any list per alt-group by emitting extra rows.
            if len(bucket["contains_any"]) == 1:
                row["contains_any"] = bucket["contains_any"][0]
            else:
                for alt in bucket["contains_any"]:
                    checks.append({"ac": f"AC{ac_i}", "path": rel, "contains_any": alt})
                    ac_i += 1
        if "contains" in row or "contains_any" in row:
            checks.append(row)
    if not checks:
        return None
    first = text.splitlines()[0].replace('"""', "").replace("'''", "").strip()
    title = (first or path.stem)[:200]
    return {
        "issue": issue,
        "title": title,
        "ok": f"OK: Issue #{issue} folded from {path.name} — all AC rows satisfied",
        "checks": checks,
    }


def _is_self_wiring(rel: str, payload: str | list[str], path: Path) -> bool:
    needles = payload if isinstance(payload, list) else [payload]
    stem = path.stem
    return rel.endswith("build.py") and any(stem in n or "check_" in n for n in needles)


def _parse_must(node: ast.Call, reads: dict[str, str]) -> list[tuple[str, str, str | list[str]]] | None:
    args = node.args
    if len(args) >= 3:
        needle = _const_str(args[0])
        hay = _name(args[2])
        if needle is None or hay is None or hay not in reads:
            return None
        return [(reads[hay], "contains", needle)]
    if len(args) >= 2:
        return _parse_in_cond(args[0], reads)
    return None


def _parse_in_cond(cond: ast.AST, reads: dict[str, str]) -> list[tuple[str, str, str | list[str]]] | None:
    if isinstance(cond, ast.Compare) and len(cond.ops) == 1 and isinstance(cond.ops[0], ast.In):
        needle = _const_str(cond.left)
        hay = _name(cond.comparators[0])
        if needle is None or hay is None or hay not in reads:
            return None
        return [(reads[hay], "contains", needle)]
    if isinstance(cond, ast.BoolOp) and isinstance(cond.op, ast.Or):
        alts: list[str] = []
        rel = None
        for v in cond.values:
            rows = _parse_in_cond(v, reads)
            if not rows or len(rows) != 1 or rows[0][1] != "contains":
                return None
            path, _, payload = rows[0]
            if rel is None:
                rel = path
            elif rel != path:
                return None
            assert isinstance(payload, str)
            alts.append(payload)
        if rel is None or not alts:
            return None
        return [(rel, "contains_any", alts)]
    if isinstance(cond, ast.BoolOp) and isinstance(cond.op, ast.And):
        out: list[tuple[str, str, str | list[str]]] = []
        for v in cond.values:
            rows = _parse_in_cond(v, reads)
            if not rows:
                return None
            out.extend(rows)
        return out or None
    return None


def _strip_missing_manifest_paths() -> int:
    """Drop manifest check rows whose path no longer exists (wrapper self-cites)."""
    n = 0
    for dest in sorted(MANIFESTS.glob("*.json")):
        data = json.loads(dest.read_text(encoding="utf-8"))
        checks = data.get("checks") or []
        keep = []
        changed = False
        for ch in checks:
            rel = ch.get("path")
            if rel and not (ROOT / rel).is_file() and not (ROOT / rel).is_dir():
                n += 1
                changed = True
                continue
            keep.append(ch)
        if changed:
            data["checks"] = keep
            dest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return n


def merge_manifest(existing: dict, extra: dict) -> dict:
    have: set[tuple[str, str, str]] = set()
    for ch in existing.get("checks") or []:
        path = ch.get("path", "")
        for n in ch.get("contains") or []:
            have.add((path, "contains", n))
        for n in ch.get("contains_any") or []:
            have.add((path, "any", n))
    for ch in extra.get("checks") or []:
        path = ch.get("path", "")
        new_contains = [n for n in (ch.get("contains") or []) if (path, "contains", n) not in have]
        new_any = [n for n in (ch.get("contains_any") or []) if (path, "any", n) not in have]
        if not new_contains and not new_any:
            continue
        row = {"ac": ch.get("ac", "folded"), "path": path}
        if new_contains:
            row["contains"] = new_contains
        if new_any:
            row["contains_any"] = new_any
        existing.setdefault("checks", []).append(row)
    return existing


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--apply", action="store_true", help="Write manifests and delete folded scripts")
    ap.add_argument("--dry-run", action="store_true", help="Print the plan (default)")
    ap.add_argument(
        "--wrappers-only",
        action="store_true",
        help="Only delete thin runner.py wrappers (do not fold simple scripts)",
    )
    args = ap.parse_args()
    apply = bool(args.apply)

    wrappers: list[Path] = []
    folded: list[tuple[Path, dict]] = []
    skipped: list[tuple[str, str]] = []

    for path in sorted(CHECKS.glob("check_*.py")):
        if path.name == "check_coverage_policy.py":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        kind = classify(path, text)
        if kind == "wrapper":
            wrappers.append(path)
            continue
        if args.wrappers_only:
            skipped.append((path.name, kind))
            continue
        if kind != "simple":
            skipped.append((path.name, kind))
            continue
        man = extract_simple(path, text)
        if man is None:
            skipped.append((path.name, "simple-unparsed"))
            continue
        dest = MANIFESTS / f"{int(man['issue'])}.json"
        if dest.is_file():
            # Existing manifest is source of truth; do not merge (avoids
            # polluting hand-written JSON / dangling check_*.py paths).
            skipped.append((path.name, "manifest-exists"))
            continue
        folded.append((path, man))

    print(f"wrappers (delete): {len(wrappers)}")
    print(f"simple folded:     {len(folded)}")
    print(f"kept (custom/other/unparsed): {len(skipped)}")
    unparsed = [n for n, k in skipped if k == "simple-unparsed"]
    if unparsed:
        print(f"  unparsed simple ({len(unparsed)}): " + ", ".join(unparsed[:12]))

    if not apply:
        print("dry-run; pass --apply to write")
        return 0

    if args.wrappers_only:
        deleted = 0
        for path in wrappers:
            path.unlink()
            deleted += 1
        stripped = _strip_missing_manifest_paths()
        print(f"deleted {deleted} wrapper script(s); stripped {stripped} dangling manifest path row(s)")
        return 0

    MANIFESTS.mkdir(parents=True, exist_ok=True)
    by_issue: dict[int, dict] = {}
    for _path, man in folded:
        issue = int(man["issue"])
        dest = MANIFESTS / f"{issue}.json"
        if issue in by_issue:
            by_issue[issue] = merge_manifest(by_issue[issue], man)
        elif dest.is_file():
            existing = json.loads(dest.read_text(encoding="utf-8"))
            by_issue[issue] = merge_manifest(existing, man)
        else:
            by_issue[issue] = man

    written = 0
    for issue, man in sorted(by_issue.items()):
        dest = MANIFESTS / f"{issue}.json"
        dest.write_text(json.dumps(man, indent=2) + "\n", encoding="utf-8")
        written += 1

    deleted = 0
    for path, _ in folded:
        path.unlink()
        deleted += 1
    for path in wrappers:
        path.unlink()
        deleted += 1

    print(f"wrote {written} manifest(s); deleted {deleted} check script(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
