# Aura

**AI-native Lisp — 代码自己进化。**

Aura 是一个为 LLM Agent 量身打造的**自修改 Lisp 运行时**，让代码在运行时成为可精确查询、可原子变异、可版本回滚的一等公民。

---

## 30 秒 Self-Modifying Agent Quickstart

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```scheme
;; 1. 将源码装入可变异工作区（FlatAST）
(set-code "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))")

;; 2. 结构化查询：定位所有递归调用点
(query:calls "fact")              ; → (5)

;; 3. 精确重写：把递归改成迭代（带摘要）
(mutate:rebind "fact"
  "(define (fact n)
     (let loop ((n n) (acc 1))
       (if (= n 0) acc (loop (- n 1) (* acc n)))))"
  "fact 改迭代")

;; 4. 增量验证（仅脏 define 重新编译执行）
(eval-current)
(display (fact 10))               ; → 3628800
```

完整端到端示例（含多 Agent 管线、snapshot/rollback、serve 协议）见
[`docs/tutorial.md`](docs/tutorial.md)。

所有原语的权威实现状态与交叉引用见：
- [`docs/design/core/query_edsl.md`](docs/design/core/query_edsl.md)（查询 EDSL）
- [`docs/design/core/mutate_api.md`](docs/design/core/mutate_api.md)（结构化变异）
- [`docs/design/core/agent_orchestration.md`](docs/design/core/agent_orchestration.md)（Agent 编排）
- [`docs/api-reference.md`](docs/api-reference.md)（中央 Primitives Surface，含代码位置）

---

## 构建

```bash
# 推荐容器环境（GCC 16 + CMake 4.3 + LLVM 22）
docker run -d -it --name aura-dev --cap-add=SYS_PTRACE \
  -e USER_UID=$(id -u) -e USER_GID=$(id -g) \
  -v $(pwd):/home/dev/code/aura -w /home/dev/code/aura \
  ghcr.io/cybrid-systems/dev:latest

docker exec -it -u dev aura-dev /bin/zsh -l
python3 build.py build
echo '(+ 1 2)' | ./build/aura   # → 3
```

原生构建同样支持（需对应工具链）。

---

## 核心技术特性

Aura 的运行时把**程序本身**暴露为可被 Agent 精确操控的活系统：

- **FlatAST 作为一等可变异状态**  
  源码被解析为可直接查询（`query:*`）、原子修改（`mutate:*`）、快照回滚（`ast:snapshot` / `ast:rollback`）的结构化表示。变异后仅受影响的 define 增量重编译（`eval-current` 通常 1–5ms）。

- **EDSL 驱动的自演化闭环**  
  Agent 可通过 `query:where` / `query:pattern` 等谓词精确导航 AST，再用 `mutate:query-and-replace`、`mutate:extract-function` 等原语进行结构化重构。所有操作默认事务性 + 并发安全（`workspace_mtx_` + per-symbol DefUse 失效 + fiber MutationBoundary）。

- **多层 Workspace 与 COW 隔离**  
  支持子工作区创建、切换、只读锁定、内存预算控制。COW 机制让多 Agent 协作与分支实验成为零拷贝操作。

- **Agent 编排原语**  
  `agent:spawn` / `agent:ask` + `orch:conduct` / `orch:pipeline` / `orch:parallel`（真并行）。底层由多线程 work-stealing fiber scheduler + mailbox 驱动，支持跨 session 消息传递。

- **高性能执行管线**  
  树遍历解释器 + IR + LLVM ORC JIT（38 opcode 完整 native 化，实测 ~7.55× 加速）。支持增量 cache（source-hash 失效）与热更新。AOT 路径正在演进中。

- **Sound Gradual Typing + 运行时反射**  
  渐进类型系统（occurrence typing、Let-Poly、ADT 穷尽性）以 warnings-only 方式集成，同时暴露 `query:type`、`ast:defs` 等反射原语供 Agent 使用。

更多实现细节、当前状态矩阵与历史决策见 [`docs/README.md`](docs/README.md)（已完成系统性整理：core/ 为活文档，history/ 为归档）。

给 evaluator 添加新原语的开发者文档：[`docs/developer/evaluator.md`](docs/developer/evaluator.md)。

## License

Apache 2.0
