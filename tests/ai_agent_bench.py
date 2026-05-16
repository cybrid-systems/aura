#!/usr/bin/env python3
"""Aura AI Agent — 多程序测试基准

运行 LLM 迭代式 Agent 解决一系列编程任务，记录成功率。
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse

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
        "model": OPENAI_MODEL, "messages": msgs, "temperature": 0.3,
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

SYSTEM = """You are an agent for Aura (Scheme-like Lisp). Generate code between ``` markers.

SUPPORTED:
- (define (fn x) ...), (lambda (x) body)
- (if cond t e), (begin expr...)
- (+ 1 2 3), (- x y), (* x y), (/ x y), (modulo n m), (abs n), (gcd n m)
- (list 1 2 3), (cons 1 2), (car x), (cdr x)
- (map fn lst), (filter fn lst), (foldl fn init lst)
- (= x y), (< x y), (> x y), (not x), (and x y), (or x y)
- (null? x), (pair? x), (integer? x), (procedure? x)
- (display x), (write x)
- (require std/list) -> sum, sort, range, foldl, zip, flatten, last, sum, product
- (require std/math) -> factorial, sqrt, pi, odd?, even?, mean
- (require std/json) -> json-stringify, json-parse
- (require std/string) -> string-split, string-join, string-upcase, string-downcase
- (require std/struct) -> define-struct

CRITICAL: The LAST expression in your code determines the returned value.
Do NOT use (display x) for the final value — just return x directly.
Example: (begin (define (fib n) ...) (fib 10)) returns 55, not 1.

UNSUPPORTED (do not use): displayln, println, print, apply, reduce, for, while, begin0

Say DONE when the program is correct and complete."""

# ── 测试用例 ──────────────────────────────────────────────────
TASKS = [
    {
        "id": "fib10",
        "prompt": "Calculate fibonacci(10) efficiently",
        "verify": lambda v: v == "55" or "55" in str(v),
    },
    {
        "id": "fib20",
        "prompt": "Calculate fibonacci(20) efficiently (must handle recursion depth)",
        "verify": lambda v: v == "6765" or "6765" in str(v),
    },
    {
        "id": "sum_1_to_100",
        "prompt": "Sum integers from 1 to 100 using std/list range and foldl",
        "verify": lambda v: v == "5050" or "5050" in str(v),
    },
    {
        "id": "filter_odd",
        "prompt": "Filter odd numbers from (range 1 20) and sum them",
        "verify": lambda v: v == "100" or "100" in str(v),  # 1+3+5+7+9+11+13+15+17+19 = 100
    },
    {
        "id": "map_square",
        "prompt": "Square each element in (list 1 2 3 4 5) using map",
        "verify": lambda v: "(1 4 9 16 25)" in str(v).replace(",", ""),
    },
    {
        "id": "json_roundtrip",
        "prompt": "Parse JSON string '{\"a\":1,\"b\":2}' and re-serialize it with std/json",
        "verify": lambda v: "a" in str(v).replace(",", ""),
    },
    {
        "id": "factorial",
        "prompt": "Compute factorial(10) using std/math",
        "verify": lambda v: v == "3628800" or "3628800" in str(v),
    },
    {
        "id": "quicksort",
        "prompt": "Implement quicksort: (qsort (list 3 1 4 1 5 9 2 6)) that returns sorted list",
        "verify": lambda v: "(1 1 2 3 4 5 6 9)" in str(v).replace(",", ""),
    },
    {
        "id": "zipper",
        "prompt": "Zip two lists (list 1 2 3) and (list 'a 'b 'c) into pairs, using std/list",
        "verify": lambda v: "a" in str(v) and "1" in str(v),
    },
    {
        "id": "string_processor",
        "prompt": "Split 'hello,world,aura' by comma, upcase each part, join with dash. Use std/string",
        "verify": lambda v: "HELLO" in str(v).upper() and "WORLD" in str(v).upper(),
    },
    {
        "id": "struct_point",
        "prompt": "Define a point struct with x and y, create (10 20), extract x value",
        "verify": lambda v: "10" in str(v),
    },
    {
        "id": "prime_sieve",
        "prompt": "Find all prime numbers up to 30 using trial division",
        "verify": lambda v: all(p in str(v) for p in ["2","3","5","7","11","13","17","19","23","29"]),
    },
]


def solve_task(task):
    print(f"\n  ┌─ {task['id']} ─────────────────────")
    s = Session()
    msgs = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": task["prompt"]}]
    last_val = None

    for rnd in range(1, MAX_ROUNDS + 1):
        print(f"  │ Round {rnd}: ", end="", flush=True)
        resp = llm(msgs)
        code = extract(resp)

        if not code:
            if "DONE" in resp.strip().split("\n")[-1].upper():
                print("DONE (verify last result)")
                break
            msgs.append({"role": "assistant", "content": resp})
            continue

        # Strip display/write calls that would shadow the return value
        code = re.sub(r'\(display\s+([^)]+)\)', r'\1', code)
        code = re.sub(r'\(write\s+([^)]+)\)', r'\1', code)
        code = re.sub(r'\(newline\)', '', code)

        lines = [l.strip() for l in code.split("\n") if l.strip() and not l.startswith(";")]
        if len(lines) > 1 and not (lines[0].startswith("(begin") and lines[-1] == ")"):
            code = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"

        t0 = time.time()
        r = s.exec(code)
        elapsed = time.time() - t0
        status = r.get("status")
        value = r.get("value", r.get("msg", ""))
        last_val = value

        if status == "ok":
            print(f"OK ({elapsed:.1f}s) val={value[:60]}")
            if task["verify"](value):
                print(f"  │ ✅ CORRECT")
                s.close()
                return True, rnd, value
        elif status == "timeout":
            print(f"TIMEOUT")
        else:
            print(f"ERROR: {value[:60]}")

        fb = f"Result: status={status} time={elapsed:.1f}s\n"
        fb += f"Value: {value}\n" if status == "ok" else f"Error: {value}\n"
        fb += "Fix and retry.\n" if status != "ok" else "Improve or say DONE if correct.\n"
        msgs.append({"role": "assistant", "content": resp})
        msgs.append({"role": "user", "content": fb})

    s.close()
    return False, MAX_ROUNDS, last_val


def main():
    if not OPENAI_KEY:
        print("Need OPENAI_API_KEY"); sys.exit(1)
    print(f"LLM: {OPENAI_MODEL}\nTasks: {len(TASKS)}")

    results = []
    for t in TASKS:
        ok, rnds, val = solve_task(t)
        results.append((t["id"], ok, rnds))
        print(f"  └─ {'✅' if ok else '❌'} {t['id']} ({rnds} rounds)")

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{'='*50}")
    print(f"Results: {passed}/{len(results)} passed")
    for tid, ok, rnds in results:
        print(f"  {'✅' if ok else '❌'} {tid} ({rnds}r)")
    print(f"{'='*50}")

if __name__ == "__main__":
    main()
