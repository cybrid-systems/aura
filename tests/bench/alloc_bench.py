#!/usr/bin/env python3
"""Issue #5637: process-level alloc bench (RSS, minor faults, ratios).

One command runs four scenarios and prints JSON:

  S2  load every top-level lib/std module (except INDEX, bus_agent_cli)
  S1  --load of 5000 / 20000 / 50000 defines; slope per define
  S3  binarytrees_cons B=9 wall divided by an in-process arithmetic loop
  S4  N=1/2/4/8 fiber escaped-pair speedup (N * wall(1) / wall(N))

No heaptrack, perf, or /usr/bin/time. Peak RSS and minor faults come from
the child's getrusage (wait4). The same process also prints
(engine:alloc-stats). Wall is only gated as a ratio.

  python3 tests/bench/alloc_bench.py
  python3 tests/bench/alloc_bench.py --check
  python3 tests/bench/alloc_bench.py --repeat 20 --write-baseline

A platform with an empty object in benchmark_baseline.json is record-only.
Deterministic metrics become a gate only when 20 repeats stay within 2%.
Wall ratios become a gate only when 20 repeats stay within 15%.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import resource
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = Path(__file__).resolve().parent
BASELINE_FILE = BENCH / "benchmark_baseline.json"
AURA = os.environ.get("AURA_BIN") or str(ROOT / "build" / "aura")

S1_SIZES = (5000, 20000, 50000)
S4_WIDTHS = (1, 2, 4, 8)
S4_WORK = 100_000
# Named-let is not tail-call optimized (see binarytrees_cons.aura). A
# while loop of this many steps is the in-process arithmetic control.
ARITH_ITERS = 200_000
DET_RANGE = 0.02
RATIO_RANGE = 0.15
MIN_QUALIFY_RUNS = 20

SKIP_STD = frozenset({"INDEX", "bus_agent_cli"})


def platform_tag() -> str:
    import platform as _platform

    return f"{_platform.system().lower()}-{_platform.machine()}"


def git_sha() -> str:
    proc = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.stdout.strip() or "unknown"


def git_log_hint(base_sha: str, head_sha: str) -> str:
    """Actionable range. Empty base still names the command to run."""
    if not base_sha or base_sha == "unknown":
        return f"git log baseline..{head_sha or 'HEAD'}"
    proc = subprocess.run(
        ["git", "log", "--oneline", f"{base_sha}..{head_sha or 'HEAD'}"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    lines = [ln for ln in proc.stdout.splitlines() if ln][:12]
    shown = "; ".join(lines) if lines else "(empty range)"
    return f"git log {base_sha}..{head_sha or 'HEAD'} :: {shown}"


def _raise_stack() -> None:
    try:
        resource.setrlimit(resource.RLIMIT_STACK, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
    except (ValueError, OSError):
        _soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
        resource.setrlimit(resource.RLIMIT_STACK, (hard, hard))


def aura_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    env["AURA_PATH"] = str(ROOT / "lib")
    env.pop("AURA_ALLOC_TRACE", None)
    if extra:
        env.update(extra)
    return env


class RunResult:
    def __init__(
        self,
        *,
        exit_code: int,
        wall_s: float,
        peak_rss_bytes: int,
        minor_faults: int,
        stdout: str,
        stderr: str,
        timed_out: bool,
    ) -> None:
        self.exit_code = exit_code
        self.wall_s = wall_s
        self.peak_rss_bytes = peak_rss_bytes
        self.minor_faults = minor_faults
        self.stdout = stdout
        self.stderr = stderr
        self.timed_out = timed_out


def run_aura(
    args: list[str],
    *,
    timeout: float,
    env_extra: dict[str, str] | None = None,
    raise_stack: bool = False,
) -> RunResult:
    """Run aura. Peak RSS is ru_maxrss (Linux kilobytes) from wait4.

    raise_stack is only for S3. An unlimited soft stack makes glibc commit a
    much larger main stack, which inflates RSS and must not be the S2 setup.
    """
    _out_fd, out_name = tempfile.mkstemp(prefix="alloc-bench-out-")
    _err_fd, err_name = tempfile.mkstemp(prefix="alloc-bench-err-")
    os.close(_out_fd)
    os.close(_err_fd)
    timed_out = False
    peak = 0
    faults = 0
    exit_code = -1
    start = time.perf_counter()
    previous_stack = resource.getrlimit(resource.RLIMIT_STACK)
    restored_stack = False
    try:
        # The child inherits the raised limit. Restore the parent immediately
        # after fork so this process keeps its own stack ceiling.
        if raise_stack:
            _raise_stack()
        with open(out_name, "w", encoding="utf-8") as out, open(err_name, "w", encoding="utf-8") as err:
            proc = subprocess.Popen(
                [AURA, *args],
                stdin=subprocess.DEVNULL,
                stdout=out,
                stderr=err,
                env=aura_env(env_extra),
            )
        resource.setrlimit(resource.RLIMIT_STACK, previous_stack)
        restored_stack = True

        def _kill() -> None:
            nonlocal timed_out
            timed_out = True
            with contextlib.suppress(OSError):
                proc.kill()

        timer = threading.Timer(timeout, _kill)
        timer.start()
        try:
            _pid, status, ru = os.wait4(proc.pid, 0)
        finally:
            timer.cancel()
        proc.returncode = os.waitstatus_to_exitcode(status)
        exit_code = proc.returncode
        # Linux: ru_maxrss is kilobytes for the child high-water mark.
        peak = int(ru.ru_maxrss) * 1024
        faults = int(ru.ru_minflt)
    finally:
        if not restored_stack:
            resource.setrlimit(resource.RLIMIT_STACK, previous_stack)
        wall = time.perf_counter() - start
        stdout = Path(out_name).read_text(encoding="utf-8", errors="replace")
        stderr = Path(err_name).read_text(encoding="utf-8", errors="replace")
        os.unlink(out_name)
        os.unlink(err_name)
    return RunResult(
        exit_code=exit_code,
        wall_s=wall,
        peak_rss_bytes=peak,
        minor_faults=faults,
        stdout=stdout,
        stderr=stderr,
        timed_out=timed_out,
    )


def json_docs(text: str) -> list[dict]:
    found: list[dict] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not (stripped.startswith("{") and stripped.endswith("}")):
            continue
        try:
            doc = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        if isinstance(doc, dict):
            found.append(doc)
    return found


def alloc_calls_of(doc: dict) -> int:
    total = 0
    subs = doc.get("subsystems")
    if not isinstance(subs, dict):
        return 0
    for sub in subs.values():
        if isinstance(sub, dict) and "alloc_calls" in sub:
            total += int(sub["alloc_calls"])
    return total


def flat_nodes_of(doc: dict) -> int:
    subs = doc.get("subsystems")
    if not isinstance(subs, dict):
        return 0
    flat = subs.get("flat_ast_columns")
    if not isinstance(flat, dict):
        return 0
    return int(flat.get("live_objects") or 0)


def _note_failure(run: RunResult, label: str) -> None:
    if run.exit_code == 0 and not run.timed_out:
        return
    tail = "\n".join(run.stderr.splitlines()[-20:])
    print(f"{label} exit={run.exit_code} timeout={run.timed_out}\n{tail}", file=sys.stderr)


def s2_source() -> str:
    lines: list[str] = []
    for path in sorted((ROOT / "lib" / "std").glob("*.aura")):
        if path.stem in SKIP_STD:
            continue
        lines.append(f'(require "std/{path.stem}" all:)')
    lines.append('(display (stats:get "engine:alloc-stats"))')
    lines.append("(newline)")
    return "\n".join(lines) + "\n"


def s1_source(n: int) -> str:
    lines = [
        "(defmacro (twice x) `(+ ,x ,x))",
        "(defmacro (sq x) `(* ,x ,x))",
    ]
    for i in range(n):
        lines.append(
            f"(define (f{i} a b) (let ((t (twice a)) (u (sq b))) (+ t u (twice {i}) (if (< a b) (sq a) (twice b)))))"
        )
    step = max(1, n // 50)
    calls = " ".join(f"(f{i} {i % 7} {i % 11})" for i in range(0, n, step))
    lines.append(f"(define (main) (+ {calls}))")
    lines.append("(display (main))")
    lines.append("(newline)")
    lines.append('(display (stats:get "engine:alloc-stats"))')
    lines.append("(newline)")
    return "\n".join(lines) + "\n"


def s3_source() -> str:
    tree = (BENCH / "binarytrees_cons.aura").read_text(encoding="utf-8")
    return (
        "(define __i 0)\n"
        "(define __acc 0)\n"
        "(define __t0 (monotonic-ms))\n"
        f"(while (< __i {ARITH_ITERS})\n"
        "  (begin\n"
        "    (set! __acc (+ __acc (modulo __i 17)))\n"
        "    (set! __i (+ __i 1))))\n"
        "(define __arith_ms (- (monotonic-ms) __t0))\n"
        "(define __t1 (monotonic-ms))\n"
        f"{tree}\n"
        "(define __tree_ms (- (monotonic-ms) __t1))\n"
        '(display "ALLOC_BENCH_S3 ")\n'
        "(display __arith_ms)\n"
        '(display " ")\n'
        "(display __tree_ms)\n"
        '(display " ")\n'
        "(display __acc)\n"
        "(newline)\n"
    )


def s4_source(n: int) -> str:
    # while, not named-let: the body is 100k cons cells and named-let
    # recurses without tail-call elimination (depth limit 2000).
    return f"""
(define N {n})
(define WORK {S4_WORK})
(define (body)
  (define i 0)
  (define xs '())
  (while (< i WORK)
    (begin
      (set! xs (cons i xs))
      (set! i (+ i 1))))
  (length xs))
(define (spawn-n n)
  (let loop ((i 0) (ids '()))
    (if (>= i n) ids (loop (+ i 1) (cons (fiber:spawn (lambda () (body))) ids)))))
(define (join-all xs acc)
  (if (null? xs) acc (join-all (cdr xs) (+ acc (fiber:join (car xs))))))
(define t0 (monotonic-ms))
(define ids (spawn-n N))
(define total (join-all ids 0))
(define ms (- (monotonic-ms) t0))
(display "ALLOC_BENCH_S4 ")
(display ms)
(display " ")
(display total)
(newline)
(display (stats:get "engine:alloc-stats"))
(newline)
"""


def _write_source(text: str) -> str:
    fd, name = tempfile.mkstemp(suffix=".aura", prefix="alloc-bench-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
    except Exception:
        os.unlink(name)
        raise
    return name


def _stats_fields(run: RunResult) -> dict:
    docs = json_docs(run.stdout)
    doc = docs[-1] if docs else {}
    return {
        "exit": run.exit_code,
        "timed_out": run.timed_out,
        "wall_s": round(run.wall_s, 6),
        "peak_rss_bytes": run.peak_rss_bytes,
        "minor_faults": run.minor_faults,
        "rss_bytes": int(doc.get("rss_bytes") or 0),
        "capacity_bytes": int(doc.get("capacity_bytes") or 0),
        "alloc_calls": alloc_calls_of(doc),
        "flat_nodes": flat_nodes_of(doc),
        "stats": bool(docs),
    }


def run_s2() -> dict:
    print("alloc-bench: S2 load std", file=sys.stderr)
    path = _write_source(s2_source())
    try:
        run = run_aura([path], timeout=180)
    finally:
        os.unlink(path)
    _note_failure(run, "s2")
    return _stats_fields(run)


def run_s1(sizes: tuple[int, ...] = S1_SIZES) -> dict:
    points = []
    for n in sizes:
        print(f"alloc-bench: S1 defines={n}", file=sys.stderr)
        path = _write_source(s1_source(n))
        try:
            run = run_aura(["--load", path], timeout=300)
        finally:
            os.unlink(path)
        _note_failure(run, f"s1 n={n}")
        point = _stats_fields(run)
        point["defines"] = n
        points.append(point)
    return {"exit": 0 if all(p["exit"] == 0 for p in points) else 1, "points": points, **_slopes(points)}


def _slopes(points: list[dict]) -> dict:
    by_n = {int(p["defines"]): p for p in points}
    if 5000 not in by_n or 50000 not in by_n:
        return {
            "rss_bytes_per_define": None,
            "peak_rss_bytes_per_define": None,
            "minor_faults_per_define": None,
            "alloc_calls_per_define": None,
        }
    span = 50000 - 5000
    lo = by_n[5000]
    hi = by_n[50000]

    def slope(key: str) -> float:
        return (int(hi[key]) - int(lo[key])) / span

    return {
        "rss_bytes_per_define": slope("rss_bytes"),
        "peak_rss_bytes_per_define": slope("peak_rss_bytes"),
        "minor_faults_per_define": slope("minor_faults"),
        "alloc_calls_per_define": slope("alloc_calls"),
    }


def _marked_ints(stdout: str, marker: str) -> tuple[int, int, int] | None:
    prefix = marker + " "
    for line in stdout.splitlines():
        if not line.startswith(prefix):
            continue
        bits = line[len(prefix) :].split()
        if len(bits) < 2:
            return None
        try:
            return int(bits[0]), int(bits[1]), int(bits[2]) if len(bits) > 2 else 0
        except ValueError:
            return None
    return None


def run_s3() -> dict:
    print("alloc-bench: S3 binarytrees B=9", file=sys.stderr)
    path = _write_source(s3_source())
    try:
        run = run_aura([path], timeout=180, env_extra={"B": "9"}, raise_stack=True)
    finally:
        os.unlink(path)
    _note_failure(run, "s3")
    marked = _marked_ints(run.stdout, "ALLOC_BENCH_S3")
    arith_ms = marked[0] if marked else 0
    tree_ms = marked[1] if marked else 0
    ratio = (tree_ms / arith_ms) if arith_ms else None
    checks_ok = (
        "stretch tree of depth 10" in run.stdout
        and "long lived tree of depth 9" in run.stdout
        and "check: -1" in run.stdout
    )
    fields = _stats_fields(run)
    fields.update(
        {
            "arith_ms": arith_ms,
            "tree_ms": tree_ms,
            "wall_ratio": None if ratio is None else round(ratio, 6),
            "checks_ok": checks_ok,
        }
    )
    return fields


def run_s4() -> dict:
    walls: dict[str, int] = {}
    totals_ok = True
    worst_exit = 0
    for n in S4_WIDTHS:
        print(f"alloc-bench: S4 fibers={n}", file=sys.stderr)
        path = _write_source(s4_source(n))
        try:
            run = run_aura(
                ["--worker-threads", str(n), "--load", path],
                timeout=180,
            )
        finally:
            os.unlink(path)
        _note_failure(run, f"s4 n={n}")
        worst_exit = worst_exit or run.exit_code
        marked = _marked_ints(run.stdout, "ALLOC_BENCH_S4")
        ms = marked[0] if marked else 0
        total = marked[1] if marked else -1
        walls[str(n)] = ms
        if total != n * S4_WORK:
            totals_ok = False
    wall1 = walls.get("1") or 0
    speedup: dict[str, float] = {}
    for n in S4_WIDTHS:
        wall_n = walls.get(str(n)) or 0
        if wall1 and wall_n:
            speedup[str(n)] = round(n * wall1 / wall_n, 6)
        else:
            speedup[str(n)] = 0.0
    return {
        "exit": worst_exit,
        "work": S4_WORK,
        "walls_ms": walls,
        "speedup": speedup,
        "totals_ok": totals_ok,
    }


def run_once(selected: set[str], *, s1_sizes: tuple[int, ...]) -> dict:
    scenarios: dict[str, dict] = {}
    if "s2" in selected:
        scenarios["s2"] = run_s2()
    if "s1" in selected:
        scenarios["s1"] = run_s1(s1_sizes)
    if "s3" in selected:
        scenarios["s3"] = run_s3()
    if "s4" in selected:
        scenarios["s4"] = run_s4()
    return {
        "issue": 5637,
        "platform": platform_tag(),
        "git_sha": git_sha(),
        "scenarios": scenarios,
    }


def load_alloc_doc() -> dict:
    if not BASELINE_FILE.is_file():
        return {}
    data = json.loads(BASELINE_FILE.read_text(encoding="utf-8"))
    alloc = data.get("alloc")
    return alloc if isinstance(alloc, dict) else {}


def limits_for(platform: str) -> dict:
    plats = load_alloc_doc().get("platforms")
    if not isinstance(plats, dict):
        return {}
    entry = plats.get(platform)
    return entry if isinstance(entry, dict) else {}


def _fail(fails: list[str], scenario: str, metric: str, baseline: str, current: object, git_range: str) -> None:
    fails.append(f"FAIL {scenario} {metric}: baseline {baseline}, current {current}, {git_range}")


def _metric_on(block: dict, key: str) -> bool:
    """Per-metric gate wins. A block-level gate covers every ceiling it stores."""
    flag = block.get(f"{key}_gate")
    if flag is None:
        return bool(block.get("gate"))
    return bool(flag)


def _over_max(block: dict, key: str, current: float | int | None) -> bool:
    cap = block.get(key)
    if cap is None:
        return False
    if current is None:
        return True
    return float(current) > float(cap)


def metric_failures(sample: dict, limits: dict, git_range: str) -> list[str]:
    """Ceiling / floor checks. An empty limits object is record-only."""
    if not limits:
        return []
    fails: list[str] = []
    scenarios = sample.get("scenarios") or {}

    s2 = scenarios.get("s2")
    block = limits.get("s2")
    if isinstance(s2, dict) and isinstance(block, dict):
        for key in ("peak_rss_bytes", "rss_bytes", "minor_faults"):
            if not _metric_on(block, key):
                continue
            cap_key = f"{key}_max"
            if _over_max(block, cap_key, s2.get(key)):
                _fail(fails, "s2", key, f"<= {block.get(cap_key)}", s2.get(key), git_range)

    s1 = scenarios.get("s1")
    block = limits.get("s1")
    if isinstance(s1, dict) and isinstance(block, dict):
        for key in (
            "rss_bytes_per_define",
            "peak_rss_bytes_per_define",
            "minor_faults_per_define",
            "alloc_calls_per_define",
        ):
            if not _metric_on(block, key):
                continue
            cap_key = f"{key}_max"
            if _over_max(block, cap_key, s1.get(key)):
                _fail(fails, "s1", key, f"<= {block.get(cap_key)}", s1.get(key), git_range)

    s3 = scenarios.get("s3")
    block = limits.get("s3")
    if isinstance(s3, dict) and isinstance(block, dict) and _metric_on(block, "wall_ratio"):
        if not s3.get("checks_ok"):
            _fail(fails, "s3", "checks_ok", "true", s3.get("checks_ok"), git_range)
        if _over_max(block, "wall_ratio_max", s3.get("wall_ratio")):
            _fail(
                fails,
                "s3",
                "wall_ratio",
                f"<= {block.get('wall_ratio_max')}",
                s3.get("wall_ratio"),
                git_range,
            )

    s4 = scenarios.get("s4")
    block = limits.get("s4")
    if isinstance(s4, dict) and isinstance(block, dict) and _metric_on(block, "speedup_8"):
        if not s4.get("totals_ok"):
            _fail(fails, "s4", "totals_ok", "true", s4.get("totals_ok"), git_range)
        speedup = (s4.get("speedup") or {}).get("8")
        floor = block.get("speedup_8_min")
        if floor is not None and (speedup is None or float(speedup) < float(floor)):
            _fail(fails, "s4", "speedup_8", f">= {floor}", speedup, git_range)
    return fails


def execution_failures(sample: dict, git_range: str) -> list[str]:
    """A crashed scenario is a failure even when the platform has no baseline."""
    fails: list[str] = []
    for name, scenario in (sample.get("scenarios") or {}).items():
        if not isinstance(scenario, dict):
            continue
        if name == "s1":
            for point in scenario.get("points") or []:
                if point.get("exit") not in (0, None) or point.get("timed_out"):
                    _fail(
                        fails,
                        "s1",
                        f"exit defines={point.get('defines')}",
                        "0",
                        point.get("exit"),
                        git_range,
                    )
            continue
        if scenario.get("exit") not in (0, None) or scenario.get("timed_out"):
            _fail(fails, name, "exit", "0", scenario.get("exit"), git_range)
        if name == "s3" and scenario.get("checks_ok") is False:
            _fail(fails, "s3", "checks_ok", "true", False, git_range)
        if name == "s4" and scenario.get("totals_ok") is False:
            _fail(fails, "s4", "totals_ok", "true", False, git_range)
    return fails


def _span(values: list[float]) -> float:
    if not values:
        return math.inf
    med = statistics.median(values)
    if med == 0:
        return 0.0 if max(values) == min(values) else math.inf
    return (max(values) - min(values)) / abs(med)


def _series(samples: list[dict], scenario: str, key: str) -> list[float]:
    out: list[float] = []
    for sample in samples:
        block = (sample.get("scenarios") or {}).get(scenario) or {}
        value = block.get(key)
        if value is None:
            continue
        out.append(float(value))
    return out


def qualify_limits(samples: list[dict], *, head_sha: str) -> dict:
    """Turn 20 repeats into per-metric gates. Unstable metrics stay recorded."""
    runs = len(samples)
    ranges: dict[str, float] = {}

    def consider(name: str, values: list[float], *, ratio: bool) -> tuple[bool, float | None, float | None]:
        if len(values) < runs or runs < MIN_QUALIFY_RUNS:
            ranges[name] = _span(values) if values else math.inf
            return False, None, None
        span = _span(values)
        ranges[name] = round(span, 6)
        limit = RATIO_RANGE if ratio else DET_RANGE
        slack = RATIO_RANGE if ratio else DET_RANGE
        if span > limit:
            return False, None, None
        ceiling = max(values) * (1.0 + slack)
        floor = min(values) * (1.0 - slack)
        return True, ceiling, floor

    s2_block: dict = {}
    for key in ("peak_rss_bytes", "rss_bytes", "minor_faults"):
        ok, ceiling, _floor = consider(f"s2.{key}", _series(samples, "s2", key), ratio=False)
        s2_block[f"{key}_gate"] = ok
        if ceiling is not None:
            s2_block[f"{key}_max"] = int(math.ceil(ceiling))
        med = _series(samples, "s2", key)
        if med:
            s2_block[f"{key}_median"] = int(statistics.median(med))
    s2_block["gate"] = any(bool(s2_block.get(f"{key}_gate")) for key in ("peak_rss_bytes", "rss_bytes", "minor_faults"))

    s1_block: dict = {}
    s1_keys = (
        "rss_bytes_per_define",
        "peak_rss_bytes_per_define",
        "minor_faults_per_define",
        "alloc_calls_per_define",
    )
    for key in s1_keys:
        ok, ceiling, _floor = consider(f"s1.{key}", _series(samples, "s1", key), ratio=False)
        s1_block[f"{key}_gate"] = ok
        if ceiling is not None:
            s1_block[f"{key}_max"] = ceiling
        med = _series(samples, "s1", key)
        if med:
            s1_block[f"{key}_median"] = statistics.median(med)
    s1_block["gate"] = any(bool(s1_block.get(f"{key}_gate")) for key in s1_keys)

    s3_ok, s3_ceiling, _s3_floor = consider("s3.wall_ratio", _series(samples, "s3", "wall_ratio"), ratio=True)
    s3_vals = _series(samples, "s3", "wall_ratio")
    s3_block: dict = {"gate": s3_ok, "wall_ratio_gate": s3_ok}
    if s3_ceiling is not None:
        s3_block["wall_ratio_max"] = round(s3_ceiling, 6)
    if s3_vals:
        s3_block["wall_ratio_median"] = round(statistics.median(s3_vals), 6)

    speedups = []
    for sample in samples:
        speed = ((sample.get("scenarios") or {}).get("s4") or {}).get("speedup") or {}
        if "8" in speed:
            speedups.append(float(speed["8"]))
    s4_ok, _s4_ceiling, s4_floor = consider("s4.speedup_8", speedups, ratio=True)
    s4_block: dict = {"gate": s4_ok, "speedup_8_gate": s4_ok}
    if s4_floor is not None:
        s4_block["speedup_8_min"] = round(s4_floor, 6)
    if speedups:
        s4_block["speedup_8_median"] = round(statistics.median(speedups), 6)

    return {
        "git_sha": head_sha,
        "runs": runs,
        "ranges": ranges,
        "s2": s2_block,
        "s1": s1_block,
        "s3": s3_block,
        "s4": s4_block,
    }


def _alloc_span(text: str) -> tuple[int, int] | None:
    key = '\n  "alloc": '
    start = text.find(key)
    if start < 0:
        return None
    brace = text.find("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                if end < len(text) and text[end] == ",":
                    end += 1
                return start + 1, end
    raise SystemExit("benchmark_baseline.json: alloc object is not closed")


def write_platform_limits(platform: str, limits: dict) -> None:
    """Insert or replace only the alloc object. The case baselines stay put."""
    text = BASELINE_FILE.read_text(encoding="utf-8")
    doc = json.loads(text)
    alloc = doc.get("alloc")
    if not isinstance(alloc, dict):
        alloc = {
            "schema": 1,
            "issue": 5637,
            "note": "Empty platform object means not measured. Do not fill estimates.",
            "platforms": {},
        }
    alloc["schema"] = 1
    alloc["issue"] = 5637
    alloc.setdefault(
        "note",
        "Empty platform object means not measured. Do not fill estimates.",
    )
    platforms = alloc.get("platforms")
    if not isinstance(platforms, dict):
        platforms = {}
    platforms.setdefault("linux-x86_64", {})
    platforms.setdefault("linux-aarch64", {})
    platforms[platform] = limits
    alloc["platforms"] = platforms
    rendered = json.dumps(alloc, indent=2)
    lines = rendered.splitlines()
    shifted = [lines[0]] + [("  " + line) for line in lines[1:]]
    block = '  "alloc": ' + "\n".join(shifted) + ","
    span = _alloc_span(text)
    if span is None:
        header = '{\n  "schema": 2,\n'
        if not text.startswith(header):
            raise SystemExit("benchmark_baseline.json: unexpected header")
        text = header + block + "\n" + text[len(header) :]
    else:
        text = text[: span[0]] + block + text[span[1] :]
    BASELINE_FILE.write_text(text, encoding="utf-8")


def self_test() -> int:
    git_range = "git log abc..def :: example"
    limits = {
        "git_sha": "abc",
        "s2": {"gate": True, "peak_rss_bytes_max": 100, "rss_bytes_max": 100, "minor_faults_max": 50},
        "s1": {"gate": True, "rss_bytes_per_define_max": 10.0, "minor_faults_per_define_max": 1.0},
        "s3": {"gate": True, "wall_ratio_max": 3.0},
        "s4": {"gate": True, "speedup_8_min": 1.5},
    }
    sample = {
        "scenarios": {
            "s2": {"peak_rss_bytes": 80, "rss_bytes": 80, "minor_faults": 40, "exit": 0},
            "s1": {"rss_bytes_per_define": 4.0, "minor_faults_per_define": 0.2, "exit": 0, "points": []},
            "s3": {"wall_ratio": 2.0, "checks_ok": True, "exit": 0},
            "s4": {"speedup": {"8": 2.0}, "totals_ok": True, "exit": 0},
        }
    }
    ok = metric_failures(sample, limits, git_range)
    if ok:
        print("self-test: in-range sample failed\n" + "\n".join(ok), file=sys.stderr)
        return 1
    blown = json.loads(json.dumps(sample))
    blown["scenarios"]["s2"]["peak_rss_bytes"] = 10_000
    blown["scenarios"]["s2"]["minor_faults"] = 9_000
    fails = metric_failures(blown, limits, git_range)
    text = "\n".join(fails)
    for needle in ("FAIL s2 peak_rss_bytes", "FAIL s2 minor_faults", "baseline", "current", "git log"):
        if needle not in text:
            print(f"self-test: missing {needle!r} in\n{text}", file=sys.stderr)
            return 1
    ratio = json.loads(json.dumps(sample))
    ratio["scenarios"]["s3"]["wall_ratio"] = 9.0
    ratio["scenarios"]["s4"]["speedup"]["8"] = 0.4
    fails = metric_failures(ratio, limits, git_range)
    text = "\n".join(fails)
    if "FAIL s3 wall_ratio" not in text or "FAIL s4 speedup_8" not in text:
        print(f"self-test: ratio gates missing\n{text}", file=sys.stderr)
        return 1
    if metric_failures(blown, {}, git_range):
        print("self-test: empty platform must be record-only", file=sys.stderr)
        return 1
    quiet = json.loads(json.dumps(limits))
    quiet["s2"]["gate"] = False
    if any(item.startswith("FAIL s2") for item in metric_failures(blown, quiet, git_range)):
        print("self-test: gate false must not fire s2", file=sys.stderr)
        return 1
    print("alloc-bench self-test ok", file=sys.stderr)
    return 0


def _parse_sizes(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in text.split(",") if part.strip())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Issue #5637 alloc bench")
    parser.add_argument("--check", action="store_true", help="exit 1 when a gated metric regresses")
    parser.add_argument("--self-test", action="store_true", help="synthetic gate checks, no aura")
    parser.add_argument("--scenarios", default="s2,s1,s3,s4")
    parser.add_argument("--s1-sizes", default="5000,20000,50000")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--write-baseline", action="store_true", help="record this platform from --repeat runs")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.repeat < 1:
        print("--repeat must be >= 1", file=sys.stderr)
        return 2
    if args.write_baseline and args.repeat < MIN_QUALIFY_RUNS:
        print(f"--write-baseline needs --repeat {MIN_QUALIFY_RUNS} or more", file=sys.stderr)
        return 2
    if not Path(AURA).is_file():
        print(f"aura binary missing: {AURA}", file=sys.stderr)
        return 2
    selected = {part.strip() for part in args.scenarios.split(",") if part.strip()}
    sizes = _parse_sizes(args.s1_sizes)
    samples = [run_once(selected, s1_sizes=sizes) for _ in range(args.repeat)]
    head = samples[-1]["git_sha"]
    if args.write_baseline:
        limits = qualify_limits(samples, head_sha=head)
        write_platform_limits(platform_tag(), limits)
        print(f"wrote {platform_tag()} alloc baseline ({args.repeat} runs)", file=sys.stderr)
    report = samples[-1]
    if args.repeat > 1:
        report = {
            "issue": 5637,
            "platform": platform_tag(),
            "git_sha": head,
            "repeat": args.repeat,
            "last": samples[-1],
        }
    json.dump(report, sys.stdout, indent=2)
    sys.stdout.write("\n")
    if not args.check:
        return 0
    limits = limits_for(platform_tag())
    base = str(limits.get("git_sha") or "")
    hint = git_log_hint(base, head)
    fails = execution_failures(samples[-1], hint)
    if not limits:
        print(
            f"alloc-bench: no baseline for {platform_tag()}; record-only",
            file=sys.stderr,
        )
    else:
        fails.extend(metric_failures(samples[-1], limits, hint))
    for line in fails:
        print(line, file=sys.stderr)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
