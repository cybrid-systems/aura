# Aura 测试

## 目录结构

```
tests/
├── run-tests.sh           # Bash 回归测试（117 个用例）
├── edsl_benchmark.py      # LLM 代码生成基准（85 个任务）
├── run_bench_all.py       # 多模型自动对比（调用 edsl_benchmark.py）
├── build.py               # 统一测试入口 → ./build.py test [suite]
│
├── suite/                 # Aura 测试套件(.aura)
│   ├── core.aura, typesystem.aura, stdlib.aura, macros.aura
│   ├── edsl.aura, errors.aura, intent.aura, module.aura
│   └── run-tests.aura
│
├── tasks/                 # EDSL 基准任务（85 个 .aura 文件）
│   ├── adt/ algorithm/ basic/ coercion/ edsl/ ffi/
│   ├── hash/ hash-algo/ json/ list/ recursion/
│   └── string/ type/
│
├── regression/            # 已知 bug 回归测试
│   ├── 01_closure_let_dangling.aura
│   ├── 02_json_get_string.aura
│   ├── 03_div_zero_ub.aura
│   ├── 04_json_roundtrip.aura
│   ├── 05_redline.aura
│   └── 06_linear.aura
│
├── fixtures/              # 测试用夹具
│   └── basic_add.aura
│
├── agent_demo.py          # Agent 管线演示（build.py test demo）
├── ai_agent_demo.py       # AI Agent 端到端演示（build.py test ai）
├── mutation_loop.py       # 变异测试循环（build.py test mutation）
├── test_fuzz.py           # LLM 驱动模糊测试（build.py test fuzz）
├── check_gradual.py       # 渐变保证验证（build.py test gradual）
├── benchmark.py           # 编译器性能基准（build.py test bench）
│
├── test_ir.cpp            # C++ 单元测试
├── validate_node_layout.cpp  # AST 节点布局验证
│
└── import_std_demo/       # C++26 模块导入示例
    └── main.cpp
```

## 运行方式

```bash
# 构建
./build.py build

# 全部测试
./build.py check

# 指定套件
./build.py test unit        # C++ 单元测试
./build.py test integ       # 集成测试（118 个用例）
./build.py test bash        # Bash 回归（117 个用例）
./build.py test typecheck   # 类型检查
./build.py test bench       # 编译器基准
./build.py test regression  # 回归测试
./build.py test gradual     # 渐变保证
./build.py test demo        # Agent 管线演示
./build.py test ai          # AI Agent 端到端
./build.py test fuzz        # 模糊测试

# LLM 基准（需 API key）
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 3

# 多模型对比
python3 tests/run_bench_all.py
```

## 测试套件说明

| 套件 | 文件 | 说明 |
|------|------|------|
| unit | `test_ir.cpp` | 内存池、ArenaGroup、quote 等 C++ 单元 |
| integ | `build.py` 内联 | 118 个端到端管线测试 |
| bash | `run-tests.sh` | 117 个 shell 级别的回归测试 |
| bench | `benchmark.py` | 编译器性能基线 + 回归检测 |
| regression | `regression/*.aura` | 已知 bug 的回归验证 |
| gradual | `check_gradual.py` | 渐变保证验证脚本 |
| demo | `agent_demo.py` | Agent 管线演示 |
| ai | `ai_agent_demo.py` | AI Agent 端到端演示 |
| mutation | `mutation_loop.py` | 变异测试循环 |
| fuzz | `test_fuzz.py` | LLM 驱动模糊测试 |
