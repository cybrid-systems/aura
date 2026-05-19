#!/usr/bin/env python3
"""Aura AI Agent — 迭代式代码生成演示

LLM 持续生成 -> 执行 -> 看结果 -> 改进 -> 再执行，直到 LLM 认为完成为止。
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse

AURA = os.environ.get("AURA_BIN", "./build/aura")
LLM_KEY = os.environ.get("LLM_API_KEY") or os.environ.get("OPENAI_API_KEY", "")
LLM_URL = os.environ.get("LLM_BASE_URL") or os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
LLM_MODEL = (os.environ.get("LLM_MODEL") or os.environ.get("OPENAI_MODEL", "deepseek-v4-flash")).split("/")[-1]
MAX_ITERATIONS = 12
EXEC_TIMEOUT = 15

def llm_chat(messages):
    parsed = urllib.parse.urlparse(LLM_URL)
    h = http.client.HTTPSConnection(parsed.netloc, timeout=60) if parsed.scheme == "https" else http.client.HTTPConnection(parsed.netloc, timeout=60)
    h.request("POST", parsed.path + "/chat/completions", json.dumps({
        "model": LLM_MODEL, "messages": messages, "temperature": 0.4,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {LLM_KEY}"})
    r = h.getresponse()
    d = json.loads(r.read())
    h.close()
    return d["choices"][0]["message"]["content"]

class AuraSession:
    def __init__(self):
        self.proc = subprocess.Popen([AURA, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    def exec(self, code, timeout=EXEC_TIMEOUT):
        self.proc.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if line and line.startswith("{"):
                return json.loads(line)
        return {"status": "timeout", "msg": f"timed out after {timeout}s"}
    def close(self):
        self.proc.terminate()
        self.proc.wait()

def extract_code(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL)
    if "```" in text:
        for p in text.split("```"):
            lines = p.strip().split("\n")
            lines = [l for l in lines if not l.startswith(("aura", "scheme", "lisp", "racket", "python"))]
            c = "\n".join(lines).strip()
            if any(k in c for k in ("define", "require", "(+", "(begin", "lambda", "import")):
                return c
    return ""

SYSTEM_PROMPT = """You are an agent for the Aura programming language (Scheme-like Lisp).
ONLY write Aura Lisp code. NEVER output Python, JavaScript, Racket, or other languages.

AVAILABLE:
- (define (fn x) body), (lambda (x) body)
- (if cond t e), (begin expr...), (cond ...), (when cond body...), (unless cond body...)
- (let ((x v)) body), (let* ...), (letrec ...), (set! name val)
- (+ 1 2 3), (- x y), (* x y), (/ x y), (modulo n m)
- (list 1 2 3), (cons 1 2), (car x), (cdr x), (cadr x) = (car (cdr x))
- (map fn lst), (filter fn lst), (foldl fn init lst)
- (display val), (write val), (newline)
- (require std/list) -> sum, sort, range, foldl, zip, flatten, last
- (require std/math) -> factorial, sqrt, pi, sin, cos, tan
- (require std/string) -> string-split, string-join, string-trim, string-contains?
- (require std/iter) -> any?, every?, find, frequencies, iota, iterate, hash-map
- (hash "k1" v1 "k2" v2), (hash-ref h key default), (hash-set! h key val), hash-has-key?
- (try body (catch (e) handler)), (raise val)
- (defmacro (name . args) body) -- macros expand at compile time
- (apply fn list) -- call fn with elements of list as args
- format: (format "~a = ~a" name val)

Defmacro example (ONLY this syntax works):
```lisp
(defmacro (twice expr)
  (list (quote begin) expr expr))
```

FLOW:
1. Write Aura code in ``` block
2. I execute it and return result
3. You see the result, then either fix errors or say DONE"""""

def main():
    if len(sys.argv) < 2:
        prompt = input("> ")
    else:
        prompt = " ".join(sys.argv[1:])
    if not LLM_KEY:
        print("Need LLM_API_KEY or OPENAI_API_KEY"); sys.exit(1)

    print(f"\n{'='*50}\nLLM: {LLM_MODEL}\nTask: {prompt}\n{'='*50}")
    aura = AuraSession()
    msgs = [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": prompt}]
    history = []

    for i in range(1, MAX_ITERATIONS + 1):
        print(f"\n-- Round {i} --")
        print("  Thinking...", end=" ", flush=True)
        resp = llm_chat(msgs)
        print("ok")

        code = extract_code(resp)
        if not code and "DONE" in resp.strip().split("\n")[-1].upper():
            print("  DONE\n")
            for line in resp.split("\n"):
                print(f"  {line}")
            break
        if not code:
            msgs.append({"role": "assistant", "content": resp})
            continue

        print("  Code:")
        for line in code.split("\n"):
            print(f"    {line}")
        history.append((i, code))

        # Strip display/write calls that would shadow the return value
        code = re.sub(r'\(display\s+([^)]+)\)', r'\1', code)
        code = re.sub(r'\(write\s+([^)]+)\)', r'\1', code)
        code = re.sub(r'\(newline\)', '', code)

        # Wrap multi-line in begin
        lines = [l.strip() for l in code.split("\n") if l.strip() and not l.startswith(";")]
        if len(lines) > 1 and not (lines[0].startswith("(begin") and lines[-1] == ")"):
            code = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"

        print("  Executing...", end=" ", flush=True)
        t0 = time.time()
        r = aura.exec(code)
        t = time.time() - t0

        s = r.get("status")
        v = r.get("value", r.get("msg", ""))
        if s == "ok":
            print(f"OK ({t:.1f}s) -> {v}")
        elif s == "timeout":
            print(f"TIMEOUT ({EXEC_TIMEOUT}s)")
        else:
            print(f"ERROR: {v}")

        fb = f"Result:\n  Status: {s}\n  Time: {t:.1f}s\n"
        if s == "ok":
            fb += f"  Value: {v}\n\nIf correct, say DONE. Otherwise improve the code.\n"
        else:
            fb += f"  Error: {v}\n\nFix the code.\n"
        fb += f"(Round {i}/{MAX_ITERATIONS})"

        msgs.append({"role": "assistant", "content": resp})
        msgs.append({"role": "user", "content": fb})
    else:
        print(f"\nMax rounds ({MAX_ITERATIONS}) reached")

    print(f"\n{'='*50}\nRounds with code: {len(history)}")
    for n, c in history:
        print(f"  #{n}: {c.split(chr(10))[0][:60]}...")
    print('='*50)
    aura.close()

if __name__ == "__main__":
    main()
