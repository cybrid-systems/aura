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
OPENAI_KEY = os.environ.get("OPENAI_API_KEY", "")
OPENAI_URL = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
OPENAI_MODEL = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
MAX_ROUNDS = 20

SYSTEM_PROMPT = build_system_prompt() + """

## TYPED MUTATION (增量变换)

不要重写整个程序。使用以下 EDSL 操作进行增量修改:

1. 先用 exec 定义程序
2. 用 query 检查 AST 节点
3. 用 mutate 做精确修改

### Query (查询 AST)
(query:node code node-id)           — 查看节点
(query:parent code node-id)         — 父节点
(query:children code node-id)       — 子节点列表
(query:siblings code node-id)       — 兄弟节点

### Mutate (修改)
(mutate:replace-value node-id new-value "summary")  — 替换值
(mutate:replace-type node-id new-type "summary")    — 替换类型
(mutate:record-patch node-id "op-name" "summary")   — 记录操作

### Fix (编译-修复循环)
(fix code error-message)            — 尝试修复编译错误
(fix:value code node-id)            — 修复特定节点的值
(fix:type code node-id)             — 修复类型

## WORKFLOW
1. 用 exec 先定义/写程序
2. 用 query 看 AST
3. 用 mutate 精确改一个节点
4. 看结果，不对就 rollback
5. 重复直到正确
"""

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
    p = urllib.parse.urlparse(OPENAI_URL)
    c = http.client.HTTPSConnection(p.netloc, timeout=90) if p.scheme == "https" else http.client.HTTPConnection(p.netloc, timeout=90)
    c.request("POST", p.path + "/chat/completions", json.dumps({
        "model": OPENAI_MODEL, "messages": msgs, "temperature": 0.3, "max_tokens": 1200,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {OPENAI_KEY}"})
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
    if not OPENAI_KEY:
        print("Need OPENAI_API_KEY"); sys.exit(1)

    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else input("> ")
    print(f"\n{'='*50}\nLLM: {OPENAI_MODEL}\nTask: {prompt}\n{'='*50}\n")

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
