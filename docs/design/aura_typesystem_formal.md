# Aura 类型系统 — 形式规则附录（代码状态标注版）

**关联主文档**：[aura_typesystem.md](./aura_typesystem.md)（**优先阅读，含代码状态**）
**状态**：本文档为设计参考，大部分规则已在代码中落地。

---

本文档保留完整的形式类型规则作为设计参考，每个规则的实现状态如下。

## 实现状态速查

| 规则/机制 | 主文档状态 | 对应代码 |
|-----------|-----------|----------|
| T-Int / T-Bool / T-String / T-Var | ✅ 已实现 | `type_checker_impl.cpp` synthesize |
| T-App / T-Lambda / T-If / T-Let | ✅ 已实现 | synthesize_flat_call/lambda/if/let |
| T-BiDirectional Check-mode | ✅ 已实现 | If/Let/Begin/Annotation/Set/Define |
| T-Cons / T-Car / T-Cdr | ✅ 已实现 | ADT 构造函数类型推断 |
| T-Cast / Cast Gradual | ✅ 已实现 | `is_coercible` + CastOp 插入 |
| T-String? / occurrence 规则 | ✅ 已实现 | `analyze_predicate_flat` (含 and/or) |
| 一致性关系 T ~ T | ✅ 已实现 | `is_coercible` / `consistent_unify` |
| Coercion soundness | ✅ 已实现 | is_coercible 扩展, Float↔Int 转换 |
| 与函数子类型 | ✅ 已实现 | `consistent_unify` 函数分解分支 |
| Let-Poly / T-Var-Inst | ⚡ 部分实现 | `is_poly` 字段存在，let 泛化未启用 |
| Union-Find 约束求解 | ✅ 已实现 | `ConstraintSystem::solve` multi-pass |
| ADT 类型推断 | ✅ 已实现 | forall-wrapped 多态构造函数, match |
| DeadCoercionElimination | ✅ 已实现 | IR 级 Pass, 消除冗余 CastOp |
| Blame 结构化 | ✅ 已实现 | BlameParty/BlameInfo 枚举 |
| Value Restriction | ✅ 已实现 | `syntactic_value` 检查 |
| Gradual Guarantee 测试 | ✅ 已实现 | 10+ 测试用例 |
| 线性所有权规则 | ✅ 已实现 | `(Linear T)` formal type, OwnershipEnv |
| Module type checking | 🟡 待实现 | import/require 大多数是 Dynamic |

## 历史定位

本文档 v0.1-v0.3（2026-05-12）是类型系统设计阶段产出，当时大部分代码未实现。
v0.4（2026-05-14）主文档经过代码审计，标记了每部分的真实状态。
v0.5（2026-05-23）T1-T3 计划全部完成，ADT 类型推断、Union-Find、Blame、Coercion 全路径均已实现。

本文档作为形式规则的历史参考保留，但**实现决策以主文档和代码为准**。
