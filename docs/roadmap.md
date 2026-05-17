# Aura — 实现进度跟踪 / 路线图

---

## 里程碑状态

```
M1  求值器                      ✅ 纯 FlatAST 管线
M2  查询引擎 (C++)              🟡 C++ 完备，Aura 原语未暴露
M3a 语言补全                    ✅ 布尔/序对/begin/set!/quote/cond/letrec
M3b 宏系统                      ✅ defmacro + quasiquote + gensym
M3c 反射                        ✅ P2996 auto_to_json
M3d 类型系统                    ✅ L6.1-L6.8: 渐进类型 + forall + Float
M3e 工具链                      ✅ Benchmark + --serve + AI Agent
M3f AI 闭环                     🟡 Agent 演示 4 版本 + 15 任务基准
M4a 缓存                        ✅ ABF v4 列式
M4b AI 协议                     ✅ docs/ai_agent_protocol.md
M4c 模块系统                    ✅ import + AURA_PATH + require
M4d 自进化                      🟡 Typed Mutation (C++完备, 原语待扩展)
M4e 语言完善                    ✅ 变参算术/TCO/equal?/match/define-struct
M4g 标准库                      ✅ list/math/string/json/struct/validate

P6  Query/Transform EDSL 设计   🟡 docs/query_edsl_design.md
P7  Aura 原语注册               ⬜ query:* + mutate:* + set-code
P8  增量编译 + 类型系统          ⬜ Dirtiness + 增量 typecheck
P9  生产后端                     ⬜ LLVM JIT / AOT / 自举
```

## 当前能力

| 领域 | 状态 | 
|------|------|
| 语言核心 | ✅ ~70 原语 + TCO + match + define-struct |
| 标准库 | ✅ list/math/string/json/struct/validate |
| require | ✅ `(require std/list)` 符号形式 |
| PrimitiveRef | ✅ `(foldl + 0 ...)` 基元可传值 |
| --serve 协议 | ✅ JSON Lines, display 走 stderr |
| AI Agent 演示 | ✅ serve/llm/iter/edsl 4 版本 |
| 基准测试 | ✅ 15 任务并行基准 |
| QueryEngine (C++) | 🟡 完备，未暴露为 Aura 原语 |
| TransformEngine (C++) | 🟡 完备，未暴露为 Aura 原语 |
| Typed Mutation (C++) | 🟡 mutate:* 存在，需扩展 |

## 下一步计划

### P6 — Query/Transform EDSL 设计 ✅
- [x] `docs/query_edsl_design.md` — 工作区模型 + query/mutate 原语规范
- [x] 类型系统 + CaaS 增量编译集成方案
- [x] 性能估算 + 优先级

### P7 — Aura 原语注册 (核心 EDSL) ✅
- [x] `set-code` — 锁定 AST 到工作区
- [x] `eval-current` — 执行工作区 AST
- [x] `query:find name` — 按名称查找节点
- [x] `query:children node-id` — 获取子节点
- [x] `query:node node-id` — 查看节点详情
- [x] `query:calls name` — 查找函数调用
- [x] `mutate:rebind name code` — 按函数名替换定义
- [x] `mutate:replace-value` — 替换节点值
- [x] `mutate:replace-type` — 替换类型注解
- [x] `mutate:record-patch` — 记录变更
- [x] `rollback / rollback-since` — 回滚
- [x] `mutation-count / mutation-history / check-preconditions` — 查询

### P8 — 完整 EDSL + 增量编译
- [x] `query:parent` — 查找父节点
- [x] `query:siblings` — 查找兄弟节点
- [x] `query:pattern` — 模式匹配搜索（`...` 通配符，lexer 新增 Ellipsis token）
- [x] `mutate:set-body` — 按函数名替换函数体（修复：解析到工作区，跨 AST 引用 bug）
- [x] `mutate:remove-node` — 删除节点 (从父节点断开)
- [x] `mutate:rebind` — 修复：原地替换而非整体换 workspace（保留其他 mutation）
- [ ] Dirtiness 标记 (被 mutate 的节点)
- [ ] 增量类型检查 (只检查修改部分)
- [ ] `typecheck-current` 原语

### P9 — 生产后端
- [ ] LLVM JIT 降级
- [ ] --jit 模式
- [ ] AOT 编译
