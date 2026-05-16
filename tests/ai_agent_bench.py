#!/usr/bin/env python3
"""Aura AI Agent — 多程序测试基准 (并行 + 精简 prompt)

并行执行 15 个任务，每个任务独立 LLM + Aura session。
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse, concurrent.futures, threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ai_agent_prompt import build_system_prompt

AURA = os.environ.get("AURA_BIN", "./build/aura")
OPENAI_KEY = os.environ.get("OPENAI_API_KEY", "")
OPENAI_URL = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
OPENAI_MODEL = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
MAX_ROUNDS = 8
LLM_TIMEOUT = 90  # max seconds per LLM call
EXEC_TIMEOUT = 30  # max seconds per Aura exec

SYSTEM_PROMPT = build_system_prompt()
_lock = threading.Lock()

def log(msg):
    with _lock:
        print(msg, flush=True)

def llm_call(msgs):
    """Single LLM call with timeout"""
    p = urllib.parse.urlparse(OPENAI_URL)
    c = http.client.HTTPSConnection(p.netloc, timeout=LLM_TIMEOUT) if p.scheme == "https" else http.client.HTTPConnection(p.netloc, timeout=LLM_TIMEOUT)
    c.request("POST", p.path + "/chat/completions", json.dumps({
        "model": OPENAI_MODEL, "messages": msgs, "temperature": 0.2, "max_tokens": 1000,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {OPENAI_KEY}"})
    r = c.getresponse()
    d = json.loads(r.read())
    c.close()
    return d["choices"][0]["message"]["content"]

def extract_code(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL)
    if "```" in text:
        for p in text.split("```"):
            ls = [l for l in p.strip().split("\n") if not l.startswith(("aura","scheme","lisp"))]
            c = "\n".join(ls).strip()
            if any(k in c for k in ("define","require","(+","(begin","lambda","import")): return c
    return ""

def run_task(tid, prompt, verify, timeout=EXEC_TIMEOUT):
    """Run one task in its own Aura session. Returns (ok, rounds, value)."""
    log(f"  {tid}: starting")
    s = subprocess.Popen([AURA, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    msgs = [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": prompt}]

    for rnd in range(1, MAX_ROUNDS + 1):
        t0 = time.time()
        try:
            resp = llm_call(msgs)
        except Exception as e:
            log(f"  {tid} r{rnd}: LLM error: {e}")
            continue
        t_llm = time.time() - t0
        code = extract_code(resp)

        if not code:
            if "DONE" in resp.strip().split("\n")[-1].upper():
                log(f"  {tid}: DONE no code")
                break
            msgs.append({"role": "assistant", "content": resp})
            continue

        # Clean and wrap
        code = re.sub(r'\(display\s+([^)]+)\)', r'\1', code)
        lines = [l.strip() for l in code.split("\n") if l.strip() and not l.startswith(";")]
        if len(lines) > 1 and not (lines[0].startswith("(begin") and lines[-1] == ")"):
            code = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"

        t0 = time.time()
        s.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n")
        s.stdin.flush()
        deadline = time.time() + timeout
        result = None
        while time.time() < deadline:
            line = s.stdout.readline()
            if line and line.startswith("{"):
                result = json.loads(line)
                break
        t_exec = time.time() - t0

        if result:
            status = result.get("status")
            value = result.get("value", result.get("msg", ""))
            if status == "ok" and verify(value):
                log(f"  {tid}: ✅ r{rnd} ({t_llm:.0f}s+{t_exec:.0f}s) val={value[:40]}")
                s.terminate(); s.wait()
                return (True, rnd, value)
            elif status == "ok":
                log(f"  {tid}: r{rnd} OK val={value[:30]} ({t_llm:.0f}s+{t_exec:.0f}s)")
            elif status == "timeout":
                log(f"  {tid}: r{rnd} timeout")
            else:
                log(f"  {tid}: r{rnd} err={value[:40]}")
            fb = f"Result: status={status} val={value if status=='ok' else error if status!='ok' else value} ({t_exec:.1f}s)\n"
            fb += "Fix errors and retry.\n" if status != "ok" else "If correct say DONE, otherwise improve.\n"
            msgs.append({"role": "assistant", "content": resp})
            msgs.append({"role": "user", "content": fb})
        else:
            log(f"  {tid}: r{rnd} no response (timeout?)")

    s.terminate(); s.wait()
    return (False, MAX_ROUNDS, None)

TASKS = [
    ("fib10",         "Compute fibonacci(10) efficiently", lambda v: "55" in str(v)),
    ("fib20",         "Compute fibonacci(20) efficiently (iterative)", lambda v: "6765" in str(v)),
    ("sum_1_to_100",  "Sum 1..100 using range and foldl from std/list", lambda v: "5050" in str(v)),
    ("filter_odd",    "Filter odds from (range 1 20) and sum them", lambda v: "100" in str(v)),
    ("map_square",    "Square each element in (list 1 2 3 4 5) using map", lambda v: "25" in str(v)),
    ("json_roundtrip","Parse '{\"a\":1,\"b\":2}' and re-serialize with std/json", lambda v: "a" in str(v) or "{" in str(v)),
    ("factorial",     "Compute factorial(10) using std/math", lambda v: "3628800" in str(v)),
    ("quicksort",     "Sort (3 1 4 1 5 9 2 6) with quicksort", lambda v: "9" in str(v) and "1" in str(v)),
    ("string_proc",   "Split 'hello,world,aura' by comma, upcase, join with dash. Use std/string", lambda v: "HELLO" in str(v).upper()),
    ("struct_point",  "Define point struct, make (10 20), extract x", lambda v: "10" in str(v)),
    ("prime_sieve",   "Find primes up to 30 by trial division. Return as list.", lambda v: all(p in str(v) for p in ["2","3","5","7","11","13","17","19","23","29"])),
    ("fib100",        "Compute fibonacci(100) iteratively. Return the number.", lambda v: str(v).startswith("35422")),
    ("tree_depth",    "Function to compute max depth of nested list. Test: (depth (list 1 (list 2 (list 3)))) => 3", lambda v: "3" in str(v)),
    ("csv_parse",     "Parse 'a,b\\nc,d' into list of lists. Return ((\"a\" \"b\") (\"c\" \"d\")).", lambda v: "a" in str(v) and "d" in str(v)),
    ("json_schema",   "Validate a hash has required keys. Return #t if valid, #f if not.", lambda v: "#t" in str(v) or "#f" in str(v)),
]

def main():
    if not OPENAI_KEY:
        print("Need OPENAI_API_KEY"); sys.exit(1)
    print(f"Model: {OPENAI_MODEL}")
    print(f"Prompt: {len(SYSTEM_PROMPT)} chars")
    print(f"Tasks: {len(TASKS)} (parallel: {os.cpu_count()} workers)\n")

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        futures = {ex.submit(run_task, tid, prompt, verify): tid for tid, prompt, verify in TASKS}
        results = {}
        for f in concurrent.futures.as_completed(futures):
            tid = futures[f]
            try:
                ok, rnds, val = f.result()
                results[tid] = (ok, rnds)
                print(f"  {'✅' if ok else '❌'} {tid} ({rnds}r)" + (f" val={val[:30]}" if val else ""))
            except Exception as e:
                results[tid] = (False, 0)
                print(f"  ❌ {tid}: exception: {e}")

    passed = sum(1 for ok, _ in results.values() if ok)
    print(f"\n{'='*50}")
    print(f"Results: {passed}/{len(results)} passed ({100*passed//len(results)}%)")
    for tid in [t[0] for t in TASKS]:
        ok, rnds = results.get(tid, (False, 0))
        print(f"  {'✅' if ok else '❌'} {tid} ({rnds}r)")
    print('='*50)

if __name__ == "__main__":
    main()
