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
LLM_MODEL = os.environ.get("LLM_MODEL") or os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
MAX_ROUNDS = 20

SYSTEM_PROMPT = build_system_prompt() + """

## WORKSPACE + QUERY + MUTATE (EDSL)

**两阶段工作流:**

### Phase 1: 写完整程序 (exec)
先用普通 Aura 代码写完整程序，快速跑通。

### Phase 2: 精确修复 (mutate)
当程序基本正确但有 bug 时，**不要重写整个函数**。改用 EDSL 精准修改：

1. `set-code "当前程序"` — 锁定到工作区（节点 ID 稳定）
2. `query:find / children / node-type` — 定位目标节点
3. `mutate:set-body / replace-value / insert-child` — 精确改一个点
4. `typecheck-current + eval-current` — 验证

### 所有原语

**Query** (必须在 set-code 后才能用):
```
(query:find "name")          → (1 5 12)     ; 按名称查找节点
(query:children node-id)      → (4 5 6)      ; 获取子节点 ID
(query:node node-id)          → (3 "fib" 3)  ; (tag name/val children_count)
(query:calls "fib")           → (8 15 22)    ; 查找函数调用点
(query:parent node-id)        → (3)          ; 查找父节点
(query:siblings node-id)      → (4 6)        ; 查找兄弟节点
(query:pattern expr)          → (12 18)      ; 结构模式匹配
(query:node-type "Call")      → (0 3 8 15)   ; 按节点类型过滤
```

**Mutate** (必须在 set-code 后才能用):
```
(mutate:rebind "name" new-code)             ; 替换整个函数定义
(mutate:set-body "name" new-body-code)      ; 替换函数体（保留签名）
(mutate:remove-node node-id)                ; 删除节点
(mutate:insert-child parent pos child-code)  ; 插入子节点（返回新节点 ID）
(mutate:replace-value node-id new-val)      ; 替换值
(mutate:replace-type node-id new-type)      ; 替换类型注解
(mutate:record-patch node-id op summary)    ; 记录操作
```

**验证** (mutate 后用):
```
(typecheck-current)  ; 增量类型检查
(eval-current)       ; 执行当前工作区
```

### EDSL 示例: 把 add 函数的 + 换成 *

第一轮写程序:
```
(define (add x y) (+ x y))
```

第二轮用 EDSL 精确修改:
```
(set-code ")  ; 锁定代码到工作区
(query:find "add") → (5)           ; Define 在节点 5
(query:children 3) → (0 1 2)       ; (+ x y) 的 3 个子节点
(mutate:replace-value 0 "*" "+"→"*")
(typecheck-current)
(eval-current)
(add 1 2) → 2                      ; 验证: (* 1 2) = 2
```

## 规则
- Phase 1 用 **exec** 写完整代码（允许大步子）
- Phase 2 用 **workspace + query + mutate** 做精确修改
- mutate 后用 `typecheck-current` 验证，`eval-current` 执行
- 只有修改很小（改一个数字/变量名）时可以一步搞定
- 任何需要改完整函数的场景 → 用 `mutate:rebind` 或 `mutate:set-body`
- 任务完成后说 DONE"""


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
