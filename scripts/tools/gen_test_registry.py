#!/usr/bin/env python3
"""Issue #1453 / #5247: TestRegistry catalog of C++ tests that CMake builds.

Scans tests/**/test_*.cpp that an enabled CMake target compiles, and writes
docs/generated/test-registry.json. Paths in cmake/AuraUnbuiltTests.cmake are
not registered: they are not sources of an enabled target.

Usage:
  python3 scripts/tools/gen_test_registry.py           # write
  python3 scripts/tools/gen_test_registry.py --check   # fail if stale or unlisted
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
OUT = ROOT / "docs" / "generated" / "test-registry.json"
UNBUILT_CMAKE = ROOT / "cmake" / "AuraUnbuiltTests.cmake"

CAT_RE = re.compile(r"^//\s*@category:\s*(\S+)", re.M)
REASON_RE = re.compile(r"^//\s*@reason:\s*(.+)$", re.M)
ISSUE_RE = re.compile(r"test_issue_(\d+)")
PATH_RE = re.compile(r"tests/[A-Za-z0-9_./+-]+\.cpp")
CALL_RE = re.compile(
    r"\b(?:aura_add_issue_test|aura_add_issue_test_standalone|"
    r"aura_add_issue_test_reflect_standalone)\(\s*([A-Za-z0-9_]+)"
)
BUNDLE_RE = re.compile(r"\baura_add_issue_bundle\(\s*([A-Za-z0-9_]+)\s*\)")
MEMBER_RE = re.compile(
    r"set\(\s*AURA_ISSUE_BUNDLE_([A-Z0-9_]+)_MEMBERS\s+([^)]*)\)",
    re.S,
)
ENTRY_RE = re.compile(r'"([^"\n]+)"')
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

SKIP_SCAN_DIRS = {"issues", "domain", "python", "fixtures", "bundles", "cases"}


def _strip_cmake_line(line: str) -> str:
    out: list[str] = []
    i = 0
    in_str = False
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "#":
            if line.endswith("\n"):
                out.append("\n")
            break
        out.append(c)
        i += 1
    return "".join(out)


def _live_cmake() -> str:
    parts: list[str] = []
    files = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake"))]
    files.extend(TESTS.rglob("CMakeLists.txt"))
    files.extend(TESTS.rglob("*.cmake"))
    for path in files:
        if not path.is_file() or path.resolve() == UNBUILT_CMAKE.resolve():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        parts.append("".join(_strip_cmake_line(line) for line in text.splitlines(keepends=True)))
    return "\n".join(parts)


def _resolve_stem(name: str) -> list[Path]:
    domain = TESTS / "domain" / f"{name}.cpp"
    if domain.is_file():
        return [domain]
    hits: list[Path] = []
    for pattern in (f"domain/*/{name}.cpp", f"*/{name}.cpp", f"*/*/{name}.cpp"):
        hits.extend(p for p in TESTS.glob(pattern) if p.is_file())
    root_file = TESTS / f"{name}.cpp"
    if root_file.is_file():
        hits.append(root_file)
    seen: set[Path] = set()
    unique: list[Path] = []
    for hit in hits:
        resolved = hit.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(hit)
    return unique


def built_cpp_rels() -> tuple[set[str], list[str]]:
    """Return (repo-relative cpp paths an enabled target compiles, errors)."""
    live = _live_cmake()
    errors: list[str] = []
    built: set[str] = set(PATH_RE.findall(live))
    stems = set(CALL_RE.findall(live))
    enabled = set(BUNDLE_RE.findall(live))
    members: dict[str, list[str]] = {}
    for prof, body in MEMBER_RE.findall(live):
        members[prof.lower()] = re.findall(r"\b(test_[A-Za-z0-9_]+)\b", body)
    for prof in enabled:
        stems.update(members.get(prof, []))
        built.add(f"tests/bundles/test_issues_{prof}_main.cpp")
        built.add("tests/bundles/issue_bundle_runner.cpp")
    for name in sorted(stems):
        hits = _resolve_stem(name)
        if len(hits) != 1:
            errors.append(f"stem {name} resolves to {len(hits)} files: {[h.as_posix() for h in hits]}")
            continue
        built.add(hits[0].relative_to(ROOT).as_posix())

    # A cpp compiled via #include from a built TU is part of that target.
    queue = [ROOT / rel for rel in sorted(built) if (ROOT / rel).is_file()]
    seen: set[Path] = set()
    while queue:
        path = queue.pop()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for inc in INCLUDE_RE.findall(text):
            cand = (path.parent / inc).resolve()
            try:
                rel = cand.relative_to(ROOT.resolve()).as_posix()
            except ValueError:
                continue
            if cand.suffix == ".cpp":
                built.add(rel)
            if cand.suffix in {".h", ".hh", ".hpp", ".inc", ".cpp"} and cand not in seen:
                queue.append(cand)
    return built, errors


def unbuilt_entries() -> list[tuple[str, str, str]]:
    if not UNBUILT_CMAKE.is_file():
        return []
    raw_text = UNBUILT_CMAKE.read_text(encoding="utf-8", errors="replace")
    text = "".join(_strip_cmake_line(line) for line in raw_text.splitlines(keepends=True))
    entries: list[tuple[str, str, str]] = []
    for raw in ENTRY_RE.findall(text):
        parts = raw.split("|", 2)
        if len(parts) != 3 or not parts[0] or not parts[1] or not parts[2]:
            raise ValueError(f"bad unbuilt entry: {raw!r}")
        entries.append((parts[0], parts[1], parts[2]))
    return entries


def unbuilt_paths() -> set[str]:
    return {path for path, _tag, _reason in unbuilt_entries()}


def check_unbuilt_inventory() -> list[str]:
    """Every tests/**/*.cpp is built, or listed with a reason. Not both."""
    built, errors = built_cpp_rels()
    try:
        entries = unbuilt_entries()
    except ValueError as exc:
        return errors + [str(exc)]
    listed: list[str] = []
    seen: set[str] = set()
    for path, tag, reason in entries:
        if path in seen:
            errors.append(f"duplicate unbuilt entry: {path}")
        seen.add(path)
        listed.append(path)
        if "for CI green" in reason or "for CI green" in tag:
            errors.append(f"{path}: reason must not be 'for CI green'")
        if not (ROOT / path).is_file():
            errors.append(f"listed unbuilt file does not exist: {path}")
        if path in built:
            errors.append(f"listed unbuilt file is an enabled target source: {path}")
    on_disk = {p.relative_to(ROOT).as_posix() for p in TESTS.rglob("*.cpp")}
    for rel in sorted(on_disk - built - seen):
        errors.append(f"not built and not listed in {UNBUILT_CMAKE.relative_to(ROOT)}: {rel}")
    return errors


def _candidates() -> list[Path]:
    tests_dir = TESTS
    found: list[Path] = []
    found.extend(sorted(tests_dir.glob("test_*.cpp")))
    issue_subdir = tests_dir / "issues"
    domain_dir = tests_dir / "domain"
    if issue_subdir.is_dir():
        found.extend(sorted(issue_subdir.glob("test_*.cpp")))
    if domain_dir.is_dir():
        found.extend(sorted(domain_dir.glob("test_*.cpp")))
        found.extend(sorted(domain_dir.glob("*/test_*.cpp")))
    for theme_dir in sorted(tests_dir.iterdir()):
        if not theme_dir.is_dir():
            continue
        name = theme_dir.name
        if name in SKIP_SCAN_DIRS or name.startswith("."):
            continue
        found.extend(sorted(theme_dir.glob("test_*.cpp")))
    skip = unbuilt_paths()
    unique = sorted(set(found))
    return [path for path in unique if path.relative_to(ROOT).as_posix() not in skip]


def scan() -> dict:
    entries: list[dict] = []
    for path in _candidates():
        try:
            head = path.read_text(encoding="utf-8", errors="replace")[:4000]
        except OSError:
            continue
        cat_m = CAT_RE.search(head)
        reason_m = REASON_RE.search(head)
        issue_m = ISSUE_RE.search(path.name)
        rel = path.relative_to(ROOT).as_posix()
        entries.append(
            {
                "file": rel,
                "category": cat_m.group(1) if cat_m else "unknown",
                "reason": reason_m.group(1).strip() if reason_m else "",
                "issue": int(issue_m.group(1)) if issue_m else None,
            }
        )

    by_cat: dict[str, int] = {}
    for entry in entries:
        by_cat[entry["category"]] = by_cat.get(entry["category"], 0) + 1

    return {
        "schema": 1,
        "note": (
            "Issue #1453 lightweight TestRegistry — generated by "
            "scripts/tools/gen_test_registry.py. Lists tests an enabled "
            "CMake target compiles (#5247). cmake/AuraUnbuiltTests.cmake "
            "holds the rest."
        ),
        "count": len(entries),
        "by_category": dict(sorted(by_cat.items())),
        "tests": entries,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="fail if registry is stale or a cpp is unlisted")
    ap.add_argument("--write", action="store_true", help="write registry (default)")
    args = ap.parse_args()
    inventory_errors = check_unbuilt_inventory()
    data = scan()
    text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"

    if args.check:
        if inventory_errors:
            print("FAIL: C++ test inventory (#5247)", file=sys.stderr)
            for err in inventory_errors:
                print(f"  {err}", file=sys.stderr)
            return 1
        if not OUT.exists():
            print(f"FAIL: missing {OUT.relative_to(ROOT)}; run gen_test_registry.py", file=sys.stderr)
            return 1
        old = OUT.read_text(encoding="utf-8")
        old_j = json.loads(old)
        old_files = {t["file"] for t in old_j.get("tests", [])}
        new_files = {t["file"] for t in data["tests"]}
        if old_files != new_files or old_j.get("count") != data["count"]:
            print(
                f"FAIL: test-registry.json stale (count {old_j.get('count')}→{data['count']}).\n"
                f"  Run: python3 scripts/tools/gen_test_registry.py\n"
                f"  And commit docs/generated/test-registry.json",
                file=sys.stderr,
            )
            return 1
        print(f"OK: test-registry.json up to date ({data['count']} tests)")
        return 0

    if inventory_errors:
        print("FAIL: C++ test inventory (#5247)", file=sys.stderr)
        for err in inventory_errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} ({data['count']} tests, categories={data['by_category']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
