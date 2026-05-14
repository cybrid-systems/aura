# Aura 类型系统 — 形式规则附录（代码状态标注版）

**关联主文档**：[aura_typesystem.md](./aura_typesystem.md)（**优先阅读，含代码状态**）
**状态**：本文档为设计参考，大部分规则尚未在代码中落地。

---

本文档保留完整的形式类型规则作为设计参考，但每个规则的实现状态已在主文档中标注。

## 实现状态速查

| 规则/机制 | 主文档状态 | 对应代码 |
|-----------|-----------|----------|
| T-Int / T-Bool / T-String / T-Var | ✅ 已实现 | `type_checker_impl.cpp` synthesize |
| T-App / T-Lambda / T-If / T-Let | ✅ 已实现 | synthesize_flat_call/lambda/if/let |
| T-Cons / T-Car / T-Cdr | ❌ 不存在 | Pair TypeTag 未注册 |
| T-Cast / Cast Gradual | ✅ 部分实现 | `add_coercion` + runtime CastNode |
| T-String? / occurrence 规则 | ✅ 已实现 | `analyze_predicate_flat` |
| 一致性关系 T ~ T | ✅ 已实现 | `is_coercible` / `consistent_unify` |
| 与函数子类型 | ⚡ 仅 func 分解 | `consistent_unify` 的 func branch |
| Let-Poly / T-Var-Inst | ❌ 不存在 | `is_poly` 字段未使用 |
| Gradual Guarantee | ❌ 无定理证明 | 纯设计文档 |
| 约束求解 + Unification | ✅ 已实现 | `ConstraintSystem::solve` |
| Coercion 插入算法 | ⚡ 内联实现 | 无独立 Pass |
| 线性所有权规则 | ❌ 不存在 | 纯规划 |

## 历史定位

本文档 v0.1-v0.3（2026-05-12）是类型系统设计阶段产出，当时大部分代码未实现。
v0.4（2026-05-14）主文档经过代码审计，标记了每部分的真实状态。
本文档作为形式规则的历史参考保留，但**实现决策以主文档和代码为准**。
