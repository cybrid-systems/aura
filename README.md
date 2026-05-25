# Aura

**AI-native Lisp — 代码自己进化。**

Aura 让 AI Agent 拥有在运行时**精确读写和修改自身代码**的能力。  
不是"让 LLM 输出文本然后粘贴"——而是把代码变成一块可查询、可变异、可版本化的活体 AST。

## 快速开始

```bash
# 构建
python3 build.py build

# 管道模式（一行代码）
echo '(+ 1 2)' | ./build/aura              # → 3

# 交互式 REPL
./build/aura                                # 输入 (+ 1 2) 回车

# 运行测试
python3 build.py test core                  # 核心管线：单元 + 集成 + 类型 + 套件
python3 build.py check                      # 全量 CI（核心 + 安全回归 + fuzz）
```

---

## 核心能力

### 🔹 自修改 EDSL — 代码有记忆

```scheme
(set-code "(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))")

;; 查询影响范围
(query:def-use "fib")           → ((21) . (23 6 12))   ; defs . uses
(query:reaches 21)              → ((21) . (23 6 12))   ; 这个定义影响到谁
(query:effects "fib")           → ((21) (23 6 12) (25 17 11)) ; + callers

;; 手术级 AST 编辑
(mutate:rebind "fib" "(lambda (n) (* n 2))" "linearize")
(mutate:wrap 3 "(display _)" "wrap")                     ; 用模板包裹表达式
(mutate:splice 0 1 "(display 1)" "(display 2)" "insert") ; 批量插入

;; 快照 + 回退
(ast:snapshot "checkpoint")
(mutate:rebind "fib" "(lambda (n) 0)" "oops")
(ast:restore 0)                                          ; 一秒回退

;; 结构化 diff
(ast:diff 0)
→ ((:removed . "(define fib (lambda (n) ...))")
   (:added . "(define fib (lambda (n) 0))"))
```

### 🔹 Workspace 分层 — 独立实验环境

```scheme
;; 在隔离的子 workspace 中实验，不影响主代码
(define sandbox (workspace:create "sandbox"))
(workspace:switch sandbox)
(mutate:rebind "fib" "(lambda (n) (fib-iter n 0 1))" "optimize")
(eval-current)                               ; 验证
(workspace:switch 0)                         ; 主代码不受影响
(workspace:merge sandbox)                    ; 合并好的版本
```

### 🔹 Inter-Agent 通信

```scheme
;; Session A
(send "agent-b" "{\"type\":\"request\",\"fn\":\"sort\"}")

;; Session B（另一个 serve 连接）
(display (recv 100))  → "{\"type\":\"request\",\"fn\":\"sort\"}"
(reply "{\"status\":\"ok\",\"code\":\"...\"}")
(session-active? "agent-a")  → #t
```

### 🔹 代码合成管线

```scheme
;; 模板生成
(synthesize:register-template "handler"
  "(define (handle-{r} req) (query \"{q}\"))" "r" "q")
(synthesize:fill "handler" "users" "SELECT *")

;; LLM 生成
(synthesize:define "fib" "Int -> Int"
  :prompt "iterative fibonacci" :max-attempts 5)

;; 遗传优化
(synthesize:optimize "fib"
  :population 20 :generations 10
  :fitness "(benchmark fib 10000)")

;; 管线编排
(synthesize:pipeline "build-api"
  (synthesize:fill "handler" "users" "SELECT *")
  (synthesize:define "sort" "List -> List" :prompt "quicksort")
  (rule:apply-all))
```

### 🔹 代码规范系统

```scheme
(rule:define 'guard-division
  :pattern "(/ ?x ?y)" :replace "(if (= ?y 0) 0 (/ ?x ?y))"
  :condition "(> ?y 0)" :description "Protect division by zero")

(rule:apply-all)           ; 自动修复所有违规
(rule:list-violations)     ; 审计模式，只查不改
(rule:save "rules.json")   ; 持久化
```

### 🔹 冻成原生二进制 (AOT)

```bash
./build/aura --emit-binary lib.aura main.aura app   # 多文件
echo '(+ 1 2)' | ./build/aura --emit-binary app     # 管道
./app  # → 3，不依赖 aura 本体
```

**编译管线：** `源码 → FlatAST → IRModule → LLVM IR (O2) → llc → .o → 链接 → ELF`

**支持：** 算术/比较/闭包递归/高阶/apply/列表/字符串/所有权/多文件/stdlib (56 emit ✅)

---

## 技术速览

Aura：C++26，LLVM ORC JIT 后端，Sound Gradual Typing。

| 维度 | 状态 |
|------|:----:|
| **核心求值** | Tree-walker + IR 双路径 + TCO |
| **类型系统** | Sound Gradual: coercion + occurrence + let-poly |
| **所有权** | M4 linear: move/borrow/drop, 编译期跟踪 |
| **ADT + match** | define-type / 穷尽性检查 |
| **JIT** | ORC JIT, 38 opcode → native, 7.55× vs TW, -O2 |
| **增量编译** | ArenaGroup + 缓存 + 热替换 + IR import |
| **EDSL 自修改** | query:def-use/reaches/effects + mutate:rebind/splice/wrap |
| **Workspace 分层** | create/switch/merge/lock/discard + COW |
| **Messaging** | send/recv/reply/my-id/mailbox-count/session-active? |
| **合成管线** | template/LLM/genetic/pipeline 多策略 |
| **规范系统** | rule:define/apply/save/load + scope/condition |
| **快照/diff** | ast:snapshot/restore/diff (line-level LCS) |
| **原生二进制** | LLVM IR → llc → ELF, 56 emit ✅ |
| **模块系统** | require/import + 路径解析 + 缓存 + 热重载 |
| **C FFI** | dlopen/dlsym + 类型签名 |
| **编译期反射** | P2996 auto_to_json / auto_serialize |
| **--serve 协议** | 多 session + mailbox + 超时 |



## 文档

- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/tutorial.md](docs/tutorial.md) — 教程
- [docs/design/](docs/design/) — 设计文档

## License

Apache 2.0
