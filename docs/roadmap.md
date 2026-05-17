# Aura — 实现进度跟踪 / 路线图

---

## 里程碑状态

```
M1  求值器                      ✅ 纯 FlatAST 管线
M2  查询引擎                    ✅ query:* 原语 (2026.05.16)
M3a 语言补全                    ✅ 布尔/序对/begin/set!/quote/cond/letrec
M3b 宏系统                      ✅ defmacro + quasiquote + gensym
M3c 反射                        ✅ P2996 auto_to_json
M3d 类型系统                    ✅ L6.1-L6.8: 渐进类型 + forall + Float
M3e 工具链                      ✅ Benchmark + --serve + AI Agent
M3f AI 闭环                     🟡 Agent + EDSL + MiniMax/M2.7
M4a 缓存                        ✅ ABF v2 列式
M4b AI 协议                     ✅ docs/ai_agent_protocol.md
M4c 模块系统 (v1)               ✅ import + AURA_PATH + require
M4d 自进化                      ✅ EDSL + typed mutation + dirty tracking
M4e 语言完善                    ✅ 变参算术/TCO/equal?/match/define-struct
M4g 标准库                      ✅ 7 libs with export declarations

M5  模块命名空间 v2             ✅ 全部 4 Phase (2026.05.17)
P8  增量编译                    ✅ dirty + 增量 typecheck
P10 大规模开发基础设施           🟡 核心完成，部分待打磨
```

## 当前能力

| 领域 | 状态 | 
|------|------|
| 语言核心 | ✅ ~75 原语 + TCO + match + define-struct |
| 标准库 | ✅ list/math/string/json/struct/validate/test + export 声明 |
| 模块系统 | ✅ 前缀注入 + 导出控制 + 循环检测 + 自动 lib 发现 |
| 增量 typecheck | ✅ 跳过 clean 子树 + 持久 TypeRegistry |
| 错误处理 | ✅ try/catch/raise + error/assert 不崩溃 |
| 测试框架 | ✅ check/check= + test-suite 宏 |
| EDSL | ✅ 15+ query/mutate 原语 + workspace 模型 |
| --serve 协议 | ✅ JSON Lines, display 走 stderr |
| AI Agent | 🟡 MiniMax M2.7 测试通过，prompt 待打磨 |

## 下一步

### 短期（让 500 行项目可写）

| 项 | 工作量 | 影响 |
|----|--------|------|
| 基础原语抛 Error（/ 0 → error，file-not-found → error） | ~30 行 | 🔴 高：LLM 生成代码常出错 |
| std/test.aura 完善 + run-tests 实现 | ~30 行 | 🟡 中：让 LLM 自测 |
| `--fmt 格式化` CLI | ~100 行 | 🟡 中：写出来的代码规范 |
| `--check` 死代码/未绑定变量检查 | ~50 行 | 🟢 低：nice to have |
| 标准库补全（io、fs、regex） | ~200 行 Aura | 🟢 低：按需加 |

### 中期（让 1000+ 行项目可维护）

| 项 | 说明 |
|----|------|
| 增量 typecheck 深度优化 | 当前已跳过 clean 子树，但未 skip 整个模块 |
| IR 管线补全 | IR executor 已存在但几乎没人用 |
| LLVM JIT | 长期，现在没必要 |
| 自举 | 用 Aura 写 Aura 编译器，极远期 |

### 当前瓶颈

```
1. 原语容错  — / 0 返回 0 而不是 error，file-error 崩进程
2. 测试运行  — check= 可用，但 run-tests 还没完整
3. 格式化    — 没有 --fmt，LLM 写的代码缩进不一
4. 实战验证  — 还没有人用 Aura 写过一个完整的 500 行项目
```
