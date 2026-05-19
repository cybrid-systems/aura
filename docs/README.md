# Aura 文档

## 用户文档
| 文档 | 说明 |
|------|------|
| [tutorial.md](./tutorial.md) | 10 分钟快速入门 |
| [roadmap.md](./roadmap.md) | 路线图与状态 |
| [known_issues.md](./known_issues.md) | 已知问题 |

## 设计文档 (`docs/design/`)
| 文档 | 说明 |
|------|------|
| [type system](./design/aura_typesystem.md) | Sound Gradual Typing 完整设计 |
| [type system formal](./design/aura_typesystem_formal.md) | 形式规则附录 |
| [LLVM JIT](./design/llvm_jit.md) | ORC JIT 后端设计 |
| [C FFI](./design/ffi_c.md) | C 互操作 (c-load/c-func) |
| [hygienic macros](./design/hygienic_macros.md) | 卫生宏实现 |
| [AST validation](./design/ast_validate.md) | 编译期 AST 验证 |
| [IR import](./design/ir_import.md) | IR 级模块导入 |
| [IR pipeline](./design/ir_pipeline_design.md) | IR 管线设计 |
| [error handling](./design/error_handling.md) | try/catch/raise |
| [macro system v2](./design/macro_system_v2.md) | 宏系统 |
| [module/namespace](./design/module_namespace_design.md) | import/export |
| [query/EDSL](./design/query_edsl_design.md) | EDSL AST 变换 |
| [typed mutation](./design/typed_mutation_design.md) | 类型安全变换 |
| [testing framework](./design/testing_framework_design.md) | 测试框架 |
| [formatter](./design/formatter_design.md) | `--fmt` 格式化器 |
| [CaaS serve](./design/caas_integration.md) | `--serve` 协议 |
| [kernel standard](./design/kernel_iteration_standard.md) | Trees That Grow 标准 |
| [C++26 guide](./design/cpp26_guide.md) | C++26 编程指南 |
| [language plan](./design/aura_language_plan.md) | Ghuloum 扩展计划 |

## 外部设计仓库
完整设计哲学、架构、序列化协议：  
https://github.com/cybrid-systems/ai-programming-language-design
