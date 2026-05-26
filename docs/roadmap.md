# Aura 路线图

**更新：2026-05-26 — 全部 P0-P5 完成**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites 通过 |
| 编译器 Bug | ✅ 0 个 open |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| AOT emit 测试 | ✅ 57/57 全绿 |
| Benchmark 自托管 | ✅ `tests/bench.aura` |

## 已完成

### 编译器 Bug 修复
- 类型标注 binding, FFI closure dispatch, pipe mode 报错, if-no-else 条件求值, rest-arg 空参
- 常量文件夹 tagged bool (AOT 兼容), if_false 测试预期

### AOT 深入修复
- Fixnum/布尔 tagged 值 (OpMul/Div/Eq/And/Or/Not/Branch)
- string 显示 STRING_BIAS, NumberToString fixnum 解码
- runtime.c IS_PAIR 替换 val<0 (修复 apply/reverse/range/unique)
- Float 显示 + float pool + 算术 (OpAdd/Sub/Mul/Div)
- 常量文件夹 tagged bool 传播修复 (IS_TRUTHY)

### 统一值表示
- EvalValue 从 std::variant<16> 改为 int64_t pointer tagging
- AOT ↔ evaluator 零转换 — filter/map/= 正确工作
- 修复 permutations segfault (P4)

### 内建 benchmark
- 135 tasks 从 JSON 加载, synthesize:test-driven 管线
- 多轮 + 结果聚合 + 表格输出 (P5)
