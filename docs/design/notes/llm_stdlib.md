# std/llm — Aura LLM stdlib 模块设计

> 把 LLM 交互逻辑从 C++/Python 搬到 Aura stdlib。
> 让 `(intend ...)` 成为纯编排器，LLM 调用全走 std/llm。

---

## 架构

```
┌─────────────────────────────────────────────────┐
│  Python (edsl_benchmark.py)                     │
│  - build_sys_prompt()  → 构建 task prompt       │
│  - run_single_task()   → 调度单任务              │
│  - 聚合统计 / 多轮 / JSON 输出                   │
└────────────────┬────────────────────────────────┘
                 │ (intend "goal" gen ver fix N)
                 ▼
┌─────────────────────────────────────────────────┐
│  C++ (intend 原语)                              │
│  - 纯循环：调用 gen → 调用 ver → 失败则 fix      │
│  - 不构建 JSON，不调 LLM                         │
└────────────────┬────────────────────────────────┘
                 │ gen/ver/fix 是 Aura closure
                 ▼
┌─────────────────────────────────────────────────┐
│  Aura stdlib (lib/std/llm.aura)                 │
│  - aura-llm-call:    http-post 调 LLM API       │
│  - aura-json-escape: 字符串 → JSON 安全转义      │
│  - aura-json-build:  构建 API 请求体             │
│  - aura-extract:     JSON 响应 → 提取代码        │
│  - aura-verify:      编译 + 运行 → 检查结果      │
│  - aura-make-gen:    生成器工厂 (sys-prompt)     │
│  - aura-make-fix:    修正器工厂 (sys-prompt)     │
└─────────────────────────────────────────────────┘
```

---

## 分层拆解

### L0: 基础字符串操作 (`lib/std/llm.aura`)

```scheme
; JSON 转义字符串
(aura-json-escape s) → str
; 输入 "hello \"world\""
; 输出 "hello \\\"world\\\""

; JSON 构建（从 escape 后的字符串构建完整 body）
(aura-json-body escaped-sys escaped-goal) → str
; 输出: {"model":"...","messages":[...],"temperature":0.3}

; LLM 调用
(aura-llm-call sys-raw goal-raw) → raw-json-response
; 1. (aura-json-escape sys) → escaped-sys
; 2. (aura-json-escape goal) → escaped-goal
; 3. (aura-json-body escaped-sys escaped-goal) → body
; 4. (http-post url body api-key) → response

; 从 JSON 响应提取 content 字段
(aura-extract-content raw-json) → content-string
```

**不依赖：** 无（只依赖 string-append, list->string, http-post, getenv）

### L1: 生成器 / 验证器 / 修正器

```scheme
; 生成器工厂：sys-prompt → (lambda (goal) → code)
(aura-make-gen sys-prompt)
; 内部: (lambda (goal) (aura-extract-content (aura-llm-call sys-prompt goal)))

; 验证器：code → "#t" 或错误字符串
(aura-verify code)
; 1. (set-code code)
; 2. (typecheck-current)
; 3. (eval-current)
; 4. 如果结果 void → "#t"
; 5. 否则 → (string-append "unexpected:" (type-of result))

; 修正器工厂：sys-prompt → (lambda (code error goal) → new-code)
(aura-make-fix sys-prompt)
; 内部: 构建 fix prompt → (aura-llm-call fix-prompt goal)
```

**依赖：** L0 + set-code, typecheck-current, eval-current, type-of

### L2: Python 桥接 (`edsl_benchmark.py`)

```python
def run_single_task_intend(...):
    # 1. 构建系统提示（含 task 特定示例）
    sys_prompt = build_sys_prompt(stdlib, api_ref, task_name=name)
    
    # 2. 构建要执行的 Aura 代码
    aura_code = f"""
    (require std/llm all:)
    (define __sp__ {sys_prompt})
    (define __gen__ (aura-make-gen __sp__))
    (define __fix__ (aura-make-fix __sp__))
    (display (intend "{goal}" __gen__ aura-verify __fix__ {max_att}))
    """
    
    # 3. 执行
    r = subprocess.run([AURA], input=aura_code, timeout=60)
    
    # 4. 解析结果
    # "#(status:"ok" goal:"..." iterations:N)" 或
    # "#(status:"failed" goal:"..." iterations:N last-error:"...")"
```

**依赖：** L1 + build_sys_prompt()

### L3: 完整 intend 管线

```
Python 构建 prompt
    → Aura 加载 std/llm
    → aura-make-gen 创建生成器
    → intend 调生成器 → LLM 返回 JSON
    → aura-extract-content 提取代码
    → aura-verify 编译+运行
    → 通过 → 返回 ok
    → 失败 → aura-make-fix 构建修正 prompt
    → intend 回调修正器 → LLM 返回修正代码
    → 循环至 max-attempts
```

---

## 实现顺序

### Step 1: `aura-json-escape` （纯 Aura，30 行）

用 `list->string` + ASCII codes 构建，**不在字符串字面量中用反斜杠转义**。

关键模式：
```scheme
(define dq (list->string (list 34)))   ; "
(define qq (list->string (list 92 34))) ; \"
```

```scheme
(define aura-json-escape
  (lambda (s)
    (let loop ((i 0) (r ""))
      (if (>= i (string-length s)) r
        (let ((c (string-ref s i)))
          (loop (+ i 1)
            (string-append r
              (cond
                ((= c 34) qq)    ; " → \"
                ((= c 92) bs)    ; \ → \\
                ((= c 10) nn)    ; newline → \n
                ((= c 13) rr)    ; CR → \r
                ((= c 9)  tt)    ; tab → \t
                (else (list->string (list c))))))))))))
```

### Step 2: `aura-json-body` （纯 Aura，20 行）

```scheme
(define aura-json-body
  (lambda (escaped-sys escaped-goal)
    (string-append
      "{" dq "model" dq ":" dq "deepseek-v4-flash" dq
      "," dq "messages" dq ":[{"
      dq "role" dq ":" dq "system" dq "," dq "content" dq ":" dq escaped-sys dq
      "},{"
      dq "role" dq ":" dq "user" dq "," dq "content" dq ":" dq escaped-goal dq
      "}]"
      "," dq "temperature" dq ":0.3," dq "max_tokens" dq ":4096}")))
```

### Step 3: `aura-llm-call` （纯 Aura，15 行）

```scheme
(define aura-llm-call
  (lambda (sys goal)
    (let ((key (getenv "LLM_API_KEY")))
      (if (or (not key) (= (string-length key) 0)) ""
        (http-post "https://api.deepseek.com/v1/chat/completions"
          (aura-json-body (aura-json-escape sys) (aura-json-escape goal))
          key)))))
```

### Step 4: `aura-extract-content` （纯 Aura，25 行）

从 LLM JSON 响应中提取 `"content":"..."` 字段的值。
需要手动扫描字符，因为 Aura 没有 JSON 解析器。

```scheme
(define aura-extract-content
  (lambda (resp)
    ; 找 "content":" 然后提取值
    ; 返回 code 字符串或 ""
    ...))
```

### Step 5: `aura-verify` 加强（纯 Aura，15 行）

当前问题：`set-code "Authentication Fails (governor)"` 返回 `#t`，
因为 parser 把任意符号序列当合法列表解析。

修复方向：验证器需要检查输出是否**匹配预期结果**。
方案：验讫器接受 `(code expected-keywords)` 两个参数。

```scheme
(define aura-verify
  (lambda (code)
    (if (set-code code)
      (let ()
        (typecheck-current)
        (let ((r (eval-current)))
          (if (void? r) "#t"
            ; 这里需要检查 r 是否包含预期关键词
            ; 但关键词是 Python 传入的，不是 hardcoded
            "..."))
        )
      "bad-code")))
```

**设计决策：** verifier 应该接受 `(code expected-keywords)` 还是只做编译检查？
- 只做编译检查：简单，但不保证语义正确性
- 检查预期输出：需要传入 expected keywords，接口更复杂

建议：做两层验证。第一层只编译检查（快）。第二层比对输出（需要传入预期关键词）。
intend 的 C++ 循环调用 verifier 只看第第一层。第二层由 Python 端做。

### Step 6: Python 桥接更新

```python
def run_single_task_intend(...):
    aura_code = [
        '(require std/llm all:)',
        '(define __sp__ "..." )',     # escaped sys_prompt
        '(define __gen__ (aura-make-gen __sp__))',
        '(define __fix__ (aura-make-fix __sp__))',
        '(display (intend "..." __gen__ aura-verify __fix__ N))'
    ]
```

---

## 文件结构

```
lib/std/llm.aura         ← L0-L1: ~100 行 Aura 代码
tests/edsl_benchmark.py  ← L2: ~30 行 Python 代码（run_single_task_intend）
tests/test_llm.aura      ← L0-L1 测试：逐项验证每个函数
```

---

## 关键设计决策

### 1. 为什么 JSON 构建在 Aura 而非 Python？

把 LLM 交互完全放在 Aura stdlib 中，使得：
- `(intend ...)` 可以在任何 Aura 程序中使用，不依赖 Python
- 换 LLM 提供商只需改 `aura-json-body`，不碰 C++
- std/llm.aura 可以被 query/mutate——LLM 交互策略本身可演化

### 2. 为什么 `list->string` 而不是字符串转义？

Aura 字符串中的 `\"` 是有效的转义序列，但和 Python 的字符串嵌套时极度容易出错。
用 `(list->string (list 92 34))` = `\"` 完全避免了这个问題。

### 3. verifier 为什么只做编译检查？

当前 `aura-verify` 只验证代码能编译通过。语义验证（输出是否匹配预期关键词）
由 Python 端在 `run_single_task_intend` 的返回值解析中完成。
这样 `intend` 循环保持通用，不绑定具体测试任务。

---

## 测试方案

```scheme
; tests/test_llm.aura

; Step 1: JSON escape
(assert (aura-json-escape "a") "a")
(assert (aura-json-escape "\"") "\\\"")
(assert (aura-json-escape "a\nb") "a\\nb")

; Step 2: JSON body
(assert (contains? (aura-json-body "x" "y") "model"))
(assert (contains? (aura-json-body "x" "y") "deepseek-v4-flash"))

; Step 3: LLM call (requires LLM_API_KEY — 跳过测试)
; (assert (not (= (aura-llm-call "hi" "say hi") "")))
```
