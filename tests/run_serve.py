#!/usr/bin/env python3
"""tests/run_serve.py — Run bench.aura via --serve-async with multi-session parallelism.

Uses session:create for isolated evaluators per task group.
Instead of N separate processes, runs one --serve-async instance with N worker sessions.

Usage:
  LLM_API_KEY="***" python3 tests/run_serve.py [workers]
  LLM_API_KEY="***" LLM_MODEL="grok-4.3" python3 tests/run_serve.py 4
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))
WORKERS = int(sys.argv[1]) if len(sys.argv) > 1 else 4
TOTAL_TASKS = 135
BATCH = (TOTAL_TASKS + WORKERS - 1) // WORKERS
TIMEOUT = 600

# Build the benchmark controller script
# This runs in the "default" session and orchestrates workers
controller = f"""
(require "std/bench" all:)

(define total {TOTAL_TASKS})
(define workers {WORKERS})
(define batch {BATCH})

; Create worker sessions
(let loop ((i 1))
  (when (<= i workers)
    (session:create (string-append "w" (number->string i)))
    (loop (+ i 1))))

; Distribute tasks: send each worker its tasks as JSON
(let loop ((w 1) (offset 0))
  (when (<= w workers)
    (define limit (min batch (- total offset)))
    (define task-data
      (let build ((i offset) (acc (list)))
        (if (< i (+ offset limit))
          (build (+ i 1) (append acc (list (list-ref all-tasks i))))
          acc)))
    (send (string-append "w" (number->string w))
          (json-encode (hash "tasks" task-data "model" (getenv "LLM_MODEL"))))
    (loop (+ w 1) (+ offset batch))))

; Collect results
(define all-results (list))
(let loop ((done 0))
  (if (< done workers)
    (let ((msg (recv 60000)))
      (if msg
        (begin
          (set! all-results (append all-results (list msg)))
          (loop (+ done 1)))
        (begin
          (display "Worker timeout")
          (newline)
          (loop (+ done 1)))))
    ; Print summary
    (begin
      (display "=== Results ===")
      (newline)
      (for-each (lambda (r) (display r)(newline)) all-results))))
"""

def run():
    print(f"=== Serve-async bench.aura ===")
    print(f"Workers: {WORKERS}")
    print(f"Total tasks: {TOTAL_TASKS}")
    print(f"Batch size: {BATCH}")
    print(f"Timeout: {TIMEOUT}s")
    print()

    proc = subprocess.Popen(
        [AURA, "--serve-async"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    # Send controller to default session
    cmd = json.dumps({"cmd": "exec", "code": controller})
    proc.stdin.write(cmd + "\n")
    proc.stdin.flush()

    # Read output until done
    start = time.time()
    while time.time() - start < TIMEOUT:
        line = proc.stdout.readline()
        if not line:
            break
        line = line.strip()
        if line.startswith("{"):
            try:
                msg = json.loads(line)
                if msg.get("status") == "ok":
                    val = msg.get("value", "")
                    if val and val != "()":
                        print(val)
                elif msg.get("status") == "error":
                    print(f"  ERROR: {msg.get('msg', '')}")
            except json.JSONDecodeError:
                pass
        elif line:
            print(line)

        if "=== Results ===" in line:
            # Read remaining output
            for _ in range(WORKERS + 5):
                line = proc.stdout.readline()
                if line:
                    print(line.strip())
            break

    proc.terminate()
    proc.wait()
    elapsed = time.time() - start
    print(f"\nDone in {elapsed:.0f}s")

if __name__ == "__main__":
    run()
