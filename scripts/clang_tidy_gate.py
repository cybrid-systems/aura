#!/usr/bin/env python3
"""Issue #4829: clang-tidy gate for the Clang-parseable, non-module subset.

The project's compile_commands.json is GCC-only (-fmodules-ts,
-fdeps-format, -fcontracts, -fmodule-mapper).  Clang cannot parse module
TUs at all, so this gate:

  1. filters the GCC-only flags out of the db and points every entry at
     clang++ (module TUs are skipped and documented as a follow-up);
  2. runs clang-tidy over the non-module src/{core,parser,serve,reflect,exec,compiler} TUs;
  3. compares diagnostics against scripts/clang_tidy_baseline.json and
     fails when a (file, check) count grows.

The baseline holds the existing debt so the gate blocks new findings
without blocking every PR on the historical 800+ diagnostics.

Usage:
  python3 scripts/clang_tidy_gate.py                 # gate (CI)
  python3 scripts/clang_tidy_gate.py --update-baseline
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
DB = BUILD / "compile_commands.json"
BASELINE = Path(__file__).resolve().parent / "clang_tidy_baseline.json"
DEFAULT_SCOPES = (
    "src/core",
    "src/parser",
    "src/serve",
    "src/reflect",
    "src/exec",
    "src/compiler",
)

DROP_FLAGS = {
    "-fmodules-ts",
    "-fcontracts",
    "-fmodule-only",
    "-Werror",
    "-Wno-maybe-uninitialized",
    "-Wno-error=maybe-uninitialized",
}
DROP_PREFIXES = (
    "-fdeps-format=",
    "-fmodule-mapper=",
    "-fmodule-file=",
)

DIAG_RE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<severity>warning|error): (?P<msg>.*?) \[(?P<check>[^\]]+)\]$"
)


def is_module_source(path: Path) -> bool:
    try:
        text = path.read_text(errors="ignore")
    except OSError:
        return True
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith(("import ", "export import ", "module ", "export module ")):
            return True
    return False


def clean_args(command: str, directory: str) -> list[str] | None:
    try:
        args = shlex.split(command)
    except ValueError:
        return None
    if not args:
        return None
    out: list[str] = ["clang++"]
    for arg in args[1:]:
        if arg in DROP_FLAGS or arg.startswith(DROP_PREFIXES):
            continue
        out.append(arg)
    return out


def select_entries(scopes: tuple[str, ...]) -> list[dict]:
    if not DB.is_file():
        sys.exit(f"error: {DB.relative_to(ROOT)} not found; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first")
    raw = json.loads(DB.read_text())
    seen: set[Path] = set()
    selected: list[dict] = []
    skipped_module = 0
    for entry in raw:
        source = Path(entry.get("file", ""))
        try:
            source = source.resolve()
            rel = source.relative_to(ROOT)
        except (OSError, ValueError):
            continue
        if source in seen or not source.is_file():
            continue
        if not any(str(rel).startswith(scope.rstrip("/") + "/") for scope in scopes):
            continue
        if source.suffix not in (".cpp", ".cc", ".cxx"):
            continue
        if is_module_source(source):
            skipped_module += 1
            continue
        args = clean_args(entry.get("command", ""), entry.get("directory", ""))
        if not args:
            continue
        seen.add(source)
        selected.append(
            {
                "directory": entry.get("directory", str(BUILD)),
                "file": str(source),
                "arguments": args,
            }
        )
    print(
        f"clang-tidy gate: {len(selected)} parseable non-module TU(s), "
        f"{skipped_module} module TU(s) skipped (follow-up: Clang module build)"
    )
    return selected


def run_one(entry: dict, db_path: Path, timeout: int) -> tuple[str, str, int, str]:
    source = Path(entry["file"])
    try:
        rel = str(source.relative_to(ROOT))
    except ValueError:
        rel = str(source)
    cmd = ["clang-tidy", "-p", str(db_path), "--quiet", str(source)]
    try:
        proc = subprocess.run(
            cmd,
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return rel, "", 124, "clang-tidy timed out"
    except FileNotFoundError:
        sys.exit("error: clang-tidy not found on PATH")
    return rel, (proc.stdout or "") + (proc.stderr or ""), proc.returncode, ""


def parse_diagnostics(rel: str, output: str, rc: int) -> tuple[dict[tuple[str, str], int], list[tuple[str, str, str]]]:
    counts: dict[tuple[str, str], int] = {}
    samples: list[tuple[str, str, str]] = []
    saw_diag = False
    for line in output.splitlines():
        match = DIAG_RE.match(line.strip())
        if not match:
            continue
        saw_diag = True
        path = match.group("path")
        try:
            resolved = Path(path).resolve()
            if resolved.is_file():
                try:
                    path = str(resolved.relative_to(ROOT))
                except ValueError:
                    path = str(resolved)
        except OSError:
            pass
        check = match.group("check")
        counts[(path, check)] = counts.get((path, check), 0) + 1
        if len(samples) < 200:
            samples.append(
                (
                    f"{path}:{match.group('line')}:{match.group('col')}",
                    match.group("msg"),
                    check,
                )
            )
    if rc != 0 and not saw_diag:
        # A TU that Clang cannot parse is existing debt too; track it so a
        # new parse failure is visible instead of silently skipped.
        counts[(rel, "clang-tidy-parse-error")] = 1
        samples.append((rel, f"clang-tidy exited {rc}", "clang-tidy-parse-error"))
    return counts, samples


def load_baseline() -> dict[str, int]:
    if not BASELINE.is_file():
        return {}
    data = json.loads(BASELINE.read_text())
    return data.get("counts", {})


def key(path: str, check: str) -> str:
    return f"{path}|{check}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="rewrite scripts/clang_tidy_baseline.json from this run",
    )
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--scopes",
        default=",".join(DEFAULT_SCOPES),
        help="comma-separated source dir prefixes",
    )
    args = parser.parse_args()

    scopes = tuple(s.strip() for s in args.scopes.split(",") if s.strip())
    entries = select_entries(scopes)
    if not entries:
        print("clang-tidy gate: no parseable non-module TUs in scope")
        return 0

    with tempfile.TemporaryDirectory(prefix="aura-tidy-") as tmp:
        db_path = Path(tmp) / "compile_commands.json"
        db_path.write_text(json.dumps(entries))
        results = []
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = [pool.submit(run_one, entry, db_path, args.timeout) for entry in entries]
            for future in futures:
                results.append(future.result())

    total: dict[str, int] = {}
    all_samples: list[tuple[str, str, str]] = []
    for rel, output, rc, err in results:
        if err:
            print(f"warning: {rel}: {err}")
        counts, samples = parse_diagnostics(rel, output, rc)
        all_samples.extend(samples)
        for (path, check), count in counts.items():
            total[key(path, check)] = total.get(key(path, check), 0) + count

    if args.update_baseline:
        payload = {
            "generated_with": "clang-tidy 23.1.4",
            "scopes": list(scopes),
            "total": sum(total.values()),
            "counts": dict(sorted(total.items())),
        }
        BASELINE.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"baseline updated: {BASELINE.relative_to(ROOT)} ({payload['total']} diagnostics, {len(total)} keys)")
        return 0

    baseline = load_baseline()
    new_keys: list[tuple[str, int, int]] = []
    for k, count in total.items():
        base = baseline.get(k, 0)
        if count > base:
            new_keys.append((k, base, count))

    if new_keys:
        print(f"clang-tidy gate: FAIL — {len(new_keys)} key(s) exceed the baseline")
        for k, base, count in sorted(new_keys)[:60]:
            print(f"  {k}: {base} -> {count}")
        samples_by_key: dict[str, list[tuple[str, str, str]]] = {k: [] for k, _, _ in new_keys}
        for where, msg, check in all_samples:
            path = where.rsplit(":", 2)[0]
            k = key(path, check)
            if k in samples_by_key and len(samples_by_key[k]) < 3:
                samples_by_key[k].append((where, msg, check))
        for k, _, _ in sorted(new_keys)[:20]:
            for where, msg, check in samples_by_key.get(k, []):
                print(f"    {where}: {msg} [{check}]")
        print(
            "hint: fix the new findings, or run "
            "python3 scripts/clang_tidy_gate.py --update-baseline only when "
            "accepting the debt deliberately"
        )
        return 1

    print(f"clang-tidy gate: OK — {sum(total.values())} diagnostics match the baseline ({len(total)} keys)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
