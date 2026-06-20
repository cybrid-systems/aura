#!/usr/bin/env python3
"""tests/run_serve.py — Parallel benchmark runner via multi-process serve mode.

Spawns N ./build/aura --serve processes with LLM_API_KEY set,
distributes tasks across them, collects results, and prints a report.

Usage:
  LLM_API_KEY="***" python3 tests/run_serve.py [workers]

Environment:
  LLM_MODEL         Model name (default: deepseek-v4-flash)
  LLM_BASE_URL      API base URL
  BENCH_LIMIT       Max tasks to run (default: all)
  BENCH_OFFSET      Task offset (default: 0)
  BENCH_ROUNDS      Number of rounds (default: 1)
  BENCH_ATTEMPTS    Max LLM attempts per task (default: 3)
"""

import concurrent.futures
import json
import os
import select
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))
WORKERS = int(sys.argv[1]) if len(sys.argv) > 1 else 4
MODEL = os.environ.get("LLM_MODEL", "deepseek-v4-flash")
LIMIT = int(os.environ.get("BENCH_LIMIT", "0"))
OFFSET = int(os.environ.get("BENCH_OFFSET", "0"))
ROUNDS = int(os.environ.get("BENCH_ROUNDS", "1"))
ATTEMPTS = int(os.environ.get("BENCH_ATTEMPTS", "3"))


def q(s):
    """JSON-escape a string for embedding in Aura code."""
    return json.dumps(s)


def sexpr(items):
    """Build (list "a" "b" ...) from Python list of strings."""
    if not items:
        return "'()"
    return "(list " + " ".join(q(x) for x in items) + ")"


class ServeClient:
    def __init__(self, wid):
        self.wid = wid
        # Pass environment so LLM_API_KEY reaches the subprocess
        sub_env = os.environ.copy()
        self.proc = subprocess.Popen(
            [AURA, "--serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            close_fds=True,
            env=sub_env,
        )
        time.sleep(0.5)

    def exec(self, code, timeout=180):
        if self.proc.poll() is not None:
            return False, "", "serve died"
        self.proc.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n")
        self.proc.stdin.flush()
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return False, buf, "process died"
            r, _, _ = select.select([self.proc.stdout], [], [], 0.1)
            if r:
                line = self.proc.stdout.readline()
                if not line:
                    break
                buf += line
                brace = buf.rfind("{")
                if brace >= 0:
                    try:
                        resp = json.loads(buf[brace:])
                        display = buf[:brace].strip()
                        if resp.get("status") == "ok":
                            val = resp.get("value", "")
                            out = (display + " " + val).strip() if val not in ("()", "") else display
                            return True, out, ""
                        return False, display, resp.get("msg", str(resp))
                    except json.JSONDecodeError:
                        pass
            time.sleep(0.05)
        return False, buf, f"timeout ({timeout}s)"

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


def run_worker_tasks(wid, tasks):
    client = ServeClient(wid)
    results = []
    for task in tasks:
        name = task[0]
        goal = task[1]
        expects = list(task[2]) if isinstance(task[2], list) else [str(task[2])]
        depend = task[3] or ""
        hints = list(task[4]) if isinstance(task[4], list) else []

        goal_j = q(goal)
        name_j = q(name)
        depend_j = q(depend) if depend else '""'
        model_j = q(MODEL)
        expects_s = sexpr(expects)
        hints_s = sexpr(hints)

        code = (
            f'(require "std/bench" all:)\n'
            f'(require "std/extract" all:)\n'
            f"(define result\n"
            f"  (run-one (list {name_j} {goal_j} {expects_s} {depend_j} {hints_s})\n"
            f"           {model_j}\n"
            f"           {ATTEMPTS}))\n"
            f"(display (car result))\n"
        )

        ok, out, err = client.exec(code, timeout=300)
        passed = out.startswith("#t") or "(#t" in out
        results.append((name, passed, out[:80]))
    client.close()
    return results


def main():
    # Load tasks
    with open(ROOT / "lib" / "std" / "bench-tasks.json") as f:
        all_tasks = json.load(f)

    start = OFFSET
    end = min(len(all_tasks), start + LIMIT) if LIMIT > 0 else len(all_tasks)
    tasks = all_tasks[start:end]

    print(f"=== Parallel Bench ({WORKERS} workers) ===")
    print(f"  Model: {MODEL}, Tasks: {len(tasks)} ({start}-{end})")
    print(f"  Rounds: {ROUNDS}, Max attempts: {ATTEMPTS}")

    total_p, total_f = 0, 0
    for rnd in range(1, ROUNDS + 1):
        if ROUNDS > 1:
            print(f"\n=== Round {rnd}/{ROUNDS} ===")

        # Partition tasks among workers
        chunk = max(1, len(tasks) // WORKERS)
        parts = [tasks[i : i + chunk] for i in range(0, len(tasks), chunk)]

        all_r = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
            fs = [pool.submit(run_worker_tasks, i, p) for i, p in enumerate(parts) if p]
            for f in concurrent.futures.as_completed(fs):
                all_r.extend(f.result())

        all_r.sort(key=lambda x: x[0])
        rp = sum(1 for _, p, _ in all_r if p)
        rf = sum(1 for _, p, _ in all_r if not p)
        total_p += rp
        total_f += rf

        for name, passed, _ in all_r:
            print(f"  {'OK' if passed else 'FAIL':4s} {name}")
        if rp + rf > 0:
            print(f"  Round {rnd}: {rp}/{rp + rf} ({rp / (rp + rf) * 100:.0f}%)")

    if total_p + total_f > 0:
        print(f"\n=== Total: {total_p}/{total_p + total_f} ({total_p / (total_p + total_f) * 100:.0f}%) ===")
    print("Done")


if __name__ == "__main__":
    main()
