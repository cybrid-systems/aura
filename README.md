# Aura

**AI-native Lisp — 代码自己进化。**

Aura 把**程序本身**变成运行时可被 Agent 精确操控的活系统。代码不再是静态文本，而是可查询、可原子变异、可版本演化的第一公民。

---

## 30 秒 Self-Modifying Agent Quickstart

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```scheme
(set-code "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))")
(query:calls "fact")                    ; 定位递归点
(mutate:rebind "fact" "..." "迭代化")   ; 精确重写
(eval-current)
```

完整示例（多 Agent 管线 + snapshot/rollback + serve 协议）见 [`docs/tutorial.md`](docs/tutorial.md)。

权威原语状态与实现映射见：
- [`docs/design/core/query_edsl.md`](docs/design/core/query_edsl.md)
- [`docs/design/core/mutate_api.md`](docs/design/core/mutate_api.md)
- [`docs/api-reference.md`](docs/api-reference.md)（中央 Surface）

---

## 构建

```bash
docker run -d -it --name aura-dev --cap-add=SYS_PTRACE \
  -e USER_UID=$(id -u) -e USER_GID=$(id -g) \
  -v $(pwd):/home/dev/code/aura -w /home/dev/code/aura \
  ghcr.io/cybrid-systems/dev:latest

docker exec -it -u dev aura-dev /bin/zsh -l
python3 build.py build
```

---

## 哲学与能力

Aura 的核心是让代码**自己进化**：

- **代码即活数据**：FlatAST 在运行时可被 `query:*` 精确导航、`mutate:*` 原子修改、`ast:snapshot/rollback` 版本控制。变异后仅脏部分增量重编译。
- **自演化闭环**：Agent 通过结构化查询与重构原语（`mutate:query-and-replace`、`extract-function` 等）实现局部搜索与精确演化，无需手动 patch。
- **原生 Agent 编排**：`agent:spawn/ask` + `orch:conduct/pipeline/parallel` 直接内建于语言，底层 fiber scheduler + mailbox 提供并发与消息基础。
- **极简而强大**：只暴露必要的查询与变异原语，最大化 LLM 自然表达力，同时提供事务性、并发安全与增量执行保障。

更多当前状态与设计细节见 [`docs/README.md`](docs/README.md)（core/ 为活文档，history/ 为归档）。

开发者扩展文档：[`docs/developer/evaluator.md`](docs/developer/evaluator.md)。

## License

Apache 2.0
