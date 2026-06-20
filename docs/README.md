# Aura 文档

> **真相层级**：代码 + 测试 > `(api-reference)` > 本目录 > `git tag docs-archive-pre-2026-06`

## 导航

| 我想… | 去看 |
|------|------|
| 快速上手 | [tutorial.md](tutorial.md) |
| 有哪些原语 | `(api-reference)` 或 [generated/primitives.md](generated/primitives.md) |
| 模块 / 管线 | [architecture.md](architecture.md) |
| 改 evaluator | [contributing.md](contributing.md) |
| Agent JSON 协议 | [wire-formats.md](wire-formats.md) |
| 自修改示例 | `tests/suite/mutate-structured.aura` |
| 标准库 | [generated/stdlib-index.md](generated/stdlib-index.md) · `lib/std/*.aura` |
| 构建测试 | `./build.py check` |
| 路线图 | [roadmap.md](roadmap.md) |
| 历史设计 | `git show docs-archive-pre-2026-06:docs/design/...` |

## 文档清单

| 文件 | 说明 |
|------|------|
| [tutorial.md](tutorial.md) | 可运行示例 |
| [api-reference.md](api-reference.md) | 如何查原语 |
| [generated/](generated/) | 从源码生成：primitives / modules / stdlib |
| [architecture.md](architecture.md) | 模块地图与数据流 |
| [contributing.md](contributing.md) | FlatAST 不变式、加 primitive |
| [wire-formats.md](wire-formats.md) | `--serve` / `--serve-async` |
| [roadmap.md](roadmap.md) | P 系列规划 |
| [benchmark.md](benchmark.md) | EDSL benchmark（源：`tests/benchmark.py`） |

## 构建

```bash
./build.py build
./build.py docs          # 更新 docs/generated/
./build.py check         # docs --check + 构建 + 测试
```

## 归档

PR2 删除了 `design/notes/`、`design/history/closings/`、`design/core/` 等 ~180 篇历史文档。查阅方式见 [design/history/README.md](design/history/README.md)。