#!/usr/bin/env python3
"""Aura AI Agent — LLM 驱动代码生成演示

端到端循环:
  用户描述需求 → LLM 生成 Aura 代码 → --serve 执行 →
  成功？展示结果 | 失败 → 错误送回 LLM → 修正 → 重试

使用:
  export OPENAI_API_KEY="sk-..."
  export OPENAI_BASE_URL="https://api.openai.com/v1"  # 可选，默认 OpenAI
  python3 tests/ai_agent_llm.py "写一个斐波那契函数，算 fib(20)"

  或使用本地模型 (如 ollama):
  export OPENAI_BASE_URL="http://localhost:11434/v1"
  export LLM_MODEL="llama3"
  python3 tests/ai_agent_llm.py "写一个 map 函数的用法示例"
"""

import subprocess
import json
import sys
import os
import http.client
import urllib.parse

# ── 配置 ──────────────────────────────────────────────────────
AURA = os.environ.get("AURA_BIN", "./build/aura")
LLM_KEY = os.environ.get("LLM_API_KEY") or os.environ.get("OPENAI_API_KEY", "")
LLM_URL = os.environ.get("LLM_BASE_URL") or os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
LLM_MODEL = (os.environ.get("LLM_MODEL") or os.environ.get("OPENAI_MODEL", "deepseek-v4-flash")).split("/")[-1]
MAX_FIX_LOOP = 5  # 最大修复轮数

# ── LLM 客户端 ────────────────────────────────────────────────
def llm_chat(messages):
    """调用 OpenAI 兼容的 API"""
    parsed = urllib.parse.urlparse(LLM_URL)
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {LLM_KEY}",
    }
    body = json.dumps({
        "model": LLM_MODEL,
        "messages": messages,
        "temperature": 0.3,
    })
    conn = http.client.HTTPSConnection(parsed.netloc, timeout=30)
    conn.request("POST", parsed.path + "/chat/completions", body, headers)
    resp = conn.getresponse()
    data = json.loads(resp.read())
    conn.close()
    return data["choices"][0]["message"]["content"]

# ── Aura 客户端 ────────────────────────────────────────────────
class AuraSession:
    def __init__(self):
        self.proc = subprocess.Popen(
            [AURA, "--serve"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
        )
    def exec(self, code):
        self.proc.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n")
        self.proc.stdin.flush()
        for _ in range(50):
            line = self.proc.stdout.readline()
            if line.startswith("{"):
                return json.loads(line)
        return {"status": "timeout"}
    def close(self):
        self.proc.terminate()
        self.proc.wait()


# ── 系统提示 ──────────────────────────────────────────────────
SYSTEM_PROMPT = f"""你是 Aura 编程语言的专家。Aura 是一个 Lisp 方言，语法类似 Scheme。

Aura 语法要点:
- 调用: (fn arg1 arg2 ...)
- 定义: (define name value) 或 (define (fn-name args...) body)
- Lambda: (lambda (x) body)
- if: (if cond then-expr else-expr)
- 列表: (list 1 2 3), (cons 1 2), (car lst), (cdr lst)
- 算术: (+ 1 2 3), (- 5 1), (* 2 3), (/ 10 2), (< a b), (= a b)
- 布尔: #t, #f, (not x), (and x y), (or x y)
- 字符串: (string-append "a" "b"), (string-length "hi"), (string-ref "hi" 0)
- 内置函数: map, filter, foldl, modulo, gcd, abs, display, write, newline
- match: (match expr ((pattern) body) ...)
- 字符串转数字: (string->number "42")

注意:
- 字符串用双引号: "hello"
- `+` 是函数名，不是操作符
- 定义函数后可以直接调用
- 返回值是最后一个表达式的值

重要: 每次 exec 调用之间，define 的状态是保持的。
所以可以先 define 函数，再在后续调用中使用。

请只返回 Aura 代码，不要额外解释。如果需要多行代码，用 begin 包裹。
"""


# ── 主循环 ────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        prompt = input("👤 描述你想让 Aura 做什么: ")
    else:
        prompt = " ".join(sys.argv[1:])

    if not LLM_KEY:
        print("❌ 需要设置 OPENAI_API_KEY 环境变量")
        print("   或使用本地模型: export OPENAI_BASE_URL=http://localhost:11434/v1")
        sys.exit(1)

    print(f"\n{'='*60}")
    print(f"🤖 LLM: {LLM_MODEL}")
    print(f"👤 需求: {prompt}")
    print(f"{'='*60}\n")

    aura = AuraSession()
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt},
    ]

    for attempt in range(1, MAX_FIX_LOOP + 2):
        # ── 1. LLM 生成代码 ──
        print(f"  ⏳ LLM 思考中... (第{attempt}次)", end=" ", flush=True)
        response = llm_chat(messages)
        print("done")

        # 提取代码（LLM 可能用代码块包裹）
        code = response.strip()
        if "```" in code:
            parts = code.split("```")
            # 取第一个代码块（跳过标记语言名称行）
            for p in parts:
                if "define" in p or "(lambda" in p or "(+" in p or "import" in p or "(begin" in p:
                    lines = p.strip().split("\n")
                    code = "\n".join(l for l in lines if not l.startswith("aura"))
                    break
        # 移除 <think> 标签内容（MiniMax 等模型会输出推理过程）
        import re
        code = re.sub(r'<think>.*?</think>', '', code, flags=re.DOTALL).strip()
        # 多个表达式时用 begin 包裹，确保全部执行
        lines = [l.strip() for l in code.split("\n") if l.strip()]
        if len(lines) > 1:
            if not (lines[0].startswith("(begin") and lines[-1] == ")"):
                code = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"

        print(f"\n  📄 生成代码:")
        for line in code.strip().split("\n"):
            print(f"    {line}")

        # ── 2. Aura 执行 ──
        print(f"\n  ⚡ 执行...", end=" ", flush=True)
        result = aura.exec(code)
        status = result.get("status")
        value = result.get("value", result.get("msg", ""))

        if status == "ok":
            print("✅ 成功!")
            print(f"\n  ✅ 结果: {value}\n")
            aura.close()
            return

        # ── 3. 错误 → 送回 LLM ──
        error = result.get("msg", result.get("value", "unknown error"))
        print(f"❌ 错误: {error}")

        if attempt > MAX_FIX_LOOP:
            print(f"\n  ❌ 超过最大修复次数 ({MAX_FIX_LOOP})，放弃")
            break

        messages.append({"role": "assistant", "content": code})
        messages.append({"role": "user", "content": f"代码执行出错：{error}\n请修正代码。"})

    aura.close()
    sys.exit(1)


if __name__ == "__main__":
    main()
