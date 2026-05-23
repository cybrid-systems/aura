#!/usr/bin/env python3
"""Run Aura EDSL benchmark across 4 models and produce comparison table.

Usage: python3 tests/run_bench_all.py [--rounds N] [--max-attempts N]
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

KEYS_DIR = Path.home() / "code" / "keys"
BENCH = Path.home() / "code" / "aura" / "tests" / "edsl_benchmark.py"
RESULTS_DIR = Path.home() / "code" / "aura" / "tests" / "bench_results"

MODELS = [
    {
        "name": "DeepSeek",
        "model": "deepseek-v4-flash",
        "key_file": KEYS_DIR / "deepseek",
        "base_url": "https://api.deepseek.com",
    },
    {
        "name": "MiniMax",
        "model": "minimax-m2.7",
        "key_file": KEYS_DIR / "minimax",
        "base_url": "https://api.minimax.chat",
    },
    {
        "name": "Kimi",
        "model": "kimi-k2.6",
        "key_file": KEYS_DIR / "kimi",
        "base_url": "https://api.moonshot.cn",
    },
    {
        "name": "Grok",
        "model": "grok-4.3",
        "key_file": KEYS_DIR / "grok",
        "base_url": "https://api.x.ai",
    },
]

# Parse --rounds and --max-attempts from args
EXTRA_ARGS = []
for a in sys.argv[1:]:
    EXTRA_ARGS.append(a)


def run_model(cfg):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    name = cfg["name"]
    model = cfg["model"]
    key = cfg["key_file"].read_text().strip()
    base_url = cfg["base_url"]
    result_file = RESULTS_DIR / f"{name.lower()}.json"
    log_file = RESULTS_DIR / f"{name.lower()}.log"

    print(f"\n{'='*60}")
    print(f"  Running: {name} ({model})")
    print(f"  Key:     {key[:12]}...{key[-4:]}")
    print(f"  URL:     {base_url}")
    print(f"{'='*60}")

    env = os.environ.copy()
    env["LLM_MODEL"] = model
    env["LLM_API_KEY"] = key
    env["LLM_BASE_URL"] = base_url + "/v1"

    cmd = [sys.executable, str(BENCH), "--json"] + EXTRA_ARGS

    t0 = time.time()
    with open(log_file, "w") as lf:
        p = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            text=True,
            bufsize=1,
        )
        all_output = []
        for line in p.stdout:
            print(line, end="", flush=True)
            lf.write(line)
            all_output.append(line)
        p.wait()

    elapsed = time.time() - t0

    # Parse the final JSON from output (edsl_benchmark outputs JSON at end with --json)
    combined_text = "".join(all_output)
    results = None
    # Find last JSON block (summary JSON at end)
    for line in reversed(all_output):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                results = json.loads(line)
                break
            except json.JSONDecodeError:
                continue

    if not results:
        results = {"raw_log": str(log_file)}

    results["model"] = model
    results["name"] = name
    results["elapsed_s"] = round(elapsed, 1)

    with open(result_file, "w") as f:
        json.dump(results, f, indent=2)

    passed = results.get("passed", results.get("total_passed", 0))
    total = results.get("total", results.get("total_tasks", 0))
    print(f"\n  {name}: {passed}/{total} passed ({elapsed:.0f}s)")
    return results


def parse_results(data):
    """Extract per-task results from different JSON formats."""
    if "tasks" in data:
        return (
            data["tasks"],
            data.get("total_passed", 0),
            data.get("total_tasks", len(data["tasks"])),
        )
    if "results" in data and isinstance(data["results"], dict):
        return (
            data["results"],
            data.get("passed", 0),
            data.get("total", len(data["results"])),
        )
    # Fallback: try to extract task names from output log
    return {}, data.get("passed", 0), data.get("total", 0)


def main():
    all_results = {}
    for cfg in MODELS:
        try:
            data = run_model(cfg)
            all_results[cfg["name"]] = data
        except Exception as e:
            print(f"  ERROR: {cfg['name']} failed: {e}")
            import traceback

            traceback.print_exc()
            all_results[cfg["name"]] = {
                "name": cfg["name"],
                "model": cfg["model"],
                "passed": 0,
                "total": 0,
                "elapsed_s": 0,
                "error": str(e),
            }

    # Print comparison table
    print("\n\n")
    print("=" * 80)
    print("  Aura EDSL Benchmark — Model Comparison")
    print("=" * 80)

    # Collect all task names
    all_tasks = set()
    for data in all_results.values():
        tasks, _, _ = parse_results(data)
        all_tasks.update(tasks.keys())
    all_tasks = sorted(all_tasks)

    if not all_tasks:
        print("  (No task details available)")
        for cfg in MODELS:
            data = all_results.get(cfg["name"], {})
            print(f"  {cfg['name']}: {data.get('passed','?')}/{data.get('total','?')}")
        return

    # Header
    header = f"{'Task':<30s}"
    for cfg in MODELS:
        header += f"  {cfg['name']:<10s}"
    print(header)
    print("-" * len(header))

    # Per-task rows
    for task in all_tasks:
        row = f"{task:<30s}"
        for cfg in MODELS:
            data = all_results.get(cfg["name"], {})
            tasks, _, _ = parse_results(data)
            res = tasks.get(task, {})
            if isinstance(res, dict):
                ok = res.get("passed", res.get("ok", False))
                row += f"  {'✅' if ok else '❌':<10s}"
            elif isinstance(res, bool):
                row += f"  {'✅' if res else '❌':<10s}"
            else:
                row += f"  {'?':<10s}"
        print(row)

    # Summary
    print("-" * len(header))
    summary = f"{'TOTAL PASSED':<30s}"
    for cfg in MODELS:
        data = all_results.get(cfg["name"], {})
        p = data.get("passed", data.get("total_passed", 0))
        t = data.get("total", data.get("total_tasks", 0))
        summary += f"  {p}/{t:<9s}" if t else f"  {'?':<10s}"
    print(summary)

    times = f"{'TIME (s)':<30s}"
    for cfg in MODELS:
        data = all_results.get(cfg["name"], {})
        t = data.get("elapsed_s", 0)
        times += f"  {t:<10.0f}"
    print(times)

    # Save combined
    combined = RESULTS_DIR / "combined.json"
    with open(combined, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"\nResults saved to {RESULTS_DIR}/")


if __name__ == "__main__":
    main()
