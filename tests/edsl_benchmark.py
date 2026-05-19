#!/usr/bin/env python3
"""Aura EDSL Benchmark — 多模型 × 多任务，系统化发现问题。

用法:
  # 单模型
  LLM_API_KEY="..." ./tests/edsl_benchmark.py --model deepseek-chat

  # 多模型对比
  LLM_API_KEY="..." ./tests/edsl_benchmark.py --models deepseek-chat,minimax,gpt4
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse

AURA = os.environ.get("AURA_BIN", "./build/aura")

# ── 任务定义 ─────────────────────────────────────────────
# 每个任务: (name, prompt, expected_keywords, stdlib_needed)
TASKS = [
    # L0: 基础算术 (无 stdlib)
    ("arith-basic", "Write (+ 1 2 3 4 5)", ["15", "6"], []),
    ("arith-chain", "Define (square x) (* x x), call (square 5)", ["25"], []),
    
    # L1: lambda + let (无 stdlib)
    ("lambda-simple", "Define (double x) (* x 2), call (double 10)", ["20"], []),
    ("letrec-fact", "Define factorial with letrec, compute (fact 5)", ["120"], []),
    ("named-let", "Use named let to sum 1..10", ["55"], []),
    
    # L2: 列表操作 (require std/list)
    ("list-range", "Use std/list range to get 1..10", ["1", "2", "10"], ["std/list"]),
    ("list-filter", "Use std/list filter to get evens from 1..10", ["2", "4", "6", "8"], ["std/list"]),
    ("list-map", "Use std/list map to double elements", ["2", "4", "6"], ["std/list"]),
    ("list-foldl", "Use std/list foldl to sum a list", ["10", "15", "55"], ["std/list"]),
    ("list-reverse", "Write reverse using foldl from std/list", ["3", "2", "1"], ["std/list"]),
    
    # L3: 高阶函数
    ("prime-test", "Write prime? that checks if n is prime, test with 17", ["#t"], []),
    ("primes-list", "Write (primes n) returning all primes ≤ n using std/list filter", ["2", "3", "5", "7"], ["std/list"]),
    ("unique-hash", "Write (unique lst) removing duplicates using hash", ["1", "2", "3", "4"], []),
    ("merge-sort", "Write merge sort using std/list", ["1", "2", "3", "4", "5"], ["std/list"]),
    
    # L4: 哈希表
    ("hash-basic", "Create hash with (hash \"a\" 1), read with hash-ref", ["1"], []),
    ("hash-stats", "Write (stats nums) returning hash with count/sum/mean/min/max", ["count", "sum", "mean"], []),
    ("word-freq", "Write word frequency counter using hash", ["hello", "world"], ["std/string"]),
    
    # L5: 类型系统
    ("type-check", "Use (check 42 : Int) to verify type", ["42"], []),
    ("type-of", "Use (type-of 42) to get type", ["Int"], []),
    ("occurrence", "Use if string? to narrow type, call string-append", ["hello"], []),
    
    # L6: C FFI
    ("ffi-sqrt", "Use (c-func) to call libm sqrt, compute (sqrt 9.0)", ["3", "3.0"], []),
    ("ffi-strlen", "Use c-func to call strlen from libc", ["5"], []),
    
    # L7: EDSL
    ("edsl-set-code", "Use set-code to create a define", ["define", "lambda"], []),
    ("edsl-query", "Use query:find to locate a function", ["node", "id"], []),
    ("edsl-mutate", "Use mutate:rebind to fix a typo", ["fixed", "done"], []),
    
    # L8: TCP socket
    ("tcp-connect", "Connect to httpbin.org:80, send HTTP GET, receive response", ["200", "OK"], []),
]

# ── LLM 调用 ──────────────────────────────────────────────
def llm_complete(model, base_url, key, messages):
    parsed = urllib.parse.urlparse(base_url)
    conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
    h = conn_cls(parsed.netloc, timeout=120)
    path = parsed.path.rstrip("/") + "/chat/completions"
    h.request("POST", path, json.dumps({
        "model": model, "messages": messages, "temperature": 0.3,
        "max_tokens": 4096,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {key}"})
    r = h.getresponse()
    d = json.loads(r.read())
    h.close()
    return d.get("choices", [{}])[0].get("message", {}).get("content", "")

# ── 代码提取 ──────────────────────────────────────────────
def extract_code(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL)
    text = re.sub(r'<[^>]+>', '', text, flags=re.DOTALL)
    if "```" in text:
        for p in text.split("```"):
            lines = p.strip().split("\n")
            lines = [l for l in lines if not l.startswith(("aura", "scheme", "lisp", "racket", "python", "#lang", "#!"))]
            c = "\n".join(lines).strip()
            if any(k in c for k in ("define", "require", "(+", "(begin", "lambda", "import",
                                     "set-code", "query:", "mutate:", "typecheck", "eval-current",
                                     "c-load", "c-func", "tcp-connect", "http-post")):
                return c
    return text.strip()

# ── 执行测试 ──────────────────────────────────────────────
def test_aura(code, timeout=10):
    try:
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except FileNotFoundError:
        return -2, "", "aura binary not found"

def check_success(out, expected):
    # Normalize: strip whitespace, handle common patterns
    norm_out = out.strip().strip('"').strip("'")
    for kw in expected:
        if kw in norm_out:
            return True
    return False

# ── 获取 api-reference ────────────────────────────────────
def get_api_ref():
    r = subprocess.run([AURA, "--eval"], input="(api-reference)", capture_output=True, text=True, timeout=5)
    return r.stdout.strip() if r.returncode == 0 else ""

# ── 主流程 ────────────────────────────────────────────────
def main():
    models = os.environ.get("LLM_MODEL", "deepseek-chat").split(",")
    base_url = os.environ.get("LLM_BASE_URL", "https://api.deepseek.com/v1")
    api_key = os.environ.get("LLM_API_KEY", "")
    
    if not api_key:
        print("❌ LLM_API_KEY not set")
        sys.exit(1)
    
    # Get current API capabilities
    api_ref = get_api_ref()
    
    results = {}
    print(f"\n{'='*70}")
    print(f"Aura EDSL Benchmark")
    print(f"Models: {', '.join(models)}")
    print(f"Tasks: {len(TASKS)}")
    print(f"{'='*70}\n")
    
    for model in models:
        model = model.strip()
        results[model] = {"pass": 0, "fail": 0, "errors": []}
        print(f"\n── Model: {model} ──")
        
        for name, prompt, expected, stdlib in TASKS:
            # Build system prompt
            sys_prompt = (
                "You are Aura Lisp. Return ONLY valid Aura code. No markdown, no explanation.\n"
                "CRITICAL: Always END your code by CALLING the function with a test case "
                "so the result is visible. For example:\n"
                "  (define (square x) (* x x))\n"
                "  (display (square 5))\n"
                "Never just define a function without calling it.\n"
                "Use (display ...) or (write ...) to show output.\n"
                "Do NOT use (newline) alone.\n"
            )
            if stdlib:
                sys_prompt += f"Available stdlib: {', '.join(stdlib)}. Use (require std/xxx) to load them.\n"
            sys_prompt += f"\nCurrent Aura primitives:\n{api_ref[:2000]}"
            
            # LLM call
            t0 = time.time()
            try:
                resp = llm_complete(model, base_url, api_key, [
                    {"role": "system", "content": sys_prompt},
                    {"role": "user", "content": prompt}
                ])
            except Exception as e:
                print(f"  ⏱️  {name}: LLM error: {e}")
                results[model]["errors"].append((name, str(e)))
                results[model]["fail"] += 1
                continue
            
            llm_time = time.time() - t0
            
            # Extract and test
            code = extract_code(resp)
            if not code:
                print(f"  ⏱️  {name}: no code extracted ({llm_time:.1f}s)")
                results[model]["errors"].append((name, "no code"))
                results[model]["fail"] += 1
                continue
            
            rc, out, err = test_aura(code)
            success = rc == 0 and check_success(out, expected)
            
            status = "✅" if success else "❌"
            print(f"  {status} {name} ({llm_time:.1f}s) {out[:60] if out else err[:60]}")
            
            if success:
                results[model]["pass"] += 1
            else:
                results[model]["errors"].append((name, err or out))
                results[model]["fail"] += 1
    
    # ── Summary ──────────────────────────────────────────
    print(f"\n{'='*70}")
    print("Summary")
    print(f"{'='*70}")
    for model, r in results.items():
        total = r["pass"] + r["fail"]
        rate = r["pass"] / total * 100 if total > 0 else 0
        print(f"\n{model}: {r['pass']}/{total} ({rate:.0f}%)")
        if r["errors"]:
            print("  Failures:")
            for name, err in r["errors"][:10]:
                err_short = err[:80].replace("\n", " ")
                print(f"    {name}: {err_short}")
    
    # ── 生成 stdlib/compiler gap 报告 ─────────────────────
    print(f"\n{'='*70}")
    print("Stdlib / Compiler Gaps")
    print(f"{'='*70}")
    
    # Collect all error patterns
    all_errors = []
    for model, r in results.items():
        for name, err in r["errors"]:
            all_errors.append((name, err))
    
    # Categorize errors
    gaps = {
        "missing-primitive": set(),
        "stdlib-gap": set(),
        "compiler-bug": set(),
        "llm-misunderstanding": set(),
    }
    
    for name, err in all_errors:
        el = err.lower()
        if "unbound variable" in el or "unbound" in el:
            var = err.split("unbound variable")[-1].strip().strip(": ")
            if any(s in var for s in ["std/", "require", "import"]):
                gaps["stdlib-gap"].add(f"{name}: missing require/module '{var}'")
            else:
                gaps["missing-primitive"].add(f"{name}: missing primitive '{var}'")
        elif "parse error" in el:
            gaps["compiler-bug"].add(f"{name}: parse error - {err[:60]}")
        elif "type error" in el:
            gaps["compiler-bug"].add(f"{name}: type error - {err[:60]}")
        else:
            gaps["llm-misunderstanding"].add(f"{name}: {err[:80]}")
    
    for category, items in gaps.items():
        if items:
            print(f"\n{category}:")
            for item in sorted(items)[:5]:
                print(f"  • {item}")
    
    print(f"\n{'='*70}")
    print("Done")
    print(f"{'='*70}")

if __name__ == "__main__":
    main()
