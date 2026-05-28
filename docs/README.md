# Aura 文档

## 概况

| 文档 | 说明 |
|------|------|
| [教程](tutorial.md) | 上手实践，从零开始 |
| [API 参考](api-reference.md) | 所有原语参考 |
| [路线图](roadmap.md) | 发展方向 |
| [基准](benchmark.md) | EDSL benchmark 数据 |
| [哲学](philosophy.md) | Aura 的设计思想 |

## 设计文档 (design/)

| 领域 | 文档 |
|------|------|
| **语言** | [语法规格](design/aura_language_spec.md)、[语言规划](design/aura_language_plan.md)、[C++26 指南](design/cpp26_guide.md) |
| **类型系统** | [类型系统设计](design/aura_typesystem.md)、[形式化](design/aura_typesystem_formal.md) |
| **ORM 管线** | [IR 管线](design/ir_pipeline_design.md)、[IR import](design/ir_import.md) |
| **LLVM JIT** | [JIT 设计](design/llvm_jit.md)、[二进制运行时](design/binary_runtime_plan.md)、[binary drop/heap](design/binary_drop_heap.md) |
| **EDSL & AST** | [查询 EDSL](design/query_edsl_design.md)、[变更 API](design/mutate_api.md)、[类型化变异](design/typed_mutation_design.md)、[AST 验证](design/ast_validate.md) |
| **Agent 编排** | [Agent 编排](design/agent_orchestration.md)、[意图编排](design/intent_orchestration.md)、[Inter-Agent 通信](design/inter_agent_messaging.md) |
| **分析 & 控制** | [def-use 分析](design/defuse_analysis.md)、[蚁群控制器](design/ant_colony_controller.md)、[PID 自适应](design/adaptive_intend_pid.md)、[蚁群架构](design/ant_architecture.md) |
| **Workspace** | [Workspace 分层](design/workspace_layering.md)、[双 Arena](design/double-arena.md)、[值编码](design/value_encoding.md) |
| **合成** | [合成策略](design/synthesize_strategies.md)、[管线策略](design/pipeline_strategy.md)、[错误处理](design/error_handling.md) |
| **模块 & 宏** | [模块命名空间](design/module_namespace_design.md)、[Functor 模块](design/functor_modules.md)、[卫生宏](design/hygienic_macros.md)、[macro v2](design/macro_system_v2.md) |
| **C FFI** | [FFI 设计](design/ffi_c.md)、[运行时 C 测试](design/runtime_c_tests.md) |
| **LLM 集成** | [LLM stdlib](design/llm_stdlib.md)、[Fuzz 测试](design/llm_fuzz_testing.md)、[CaaS 集成](design/caas_integration.md) |
| **其他** | [编译期反射](design/compile_time_reflection.md)、[规范系统](design/rule_normalize.md)、[格式化器](design/formatter_design.md)、[测试框架](design/testing_framework_design.md)、[可演化策略](design/e4_evolvable_strategies.md)、[capability/functor 集成](design/capability_functor_integration.md)、[核迭代标准](design/kernel_iteration_standard.md) |

## 其他

| 文档 | 说明 |
|------|------|
| [async serve 设计](async_serve_design.md) | 多 session serve 架构 |
| [synthesize pipeline v2](synthesize-pipeline-v2.md) | 合成管线 v2 |
| [inspect](inspect.md) | P2996 编译期反射自省 |

## 构建

构建指南见[项目 README](../README.md#构建)。
