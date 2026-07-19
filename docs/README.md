# Aura 文档

代码 + 测试 > `(api-reference)` > 本目录。

## 导航

| 我想… | 去看 |
|------|------|
| 模块 / 管线 | [architecture.md](architecture.md) |
| 改 evaluator / 加 primitive | [contributing.md](contributing.md) |
| Agent JSON 协议 | [wire-formats.md](wire-formats.md) |
| 有哪些原语 | `(api-reference)` 或 [generated/primitives.md](generated/primitives.md) |
| 标准库 | [generated/stdlib-index.md](generated/stdlib-index.md) · `lib/std/*.aura` |
| 构建 / 测试 / gate | `./build.py gate` · `./build.py check` |

## 构建

```bash
./build.py build
./build.py docs          # 更新 docs/generated/
./build.py check         # docs --check + 构建 + 测试
```