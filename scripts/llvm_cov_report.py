#!/usr/bin/env python3
"""llvm_cov_report.py — Issue #1933 merge profraw + llvm-cov HTML/JSON.

Pure tooling (no full rebuild). Used by ./build.py coverage.

Usage:
  python3 scripts/llvm_cov_report.py \\
      --build-dir build_coverage \\
      --profraw-dir build_coverage/profraw \\
      --out-dir build_coverage/coverage \\
      --binaries build_coverage/aura build_coverage/test_foo \\
      --html --json \\
      [--min-line-pct 0] \\
      [--ignore-regex 'tests/|third_party/']
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def find_tool(names: list[str]) -> str | None:
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return None


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


def collect_profraw(profraw_dir: Path, extra_roots: list[Path]) -> list[Path]:
    files: list[Path] = []
    if profraw_dir.is_dir():
        files.extend(sorted(profraw_dir.glob("**/*.profraw")))
    for root in extra_roots:
        if root.is_dir():
            files.extend(sorted(root.glob("*.profraw")))
        elif root.is_file() and root.suffix == ".profraw":
            files.append(root)
    # de-dup
    seen: set[Path] = set()
    out: list[Path] = []
    for f in files:
        r = f.resolve()
        if r not in seen:
            seen.add(r)
            out.append(f)
    return out


def merge_profdata(profdata_bin: str, profraw: list[Path], out: Path) -> int:
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [profdata_bin, "merge", "-sparse", "-o", str(out), *[str(p) for p in profraw]]
    r = run(cmd, timeout=300)
    if r.returncode != 0:
        print(r.stderr or r.stdout, file=sys.stderr)
    return r.returncode


def export_json(
    cov_bin: str,
    profdata: Path,
    binaries: list[Path],
    out_json: Path,
    ignore_regex: str | None,
) -> int:
    out_json.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cov_bin, "export", f"-instr-profile={profdata}", "-format=text"]
    if ignore_regex:
        cmd.append(f"-ignore-filename-regex={ignore_regex}")
    for b in binaries:
        cmd.extend(["-object", str(b)])
    r = run(cmd, timeout=600)
    if r.returncode != 0:
        print(r.stderr or r.stdout, file=sys.stderr)
        return r.returncode
    out_json.write_text(r.stdout, encoding="utf-8")
    return 0


def export_html(
    cov_bin: str,
    profdata: Path,
    binaries: list[Path],
    out_dir: Path,
    ignore_regex: str | None,
) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        cov_bin,
        "show",
        f"-instr-profile={profdata}",
        f"-output-dir={out_dir}",
        "-format=html",
        "-show-line-counts-or-regions",
        "-Xdemangler=c++filt",
    ]
    if ignore_regex:
        cmd.append(f"-ignore-filename-regex={ignore_regex}")
    for b in binaries:
        cmd.extend(["-object", str(b)])
    r = run(cmd, timeout=600)
    if r.returncode != 0:
        print(r.stderr or r.stdout, file=sys.stderr)
    return r.returncode


def summary_report(cov_bin: str, profdata: Path, binaries: list[Path], ignore_regex: str | None) -> str:
    cmd = [cov_bin, "report", f"-instr-profile={profdata}"]
    if ignore_regex:
        cmd.append(f"-ignore-filename-regex={ignore_regex}")
    for b in binaries:
        cmd.extend(["-object", str(b)])
    r = run(cmd, timeout=300)
    return (r.stdout or "") + (r.stderr or "")


def parse_totals_from_export(export_path: Path) -> dict[str, float]:
    """Parse llvm-cov export JSON for overall line/region totals."""
    if not export_path.is_file():
        return {}
    try:
        data = json.loads(export_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    # llvm-cov export format: data[0].totals.lines.{count,covered,percent}
    try:
        totals = data["data"][0]["totals"]
        lines = totals.get("lines", {})
        regions = totals.get("regions", {})
        return {
            "line_pct": float(lines.get("percent", 0.0)),
            "line_covered": float(lines.get("covered", 0)),
            "line_count": float(lines.get("count", 0)),
            "region_pct": float(regions.get("percent", 0.0)),
        }
    except (KeyError, IndexError, TypeError, ValueError):
        return {}


def module_line_pct(export_path: Path, substr: str) -> float | None:
    """Best-effort per-file line % for paths containing substr."""
    if not export_path.is_file():
        return None
    try:
        data = json.loads(export_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    try:
        files = data["data"][0]["files"]
    except (KeyError, IndexError, TypeError):
        return None
    covered = 0
    count = 0
    for f in files:
        name = f.get("filename") or f.get("name") or ""
        if substr not in name:
            continue
        summary = f.get("summary", {}).get("lines", {})
        covered += int(summary.get("covered", 0))
        count += int(summary.get("count", 0))
    if count == 0:
        return None
    return 100.0 * covered / count


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--profraw-dir", type=Path, default=None)
    ap.add_argument("--out-dir", type=Path, default=None)
    ap.add_argument("--binaries", nargs="*", default=[])
    ap.add_argument("--html", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min-line-pct", type=float, default=0.0)
    ap.add_argument(
        "--ignore-regex",
        default=r"(/tests/|/third_party/|/build|/CMakeFiles/)",
        help="llvm-cov -ignore-filename-regex",
    )
    ap.add_argument(
        "--require-module",
        action="append",
        default=[],
        metavar="PATH_SUBSTR:MIN_PCT",
        help="soft/hard module gate e.g. evaluator:0 (checked when export JSON exists)",
    )
    args = ap.parse_args(argv)

    build_dir: Path = args.build_dir
    profraw_dir = args.profraw_dir or (build_dir / "profraw")
    out_dir = args.out_dir or (build_dir / "coverage")
    do_html = args.html or not args.json  # default html if neither? prefer both if --html
    if not args.html and not args.json:
        do_html = True
        args.json = True

    profdata_bin = find_tool(["llvm-profdata", "llvm-profdata-22", "llvm-profdata-20", "llvm-profdata-19"])
    cov_bin = find_tool(["llvm-cov", "llvm-cov-22", "llvm-cov-20", "llvm-cov-19"])
    if not profdata_bin or not cov_bin:
        print("ERROR: llvm-profdata / llvm-cov not found on PATH", file=sys.stderr)
        return 2

    binaries = [Path(b) for b in args.binaries if Path(b).is_file()]
    if not binaries:
        # Auto-discover common bins
        for name in ("aura",):
            p = build_dir / name
            if p.is_file():
                binaries.append(p)
        for p in sorted(build_dir.glob("test_*")):
            if p.is_file() and p.stat().st_mode & 0o111:
                binaries.append(p)
    if not binaries:
        print("ERROR: no instrumented binaries found", file=sys.stderr)
        return 2

    profraw = collect_profraw(profraw_dir, [build_dir, Path.cwd()])
    if not profraw:
        print(f"ERROR: no .profraw under {profraw_dir} (run instrumented tests first)", file=sys.stderr)
        return 2

    print("llvm-cov report (#1933)")
    print(f"  profdata tool : {profdata_bin}")
    print(f"  cov tool      : {cov_bin}")
    print(f"  profraw files : {len(profraw)}")
    print(f"  binaries      : {len(binaries)}")

    profdata = out_dir / "merged.profdata"
    if merge_profdata(profdata_bin, profraw, profdata) != 0:
        print("ERROR: llvm-profdata merge failed", file=sys.stderr)
        return 1
    print(f"  merged        : {profdata}")

    ignore = args.ignore_regex or None
    export_path = out_dir / "coverage.json"
    if args.json:
        if export_json(cov_bin, profdata, binaries, export_path, ignore) != 0:
            print("ERROR: llvm-cov export failed", file=sys.stderr)
            return 1
        print(f"  json          : {export_path}")

    if do_html:
        html_dir = out_dir / "html"
        if export_html(cov_bin, profdata, binaries, html_dir, ignore) != 0:
            print("ERROR: llvm-cov show (html) failed", file=sys.stderr)
            return 1
        print(f"  html          : {html_dir / 'index.html'}")

    report_txt = summary_report(cov_bin, profdata, binaries, ignore)
    (out_dir / "summary.txt").write_text(report_txt, encoding="utf-8")
    print("── llvm-cov report (totals) ──")
    # Print last ~20 lines (TOTAL row)
    lines = [ln for ln in report_txt.splitlines() if ln.strip()]
    for ln in lines[-15:]:
        print(ln)

    totals = parse_totals_from_export(export_path) if export_path.is_file() else {}
    if totals:
        summary = {
            "schema": 1933,
            "issue": 1933,
            "line_pct": totals.get("line_pct", 0.0),
            "region_pct": totals.get("region_pct", 0.0),
            "line_covered": totals.get("line_covered", 0),
            "line_count": totals.get("line_count", 0),
            "binaries": [str(b) for b in binaries],
            "profraw_count": len(profraw),
        }
        (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(f"  line coverage : {summary['line_pct']:.2f}%")

        if args.min_line_pct > 0 and summary["line_pct"] + 1e-9 < args.min_line_pct:
            print(
                f"FAIL: line coverage {summary['line_pct']:.2f}% < min {args.min_line_pct}%",
                file=sys.stderr,
            )
            return 1

    # Optional per-module gates (PATH_SUBSTR:MIN_PCT)
    for spec in args.require_module:
        if ":" not in spec:
            continue
        sub, min_s = spec.rsplit(":", 1)
        try:
            min_pct = float(min_s)
        except ValueError:
            continue
        pct = module_line_pct(export_path, sub) if export_path.is_file() else None
        if pct is None:
            print(f"  module {sub!r}: no data (skip)")
            continue
        print(f"  module {sub!r}: {pct:.2f}% (min {min_pct})")
        if pct + 1e-9 < min_pct:
            print(f"FAIL: module {sub!r} line coverage {pct:.2f}% < {min_pct}", file=sys.stderr)
            return 1

    print(f"OK: coverage artifacts under {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
