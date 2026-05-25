# Aura 路线图

**更新：2026-05-25 (2.5h AOT 攻坚完成)**

---

## AOT 测试覆盖：54 emit 全部通过

```
算术:  + - * / 链式
比较:  = < > <= >=
逻辑:  and or not
类型:  pair? null?
对:    cons car cdr
列表:  list length list-ref reverse append member
       map filter foldl apply named-let
字符串: string-length string=? string-append string-ref
       string<? number->string string->number
条件:  if let
闭包:  lambda, closure, 原语传递 (+ - * / = < > <= >= not)
所有权: drop move borrow linear
高阶:  map filter foldl append member apply permutations
IO:    display (含列表格式化)
stdlib: sorted? merge-sorted binary-search unique combinations
       min-by max-by sort-stable
多文件: ./aura --emit-binary a.aura b.aura out
```

### 技术要点

**编译管线：** `源码 → FlatAST → IRModule → FlatFunction → LLVM IR (O2) → .ll → llc -filetype=obj → .o → 链接 runtime.c → 独立 ELF`

**运行时：** 单个 `lib/runtime.c` 提供 bump allocator、pair/cell/closure heap、string pool、drop 函数族、PrimId/原语派发表、func_table、作用域 env 设置。

**关键修复链：**
- 原语派发表扩展 → `null?/pair?/cons/car/cdr/length/list-ref/reverse/append/member/map/filter/foldl/display/list`
- `aura_closure_call` env 设置 → named let + 递归闭包
- 函数名唯一化 → 多 lambda 不冲突
- import 源码内联 → 绕过 cache_module 的保守 FnCheck
- `apply` runtime 实现 → permutations + 高阶

**54 emit，106 core，全部通过。**
