# Aura 文档

**语言为 AI 而生。**

---

Aura 不是给人类写的语言——  
它是给 AI 写的语言。

它把 AST 变成 AI 可以直接思考和重写的记忆，  
让代码第一次拥有了自我演化的能力。

---

## 快速入口

| 文档 | 适合谁 |
|------|--------|
| **[哲学](philosophy.md)** | 想理解 Aura 为什么存在的人 |
| **[教程](tutorial.md)** | 想上手试的人 |
| **[基准](benchmark.md)** | 想看数据的人 |
| **[路线图](roadmap.md)** | 想看方向的人 |
| **[语法规格](design/aura_language_spec.md)** | 需要完整参考的人 |

## 核心思想

Aura 的出发点是三个洞察：

1. **LLM 不是工具，是同伴。**  
   语言应该为 LLM 的自然输出做兼容，而不是让 LLM 适应语言。

2. **代码应该能修改自己。**  
   EDSL 让 AI 可以直接操作 AST——读、改、验证，闭环。

3. **控制论比蛮力更优雅。**  
   用距离测量 + 信息素 + 蚁群搜索代替"重新生成直到对"。

[深入 → 哲学文档](philosophy.md)

## 快速开始

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2 3)' | ./build/aura          # → 6
echo '(display (list 1 2 3))' | ./build/aura    # → (1 2 3)
echo '(display (map square (list 1 2 3)))' | ./build/aura
  # → (1 4 9) (with std/math)
echo '(type-of 42)' | ./build/aura           # → Int
```

```bash
LLM_API_KEY="***" python3 tests/edsl_benchmark.py
```

## 项目结构

```
docs/           文档入口
├── README.md    ← 你在这里
├── philosophy.md    核心思想
├── tutorial.md      上手实践
├── benchmark.md     基准数据
├── roadmap.md       发展方向
├── known_issues.md  当前局限
├── inspect.md       P2996 编译期反射自省
└── design/          深度设计文档
    ├── ant_colony_controller.md     蚁群控制器
    ├── ant_architecture.md          解耦架构
    ├── adaptive_intend_pid.md       PID 控制理论
    ├── aura_language_spec.md        语法规格
    ├── aura_typesystem.md          类型系统设计
    ├── ffi_c.md                     C FFI
    ├── hygienic_macros.md           卫生宏
    ├── ir_pipeline_design.md        IR 管线
    ├── llvm_jit.md                  LLVM JIT
    ├── query_edsl_design.md         EDSL 查询
    ├── typed_mutation_design.md     类型化变异
    └── ...
```
