#!/usr/bin/env python3
"""Issue #4566: three-class benchmark harness.

In-process time is `(monotonic-ms)` around the workload. Process startup
is not part of the sample. `monotonic-ms` is the existing steady clock.
A new `bench:` primitive or `(current-time-ns)` would add a public
primitive past the ceiling, and 1 ms is enough for these cases.

    python3 bench/run.py --class compute
    python3 bench/run.py --class agent
    python3 bench/run.py --class workload
    python3 bench/run.py --gate
    python3 bench/run.py --full --runs 3

`--gate` is the CI subset: class 2 at 10 defines (leaf, hot, rollback,
8 fibers) plus a short class-1 and class-3 slice. It fails only when a
class-2 checksum is wrong or `classify_regression` from the #1936 gate
reports a hit. 200 and 2000 defines are `--full` (200 is tens of seconds
per sample on this box; 2000 is much slower).

Class 2 still sets `AURA_SANDBOX=off` on the child. That variable is the
dev escape hatch. New Soft experiments use `aura soft`. The harness does
not set `AURA_SOFT` and does not mint that grant.

LuaJIT is reported as skipped when it is not on PATH. There is no
separate speculative-JIT mode, so that column is absent. The CPython
column is reported and never gates.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE_PATH = ROOT / "bench" / "baselines" / "agent.json"
META_PATH = ROOT / "bench" / "baselines" / "meta.json"
PERFORMANCE_DOC = ROOT / "docs" / "performance.md"
BENCH_RE = re.compile(r"^BENCH\s+(\S+)\s+(\S+)\s+(-?\d+)\s*$", re.M)


def _refs():
    """Load bench/ref_compute.py. Done lazily so this file stays import-clean."""
    sys.path.insert(0, str(ROOT / "bench"))
    import ref_compute

    return ref_compute


def _fmt_result(value: object) -> str:
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def norm_result(text: str) -> str:
    """Drop a trailing `.0` so 9000.0 and 9000 compare equal."""
    match = re.fullmatch(r"(-?\d+)\.0+", text)
    if match:
        return match.group(1)
    return text


def fmt_ms(value: float) -> str:
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    rendered = f"{value:.3f}".rstrip("0").rstrip(".")
    return rendered


def percentile(samples: list[float], pct: float) -> float:
    if not samples:
        return 0.0
    ordered = sorted(samples)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (len(ordered) - 1) * (pct / 100.0)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return float(ordered[int(rank)])
    weight = rank - lo
    return float(ordered[lo]) * (1.0 - weight) + float(ordered[hi]) * weight


def noise_ratio(samples: list[float], median: float) -> float:
    if median <= 0 or len(samples) < 2:
        return 0.0
    return float(statistics.pstdev(samples)) / median


@dataclass(frozen=True)
class Case:
    name: str
    klass: str
    path: str
    expect: str
    short: bool = False
    gated: bool = False
    jit: bool = False
    cpython: str | None = None
    timeout_s: float = 30.0
    tier: str = "ci"  # ci | full
    env: dict[str, str] = field(default_factory=dict)


def leaf_src(n: int) -> str:
    return "\n".join(f"(define (f{i} x) (+ x {i % 5}))" for i in range(n))


def hot_src(n: int) -> str:
    lines = ["(define (shared x) (+ x 1))"]
    lines.extend(f"(define (f{i} x) (shared x))" for i in range(n))
    return "\n".join(lines)


def rollback_src(n: int) -> str:
    lines = ["(define x 0)"]
    lines.extend(f"(define (f{i} y) (+ y {(i % 5) + 1}))" for i in range(n))
    return "\n".join(lines)


def build_cases() -> list[Case]:
    refs = _refs()
    expect = {
        "fib": _fmt_result(refs.fib()),
        "int-loop": _fmt_result(refs.int_loop()),
        "float-loop": _fmt_result(refs.float_loop()),
        "nbody": _fmt_result(refs.nbody()),
        "fannkuch": _fmt_result(refs.fannkuch()),
        "hash-lookup": _fmt_result(refs.hash_lookup()),
        "cons-throughput": _fmt_result(refs.cons_throughput()),
        "string-build": _fmt_result(refs.string_build()),
        "evo-kv": _fmt_result(refs.evo_kv()),
        "multi-agent": _fmt_result(refs.multi_agent()),
        "control-plane": _fmt_result(refs.control_plane()),
        "agent-fibers-8": _fmt_result(refs.fibers()),
    }
    compute_specs = [
        ("fib", "compute/fib.aura", True, 20.0),
        ("int-loop", "compute/int_loop.aura", True, 20.0),
        ("float-loop", "compute/float_loop.aura", False, 30.0),
        ("nbody", "compute/nbody.aura", False, 20.0),
        ("fannkuch", "compute/fannkuch.aura", False, 20.0),
        ("hash-lookup", "compute/hash_lookup.aura", False, 30.0),
        ("cons-throughput", "compute/cons_throughput.aura", False, 30.0),
        ("string-build", "compute/string_build.aura", False, 30.0),
    ]
    cases: list[Case] = []
    for name, rel, short, timeout in compute_specs:
        cases.append(
            Case(
                name=name,
                klass="compute",
                path=f"bench/cases/{rel}",
                expect=expect[name],
                short=short,
                jit=True,
                cpython=name,
                timeout_s=timeout,
            )
        )

    def agent(name: str, rel: str, src: str, *, short: bool, timeout: float, tier: str) -> Case:
        return Case(
            name=name,
            klass="agent",
            path=f"bench/cases/agent/{rel}",
            expect="4" if "leaf" in name else "11" if "hot" in name else "70",
            short=short,
            gated=True,
            timeout_s=timeout,
            tier=tier,
            env={"BENCH_SRC": src, "BENCH_NAME": name},
        )

    cases.append(agent("agent-leaf-10", "leaf.aura", leaf_src(10), short=True, timeout=20, tier="ci"))
    cases.append(agent("agent-hot-10", "hot.aura", hot_src(10), short=True, timeout=20, tier="ci"))
    for _nm, _env, _exp in (
        ("agent-rebind-w1000", {"BENCH_W": "1000", "BENCH_D": "1"}, "2190"),
        ("agent-rebind-d100", {"BENCH_W": "10", "BENCH_D": "100"}, "2190"),
    ):
        # Issue #4598 / #4678: rebind latency at W=1000 fillers / D=100 callers.
        cases.append(
            Case(
                name=_nm,
                klass="agent",
                path="bench/cases/agent/rebind_scale.aura",
                expect=_exp,
                short=False,
                gated=False,
                timeout_s=120,
                tier="full",
                env=_env,
            )
        )
    cases.append(
        Case(
            name="agent-rollback",
            klass="agent",
            path="bench/cases/agent/rollback.aura",
            expect="70",
            short=True,
            gated=True,
            timeout_s=20,
            env={"BENCH_SRC": rollback_src(10), "BENCH_NAME": "agent-rollback"},
        )
    )
    cases.append(
        Case(
            name="agent-fibers-8",
            klass="agent",
            path="bench/cases/agent/fibers.aura",
            expect=expect["agent-fibers-8"],
            short=True,
            gated=True,
            timeout_s=30,
            env={"BENCH_SPIN": "500", "BENCH_NAME": "agent-fibers-8"},
        )
    )
    cases.append(agent("agent-leaf-200", "leaf.aura", leaf_src(200), short=False, timeout=90, tier="full"))
    cases.append(agent("agent-hot-200", "hot.aura", hot_src(200), short=False, timeout=90, tier="full"))
    cases.append(agent("agent-leaf-2000", "leaf.aura", leaf_src(2000), short=False, timeout=2700, tier="full"))
    cases.append(agent("agent-hot-2000", "hot.aura", hot_src(2000), short=False, timeout=2700, tier="full"))

    for name, rel, short, timeout in (
        ("evo-kv", "evo_kv.aura", True, 20.0),
        ("multi-agent", "multi_agent.aura", False, 30.0),
        ("control-plane", "control_plane.aura", False, 20.0),
    ):
        cases.append(
            Case(
                name=name,
                klass="workload",
                path=f"bench/cases/workload/{rel}",
                expect=expect[name],
                short=short,
                cpython=name if name in refs.REFS else None,
                timeout_s=timeout,
            )
        )
    return cases


CASES = build_cases()


def child_env(extra: dict[str, str]) -> dict[str, str]:
    env = os.environ.copy()
    # Dev escape hatch. New Soft experiments use `aura soft`.
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    env.update(extra)
    return env


def git_commit() -> str:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return "unknown"
    if out.returncode != 0:
        return "unknown"
    return out.stdout.strip()


def build_flags(binary: Path) -> dict[str, str]:
    flags = {
        "AURA_SANDBOX": "off",
        "AURA_PIPELINE_STRICT": "0",
        "clock": "monotonic-ms",
        "binary": str(binary),
    }
    cache = ROOT / "build" / "CMakeCache.txt"
    if cache.is_file():
        wanted = {"CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER"}
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if ":" not in line or "=" not in line:
                continue
            key = line.split(":", 1)[0]
            if key in wanted:
                flags[key] = line.split("=", 1)[1].strip()
    return flags


def box_info() -> dict[str, object]:
    return {
        "ncpu": os.cpu_count(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    }


def run_process(cmd: list[str], env: dict[str, str], timeout_s: float) -> tuple[str, str, int, bool]:
    try:
        proc = subprocess.run(
            cmd,
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or ""
        err = exc.stderr or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        if isinstance(err, bytes):
            err = err.decode("utf-8", "replace")
        return out, err, -1, True
    return proc.stdout, proc.stderr, proc.returncode, False


def parse_bench(stdout: str) -> tuple[str, str, int] | None:
    matches = BENCH_RE.findall(stdout)
    if not matches:
        return None
    name, result, elapsed = matches[-1]
    return name, norm_result(result), int(elapsed)


def ms_num(value: float) -> int | float:
    rounded = round(float(value), 3)
    if abs(rounded - round(rounded)) < 1e-9:
        return int(round(rounded))
    return rounded


def summarize(samples: list[float]) -> dict[str, float | int]:
    med = float(statistics.median(samples)) if samples else 0.0
    return {
        "median_ms": ms_num(med),
        "min_ms": ms_num(min(samples) if samples else 0.0),
        "max_ms": ms_num(max(samples) if samples else 0.0),
        "p50_ms": ms_num(percentile(samples, 50)),
        "p95_ms": ms_num(percentile(samples, 95)),
        "p99_ms": ms_num(percentile(samples, 99)),
        "noise": round(noise_ratio(samples, med), 4),
    }


def load_classifier():
    path = ROOT / "tests" / "bench" / "benchmark.py"
    spec = importlib.util.spec_from_file_location("benchmark_mod", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["benchmark_mod"] = mod
    spec.loader.exec_module(mod)
    return mod


def load_meta() -> dict:
    return json.loads(META_PATH.read_text(encoding="utf-8"))


def load_baseline() -> dict:
    if not BASELINE_PATH.is_file():
        return {"schema": 4566, "issue": 4566, "cases": {}}
    return json.loads(BASELINE_PATH.read_text(encoding="utf-8"))


def cpython_column(name: str) -> dict[str, object] | None:
    fn = _refs().REFS.get(name)
    if fn is None:
        return None
    start = time.perf_counter()
    value = fn()
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return {"result": norm_result(_fmt_result(value)), "ms": round(elapsed_ms, 3), "gated": False}


def luajit_column() -> dict[str, str]:
    if shutil.which("luajit") or shutil.which("lua"):
        return {
            "status": "skipped",
            "reason": "no Lua port of these cases; interpreter is on PATH but unused",
        }
    return {"status": "skipped", "reason": "luajit and lua are not on PATH"}


def select_cases(klass: str, full: bool, *, ci_only: bool) -> list[Case]:
    """Pick cases.

    `--class compute|agent|workload` runs that class. Tier `full` (200 and
    2000 defines) is included only with `--full`. `--gate` passes ci_only
    and keeps the short slice: 10-define agent rows, fib, int-loop, evo-kv.
    """
    chosen: list[Case] = []
    for case in CASES:
        if klass != "all" and case.klass != klass:
            continue
        if case.tier == "full" and not full:
            continue
        if ci_only and not case.short:
            continue
        chosen.append(case)
    return chosen


def measure_case(case: Case, binary: Path, runs: int, do_jit: bool) -> dict[str, object]:
    env = child_env(case.env)
    samples: list[float] = []
    result = ""
    ok = True
    error = ""
    timed_out = False
    for _ in range(runs):
        stdout, stderr, rc, timed_out = run_process([str(binary), str(ROOT / case.path)], env, case.timeout_s)
        parsed = parse_bench(stdout)
        if timed_out or rc != 0 or parsed is None:
            ok = False
            error = (stderr or stdout)[-500:]
            break
        _name, result, elapsed = parsed
        if result != case.expect:
            ok = False
            error = f"result {result} != {case.expect}"
            samples.append(float(elapsed))
            break
        samples.append(float(elapsed))
    stats = summarize(samples)
    row: dict[str, object] = {
        "name": case.name,
        "class": case.klass,
        "mode": "interp",
        "result": result,
        "expected": case.expect,
        "ok": ok,
        "samples_ms": samples,
        "gated": case.gated,
        **stats,
    }
    if error:
        row["error"] = error
    if timed_out:
        row["timed_out"] = True
    if case.jit and do_jit and ok:
        source = (ROOT / case.path).read_text(encoding="utf-8")
        stdout, stderr, rc, jit_timeout = run_process([str(binary), "--jit", source], env, min(case.timeout_s, 5.0))
        parsed = parse_bench(stdout)
        jit: dict[str, object] = {"gated": False}
        if jit_timeout:
            jit["ok"] = False
            jit["timed_out"] = True
        elif rc != 0 or parsed is None:
            jit["ok"] = False
            jit["error"] = (stderr or stdout)[-300:]
        else:
            _name, jit_result, jit_ms = parsed
            jit["ok"] = jit_result == case.expect
            jit["result"] = jit_result
            jit["ms"] = jit_ms
            if jit_result != case.expect:
                jit["note"] = "mismatch is #4567; the column is not gated"
        row["jit"] = jit
    if case.cpython:
        column = cpython_column(case.cpython)
        if column is not None:
            row["cpython"] = column
    return row


def regression_hits(rows: list[dict[str, object]], meta: dict) -> list[dict[str, object]]:
    mod = load_classifier()
    baseline = load_baseline()
    base_cases = baseline.get("cases") or {}
    hits: list[dict[str, object]] = []
    for row in rows:
        if not row.get("gated"):
            continue
        name = str(row["name"])
        if not row.get("ok"):
            hits.append({"name": name, "kind": "checksum", "detail": row.get("error", "failed")})
            continue
        previous = base_cases.get(name)
        if not previous:
            hits.append({"name": name, "kind": "missing-baseline"})
            continue
        ratio, min_delta_s, catastrophic = mod.case_thresholds(
            name,
            tolerance_percent=20,
            min_delta_s=0.02,
            catastrophic_ratio=3.0,
            meta=meta,
        )
        base_s = float(previous["median_ms"]) / 1000.0
        cur_s = float(row["median_ms"]) / 1000.0
        hit = mod.classify_regression(
            base_s,
            cur_s,
            ratio_threshold=ratio,
            min_delta_s=min_delta_s,
            catastrophic_ratio=catastrophic,
        )
        if hit is not None:
            hits.append(
                {
                    "name": name,
                    "kind": "regression",
                    "base_ms": previous["median_ms"],
                    "cur_ms": row["median_ms"],
                    "ratio": hit.ratio,
                    "delta_s": hit.delta_s,
                    "catastrophic": hit.catastrophic,
                }
            )
    return hits


def merge_baseline(rows: list[dict[str, object]], commit: str) -> None:
    baseline = load_baseline()
    baseline["schema"] = 4566
    baseline["issue"] = 4566
    baseline["commit"] = commit
    baseline["clock"] = "monotonic-ms"
    cases = baseline.setdefault("cases", {})
    for row in rows:
        if not row.get("gated") or not row.get("ok"):
            continue
        cases[str(row["name"])] = {
            "median_ms": row["median_ms"],
            "min_ms": row["min_ms"],
            "max_ms": row["max_ms"],
            "p50_ms": row["p50_ms"],
            "p95_ms": row["p95_ms"],
            "p99_ms": row["p99_ms"],
            "noise": row["noise"],
            "samples_ms": row["samples_ms"],
            "result": row["result"],
        }
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_PATH.write_text(json.dumps(baseline, indent=2) + "\n", encoding="utf-8")


def median_token(name: str, median_ms: object) -> str:
    return f"{name} median_ms={fmt_ms(float(median_ms))}"


def check_docs() -> list[str]:
    if not PERFORMANCE_DOC.is_file():
        return ["missing docs/performance.md"]
    text = PERFORMANCE_DOC.read_text(encoding="utf-8")
    missing: list[str] = []
    for name, row in (load_baseline().get("cases") or {}).items():
        token = median_token(name, row["median_ms"])
        if token not in text:
            missing.append(token)
    bench_doc = ROOT / "docs" / "benchmark.md"
    if not bench_doc.is_file():
        missing.append("missing docs/benchmark.md")
    else:
        body = bench_doc.read_text(encoding="utf-8")
        for needle in ("1936", "tolerance", "rationale"):
            if needle not in body:
                missing.append(f"docs/benchmark.md missing {needle}")
    return missing


def render_performance(report: dict[str, object]) -> str:
    baseline = load_baseline()
    lines = [
        "# Performance",
        "",
        "Numbers on this page come from `python3 bench/run.py`. The class-2",
        "medians below are the committed baseline (`bench/baselines/agent.json`).",
        "Re-run the harness and it must reproduce these checksums. Times move",
        "with the machine; the statistical gate allows that within the #1936",
        "tolerance.",
        "",
        f"Measured at commit `{report.get('commit')}` on {report.get('box', {}).get('platform')}",
        f"({report.get('box', {}).get('ncpu')} CPUs). Clock is in-process",
        "`(monotonic-ms)`. Startup is excluded. No new primitive.",
        "",
        "Class 2 still runs with `AURA_SANDBOX=off`. That variable is the dev",
        "escape hatch. New Soft experiments use `aura soft`. `AURA_SOFT` is not",
        "set and the harness does not mint the scoped grant.",
        "",
        "LuaJIT column: skipped (interpreter not on PATH in the dev image that",
        "produced this page). Speculative JIT: no separate mode, column absent.",
        "CPython is reported and does not gate. JIT mismatches are #4567 and",
        "do not gate.",
        "",
        "CI (`bench/run.py --gate`, also the pre-push hook) runs the 10-define",
        "agent rows, rollback, 8 fibers, fib, the int loop, and evo-kv.",
        "It fails only on a wrong class-2 checksum or a #1936 regression",
        "against the medians below. 200 and 2000 defines are `--full`.",
        "",
        "## Class 2 baseline",
        "",
        "| case | median ms | p95 ms | min ms | max ms | noise | result |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for name, row in (baseline.get("cases") or {}).items():
        lines.append(
            "| {name} | {med} | {p95} | {mn} | {mx} | {noise} | {result} |".format(
                name=name,
                med=fmt_ms(float(row["median_ms"])),
                p95=fmt_ms(float(row["p95_ms"])),
                mn=fmt_ms(float(row["min_ms"])),
                mx=fmt_ms(float(row["max_ms"])),
                noise=row["noise"],
                result=row["result"],
            )
        )
    lines.extend(["", "## Recorded medians", ""])
    for name, row in (baseline.get("cases") or {}).items():
        lines.append(median_token(name, row["median_ms"]))
    lines.extend(["", "## This run", ""])
    lines.append("| class | case | ok | median ms | result | jit |")
    lines.append("| --- | --- | --- | ---: | --- | --- |")
    for row in report.get("cases") or []:
        jit = row.get("jit") or {}
        jit_cell = ""
        if jit:
            if jit.get("timed_out"):
                jit_cell = "timeout"
            elif jit.get("ok"):
                jit_cell = f"ok {jit.get('ms')}ms"
            elif jit.get("result"):
                jit_cell = f"mismatch {jit.get('result')}"
            else:
                jit_cell = "error"
        med = row.get("median_ms")
        med_cell = fmt_ms(float(med)) if isinstance(med, (int, float)) else ""
        lines.append(
            f"| {row.get('class')} | {row.get('name')} | {row.get('ok')} | {med_cell} | {row.get('result')} | {jit_cell} |"
        )
    lines.append("")
    return "\n".join(lines)


def emit_report(report: dict[str, object], as_json: bool, out_path: Path | None) -> None:
    payload = json.dumps(report, indent=2)
    if out_path is not None:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(payload + "\n", encoding="utf-8")
    if as_json:
        print(payload)
        return
    box = report["box"]
    print(f"commit {report['commit']}  ncpu {box.get('ncpu')}  {box.get('machine')}")
    print(f"subset {report['subset']}  clock monotonic-ms  speculative-jit absent")
    luajit = report["luajit"]
    print(f"luajit {luajit.get('status')}: {luajit.get('reason')}")
    print(f"{'class':<10} {'case':<18} {'ok':<5} {'median':>8} {'min':>8} {'max':>8} {'p95':>8} {'noise':>8} result")
    for row in report["cases"]:
        print(
            f"{row['class']:<10} {row['name']:<18} {str(row['ok']):<5} "
            f"{fmt_ms(float(row['median_ms'])):>8} {fmt_ms(float(row['min_ms'])):>8} "
            f"{fmt_ms(float(row['max_ms'])):>8} {fmt_ms(float(row['p95_ms'])):>8} "
            f"{row['noise']:>8} {row.get('result')}"
        )
        jit = row.get("jit")
        if isinstance(jit, dict):
            print(f"{'':<10} {'jit':<18} {str(jit.get('ok')):<5} result={jit.get('result')} ms={jit.get('ms')}")
        cpy = row.get("cpython")
        if isinstance(cpy, dict):
            print(f"{'':<10} {'cpython':<18} {'':<5} {cpy.get('ms')}ms result={cpy.get('result')} (not gated)")
    for hit in report.get("regressions") or []:
        print(f"HIT {hit}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Aura class benchmark (#4566)")
    parser.add_argument("--class", dest="klass", choices=("compute", "agent", "workload", "all"), default="all")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--gate", action="store_true", help="CI short subset; fail on class-2 regression")
    parser.add_argument("--full", action="store_true", help="include 200 and 2000 define rows and every class-1/3 case")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--binary", default=str(ROOT / "build" / "aura"))
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--write-docs", action="store_true")
    parser.add_argument("--check-docs", action="store_true")
    parser.add_argument("--no-jit", action="store_true")
    parser.add_argument("--case", action="append", default=[], help="run only these case names")
    args = parser.parse_args(argv)

    if args.runs < 1:
        print("runs must be >= 1", file=sys.stderr)
        return 2
    # `--check-docs` alone compares the page to the baseline and does not run cases.
    if (
        args.check_docs
        and not args.gate
        and not args.case
        and not args.full
        and not args.update_baseline
        and not args.write_docs
        and args.klass == "all"
    ):
        missing = check_docs()
        if missing:
            print("docs out of date:", ", ".join(missing), file=sys.stderr)
            return 1
        print("docs match baseline")
        return 0
    binary = Path(args.binary)
    if not binary.is_file():
        print(f"missing aura binary: {binary}", file=sys.stderr)
        return 2

    full = args.full
    klass = args.klass
    if args.gate:
        klass = "all"
        full = False
    if args.case:
        wanted = set(args.case)
        cases = [case for case in CASES if case.name in wanted]
        missing = wanted - {case.name for case in cases}
        if missing:
            print("unknown case: " + ", ".join(sorted(missing)), file=sys.stderr)
            return 2
    else:
        cases = select_cases(klass, full, ci_only=args.gate)
    if not cases and not args.check_docs:
        print("no cases selected", file=sys.stderr)
        return 2

    # Page the binary in. Not a sample.
    run_process([str(binary), "-e", "(+ 1 1)"], child_env({}), 20.0)

    rows = [measure_case(case, binary, args.runs, do_jit=not args.no_jit) for case in cases]
    commit = git_commit()
    report: dict[str, object] = {
        "schema": 4566,
        "issue": 4566,
        "commit": commit,
        "binary": str(binary),
        "flags": build_flags(binary),
        "box": box_info(),
        "subset": "full" if full else "ci" if args.gate else "suite" if klass == "all" else klass,
        "clock": "monotonic-ms",
        "speculative_jit": {"status": "absent", "reason": "no separate speculative JIT mode"},
        "luajit": luajit_column(),
        "cases": rows,
    }
    if args.update_baseline:
        merge_baseline(rows, commit)
    if args.gate or args.check_docs or args.update_baseline:
        meta = load_meta()
        report["regressions"] = regression_hits(rows, meta) if rows else []
    else:
        report["regressions"] = []
    if args.write_docs:
        PERFORMANCE_DOC.write_text(render_performance(report), encoding="utf-8")
    doc_missing = check_docs() if (args.gate or args.check_docs or args.write_docs or args.update_baseline) else []
    report["doc_mismatches"] = doc_missing
    emit_report(report, args.json, args.out)

    if not args.gate:
        bad = [row for row in rows if not row.get("ok")]
        if args.check_docs and doc_missing:
            print("docs out of date:", ", ".join(doc_missing), file=sys.stderr)
            return 1
        return 1 if bad else 0

    # CI: fail only on class-2 checksum / missing baseline / regression.
    # A doc drift fails too, otherwise the page can disagree with the baseline.
    hits = report["regressions"]
    if hits or doc_missing:
        if doc_missing:
            print("docs out of date:", ", ".join(doc_missing), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
