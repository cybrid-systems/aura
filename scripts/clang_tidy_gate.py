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
MODULE_BASELINE = Path(__file__).resolve().parent / "clang_tidy_baseline_modules.json"
MODULE_SKIP = Path(__file__).resolve().parent / "clang_tidy_module_skip.txt"
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


def select_entries(
    scopes: tuple[str, ...], db_path: Path, module_mode: bool = False, skip: set[str] | None = None
) -> list[dict]:
    skip = skip or set()
    if not db_path.is_file():
        sys.exit(f"error: {db_path} not found; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first")
    raw = json.loads(db_path.read_text())
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
        wanted = (".ixx", ".cppm") if module_mode else (".cpp", ".cc", ".cxx")
        if source.suffix not in wanted:
            continue
        if str(rel) in skip:
            print(f"clang-tidy gate: skipping {rel} (documented Clang exclusion)")
            continue
        if not module_mode and is_module_source(source):
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
    kind = "module" if module_mode else "non-module"
    print(f"clang-tidy gate: {len(selected)} parseable {kind} TU(s), {skipped_module} module TU(s) skipped")
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


def parse_diagnostics(
    rel: str, output: str, rc: int, seen_diag: set[tuple[str, str, str, str, str]] | None = None
) -> tuple[dict[tuple[str, str], int], list[tuple[str, str, str]]]:
    counts: dict[tuple[str, str], int] = {}
    samples: list[tuple[str, str, str]] = []
    # A shared set dedupes across TUs too: a finding in a header included by
    # N parseable TUs is one finding, not N (a new #include elsewhere must not
    # multiply the header's debt past its baseline).
    if seen_diag is None:
        seen_diag = set()
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
        # Count unique diagnostics only. clang-tidy can emit the same
        # (file, line, col, check, message) more than once for a TU (header
        # context / duplicate emission); counting those as new findings made
        # the CI run fail against a baseline produced by a single emission.
        diag_key = (
            path,
            match.group("line"),
            match.group("col"),
            check,
            match.group("msg"),
        )
        if diag_key in seen_diag:
            continue
        seen_diag.add(diag_key)
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


def load_baseline(path: Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text())
    return data.get("counts", {})


def key(path: str, check: str) -> str:
    return f"{path}|{check}"


def clang_tidy_version() -> str:
    """The tool that produced this run, e.g. `clang-tidy 23.1.5`.

    The baseline is only comparable across the same analyzer version (the
    CI image's clang-tidy reports more readability findings than a local
    23.1.4 run). Record the real version instead of a hard-coded string so
    a toolchain bump shows up in the diff.
    """
    try:
        out = subprocess.run(["clang-tidy", "--version"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return "clang-tidy unknown"
    for line in (out.stdout or out.stderr).splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[0] == "LLVM" and parts[1] == "version":
            return f"clang-tidy {parts[2]}"
    return "clang-tidy unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="rewrite scripts/clang_tidy_baseline.json from this run",
    )
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument(
        "--db",
        default=str(DB),
        help="compile_commands.json to filter (default: build/compile_commands.json)",
    )
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--module",
        action="store_true",
        help="cover module interface TUs from a Clang module build db (#4837)",
    )
    parser.add_argument(
        "--scopes",
        default=",".join(DEFAULT_SCOPES),
        help="comma-separated source dir prefixes",
    )
    args = parser.parse_args()

    scopes = tuple(s.strip() for s in args.scopes.split(",") if s.strip())
    baseline_path = MODULE_BASELINE if args.module else BASELINE
    skip: set[str] = set()
    if MODULE_SKIP.is_file():
        skip = {
            line.strip() for line in MODULE_SKIP.read_text().splitlines() if line.strip() and not line.startswith("#")
        }
    entries = select_entries(scopes, Path(args.db), module_mode=args.module, skip=skip)
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
    seen_all: set[tuple[str, str, str, str, str]] = set()
    for rel, output, rc, err in results:
        if err:
            print(f"warning: {rel}: {err}")
        counts, samples = parse_diagnostics(rel, output, rc, seen_all)
        all_samples.extend(samples)
        for (path, check), count in counts.items():
            total[key(path, check)] = total.get(key(path, check), 0) + count

    if args.module:
        # Issue #5269: misc-include-cleaner does not model C++ module imports.
        # In a module TU it reports every std/project declaration that arrives
        # via `import std;` / `import x;` as "no header providing ...".
        # Drop that check for files that contain an import; the non-module
        # gate keeps it (there, includes are the only source of declarations).
        _import_cache: dict[str, bool] = {}

        def _has_import(rel: str) -> bool:
            if rel not in _import_cache:
                try:
                    text = (ROOT / rel).read_text(errors="ignore")
                except OSError:
                    text = ""
                _import_cache[rel] = re.search(r"^\s*(?:export\s+)?import\s", text, re.MULTILINE) is not None
            return _import_cache[rel]

        for k in list(total):
            path, _, check = k.partition("|")
            if check == "misc-include-cleaner" and _has_import(path):
                del total[k]
        all_samples = [
            s for s in all_samples if not (s[2] == "misc-include-cleaner" and _has_import(s[0].rsplit(":", 2)[0]))
        ]

    if args.update_baseline:
        payload = {
            "generated_with": clang_tidy_version(),
            "scopes": list(scopes),
            "total": sum(total.values()),
            "counts": dict(sorted(total.items())),
        }
        baseline_path.write_text(json.dumps(payload, indent=2) + "\n")
        print(
            f"baseline updated: {baseline_path.relative_to(ROOT)} ({payload['total']} diagnostics, {len(total)} keys)"
        )
        return 0

    baseline = load_baseline(baseline_path)
    new_keys: list[tuple[str, int, int]] = []
    for k, count in total.items():
        base = baseline.get(k, 0)
        # Existing debt gets a small absolute drift allowance: the CI image's
        # clang-tidy emits two extra bugprone-narrowing diagnostics for one
        # TU depending on encode/version, while a brand-new key still fails on
        # its first finding (base == 0 -> allowed == 0).
        allowed = base + (2 if base > 0 else 0)
        if count > allowed:
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
