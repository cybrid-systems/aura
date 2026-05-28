#!/usr/bin/env python3
"""
GC Benchmark Profile — 模拟 benchmark 内存压力，采集 gc-stats 曲线

工作原理：
1. 用 Aura REPL 模式，逐个输入表达式并采集 gc-stats
2. 每步只做一件事，避免管道输出混排
3. 输出 CSV 方便绘图

用法: python3 tests/gc_bench_profile.py
"""

import subprocess
import sys
import os
import re

AURA = os.path.join(os.path.dirname(__file__), "..", "build", "aura")

def aura(cmd: str) -> str:
    """Run a single Aura expression, return stdout."""
    r = subprocess.run(
        [AURA],
        input=cmd + "\n",
        capture_output=True,
        text=True,
        timeout=30,
    )
    output = r.stdout.strip()
    if r.returncode != 0:
        stderr = r.stderr.strip()
        if stderr and "error:" in stderr:
            return f"ERROR:{stderr}"
    return output

def aura_stats(cmd: str) -> dict:
    """Run an Aura expression and return gc-stats as dict."""
    # gc-stats returns a string like "string:N/pairs:N/cells:N/..."
    script = f"""
(require "std/json" all:)
{cmd}
(define _s (gc-stats))
(display _s)
"""
    raw = aura(script)
    # Parse "string:N/pairs:N/cells:N/err:N/hash:N/vec:N/opq:N/cls:N/root:N"
    stats = {}
    if raw and not raw.startswith("ERROR"):
        for part in raw.split("/"):
            if ":" in part:
                k, v = part.split(":", 1)
                try:
                    stats[k] = int(v)
                except ValueError:
                    pass
    return stats

def main():
    TASKS = 135
    ROUNDS = 3
    LOG_INTERVAL = 10
    TOTAL = TASKS * ROUNDS

    print("=== GC Benchmark Profile ===")
    print(f"Tasks: {TASKS}  Rounds: {ROUNDS}  Log interval: {LOG_INTERVAL}")
    print()

    # Header
    print("iter,round,task,phase,string,pairs,cells,err,hash,vec,opq,cls,root")

    # Init
    stats = aura_stats("(gc-freeze)")
    if stats:
        print(f"0,0,0,init,{stats.get('string',0)},{stats.get('pairs',0)},"
              f"{stats.get('cells',0)},{stats.get('err',0)},{stats.get('hash',0)},"
              f"{stats.get('vec',0)},{stats.get('opq',0)},{stats.get('cls',0)},"
              f"{stats.get('root',0)}")

    iter_num = 1
    for round_num in range(1, ROUNDS + 1):
        for task_id in range(TASKS):
            # Simulate task: json-parse + set-code + eval-current
            script = f"""
(json-parse "{{\\"name\\":\\"t{task_id}\\",\\"goal\\":\\"write fn\\",\\"expects\\":[\\"ok\\"]}}")
(set-code "(define (f x) (+ x {task_id}))")
(eval-current)
(json-parse "{{\\"status\\":\\"ok\\",\\"task\\":{task_id}}}")
"""
            # Collect pre-gc stats every LOG_INTERVAL
            log_this = (task_id % LOG_INTERVAL == 0)
            if log_this:
                pre = aura_stats(script)
                if pre:
                    print(f"{iter_num},{round_num},{task_id},pre-gc,"
                          f"{pre.get('string',0)},{pre.get('pairs',0)},"
                          f"{pre.get('cells',0)},{pre.get('err',0)},{pre.get('hash',0)},"
                          f"{pre.get('vec',0)},{pre.get('opq',0)},{pre.get('cls',0)},"
                          f"{pre.get('root',0)}", flush=True)
                # gc-heap
                post = aura_stats("(gc-heap)")
                if post:
                    print(f"{iter_num},{round_num},{task_id},post-gc,"
                          f"{post.get('string',0)},{post.get('pairs',0)},"
                          f"{post.get('cells',0)},{post.get('err',0)},{post.get('hash',0)},"
                          f"{post.get('vec',0)},{post.get('opq',0)},{post.get('cls',0)},"
                          f"{post.get('root',0)}", flush=True)
            else:
                # Run task + gc-heap without logging
                aura(f"""
(require "std/json" all:)
(json-parse "{{\\"name\\":\\"t{task_id}\\"}}")
(set-code "(define (f x) (+ x {task_id}))")
(eval-current)
(gc-heap)
""")

            iter_num += 1

        print(f"--- Round {round_num} complete (iter {iter_num-1}) ---", flush=True)

    # Final stats
    final = aura_stats("")
    print(f"\nFinal gc-stats: "
          f"string={final.get('string','?')} pairs={final.get('pairs','?')} "
          f"cells={final.get('cells','?')} hash={final.get('hash','?')} "
          f"cls={final.get('cls','?')} root={final.get('root','?')}")
    if final.get('cls', 0) < 200:
        print("✅ PASS: closures_ stable (< 200)")
    else:
        print(f"⚠️  REVIEW: closures_ = {final.get('cls', 0)} (expected < 200)")

if __name__ == "__main__":
    main()
