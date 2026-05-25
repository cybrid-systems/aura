# Aura 路线图

**更新：2026-05-25 (AOT 完整管线完成)**

---

## AOT 测试覆盖：56 emit 全部通过

```
算术:    + - * / 链式
比较:    = < > <= >=
逻辑:    and or not
类型:    pair? null?
对:      cons car cdr (含 improper list)
列表:    list length list-ref reverse append member
         map filter foldl apply named-let
字符串:  string-length string=? string-append string-ref
         string<? number->string string->number
条件:    if let
闭包:    lambda closure (含自引用递归 + 环境捕获)
原语:    作为闭包值传递 (+ - * / = < > <= >= not)
所有权:  drop move borrow linear
高阶:    map filter foldl append member apply permutations
IO:      display (列表/嵌套/improper 格式化)
多文件:  ./aura --emit-binary a.aura b.aura out

stdlib 已验证:
  algorithm: sorted? merge-sorted binary-search unique combinations
             permutations sort-stable min-by max-by
  list:      map filter foldl range take drop
  math:      factorial sin cos (需 -lm 链接)
  maybe/csv/set/queue: 基本导入编译 ✅
```

## 技术要点

**编译管线：**
```
源码 → FlatAST → IRModule → FlatFunction
  → LLVM IR (O2) → .ll → llc -filetype=obj → .o
  → 链接 runtime.c → 独立 ELF
```

**关键修复：**
- 原语派发表: null?/pair?/cons/car/cdr/length/reverse/append/member/map/filter/foldl/list/display
- aura_closure_call: 创建 locals 数组填充 env[0..N] + args[0..N]
- 函数名唯一化: `__lambda__` → `__lambda___0`, `__lambda___1`, ...
- import 内联: 绕过 cache_module 保守 FnCheck → 模块源码直接拼接到输入
- apply: 遍历参数列表收集元素后调用 aura_closure_call

## 剩余工作

| 优先级 | 任务 | 说明 |
|:------:|:-----|------|
| 🟢 | struct 模块 AOT | 使用 `define-type` (EDSL)，IR 路径不支持 |
| 🟢 | LSP / 包管理 / 自举 | 独立长期项目 |
| 🟢 | 性能优化 (O3/LTO) | 功能完整后再做 |

## 已完成

- P0 (05-23): 核心求值 + 类型系统 + ADT + EDSL
- P1 (05-23): IR + Pass Manager + 增量编译
- P2 (05-23~25): JIT + 真实 AOT 编译器 + stdlib 集成
