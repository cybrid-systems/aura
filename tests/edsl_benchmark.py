#!/usr/bin/env python3
"""Aura EDSL Benchmark — 多模型 × 多任务 × 多轮 × 迭代修正。

支持：
  - 多轮运行抵消 LLM 方差 (--rounds N)
  - 迭代修正：失败后自动反馈错误给 LLM 重试 (--fix)
  - JSON 输出 (--json)

用法:
  # 单轮单次（原始行为）
  LLM_API_KEY="***" ./tests/edsl_benchmark.py

  # 多轮聚合
  LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 5

  # 多轮 + 迭代修正（失败后自动反馈错误给 LLM 重试，最多 3 次）
  LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 3 --fix

  # 多轮 + 修正 + 最多 5 次尝试
  LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 3 --fix --max-attempts 5 --json
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse
from pathlib import Path
from collections import defaultdict

AURA = os.environ.get("AURA_BIN", "./build/aura")

# ── 任务定义 ─────────────────────────────────────────────
# ── 从 edsl_tasks/ 目录加载任务 ────────────────────────
TASKS_DIR = Path(__file__).resolve().parent / "edsl_tasks"
_TASK_HINTS = {}

def load_tasks():
    """从 edsl_tasks/*.aura 加载任务定义"""
    tasks = []
    if not TASKS_DIR.exists():
        print(f"Warning: {TASKS_DIR} not found")
        return tasks
    for fpath in sorted(TASKS_DIR.glob("*.aura")):
        name = fpath.stem
        if name == "README":
            continue
        text = fpath.read_text()
        goal = ""
        expected = []
        stdlib = []
        hints = []
        for line in text.splitlines():
            if line.startswith(";; goal:"):
                goal = line[len(";; goal:"):].strip()
            elif line.startswith(";; expect:"):
                expected.append(line[len(";; expect:"):].strip())
            elif line.startswith(";; depend:"):
                stdlib.append(line[len(";; depend:"):].strip())
            elif line.startswith(";; hint:"):
                hints.append(line[len(";; hint:"):].strip())
        if goal:
            tasks.append((name, goal, expected, stdlib))
        else:
            print(f"Warning: {fpath} has no ;; goal: metadata")
        if hints:
            _TASK_HINTS[name] = "\\n".join(hints)
    return tasks

TASKS = load_tasks()

# ── CLI 配置默认值 ───────────────────────────────────────
ROUNDS = 1
FIX_MODE = False
INTEND_MODE = False
EVOLVE_MODE = False
TRACE_MODE = False
MAX_ATTEMPTS = 3

# ── LLM 调用 ──────────────────────────────────────────────
def llm_complete(model, base_url, key, messages, retries=3):
    parsed = urllib.parse.urlparse(base_url)
    path = parsed.path.rstrip("/") + "/chat/completions"
    for attempt in range(retries):
        try:
            conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
            h = conn_cls(parsed.netloc, timeout=120)
            h.request("POST", path, json.dumps({
                "model": model, "messages": messages, "temperature": 0.3,
                "max_tokens": 4096,
            }), {"Content-Type": "application/json", "Authorization": f"Bearer {key}"})
            r = h.getresponse()
            d = json.loads(r.read())
            h.close()
            content = d.get("choices", [{}])[0].get("message", {}).get("content", "")
            if content:
                return content
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(2)
            else:
                raise e
    return ""

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

def test_aura_serve(code, timeout=10):
    try:
        r = subprocess.run([AURA, "--serve"], input=code, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except FileNotFoundError:
        return -2, "", "aura binary not found"

def check_success(out, expected):
    norm_out = out.strip().strip('"').strip("'")
    for kw in expected:
        if kw in norm_out:
            return True
    return False

# ── 获取 api-reference ────────────────────────────────────
def get_api_ref():
    r = subprocess.run([AURA, "--eval"], input="(api-reference)", capture_output=True, text=True, timeout=5)
    return r.stdout.strip() if r.returncode == 0 else ""

# ── 构建 system prompt ────────────────────────────────────
def build_sys_prompt(stdlib, api_ref, task_name=""):
    sp = (
        "You are Aura Lisp. THIS IS NOT Common Lisp, Racket, or Scheme.\n"
        "Do NOT use: defun, let*, letrec* (use letrec), dolist, "
        "loop, progn, setf, gethash, incf, decf, push, pop,\n"
        "first, rest, second, third, cadddr, caaaar, caaadr, caadar, "
        "caaddr, cadaar, cadadr, caddar,\n"
        "cdaaar, cdaadr, cdadar, cdaddr, cddaar, cddadr, cdddar, cddddr,\n"
        "format, princ, print, terpri, make-hash-table, mapcar, funcall,\n"
        "Return ONLY valid Aura code. No markdown, no explanation.\n"
        "CRITICAL: Always END your code by CALLING the function with (display ...).\n"
        "  (define (square x) (* x x))\n"
        "  (display (square 5))\n"
        "\n"
        "=== STD LIBRARY ===\n"
        "Load with (require std/name all:).\n"
        "  std/list:  filter, map, foldl, range, sort, take, drop, length, reverse, zip\n"
        "  std/string: string-split, string-trim, string-join\n"
        "  std/hash:   hash-keys, hash-values, hash-has-key?, hash-ref, hash-set!\n"
        "  std/iter:   for-each, for\n"
        "  std/math:   square, sqrt, factorial\n"
        "\n"
        "Examples:\n"
        "  (require std/list all:)\n"
        "  (filter (lambda (x) (> x 5)) (list 1 2 3 4 5 6))\n"
        "  (sort (list 3 1 2) (lambda (a b) (< a b)))\n"
        "  (take 3 (list 1 2 3 4 5))\n"
        "  (require std/string all:)\n"
        "  (string-split \"a,b,c\" \",\")\n"
        "  (require std/hash all:)\n"
        "  (hash-keys h)           ; (\"a\" \"b\" ...)\n"
        "  (hash-values h)         ; (1 2 ...)\n"
        "\n"
        "=== C FFI ===\n"
        "  lib-id -1 = RTLD_DEFAULT. No c-load for libc/libm.\n"
        "  (define sqrt-fn (c-func -1 \"sqrt\" \"(Float) -> Float\"))\n"
        "  (display (sqrt-fn 9.0))  ; 3\n"
        "  (define strlen-fn (c-func -1 \"strlen\" \"(String) -> Int\"))\n"
        "  (display (strlen-fn \"hello\"))  ; 5\n"
        "\n"
        "=== HASH DISPLAY WARNING ===\n"
        "(display <hash>) prints <hash[N]> which does NOT show keys!\n"
        "To show hash contents, use (hash-keys h) or (hash-values h).\n"
        "  (display (hash-keys (hash \"a\" 1 \"b\" 2)))  ; shows (\"a\" \"b\")\n"
        "\n"
        "=== LOOPING ===\n"
        "  (let loop ((i 0) (acc 0))       ; named let\n"
        "    (if (= i 10) acc\n"
        "      (loop (+ i 1) (+ acc i))))\n"
        "  (letrec ((fact (lambda (n) ...))) (fact 5))\n"
        "\n"
        "=== TYPE SYSTEM ===\n"
        "  (check 42 : Int)       ; compile-time check, returns 42\n"
        "  (type-of 42)           ; runtime -> Int\n"
        "  (string? x)            ; predicate for occurrence typing\n"
        "  (if (string? x) (string-append x \"!\") 0)\n"
        "\n"
        "=== TCP ===\n"
        "  (tcp-connect \"host\" port)  ; DO NOT use c-func for networking.\n"
    )
    if stdlib:
        sp += f"Available stdlib: {', '.join(stdlib)}. Use (require std/name all:) to load them.\n"
    if task_name and task_name in _TASK_HINTS:
        sp += "\n" + "=" * 40 + "\n"
        sp += f"TASK-SPECIFIC HINT for \"{task_name}\":\n"
        sp += _TASK_HINTS[task_name]
    evolved = os.environ.get("EVOLVED_HINTS", "")
    if evolved:
        sp += "\n" + "=" * 40 + "\n"
        sp += f"EVOLVED HINTS (from past runs):\n{evolved}\n"
    sp += f"\nCurrent Aura primitives:\n{api_ref[:2000]}"
    return sp

# ── 构建 correction prompt ────────────────────────────────
def build_correction_prompt(code, error, expected):
    """Build a follow-up prompt asking LLM to fix its code."""
    err_preview = error[:200]
    exp_preview = ", ".join(expected)
    return (
        "Your previous code FAILED. Please fix it.\n"
        f"Your previous code:\n{code}\n\n"
        f"Aura produced: {err_preview}\n\n"
        f"Expected output to contain: [{exp_preview}]\n\n"
        "Checklist:\n"
        "- Did you forget (require ...)?\n"
        "- Did you call (display ...)?\n"
        "- Did you use non-existent primitives? Check the banned list above.\n"
        "- (display <hash>) shows <hash[N]>, use (hash-keys h) to see keys.\n"
        "- For TCP: use (tcp-connect \"host\" port), NOT c-func.\n"
        "Output ONLY corrected Aura code, nothing else."
    )

# ── 单任务（支持迭代修正）──────────────────────────────────
def run_single_task(model, base_url, api_key, name, prompt, expected, stdlib, api_ref):
    """Run one task with optional multi-turn correction.

    When FIX_MODE is enabled, keeps feeding errors back to LLM
    up to MAX_ATTEMPTS.

    Returns (success, output, error, total_llm_time, attempts).
    """
    sys_prompt = build_sys_prompt(stdlib, api_ref, task_name=name)
    messages = [
        {"role": "system", "content": sys_prompt},
        {"role": "user", "content": prompt}
    ]

    attempts = 0
    total_llm_time = 0.0
    max_att = MAX_ATTEMPTS if FIX_MODE else 1

    while attempts < max_att:
        attempts += 1
        t0 = time.time()
        try:
            resp = llm_complete(model, base_url, api_key, messages)
        except Exception as e:
            total_llm_time += time.time() - t0
            return False, "", str(e), total_llm_time, attempts
        llm_t = time.time() - t0
        total_llm_time += llm_t

        code = extract_code(resp)
        if not code:
            if attempts >= max_att:
                return False, "", "no code extracted", total_llm_time, attempts
            messages.append({"role": "assistant", "content": resp or ""})
            messages.append({"role": "user", "content":
                "No Aura code found. Output ONLY Aura code, no markdown."})
            continue

        if name.startswith("edsl-"):
            rc, out, err = test_aura_serve(code)
        else:
            rc, out, err = test_aura(code)

        success = rc == 0 and check_success(out, expected)
        if success:
            return True, out, "", total_llm_time, attempts

        if attempts >= max_att:
            return False, out if rc == 0 else "", err or out, total_llm_time, attempts

        msg = err or out
        if not msg:
            msg = f"exit code {rc}, no output"
        messages.append({"role": "assistant", "content": resp})
        messages.append({"role": "user", "content": build_correction_prompt(code, msg, expected)})

    return False, "", "max attempts", total_llm_time, attempts

# ── Adaptive.aura 调用封装 ────────────────────────────────

def _ada_esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\\n', '\\n')

def _ada_list(items):
    if not items:
        return "(list)"
    return "(list " + " ".join('"' + _ada_esc(str(it)) + '"' for it in items) + ")"

def call_adaptive(rc, output, expected_list):
    exp_list = _ada_list(expected_list)
    out_esc = _ada_esc(output)
    code = (
        '(require "std/adaptive" all:)'
        '(define _d (measure-distance ' + str(rc) + ' "' + out_esc + '" ' + exp_list + '))'
        '(display (car _d))(display "||D||")'
        '(display (number->string (car (cdr _d))))(display "||D||")'
        '(display (car (cdr (cdr _d))))(display "||D||")'
        '(display (structured-diagnosis "' + out_esc + '" ' + exp_list + '))'
    )
    try:
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return "fine", 0.0, "(ada-unavail)", ""
        parts = r.stdout.strip().split('||D||')
        phase = parts[0].strip() if len(parts) >= 1 else "fine"
        try:
            ratio = float(parts[1].strip()) if len(parts) >= 2 and parts[1].strip() else 0.0
        except ValueError:
            ratio = 0.0
        diag = parts[2].strip() if len(parts) >= 3 else ""
        diag_text = parts[3].strip() if len(parts) >= 4 else ""
        return phase, ratio, diag, diag_text
    except Exception:
        return "fine", 0.0, "(ada-err)", ""

def call_api_ref(stdlib_list):
    if not stdlib_list:
        return ""
    lst = "(list " + " ".join('"' + m + '"' for m in stdlib_list) + ")"
    code = '(require "std/adaptive" all:)(display (get-api-ref ' + lst + '))'
    try:
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except Exception:
        pass
    return ""

# ── 单任务（通过内置 intend 原语）────────────────────────






def get_execution_trace(code_str, timeout=10):
    """Run the generated code in Aura and capture all output as trace.
    Returns (stdout, stderr) or empty strings on failure.
    """
    if not code_str or code_str == "(not available)":
        return "", ""
    # Escape the code for embedding in set-code
    esc = code_str.replace('\\', '\\\\').replace('"', '\\"')
    aura = '(set-code "' + esc + '")(eval-current)'
    try:
        r = subprocess.run([AURA], input=aura, capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip() if r.returncode == 0 else "", r.stderr.strip()
    except Exception:
        return "", ""


def run_single_task_intend(model, base_url, api_key, name, prompt, expected, stdlib, api_ref):
    max_att = MAX_ATTEMPTS if FIX_MODE else 3
    sys_prompt = build_sys_prompt(stdlib, api_ref, task_name=name)

    def js(s):
        return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')

    sp_esc = js(sys_prompt)
    goal_esc = js(prompt)

    def build_aura_code(sp, temp_val=0.3, tokens_val=4096):
        sp_e = js(sp)
        t_str = str(temp_val)
        tk_str = str(tokens_val)
        lines = ['(require "std/llm" all:)']
        lines.append('(define __sp__ "' + sp_e + '")')
        lines.append('(define __gen__ (lambda (g)')
        lines.append('  (json-get-string (aura-llm-call (json-encode (hash')
        lines.append('    "model" "deepseek-v4-flash"')
        lines.append('    "messages" (list')
        lines.append('      (hash "role" "system" "content" __sp__)')
        lines.append('      (hash "role" "user" "content" g))')
        lines.append('    "temperature" ' + t_str)
        lines.append('    "max_tokens" ' + tk_str + '))) "content")))')
        lines.append('(define __fix__ (lambda (code err goal)')
        lines.append('  (json-get-string (aura-llm-call (json-encode (hash')
        lines.append('    "model" "deepseek-v4-flash"')
        lines.append('    "messages" (list')
        lines.append('      (hash "role" "system" "content" __sp__)')
        lines.append('      (hash "role" "user" "content"')
        lines.append('        (begin')
        lines.append('          (set-code code)')
        lines.append('          (let ((src (current-source)))')
        lines.append('            (string-append')
        lines.append('              "=== Source ===\n" src')
        lines.append('              "\n=== Error ===\n" err')
        lines.append('              "\n=== Goal ===\n" goal)))))')
        lines.append('    "temperature" ' + t_str)
        lines.append('    "max_tokens" ' + tk_str + '))) "content")))')
        lines.append('(display (intend "' + goal_esc + '" __gen__ aura-verify __fix__ ' + str(max_att) + '))')
        # Capture current-source for diagnostics
        lines.append('(display "||CURRENT_SRC_START||")')
        lines.append('(display (current-source))')
        lines.append('(display "||CURRENT_SRC_END||")')
        return '\n'.join(lines)
    def run_intend(code):
        t0 = time.time()
        try:
            r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=60)
            rc, out, err = r.returncode, r.stdout.strip(), r.stderr.strip()
            elapsed = time.time() - t0
        except subprocess.TimeoutExpired:
            return False, '', 'timeout', time.time() - t0, 0
        except FileNotFoundError:
            return False, '', 'aura binary not found', time.time() - t0, 0
        # Parse out current-source from output
        src_start = out.find('||CURRENT_SRC_START||')
        src_end = out.find('||CURRENT_SRC_END||')
        current_src = ''
        if src_start >= 0 and src_end > src_start:
            src_body_start = src_start + len('||CURRENT_SRC_START||')
            current_src = out[src_body_start:src_end].strip()
            out = out[:src_start].strip()
        return rc, out, err, elapsed, current_src

    aura_code = build_aura_code(sys_prompt)
    rc, out, err, elapsed, current_src = run_intend(aura_code)
    if rc != 0 or not out:
        return False, '', err or 'intend failed', elapsed, 0
    m_iter = re.search(r'iterations:(\d+)', out)
    iterations = int(m_iter.group(1)) if m_iter else 0

    # Adaptive retry loop: call_adaptive for phase-based control
    for retry_attempt in range(max_att):
        expected_match = check_success(out, expected)
        if '"ok"' in out and not expected_match:
            actual = out.strip()
            status_pos = actual.rfind('#')
            actual_output = actual[:status_pos].strip() if status_pos > 0 else actual[:200]

            # Call adaptive.aura for phase/diagnosis
            p, ratio, diag, diag_text = call_adaptive(0, actual_output, expected)

            # Find missing keywords for structured feedback
            missing_kws = [kw for kw in expected if kw not in actual_output]
            
            # Build fix instructions
            fix_instructions = []
            if missing_kws:
                fix_instructions.append("- Missing in output: " + ", ".join(missing_kws[:5]))
            if "<hash" in actual_output:
                fix_instructions.append("- display <hash> shows reference, not content. Use hash-keys/hash-values.")
            if actual_output.strip() in ("", "()"):
                fix_instructions.append("- Output is empty. Did you forget (display ...)?")
            fix_instructions.append("- Keep the existing function structure. Only modify display/output code.")

            fb = [
                "=== OUTPUT MISMATCH ===",
                "Phase: " + p + " (ratio: " + str(ratio) + ")",
                "Expected to contain: " + str(expected),
                "Actual output: " + actual_output[:300],
            ]
            if missing_kws:
                fb.append("Missing keywords: " + ", ".join(missing_kws[:5]))
            # Get execution trace for algorithm-debug tasks
            if name in ("primes-list", "quicksort"):
                trace_out, trace_err = get_execution_trace(current_src, timeout=5)
                if trace_out:
                    fb.append("")
                    fb.append("=== Execution Trace ===")
                    fb.append(trace_out[:500])
                if trace_err:
                    fb.append("")
                    fb.append("=== Execution Errors ===")
                    fb.append(trace_err[:200])
            elif name == "tcp-connect":
                trace_out, trace_err = get_execution_trace(current_src, timeout=15)
                if trace_out:
                    fb.append("")
                    fb.append("=== Execution Trace ===")
                    fb.append("HTTP Response (first 500 chars):")
                    fb.append(trace_out[:500])
                if trace_err:
                    fb.append("")
                    fb.append("=== Connection Errors ===")
                    fb.append(trace_err[:200])

            if fix_instructions:
                fb.append("")
                fb.append("### Fix Instructions ###")
                fb.extend(fix_instructions)
            if diag and diag != "":
                fb.append("")
                fb.append("Diagnosis: " + diag)
            if diag_text:
                fb.append(diag_text)
            fb.append("")
            fb.append("=== Current Source (AST) ===")
            fb.append(current_src if current_src else "(not available)")
            fb.append("")
            fb.append("=== Goal ===")
            fb.append(prompt)

            # Inject API ref for fine/putt phases
            ada_api_ref = call_api_ref(stdlib)
            if p in ("fine", "putt") and ada_api_ref:
                fb.append("")
                fb.append(ada_api_ref)

            structured_feedback = "\n".join(fb)
            updated_sp = sys_prompt + "\n\n" + structured_feedback

            # Adjust params by phase
            if p == "coarse":
                temp_v, tokens_v = 0.3, 4096
            elif p == "fine":
                temp_v, tokens_v = 0.2, 2048
            else:
                temp_v, tokens_v = 0.1, 1024

            retry_code = build_aura_code(updated_sp, temp_v, tokens_v)
            rc2, out2, err2, elapsed2, current_src2 = run_intend(retry_code)
            elapsed += elapsed2
            if current_src2:
                current_src = current_src2
            if rc2 != 0 or not out2:
                iterations += 1
                break
            m_iter2 = re.search(r'iterations:(\d+)', out2)
            iterations += int(m_iter2.group(1)) if m_iter2 else 0
            out = out2.strip()
            err = err2 or err
        elif '"ok"' in out and expected_match:
            return True, out, '', elapsed, iterations
        else:
            break

    success = '"ok"' in out and check_success(out, expected)
    if not success and TRACE_MODE:
        err_type = "output-mismatch" if '"ok"' in out else "compile-fail"
        ph = p if 'p' in dir() else "?"
        print(f"    TRACE {name}: {err_type}, {iterations} attempts, {elapsed:.1f}s, phase:{ph} last-error: {(err or 'unknown')[:60]}")
    return success, out, err, elapsed, iterations


# ── 打印结果表 ────────────────────────────────────────────
def print_task_table(task_results):
    print(f"  {'Task':22s} {'Pass/Total':>10s} {'Rate':>6s}  {'Verdict':>12s}")
    print(f"  {'─'*22} {'─'*10} {'─'*6}  {'─'*12}")
    stable_pass = stable_fail = volatile = 0
    for name, passes, total in sorted(task_results):
        rate = passes / total * 100 if total > 0 else 0
        if rate == 100:
            verdict = "✅  Stable PASS"
            stable_pass += 1
        elif rate == 0:
            verdict = "❌  Stable FAIL"
            stable_fail += 1
        elif rate >= 50:
            verdict = "🔄  Volatile (pass)"
            volatile += 1
        else:
            verdict = "🔄  Volatile (fail)"
            volatile += 1
        print(f"  {name:22s} {passes:3d}/{total:<4d}  {rate:5.0f}%  {verdict}")
    print(f"\n  ➤ Stable: {stable_pass}✅ / {stable_fail}❌ + Volatile: {volatile}🔄 = {stable_pass+stable_fail+volatile}")
    return stable_pass, stable_fail, volatile

# ── 主流程 ────────────────────────────────────────────────
def main():
    global ROUNDS, FIX_MODE, INTEND_MODE, EVOLVE_MODE, TRACE_MODE, MAX_ATTEMPTS

    models = os.environ.get("LLM_MODEL", "deepseek-v4-flash").split(",")
    base_url = os.environ.get("LLM_BASE_URL", "https://api.deepseek.com/v1")
    api_key = os.environ.get("LLM_API_KEY", "")
    output_json = False

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ("-r", "--rounds"):
            i += 1
            if i < len(args):
                try:
                    ROUNDS = int(args[i])
                except ValueError:
                    pass
        elif args[i] == "--json":
            output_json = True
        elif args[i] == "--tasks":
            i += 1
            if i < len(args):
                os.environ["BENCH_TASK_FILTER"] = args[i]
        elif args[i] == "--failed":
            os.environ["BENCH_TASK_FILTER"] = "primes-list,quicksort,tcp-connect"
        elif args[i] == "--fix":
            FIX_MODE = True
        elif args[i] == "--intend":
            INTEND_MODE = True
            FIX_MODE = True
        elif args[i] == '--trace':
            TRACE_MODE = True
        elif args[i] == '--evolve':
            EVOLVE_MODE = True
        elif args[i] == "--max-attempts":
            i += 1
            if i < len(args):
                try:
                    MAX_ATTEMPTS = int(args[i])
                except ValueError:
                    pass
        i += 1

    if not api_key:
        print("❌ LLM_API_KEY not set")
        sys.exit(1)

    api_ref = get_api_ref()

    mode_tag = ""
    if EVOLVE_MODE:
        mode_tag = "  (evolve mode: evolve strategy after each round)"
    elif INTEND_MODE:
        mode_tag = f"  (intend mode: up to {MAX_ATTEMPTS} attempts)"
    elif FIX_MODE:
        mode_tag = f"  (fix mode: up to {MAX_ATTEMPTS} attempts per task per round)"

    print(f"\n{'='*70}")
    print(f"Aura EDSL Benchmark")
    print(f"Models: {', '.join(models)}")
    print(f"Tasks: {len(TASKS)}")
    print(f"Rounds: {ROUNDS}{mode_tag}")
    print(f"{'='*70}\n")

    all_results = {}

    for model in models:
        model = model.strip()
        all_results[model] = {}
        task_stats = defaultdict(lambda: {"passes": 0, "total": 0, "errors": [], "llm_times": [], "attempts": []})
        print(f"\n{'─'*70}")
        print(f"  Model: {model}")
        print(f"{'─'*70}")

        start_time = time.time()

        task_filter = os.environ.get("BENCH_TASK_FILTER", "")
        filter_list = [t.strip() for t in task_filter.split(",") if t.strip()] if task_filter else []
        for name, prompt, expected, stdlib in TASKS:
            if filter_list and name not in filter_list:
                continue
            task_passes = 0
            print(f"\n  ── {name} ──")
            for round_i in range(1, ROUNDS + 1):
                runner_fn = run_single_task_intend if INTEND_MODE else run_single_task
                success, out, err, llm_t, attempts = runner_fn(
                    model, base_url, api_key, name, prompt, expected, stdlib, api_ref
                )
                task_stats[name]["llm_times"].append(llm_t)
                task_stats[name]["attempts"].append(attempts)
                if success:
                    task_passes += 1
                    att = f" (attempts={attempts})" if (FIX_MODE or INTEND_MODE) else ""
                    line = f"    Round {round_i:2d}/{ROUNDS}: ✅ ({llm_t:.1f}s{att})"
                else:
                    task_stats[name]["errors"].append(err[:80])
                    att = f" in {attempts}" if (FIX_MODE or INTEND_MODE) else ""
                    line = f"    Round {round_i:2d}/{ROUNDS}: ❌ {err[:50]} ({llm_t:.1f}s{att})"
                    etype = "output-mismatch" if ('"ok"' in (out or '')[:50] and not success) else (err[:25] or "unknown")
                    task_stats.setdefault("__errors__", []).append((name, etype, attempts, llm_t))
                print(line)
                sys.stdout.flush()

            task_stats[name]["passes"] = task_passes
            task_stats[name]["total"] = task_stats[name]["passes"] + len(task_stats[name]["errors"])

        if EVOLVE_MODE:
            # Evolve strategy based on this round's analytics
            # Ensure base strategy exists, then evolve
            evolve_code = ('(require "std/evolve" all:)'
                           '(register-strategy! "default" "")'
                           '(display (evolve-strategy "default"))\n')
            try:
                r = subprocess.run([AURA], input=evolve_code, capture_output=True, text=True, timeout=10)
                evolved = r.stdout.strip()
                print(f"\n  ⚡ Evolved: {evolved}")
            except Exception as e:
                print(f"\n  ⚡ Evolve failed: {e}")
            # Read evolved body and inject hints into system prompt for next round
            if evolved:
                try:
                    r2 = subprocess.run([AURA],
                        input=f'(display (strategy-field "{evolved}" "body"))\n',
                        capture_output=True, text=True, timeout=5)
                    evolved_body = r2.stdout.strip()
                    if evolved_body and evolved_body != "()":
                        # Store for build_sys_prompt to inject
                        evolved_hints = evolved_body.replace('"', '\\"')
                        os.environ["EVOLVED_HINTS"] = evolved_hints
                        print(f"  ⚡ Injected {len(evolved_body)} chars of hints")
                except Exception:
                    pass
            # Reset history for next round
            print("  History cleared for next round.\n")

        elapsed = time.time() - start_time
        task_stats["__meta__"] = {"elapsed": round(elapsed, 1)}

        if task_stats.get("__errors__"):
            errors = task_stats["__errors__"]
            print(f"\n  ❌ Failure Analysis ({len(errors)} tasks):")
            # Group by error type
            by_type = {}
            for name, etype, att, t in errors:
                by_type.setdefault(etype, []).append(name)
            for etype, names in sorted(by_type.items()):
                print(f"    {etype:25s}: {len(names)} tasks — {', '.join(names[:5])}{'...' if len(names) > 5 else ''}")

        print(f"{'─'*70}")
        print(f"  {model} — Per-Task Results ({ROUNDS} rounds)")
        print(f"{'─'*70}")
        task_rows = [(n, s["passes"], s["total"]) for n, s in task_stats.items() if n not in ("__meta__", "__errors__")]
        sp, sf, sv = print_task_table(task_rows)

        if FIX_MODE:
            print(f"\n  📊 Attempt stats:")
            for n in sorted(s for s in task_stats if s not in ("__meta__", "__errors__")):
                s = task_stats[n]
                if s["attempts"]:
                    avg = sum(s["attempts"]) / len(s["attempts"])
                    print(f"    {n:22s} avg {avg:.1f} attempts, {s['passes']}/{s['total']} passed")

        volatile_tasks = [n for n, s in task_stats.items()
                          if n not in ("__meta__", "__errors__") and 0 < s["passes"] < s["total"]]
        if volatile_tasks:
            print(f"\n  ⚠️  Volatile tasks:")
            for n in sorted(volatile_tasks):
                s = task_stats[n]
                print(f"    {n}: {s['passes']}/{s['total']} ({s['passes']/s['total']*100:.0f}%)")
                for err in s["errors"][:3]:
                    print(f"      · {err}")

        all_results[model] = dict(task_stats)

    if output_json:
        output = {}
        for model, tasks in all_results.items():
            mout = {}
            for task_name, stats in tasks.items():
                if task_name == "__meta__":
                    mout["__meta__"] = stats
                else:
                    entry = {
                        "passes": stats["passes"],
                        "total": stats["total"],
                        "pass_rate": round(stats["passes"] / stats["total"] * 100, 1) if stats["total"] else 0,
                        "avg_llm_time": round(sum(stats["llm_times"]) / len(stats["llm_times"]), 2) if stats["llm_times"] else 0,
                    }
                    if stats["attempts"]:
                        entry["avg_attempts"] = round(sum(stats["attempts"]) / len(stats["attempts"]), 1)
                    mout[task_name] = entry
            output[model] = mout
        print(f"\n{'='*70}")
        print(json.dumps(output, indent=2))

    print(f"\n{'='*70}")
    print("Done")
    print(f"{'='*70}")

if __name__ == "__main__":
    main()
