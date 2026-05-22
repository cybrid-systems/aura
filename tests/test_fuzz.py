#!/usr/bin/env python3
"""LLM-Driven Fuzz: detect compiler crashes from LLM-generated code.

Usage:
  # Run fuzz (needs LLM_API_KEY)
  LLM_API_KEY="***" LLM_MODEL="deepseek-v4-flash" python3 tests/test_fuzz.py

  # CI mode: skip
  python3 tests/test_fuzz.py
  → "Skipping: LLM_API_KEY not set"
"""
import subprocess, sys, os, datetime
from pathlib import Path

def test_fuzz():
    """Run each benchmark task as isolated Aura subprocess, detect crashes."""
    api_key = os.environ.get("LLM_API_KEY", "")
    if not api_key or len(api_key) < 10:
        print("  Skipping fuzz: LLM_API_KEY not available")
        return True

    print("  Running LLM fuzz tests...", flush=True)

    base = Path(__file__).resolve().parent.parent
    aura_bin = os.environ.get("AURA_BIN", str(base / "build" / "aura"))
    task_dir = base / "tests" / "edsl_tasks"
    repro_dir = base / "tests" / "regression"
    repro_dir.mkdir(exist_ok=True)

    results = {"crashes": [], "timeouts": [], "internal_errors": [],
               "pass": 0, "fail": 0}

    for fpath in sorted(task_dir.glob("*.aura")):
        if fpath.stem == "README":
            continue
        text = fpath.read_text()
        goal = ""
        for line in text.splitlines():
            if line.startswith(";; goal:"):
                goal = line[len(";; goal:"):].strip()
                break
        if not goal:
            continue

        name = fpath.stem
        esc_goal = goal.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

        aura_code = (
            '(require "std/llm" all:)\n'
            '(define __sp__ "You are Aura Lisp. Return ONLY valid Aura code.")\n'
            '(define __gen__ (lambda (g)\n'
            '  (json-get-string (aura-llm-call (json-encode (hash\n'
            '    "model" "deepseek-v4-flash"\n'
            '    "messages" (list\n'
            '      (hash "role" "system" "content" __sp__)\n'
            '      (hash "role" "user" "content" g))\n'
            '    "temperature" 0.3\n'
            '    "max_tokens" 4096))) "content")))\n'
            '(define __fix__ (lambda (code err goal) ""))\n'
            '(display (intend "' + esc_goal + '" __gen__ aura-verify __fix__ 3))\n'
            '(display (coverage-report))\n'
        )

        try:
            r = subprocess.run(
                [aura_bin], input=aura_code, capture_output=True,
                text=True, timeout=60
            )

            if r.returncode < 0:
                sig = -r.returncode
                if sig in (6, 8, 11):
                    sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{sig}")
                    results["crashes"].append((name, sig_name))
                    repro = ";; regression: compiler should not crash on this code\n;; bug: " + sig_name + " in task '" + name + "'\n;; expect: no-crash\n"
                    repro += ";; discovered: " + datetime.datetime.now().isoformat() + "\n" + aura_code
                    (repro_dir / (datetime.date.today().isoformat() + "_" + name + "_" + sig_name + ".aura")).write_text(repro)
                    print("    CRASH " + name + ": " + sig_name, flush=True)
                    continue

            stderr = r.stderr or ""
            if "internal error" in stderr:
                results["internal_errors"].append(name)
                repro = ";; regression: compiler should not error on this code\n;; bug: internal error in '" + name + "'\n;; expect: no-error\n"
                repro += ";; stderr: " + stderr[:200] + "\n" + aura_code
                (repro_dir / (datetime.date.today().isoformat() + "_" + name + "_internal.aura")).write_text(repro)
                print("    INTERNAL " + name, flush=True)
                continue

            if '"ok"' in (r.stdout or ""):
                results["pass"] += 1
            else:
                results["fail"] += 1

            # Coverage is collected from the task's own stdout (appended to aura_code)
            if "#(coverage" in (r.stdout or ""):
                for line in (r.stdout or "").split("\n"):
                    if "#(coverage" in line:
                        print("      " + line.strip(), flush=True)

        except subprocess.TimeoutExpired:
            results["timeouts"].append(name)
            repro = ";; regression: compiler should not timeout on this code\n;; bug: timeout in task '" + name + "'\n;; expect: no-timeout\n" + aura_code
            (repro_dir / (datetime.date.today().isoformat() + "_" + name + "_timeout.aura")).write_text(repro)
            print("    TIMEOUT " + name, flush=True)

        total = sum(len(v) if isinstance(v, list) else 0 for v in results.values()) + results["pass"] + results["fail"]
        if total % 5 == 0:
            print("    " + str(total) + "/47 ... (" + str(results["pass"]) + " pass, " + str(results["fail"]) + " fail, "
                  + str(len(results["crashes"])) + " crash, " + str(len(results["timeouts"])) + " timeout)", flush=True)

    print("  Fuzz: " + str(results["pass"]) + " pass, " + str(results["fail"]) + " fail, "
          + str(len(results["crashes"])) + " crashes, " + str(len(results["timeouts"])) + " timeouts, "
          + str(len(results["internal_errors"])) + " internal", flush=True)

    if results["crashes"] or results["internal_errors"]:
        return False
    return True


if __name__ == "__main__":
    rc = test_fuzz()
    sys.exit(0 if rc else 1)
