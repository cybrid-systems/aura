# Aura 测试

## 目录结构（2026 结构优化）

```
tests/
├── domain/                # ★ 首选：C++ 领域套件（issue = 标签）
│   ├── cases/             #   表驱动 case（obs_schema_cases.hpp 等）
│   ├── test_domain_*.cpp
│   └── test_obs_schema_matrix.cpp
│
├── suite/                 # Aura 语言 E2E（.aura）— 注意是单数 suite
├── suites/                # 重定向说明 → domain/（勿再往这里加文件）
│
├── fixtures/              # issues_fast / link profiles / integ JSON
├── bundles/               # 生成的 bundle main（gen_issue_bundles.py）
├── regression/            # 已知 bug 回归（.aura）
├── tasks/                 # EDSL 基准任务
│
├── test_issue_*.cpp       # 遗留：历史 issue 二进制（只减不增）
├── test_ir.cpp            # C++ 单元（IR 等）
├── test_concurrent.cpp
│
├── run_issue_tests.py     # issue/domain 统一 runner
├── issue_tier.py          # fast/full 档
├── fixture_check.py
└── *_harness* / fuzz / bench / demos …
```

### 新测试落点（强制偏好）

| 类型 | 落点 |
|------|------|
| `query:*` schema / stats 门禁 | `domain/cases/` + `test_obs_schema_matrix` |
| Fiber / hygiene / typed 行为 | `domain/test_domain_*.cpp` |
| Aura 语言行为 | `suite/*.aura` 或 fixtures JSON |
| 真·单测（IR/AST） | `test_ir.cpp` 等 unit，或新 `test_<topic>.cpp` |
| **禁止默认** | `test_issue_<N>.cpp` |

详见 [`domain/README.md`](domain/README.md) 与 [`docs/contributing.md`](../docs/contributing.md)。

## 运行方式

```bash
# 构建
./build.py build

# 全部测试
./build.py check

# 指定套件
./build.py test unit        # C++ 单元测试
./build.py test integ       # 集成测试
./build.py test issues      # issue/domain runner（AURA_ISSUES_TIER）
./build.py test issues-fast # PR fast 档

# 领域套件（日常推荐）
ninja -C build test_obs_schema_matrix test_domain_fiber_orchestration
./build/test_obs_schema_matrix
```

## Issue 档位

| 环境变量 | 含义 |
|----------|------|
| `AURA_ISSUES_TIER=fast` | `fixtures/issues_fast.json` + git 变更 |
| `AURA_ISSUES_TIER=full` | `all_test_issue_targets`（bundle + domain + 少量 standalone） |

## 历史说明

旧 README 中的 `edsl_benchmark` / bash 回归数字会随时间漂移；以
`./build.py list` 与 CI 配置为准。
