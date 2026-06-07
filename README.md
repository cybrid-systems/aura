# Aura

**AI-native Lisp — 代码自己进化。**

Aura 让 AI Agent 拥有在运行时**精确读写和修改自身代码**的能力。

---

## 30 秒 Self-Modifying Agent Quickstart

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```scheme
;; 1. 装入代码到工作区
(set-code "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))")

;; 2. 找递归调用
(query:calls "fact")              ; → (5)

;; 3. 改成迭代版
(mutate:rebind "fact"
  "(define (fact n)
     (let loop ((n n) (acc 1))
       (if (= n 0) acc (loop (- n 1) (* acc n)))))"
  "fact 改迭代")

;; 4. 验证
(eval-current)
(display (fact 10))               ; → 3628800
```

完整 Quickstart（含 Agent 编排 + snapshot/rollback）见
[`docs/tutorial.md §10.5`](docs/tutorial.md)。
所有原语的完整 API 见 [`docs/design/query_edsl_design.md`](docs/design/query_edsl_design.md) /
[`docs/design/mutate_api.md`](docs/design/mutate_api.md) /
[`docs/design/agent_orchestration.md`](docs/design/agent_orchestration.md)。

---

## 构建

```bash
docker run -d -it --name aura-dev --cap-add=SYS_PTRACE -e USER_UID=$(id -u) -e USER_GID=$(id -g) -v $(pwd):/home/dev/code/aura -w /home/dev/code/aura ghcr.io/cybrid-systems/dev:latest
docker exec -it -u dev aura-dev /bin/zsh -l
python3 build.py build
echo '(+ 1 2)' | ./build/aura   # → 3
```

环境：`cybrid-systems/dev`（GCC 16 + CMake 4.3 + LLVM 22）。

---

## 核心能力

Aura 的核心是一个**自修改 Lisp 运行时**，专为 LLM Agent 编排设计：

- **代码即数据** — 代码被表示为可查询、可变异、可版本化的 FlatAST。Agent 通过 `query:*` / `mutate:*` 原语在运行时精确读写自身代码，通过 `ast:snapshot` / `ast:rollback` 管理版本
- **Agent 编排** — 通过 `agent:spawn`、`orch:conduct`、`orch:pipeline`、`orch:parallel` 原语组织多 Agent 协作管线，支持并行/串行/重试/条件分支
- **增量编译** — EDSL V2 cache 用 source-hash 失效，mutate 后只重 build 受影响的 define（`eval-current` ~1-5ms）
- **并发安全** — `workspace_mtx_` shared/exclusive + per-sym defuse staleness（#107）；多线程 fiber scheduler + work-stealing（#109）
- **JIT/AOT** — LLVM ORC JIT 后端（38 opcode → native，~7.55× 加速），AOT 二进制 emit

其他技术细节（类型系统、IR、JIT、所有权、合成管线、AOT 等）详见 [docs/](/docs)。
给 evaluator 加新原语（开发文档）见 [`docs/developer/evaluator.md`](docs/developer/evaluator.md)。

## License

Apache 2.0
