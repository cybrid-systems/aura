#!/usr/bin/env python3
"""Categorize dead bumps by prefix + show which fall in #1645's hot paths."""

import os
import re

AURA = "/home/dev/code/aura"
os.chdir(AURA)

decl_pattern = re.compile(r"\b(?:void|std::uint64_t|int64_t|bool|auto)\s+bump_([a-z_][a-z0-9_]*)\s*\(")
call_pattern = re.compile(r"\bbump_([a-z_][a-z0-9_]*)\s*\(")

decls = set()
with open("src/compiler/evaluator.ixx") as f:
    for m in decl_pattern.finditer(f.read()):
        decls.add("bump_" + m.group(1))

caller_files = {}
for root, _dirs, files in os.walk("src"):
    for fn in files:
        if not fn.endswith((".cpp", ".h", ".hpp", ".ixx", ".inc", ".hh", ".c", ".cc")):
            continue
        if fn in ("evaluator.ixx", "observability_metrics.h"):
            continue
        p = os.path.join(root, fn)
        try:
            with open(p) as f:
                for m in call_pattern.finditer(f.read()):
                    n = "bump_" + m.group(1)
                    caller_files.setdefault(n, set()).add(p)
        except (UnicodeDecodeError, OSError):
            pass

dead = sorted(d for d in decls if d not in caller_files)


# Categorize by prefix (between "bump_" and the first non-prefix separator)
def category(name):
    parts = name[5:].split("_")  # strip bump_
    # Heuristic: first 1-2 parts before a meaningful keyword.
    if len(parts) >= 2 and parts[1] in (
        "total",
        "count",
        "prevented",
        "detected",
        "enforced",
        "rate",
        "failure",
        "success",
        "attempts",
        "blocks",
        "mismatch",
        "rollback",
        "refresh",
        "bump",
        "hygiene",
        "dirty",
        "stable",
    ):
        return "_".join(parts[:2])
    return parts[0]


cats = {}
for n in dead:
    cats.setdefault(category(n), []).append(n)

print(f"Total: {len(dead)} dead bumps")
print()
print("Categories (top 50):")
for cat, lst in sorted(cats.items(), key=lambda kv: -len(kv[1]))[:50]:
    print(f"  {cat:30s}  {len(lst):3d}")

# Map to #1645 hot paths.
hot_paths = {
    "guard": [],
    "stable_ref": [],
    "tag_arity": [],
    "dirty": [],
    "macro_introduced": [],
    "hygiene": [],
    "cross_cow": [],
    "fiber": [],
    "cow": [],
}

for n in dead:
    for prefix, _ in hot_paths.items():
        if prefix in n.lower():
            hot_paths[prefix].append(n)
            break

print("\nHot-path mapping (per #1645 body's explicit categories):")
for path, lst in hot_paths.items():
    print(f"  {path:25s}  {len(lst):3d} dead bumps")
    for n in lst[:8]:
        print(f"      {n}")

# Full list of dead, for picking top targets.
print(f"\n=== FULL DEAD LIST ({len(dead)} bumps) ===")
for n in dead:
    print(f"  {n}")
