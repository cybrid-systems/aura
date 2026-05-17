# Aura — 实现进度跟踪 / 路线图

---

## 里程碑状态

```
M1  求值器                      ✅ 纯 FlatAST 管线
M2  查询引擎 (C++)              ✅ 已暴露为 Aura query:* 原语
M3a 语言补全                    ✅ 布尔/序对/begin/set!/quote/cond/letrec
M3b 宏系统                      ✅ defmacro + quasiquote + gensym
M3c 反射                        ✅ P2996 auto_to_json
M3d 类型系统                    ✅ L6.1-L6.8: 渐进类型 + forall + Float
M3e 工具链                      ✅ Benchmark + --serve + AI Agent
M3f AI 闭环                     🟡 Agent 演示 + EDSL 工作流
M4a 缓存                        ✅ ABF v2 列式
M4b AI 协议                     ✅ docs/ai_agent_protocol.md
M4c 模块系统 (v1)               ✅ import + AURA_PATH + require（注入式）
M4d 自进化                      ✅ Typed Mutation + EDSL query/mutate/typecheck
M4e 语言完善                    ✅ 变参算术/TCO/equal?/match/define-struct
M4g 标准库                      ✅ list/math/string/json/struct/validate

M5  模块命名空间 (v2)           🟡 Phase 1 in progress
P6  Query/Transform EDSL        ✅
P7  EDSL 原语注册               ✅
P8  增量编译                    ✅ (除增量求值)
P9  生产后端                    ⬜ LLVM JIT / AOT / 自举
P10 大规模开发基础设施           🟡 设计完成，逐步实现
```

## 当前能力

| 领域 | 状态 | 
|------|------|
| 语言核心 | ✅ ~70 原语 + TCO + match + define-struct |
| 标准库 | ✅ list/math/string/json/struct/validate |
| require | ✅ `(require std/list)` 符号形式 |
| 模块系统 | 🟡 注入式 → 命名空间迁移中 |
| 增量 typecheck | ✅ 跳过 clean 子树 + 缓存 |
| EDSL (query/mutate) | ✅ 完整工作区模型 |
| --serve 协议 | ✅ JSON Lines, display 走 stderr |
| AI Agent 演示 | 🟡 serve/llm/iter/edsl 4 版本 |
| 基准测试 | ✅ 15 任务并行基准 |

## 下一步计划

### M5 — 模块命名空间 v2 🟡

**目标：** 从注入式模块升级到带前缀的命名空间模块。

设计文档: `docs/module_namespace_design.md`

#### Phase 1: 模块对象 + module-get (当前)
- [x] 设计文档
- [ ] EvalValue 新增 `<module>` 变体
- [ ] `module-get` / `module-keys` / `module?` 原语
- [ ] `load-module` 返回模块对象（同时保留旧注入行为）
- [ ] 新增 `format_value` 对 `<module>` 的支持

#### Phase 2: 前缀注入
- [ ] `(import path prefix:)` 语法变换
- [ ] 内部展开: `(define prefix:name (module-get mod 'name))`
- [ ] `(use path)` — 返回模块对象，不注入

#### Phase 3: 显式导出
- [ ] `(export sym ...)` 语法
- [ ] `load-module` 只暴露 export 的符号
- [ ] 未 export 的函数对其他模块不可见

#### Phase 4: 标准库迁移
- [ ] std/list → require 默认加前缀
- [ ] 所有 std 库改用前缀
- [ ] 保留 `all:` 兼容开关

### P10 — 大规模开发基础设施

设计文档:
- `docs/error_handling_v2.md` — try/catch/raise
- `docs/testing_framework_design.md` — check/check=/test-suite
- `docs/formatter_design.md` — --fmt CLI

| 项 | 优先级 | 状态 |
|----|--------|------|
| try/catch (error handling) | P0 | 设计完成 |
| check/check= 断言 | P1 | 设计完成 |
| test-suite / run-tests | P2 | 设计完成 |
| --fmt 格式化 | P3 | 设计完成 |
| 增量求值 | P4 | 低优先级 |

### P9 — 生产后端
- [ ] LLVM JIT 降级
- [ ] --jit 模式
- [ ] AOT 编译
