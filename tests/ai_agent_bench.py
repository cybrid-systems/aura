#!/usr/bin/env python3
"""Aura AI Agent — 多程序测试基准 (带完整标准库 API)

运行 LLM 迭代式 Agent 解决一系列编程任务，记录成功率。
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ai_agent_prompt import build_system_prompt

AURA = os.environ.get("AURA_BIN", "./build/aura")
OPENAI_KEY = os.environ.get("OPENAI_API_KEY", "")
OPENAI_URL = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
OPENAI_MODEL = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
MAX_ROUNDS = 10
EXEC_TIMEOUT = 15

def llm(msgs):
    p = urllib.parse.urlparse(OPENAI_URL)
    c = http.client.HTTPSConnection(p.netloc, timeout=60) if p.scheme == "https" else http.client.HTTPConnection(p.netloc, timeout=60)
    c.request("POST", p.path + "/chat/completions", json.dumps({
        "model": OPENAI_MODEL, "messages": msgs, "temperature": 0.2, "max_tokens": 2000,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {OPENAI_KEY}"})
    r = c.getresponse()
    d = json.loads(r.read())
    c.close()
    return d["choices"][0]["message"]["content"]

class Session:
    def __init__(self):
        self.p = subprocess.Popen([AURA, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    def exec(self, code, t=EXEC_TIMEOUT):
        self.p.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n"); self.p.stdin.flush()
        d = time.time() + t
        while time.time() < d:
            l = self.p.stdout.readline()
            if l and l.startswith("{"): return json.loads(l)
        return {"status": "timeout"}
    def close(self):
        self.p.terminate(); self.p.wait()

def extract(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL)
    if "```" in text:
        for p in text.split("```"):
            ls = [l for l in p.strip().split("\n") if not l.startswith(("aura","scheme","lisp"))]
            c = "\n".join(ls).strip()
            if any(k in c for k in ("define","require","(+","(begin","lambda","import")): return c
    return ""

SYSTEM_PROMPT = build_system_prompt()

TASKS = [
    {"id": "fib10",         "prompt": "Compute fibonacci(10) efficiently",
     "verify": lambda v: "55" in str(v)},
    {"id": "fib20",         "prompt": "Compute fibonacci(20) efficiently (iterative, not recursive)",
     "verify": lambda v: "6765" in str(v)},
    {"id": "sum_1_to_100",  "prompt": "Sum 1..100 using range and foldl from std/list",
     "verify": lambda v: "5050" in str(v)},
    {"id": "filter_odd",    "prompt": "Filter odd numbers from (range 1 20) and sum them",
     "verify": lambda v: str(v).replace(",","") == "100" or "100" in str(v)},
    {"id": "map_square",    "prompt": "Square each element in (list 1 2 3 4 5) using map",
     "verify": lambda v: "25" in str(v) and "1" in str(v)},
    {"id": "json_roundtrip", "prompt": "Parse '{\"a\":1,\"b\":2}' and re-serialize with std/json",
     "verify": lambda v: "a" in str(v) or "{\"" in str(v)},
    {"id": "factorial",     "prompt": "Compute factorial(10) using std/math",
     "verify": lambda v: "3628800" in str(v)},
    {"id": "quicksort",     "prompt": "Sort (list 3 1 4 1 5 9 2 6) with quicksort",
     "verify": lambda v: "1 2 3 4 5 6 9" in str(v).replace(",","") or "1234569" in str(v).replace(",","")},
    {"id": "string_proc",   "prompt": "Split 'hello,world,aura' by comma, upcase each, join with dash. Use std/string",
     "verify": lambda v: "HELLO" in str(v).upper() and "WORLD" in str(v).upper() and "AURA" in str(v).upper()},
    {"id": "struct_point",  "prompt": "Define point struct with x y, make (10 20), extract x",
     "verify": lambda v: "10" in str(v)},
    {"id": "prime_sieve",   "prompt": "Find primes up to 30 using trial division",
     "verify": lambda v: all(p in str(v) for p in ["2","3","5","7","11","13","17","19","23","29"])},
    {"id": "fib100_foldl",  "prompt": "Compute fibonacci(100) using iterative approach (big number)",
     "verify": lambda v: "354224848179261915075" in str(v) or str(v).startswith("35422")},
]

def solve(task):
    print(f"\n  {task['id']}: ", end="", flush=True)
    s = Session()
    msgs = [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": task["prompt"]}]
    last_val, success = None, False

    for rnd in range(1, MAX_ROUNDS + 1):
        t0 = time.time()
        resp = llm(msgs)
        t_llm = time.time() - t0
        code = extract(resp)

        if not code:
            if "DONE" in resp.strip().split("\n")[-1].upper():
                print(f"r{rnd} done", end=" "); break
            msgs.append({"role": "assistant", "content": resp})
            continue

        # Clean up display calls that would shadow return value
        code = re.sub(r'\(display\s+([^)]+)\)', r'\1', code)
        lines = [l.strip() for l in code.split("\n") if l.strip() and not l.startswith(";")]
        if len(lines) > 1 and not (lines[0].startswith("(begin") and lines[-1] == ")"):
            code = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"

        t0 = time.time()
        r = s.exec(code)
        t_exec = time.time() - t0
        status = r.get("status")
        value = r.get("value", r.get("msg", ""))
        last_val = value

        if status == "ok" and task["verify"](value):
            print(f"✅ r{rnd} ({t_llm:.0f}s+{t_exec:.0f}s)", end=" ")
            success = True; break
        elif status == "ok":
            print(f"r{rnd} val={value[:30]}", end=" ")
            # LLM decides: improve or DONE
        elif status == "timeout":
            print(f"r{rnd} timeout", end=" ")
        else:
            print(f"r{rnd} err={value[:30]}", end=" ")

        fb = f"Result: status={status}"
        fb += f" value={value}" if status == "ok" else f" error={value}"
        fb += f"\nTime: {t_exec:.1f}s"
        fb += "\nFix errors and retry.\n" if status != "ok" else "\nIf correct say DONE, otherwise improve.\n"
        msgs.append({"role": "assistant", "content": resp})
        msgs.append({"role": "user", "content": fb})

    s.close()
    return success, last_val

def main():
    if not OPENAI_KEY:
        print("Need OPENAI_API_KEY"); sys.exit(1)
    print(f"Model: {OPENAI_MODEL}\nSystem prompt: {len(SYSTEM_PROMPT)} chars\nTasks: {len(TASKS)}\n")

    results = []
    for t in TASKS:
        ok, val = solve(t)
        results.append((t["id"], ok))
        print(f"{'✅' if ok else '❌'}")

    print(f"\n{'='*50}")
    passed = sum(1 for _, ok in results if ok)
    print(f"Results: {passed}/{len(results)} passed ({100*passed//len(results)}%)")
    for tid, ok in results:
        print(f"  {'✅' if ok else '❌'} {tid}")
    print('='*50)

if __name__ == "__main__":
    main()
