#!/usr/bin/env python3
"""
Audit standalone aura_add_issue_test targets in CMakeLists.txt.

For each of the ~368 calls, produce:
  - profile: which link helper (or "none")
  - name_pattern: "issue_NNN" vs "descriptive" vs "domain_feature" vs "batch"
  - source_path: where the .cpp lives (tests/issues/, tests/, tests/<domain>.cpp)
  - in_bundle: which bundle profile (LIGHT/JIT/JIT_MINIMAL/JIT_CONTRACT/JIT_TESTS/FIBER/JIT_LATE*/LIGHT_LATE) — or "not_bundled"
  - is_3arg: whether it uses the optional 3rd arg

Output: stdout report + writes /tmp/standalone_audit.json
"""

from __future__ import annotations

import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CMAKE = REPO / "CMakeLists.txt"
BUNDLES_CMAKE = REPO / "cmake" / "AuraIssueBundles.cmake"


def parse_cmakelists_adds():
    """Return list of (line_no, name, third_arg_or_None)."""
    text = CMAKE.read_text()
    lines = text.split("\n")
    adds = []
    for i, line in enumerate(lines, 1):
        m = re.match(r"^\s*aura_add_issue_test\(([a-zA-Z_0-9]+)(.*?)\)\s*(#.*)?$", line)
        if not m:
            continue
        name = m.group(1)
        rest = m.group(2).strip()
        third = rest if rest else None
        adds.append((i, name, third))
    return adds, lines


def link_for(name, lines, start_line_idx):
    """Look ahead for matching aura_issue_test_link_*(name) call within next 8 lines."""
    for j in range(start_line_idx, min(start_line_idx + 8, len(lines))):
        m = re.match(r"^\s*(aura_issue_test_link_[a-z_]+)\(([a-zA-Z_0-9]+)\)", lines[j])
        if m and m.group(2) == name:
            return m.group(1)
    return None


def parse_bundle_members():
    """Return {profile_name: set(members)} for all AURA_ISSUE_BUNDLE_*_MEMBERS."""
    text = BUNDLES_CMAKE.read_text()
    out = {}
    for m in re.finditer(r"^set\(AURA_ISSUE_BUNDLE_([A-Z_0-9]+)_MEMBERS\s+(.*?)\)\s*$", text, re.M):
        profile = m.group(1)
        items = set(x.strip() for x in m.group(2).split(";") if x.strip())
        out[profile] = items
    return out


def classify_name(name):
    """Classify naming pattern of the standalone target."""
    if re.fullmatch(r"test_issue_\d+(?:_[a-zA-Z]+)?", name):
        # Could be plain issue or issue with descriptive suffix
        if re.fullmatch(r"test_issue_\d+", name):
            return "issue_NNN_plain"
        return "issue_NNN_suffixed"
    if re.fullmatch(r"test_(fiber|ir|mutation|observability|persist)\b.*", name):
        return "domain_stub_or_ext"
    if name.startswith("test_issues_") or name.startswith("test_") and "_batch" in name:
        return "batch_test"
    if "sweep" in name:
        return "production_sweep"
    return "descriptive"


def resolve_source(name):
    """Find where the .cpp file for this name lives."""
    candidates = [
        REPO / "tests" / "issues" / f"{name}.cpp",
        REPO / "tests" / f"{name}.cpp",
        REPO / "tests" / "domain" / f"{name}.cpp",  # not actually used but check
    ]
    for c in candidates:
        if c.exists():
            return str(c.relative_to(REPO))
    return "MISSING"


def profile_from_link(link):
    return link.replace("aura_issue_test_link_", "") if link else "NONE"


def bundle_for(name, bundle_members):
    """Return list of bundle profiles this name belongs to."""
    hits = []
    for profile, members in bundle_members.items():
        if name in members:
            hits.append(profile)
    return hits


def main() -> int:
    adds, lines = parse_cmakelists_adds()
    bundle_members = parse_bundle_members()

    # Adjust: lines is 0-indexed; parse_cmakelists_adds returns 1-indexed line_no
    records = []
    for line_no, name, third in adds:
        link = link_for(name, lines, line_no)  # pass 1-indexed start
        prof = profile_from_link(link)
        pattern = classify_name(name)
        src = resolve_source(name)
        bundles = bundle_for(name, bundle_members)

        records.append(
            {
                "line": line_no,
                "name": name,
                "link": link,
                "profile": prof,
                "name_pattern": pattern,
                "third_arg": third,
                "source": src,
                "in_bundles": bundles,
            }
        )

    # Categorize into the 4 buckets
    buckets = defaultdict(list)
    for r in records:
        if r["link"] is None:
            buckets["(c)_no_link"].append(r)
        elif r["in_bundles"]:
            buckets["(a)_overlap_with_bundle"].append(r)
        else:
            buckets["(b)_orphan_not_in_bundle"].append(r)

    # Report
    print(f"Total standalone aura_add_issue_test calls: {len(records)}")
    print()
    profile_dist = Counter(r["profile"] for r in records)
    print("By profile:")
    for p, c in profile_dist.most_common():
        print(f"  {p}: {c}")
    print()
    pattern_dist = Counter(r["name_pattern"] for r in records)
    print("By name pattern:")
    for p, c in pattern_dist.most_common():
        print(f"  {p}: {c}")
    print()
    source_dist = Counter(r["source"].split("/")[1] if "/" in r["source"] else r["source"] for r in records)
    print("By source location (top dir):")
    for s, c in source_dist.most_common():
        print(f"  {s}: {c}")
    print()
    print(f"=== Bucket (a): overlap with bundle — {len(buckets['(a)_overlap_with_bundle'])} ===")
    print(f"=== Bucket (b): orphan, not in any bundle — {len(buckets['(b)_orphan_not_in_bundle'])} ===")
    print(f"=== Bucket (c): no link helper — {len(buckets['(c)_no_link'])} ===")
    print()

    # Detailed breakdown of (c)
    if buckets["(c)_no_link"]:
        print("--- (c) no link helper — names ---")
        for r in buckets["(c)_no_link"]:
            print(f"  L{r['line']:>4}  {r['name']}  src={r['source']}")
        print()

    # Detailed breakdown of (a) by bundle profile
    print("--- (a) overlap with bundle: by which bundle ---")
    a_bundle_dist = Counter()
    for r in buckets["(a)_overlap_with_bundle"]:
        for b in r["in_bundles"]:
            a_bundle_dist[b] += 1
    for b, c in a_bundle_dist.most_common():
        print(f"  {b}: {c}")
    print()

    # Detailed breakdown of (b) by profile
    print("--- (b) orphan not in bundle: by profile ---")
    b_profile_dist = Counter(r["profile"] for r in buckets["(b)_orphan_not_in_bundle"])
    for p, c in b_profile_dist.most_common():
        print(f"  {p}: {c}")
    print()
    print("--- (b) sample names (first 20) ---")
    for r in buckets["(b)_orphan_not_in_bundle"][:20]:
        print(f"  L{r['line']:>4}  {r['name']}  prof={r['profile']}  src={r['source']}")
    print()

    # Save JSON
    out = {
        "total": len(records),
        "by_profile": dict(profile_dist),
        "by_name_pattern": dict(pattern_dist),
        "by_source_dir": dict(source_dist),
        "buckets": {k: v for k, v in buckets.items()},
        "all_records": records,
    }
    Path("/tmp/standalone_audit.json").write_text(json.dumps(out, indent=2))
    print("Full records → /tmp/standalone_audit.json")


if __name__ == "__main__":
    sys.exit(main())
