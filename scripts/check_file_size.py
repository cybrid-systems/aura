#!/usr/bin/env python3
"""check_file_size.py — file size policy enforcer (#382 + structure P0).

Walks src/ for .ixx (and optionally large .cpp/.h) and reports files over
thresholds. See scripts/file_size_policy.md.

Exit codes:
  0  no blocker
  1  at least one blocker
  2  script misuse
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

# Known oversized TUs — must not grow past these caps.
# Shrink over time; remove an entry when under the normal blocker.
# Caps are slightly above the measured size at introduction of this check.
FILE_LINE_CAPS: dict[str, int] = {
    # .ixx interfaces (pre-existing #382 debt)
    "src/compiler/evaluator.ixx": 9600,
    "src/compiler/service.ixx": 9200,
    "src/core/ast.ixx": 6600,
    "src/compiler/pass_manager.ixx": 4000,
    # .cpp / .h implementations
    "src/compiler/evaluator_primitives_observability.cpp": 22000,
    "src/compiler/evaluator_primitives_query.cpp": 8000,
    "src/compiler/type_checker_impl.cpp": 6500,
    "src/compiler/observability_metrics.h": 5600,
    "src/compiler/evaluator_primitives_compile.cpp": 5400,
    "src/compiler/evaluator_primitives_mutate.cpp": 4300,
    "src/compiler/evaluator_eval_flat.cpp": 4200,
    "src/compiler/evaluator_primitives_security.cpp": 3600,
    "src/compiler/aura_jit.cpp": 3100,
    "src/main.cpp": 2900,
}

# Back-compat alias for docs / --json consumers.
CPP_LINE_CAPS = FILE_LINE_CAPS


@dataclass(frozen=True)
class FileReport:
    path: str
    lines: int
    status: str  # "ok" | "warning" | "blocker" | "capped"
    kind: str  # "ixx" | "cpp" | "h"


def count_lines(path: Path) -> int:
    count = 0
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            count += chunk.count(b"\n")
    return count


def walk_files(src_root: Path, suffixes: tuple[str, ...]) -> list[Path]:
    if not src_root.is_dir():
        return []
    files: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(src_root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for name in filenames:
            if name.endswith(suffixes):
                files.append(Path(dirpath) / name)
    return sorted(files)


def classify_file(rel: str, lines: int, warning: int, blocker: int) -> str:
    """Capped files: blocker only if over freeze cap. Others: normal thresholds."""
    if rel in FILE_LINE_CAPS:
        if lines > FILE_LINE_CAPS[rel]:
            return "blocker"
        if lines >= warning:
            return "capped"
        return "ok"
    if lines >= blocker:
        return "blocker"
    if lines >= warning:
        return "warning"
    return "ok"


def rel_path(path: Path, repo: Path) -> str:
    try:
        return str(path.resolve().relative_to(repo.resolve()))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Enforce file size policy on .ixx and large .cpp/.h under src/",
    )
    parser.add_argument("--src", default="src", help="Source root (default: src)")
    parser.add_argument("--warning", type=int, default=800, help="ixx warning lines")
    parser.add_argument("--blocker", type=int, default=2000, help="ixx blocker lines")
    parser.add_argument(
        "--cpp-warning",
        type=int,
        default=2500,
        help="cpp/h warning lines for non-capped files (default: 2500)",
    )
    parser.add_argument(
        "--cpp-blocker",
        type=int,
        default=5000,
        help="cpp/h blocker for non-capped files (default: 5000)",
    )
    parser.add_argument(
        "--no-cpp",
        action="store_true",
        help="Only scan .ixx (legacy #382 behavior)",
    )
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    if args.warning <= 0 or args.blocker <= 0:
        print("error: thresholds must be positive", file=sys.stderr)
        return 2
    if args.blocker < args.warning:
        print("error: blocker must be >= warning", file=sys.stderr)
        return 2

    src_root = Path(args.src)
    if not src_root.is_dir():
        print(f"error: source root '{src_root}' is not a directory", file=sys.stderr)
        return 2

    repo = src_root.parent if src_root.name == "src" else Path.cwd()
    classified: list[FileReport] = []

    ixx_files = walk_files(src_root, (".ixx",))
    if not ixx_files:
        print(f"error: no .ixx files found under '{src_root}'", file=sys.stderr)
        return 2

    for f in ixx_files:
        lines = count_lines(f)
        rel = rel_path(f, repo)
        classified.append(
            FileReport(
                path=rel,
                lines=lines,
                status=classify_file(rel, lines, args.warning, args.blocker),
                kind="ixx",
            )
        )

    if not args.no_cpp:
        for f in walk_files(src_root, (".cpp", ".h", ".hh", ".hpp")):
            lines = count_lines(f)
            rel = rel_path(f, repo)
            kind = "h" if f.suffix in {".h", ".hh", ".hpp"} else "cpp"
            classified.append(
                FileReport(
                    path=rel,
                    lines=lines,
                    status=classify_file(rel, lines, args.cpp_warning, args.cpp_blocker),
                    kind=kind,
                )
            )

    blockers = [r for r in classified if r.status == "blocker"]
    warnings = [r for r in classified if r.status == "warning"]
    capped = [r for r in classified if r.status == "capped"]
    ok = [r for r in classified if r.status == "ok"]

    if args.json:
        out = {
            "thresholds": {
                "ixx_warning": args.warning,
                "ixx_blocker": args.blocker,
                "cpp_warning": args.cpp_warning,
                "cpp_blocker": args.cpp_blocker,
            },
            "file_line_caps": FILE_LINE_CAPS,
            "summary": {
                "total": len(classified),
                "ok": len(ok),
                "warning": len(warnings),
                "capped": len(capped),
                "blocker": len(blockers),
            },
            "files": [{"path": r.path, "lines": r.lines, "status": r.status, "kind": r.kind} for r in classified],
        }
        print(json.dumps(out, indent=2))
    else:
        print(f"File size policy — {len(classified)} files under {src_root}")
        print(f"  ixx:  warning={args.warning}  blocker={args.blocker}")
        if not args.no_cpp:
            print(f"  cpp:  warning={args.cpp_warning}  blocker={args.cpp_blocker}  (+ caps)")
        print()
        if blockers:
            print(f"BLOCKERS ({len(blockers)}) — must shrink / split before merge:")
            for r in sorted(blockers, key=lambda x: -x.lines):
                cap = FILE_LINE_CAPS.get(r.path)
                extra = f"  (cap {cap})" if cap is not None else ""
                print(f"  {r.lines:>6d}  [{r.kind}]  {r.path}{extra}")
            print()
        if capped:
            print(f"CAPPED ({len(capped)}) — grandfathered; must not exceed cap:")
            for r in sorted(capped, key=lambda x: -x.lines):
                print(f"  {r.lines:>6d}  [{r.kind}]  {r.path}  cap={FILE_LINE_CAPS[r.path]}")
            print()
        if warnings:
            print(f"WARNINGS ({len(warnings)}) — schedule split:")
            for r in sorted(warnings, key=lambda x: -x.lines):
                print(f"  {r.lines:>6d}  [{r.kind}]  {r.path}")
            print()
        print(f"OK ({len(ok)} files under warning / within policy)")

    return 1 if blockers else 0


if __name__ == "__main__":
    sys.exit(main())
