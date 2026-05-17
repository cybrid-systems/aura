#!/usr/bin/env python3
"""Aura AI Agent — 增量变换式开发 (Typed Mutation)

LLM 不重写整段代码，而是生成 EDSL 变换操作:
  (mutate:replace-value node-id new-value "summary")
  (mutate:replace-type node-id new-type "summary")
  (mutate:record-patch node-id op-name "summary")

每次只改一个节点，且类型安全、可回滚。
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ai_agent_prompt import build_system_prompt

AURA = os.environ.get("AURA_BIN", "./build/aura")
# LLM_* 优先, 兼容 OPENAI_* (任何 OpenAI-compatible provider: OpenAI / MiniMax / DeepSeek / Ollama)
LLM_KEY = os.environ.get("LLM_API_KEY") or os.environ.get("OPENAI_API_KEY", "")
LLM_URL = os.environ.get("LLM_BASE_URL") or os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
LLM_MODEL = os.environ.get("LLM_MODEL") or os.environ.get("OPENAI_MODEL", "deepseek-v4-flash")
MAX_ROUNDS = 20

SYSTEM_PROMPT = build_system_prompt() + """

## WORKSPACE + EDSL (Phase 2: precise fixes)

Two-phase workflow:

### Phase 1 (exec)
Write complete Aura code and run it. Get it working first.

### Phase 2 (set-code + query + mutate)
**ONLY** when code compiles but needs a small fix: lock into workspace and mutate.

```
(set-code "the current program source")  ; Lock code, node IDs become stable
(query:find "func-name")                  ; Find definition node IDs
(query:children 5)                         ; See children of node 5
(mutate:set-body "func-name" "new body")  ; Replace function body (keep signature)
(mutate:rebind "func-name" "new def")     ; Replace entire function definition
(mutate:replace-value 3 "42")             ; Replace literal value at node 3
(typecheck-current)                        ; Run type checker
(eval-current)                             ; Run the workspace and test
```

### EDSL Example: Change + to * in add

Round 1 (exec - write):
```lisp
(define (add x y) (+ x y))
```

Round 2 (mutate - fix):
```lisp
(set-code "(define (add x y) (+ x y))")
(query:find "add")       ; returns (5) — Define is node 5
(query:children 5)        ; (0 1 2) = (name args body)
(mutate:replace-value 2 "*" "plus→times")
(typecheck-current)
(eval-current)
(add 1 2)                 ; now returns 2 because (* 1 2) = 2
```

## RULES
- Phase 1: write full code (exec), keep it simple
- Phase 2: EDSL for tiny fixes only (one value, one body)
- If a whole function needs rewriting → go back to Phase 1 exec
- Say DONE when correct."""


class AuraSession:
    def __init__(self):
        self.p = subprocess.Popen([AURA, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    def send(self, cmd, timeout=30):
        self.p.stdin.write(json.dumps(cmd) + "\n")
        self.p.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            l = self.p.stdout.readline()
            if l and l.startswith("{"):
                return json.loads(l)
        return {"status": "timeout"}
    def exec(self, code, timeout=30):
        return self.send({"cmd": "exec", "code": code}, timeout)
    def query(self, op, code, node_id):
        return self.send({"cmd": "query", "op": op, "code": code, "node": str(node_id)}, 10)
    def mutate(self, op_name, node_id, value="", summary=""):
        return self.send({"cmd": "mutate", "op": op_name, "node": str(node_id), "value": str(value), "summary": summary}, 10)
    def close(self):
        self.p.terminate(); self.p.wait()

def llm_call(msgs):
    p = urllib.parse.urlparse(LLM_URL)
    c = http.client.HTTPSConnection(p.netloc, timeout=90) if p.scheme == "https" else http.client.HTTPConnection(p.netloc, timeout=90)
    c.request("POST", p.path + "/chat/completions", json.dumps({
        "model": LLM_MODEL, "messages": msgs, "temperature": 0.3, "max_tokens": 1200,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {LLM_KEY}"})
    r = c.getresponse()
    d = json.loads(r.read())
    c.close()
    return d["choices"][0]["message"]["content"]

def extract_aura(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL)
    if "```" in text:
        for p in text.split("```"):
            c = "\n".join(l for l in p.strip().split("\n") if not l.startswith(("aura","scheme","lisp"))).strip()
            if any(k in c for k in ("define","require","(+","(begin","lambda","import","fix","query","mutate")): return c
    return ""

def main():
    if not LLM_KEY:
        print("Need LLM_API_KEY or OPENAI_API_KEY"); sys.exit(1)

    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else input("> ")
    print(f"\n{'='*50}\nLLM: {LLM_MODEL}\nTask: {prompt}\n{'='*50}\n")

    aura = AuraSession()
    msgs = [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": prompt}]
    history = []

    for rnd in range(1, MAX_ROUNDS + 1):
        print(f"\n── Round {rnd} ──")
        print("  Thinking...", end=" ", flush=True)
        resp = llm_call(msgs)
        print("ok")

        code = extract_aura(resp)
        if not code:
            if "DONE" in resp.strip().split("\n")[-1].upper():
                print("  DONE"); break
            msgs.append({"role": "assistant", "content": resp})
            continue

        print(f"  Code:\n    {code[:200]}")

        # Detect operation type
        if code.startswith("(query") or code.startswith("(fix") or code.startswith("(mutate"):
            # It's an EDSL operation — send directly
            cmd_type = "exec"
            if code.startswith("(query"):
                # Parse (query:* code node-id)
                pass  # send as exec
            result = aura.exec(code)
        else:
            # It's regular Aura code — use exec
            code = re.sub(r'\(display\s+([^)]+)\)', r'\1', code)
            if not code.startswith("(begin"):
                lines = [l.strip() for l in code.split("\n") if l.strip() and not l.startswith(";")]
                if len(lines) > 1:
                    code = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"
            result = aura.exec(code)

        if result:
            status = result.get("status")
            value = result.get("value", result.get("msg", ""))
            print(f"  → {status}: {value[:80]}")

            if status == "ok" and "DONE" not in resp:
                # Let LLM decide: improve or done
                fb = f"Result: status={status} value={value}\nImprove further or say DONE.\n"
            else:
                fb = f"Result: status={status} value={value}\nFix errors and retry.\n"
            msgs.append({"role": "assistant", "content": resp})
            msgs.append({"role": "user", "content": fb})
        else:
            print("  → no response")
            msgs.append({"role": "assistant", "content": resp})

    aura.close()
    print(f"\n{'='*50}\nDone after {rnd} rounds\n{'='*50}")

if __name__ == "__main__":
    main()
