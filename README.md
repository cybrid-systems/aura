# Aura

**AI-native Lisp — 代码自己进化。**

Aura 让 AI Agent 拥有在运行时**精确读写和修改自身代码**的能力。
不是"让 LLM 输出文本然后粘贴"——而是把代码变成一块可查询、可变异、可版本化的活体 AST。

---

## 构建

### 环境要求

Aura 使用 **C++26 modules**（`ixx` 文件）和 CMake 4.3+，对工具链和 CMake 版本要求较高。
推荐在 **[cybrid-systems/dev](https://github.com/cybrid-systems/dev)** Docker 开发环境中构建，该环境提供了完整的构建依赖。

| 依赖 | 版本 | 提供方 |
|------|------|--------|
| CMake | ≥ 4.3 (推荐 4.3.2) | dev 环境预装 |
| Ninja | ≥ 1.12 | dev 环境预装 |
| GCC | ≥ 16 | dev 环境预装 |
| LLVM | ≥ 22 (ORC JIT, 可选) | dev 环境预装 |
| Python | ≥ 3.11 | dev 环境预装 |

> **为什么需要 cybrid-systems/dev？** Aura 重度依赖 C++26 模块支持，目前只有 **GCC 16** 和 **Clang 22+**（借助 clang-scan-deps）能完整编译。`dev` 镜像提供了 Ubuntu 26.04 + GCC 16 + LLVM 22.1.6 的黄金组合，跨 arm64/x64 一致可重现。

### 使用 Docker 环境构建

```bash
# 1. 启动 dev 容器（挂载 aura 源码）
docker run -d -it --name aura-dev \
  --cap-add=SYS_PTRACE \
  -e USER_UID=$(id -u) \
  -e USER_GID=$(id -g) \
  -v $(pwd):/home/dev/code/aura \
  -w /home/dev/code/aura \
  ghcr.io/cybrid-systems/dev:latest

# 2. 进入容器
docker exec -it -u dev aura-dev /bin/zsh -l

# 3. 构建
python3 build.py build

# 4. 验证
echo '(+ 1 2)' | ./build/aura              # → 3
```

### 在 dev 容器外手动构建

如果要在非 dev 环境中构建，需要确保以下工具链可用：

```bash
# 安装系统级依赖（Ubuntu 26.04 示例）
sudo apt-get install -y \
  build-essential cmake ninja-build \
  gcc-16 g++-16 \
  python3 python3-pip

# 可选：安装 LLVM 22 启用 JIT
# 参见 dev 仓库的 install-llvm.sh 脚本

# 确保 CMake ≥ 4.3（Ubuntu 26.04 仓库可能不够新，需要源码安装）
wget -q https://github.com/Kitware/CMake/releases/download/v4.3.2/cmake-4.3.2.tar.gz
tar -zxf cmake-4.3.2.tar.gz && cd cmake-4.3.2
./bootstrap --parallel=$(nproc) --prefix=/usr/local && make -j$(nproc) && sudo make install

# 选型：指定 GCC 16（系统默认可能是其他版本）
export CC=gcc-16
export CXX=g++-16

# 构建
python3 build.py build
```

### 常用构建命令

```bash
# 构建可执行文件
python3 build.py build

# 管道模式（一行代码）
echo '(+ 1 2)' | ./build/aura              # → 3

# 交互式 REPL
./build/aura

# 运行测试
python3 build.py test core                  # 核心管线：单元 + 集成 + 类型 + 套件
python3 build.py test all                   # 全部测试
python3 build.py check                      # 全量 CI（构建 + 核心 + 安全回归 + fuzz）

# 清理
python3 build.py clean
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



## EDSL Benchmark

135 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL 自修改、Workspace、ADT、所有权、Synthesize 管线。  
自适应迭代修正（intend 模式，最多 3 次重试 + ant colony 零 LLM 变异修复）。

| 模型 | 任务数 | 通过 | 通过率 | 耗时 | 运行方式 |
|:----|:-----:|:----:|:-----:|:----:|:--------|
| 🥇 **Grok (xAI)** | 135 | **135** | **100%** | ~2min | Aura-native, 6 workers |
| 🥈 **DeepSeek v4 Flash** | 135 | 103-106 | 76-79% | ~3-25min | Python (103) / Aura-native (106) |
| 🥉 **MiniMax M2.7** | 135 | 29 | 21.5% | ~8min | Python runner (variance±13%) |

> Aura-native 和 Python runner 的通过率一致，差异在 LLM 方差范围内。  
> Grok 首次生成通过率 ~22%，ant colony 零 LLM 开销修复剩余 78%。

### 运行方式

**Aura-native（完全自托管，推荐）：**
```bash
# 单机串行（用于调试）
LLM_API_KEY="***" BENCH_LIMIT=10 ./build/aura < tests/bench.aura

# 多进程并行
LLM_API_KEY="***" bash tests/run_parallel.sh 6
```

**Python runner（多模型对比）：**
```bash
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 2
LLM_API_KEY="***" LLM_MODEL="grok-beta" LLM_BASE_URL="https://api.x.ai/v1" python3 tests/edsl_benchmark.py
```

## 文档

- [docs/benchmark.md](docs/benchmark.md) — Benchmark 详细历史和数据分析
- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/tutorial.md](docs/tutorial.md) — 教程
- [docs/design/](docs/design/) — 设计文档

## License

Apache 2.0
