# Aura

**AI-native Lisp — 代码自己进化。**

Aura 让 AI Agent 拥有在运行时**精确读写和修改自身代码**的能力。

---

## 构建

推荐在 **[cybrid-systems/dev](https://github.com/cybrid-systems/dev)** Docker 开发环境中构建。该环境预装了 GCC 16 + CMake 4.3 + LLVM 22，是 Aura（C++26 modules）目前唯一一致可重现的构建环境。

```bash
# 启动 dev 容器（挂载 aura 源码）
docker run -d -it --name aura-dev \
  --cap-add=SYS_PTRACE \
  -e USER_UID=$(id -u) \
  -e USER_GID=$(id -g) \
  -v $(pwd):/home/dev/code/aura \
  -w /home/dev/code/aura \
  ghcr.io/cybrid-systems/dev:latest

# 进入容器 → 构建 → 验证
docker exec -it -u dev aura-dev /bin/zsh -l
python3 build.py build
echo '(+ 1 2)' | ./build/aura   # → 3
```

```bash
# 常用命令
python3 build.py build           # 构建
python3 build.py check           # 全量测试
python3 build.py clean           # 清理
```

---

## 核心能力

Aura 的核心是一个**自修改 Lisp 运行时**，专为 LLM Agent 编排设计：

- **代码即数据** — 代码被表示为可查询、可变异、可版本化的 FlatAST，Agent 可以通过 `query:*` / `mutate:*` 原语在运行时精确读写自身代码
- **Agent 编排** — 通过 `agent:spawn`、`orch:conduct`、`orch:pipeline` 等原语组织多 Agent 协作管线，支持并行/串行/重试/条件分支
- **Workspace 分层** — 提供隔离的实验环境，Agent 可以在 sandbox 中安全试错，通过 `workspace:merge` 合并良好版本

其他技术细节（类型系统、IR、JIT、所有权、快照/diff、合成管线、规范系统、AOT 等）详见 [docs/](/docs)。

## License

Apache 2.0
