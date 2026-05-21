# --inspect: Compiler Self-Inspection

`--inspect` 使用 P2996 编译期反射 dump 编译器内部状态为 JSON。

## 用法

```bash
# 默认: dump IRModule 为 JSON
echo '(+ 1 2)' | ./aura --inspect

# 显式指定模式
echo '(+ 1 2)' | ./aura --inspect ir         # IRModule JSON (默认)
echo '((lambda (x) (+ x 1)) 41)' | ./aura --inspect closures  # 闭包快照
echo '(+ 1 2)' | ./aura --inspect cache       # 缓存函数摘要

# 所有模式
echo '(+ 1 2)' | ./aura --inspect all
```

## IR 模式 (`--inspect ir`)

输出整个 IRModule 的 JSON 表示，包含所有函数、基本块、指令、操作数。

示例输出:
```json
{
  "functions": [{
    "id": 0,
    "name": "__top__",
    "entry_block": 0,
    "blocks": [{
      "id": 0,
      "instructions": [
        {"opcode": 1, "operands": [1,1,0,0], "source_ast_node_id": 0, "type_id": 0},
        {"opcode": 20, "operands": [0,0,0,0], "source_ast_node_id": 0, "type_id": 0}
      ],
      "successors": []
    }],
    "params": [],
    "free_vars": [],
    "local_count": 3,
    "arg_count": 0,
    "variadic": false
  }],
  "closure_bridge": [{"flat": null, "pool": null, "body_id": 4294967295, "body_source": ""}],
  "entry_function_id": 0,
  "string_pool": []
}
```

## 闭包模式 (`--inspect closures`)

输出闭包包总数和各闭包的简要信息（函数 ID、名称、env 大小）。

## 缓存模式 (`--inspect cache`)

输出 `ir_cache_` 中缓存的函数数量。

## 实现机制

1. `reflect.hh` 的 `to_json<T>()` 使用 `template for` (P1306) + P2996 反射递归序列化任何类型
2. `ir_reflect_serialize.cpp` 提供 C-linkage `aura_inspect_ir_json()`，在镜像 IR struct 上调用 `to_json`
3. `main.cpp` 处理后 eval → 调用 inspect → 输出 JSON

## 未来扩展

- [ ] `--inspect typecheck` 类型推断状态 dump
- [ ] `--inspect evaluator` 树遍历器环境 dump
- [ ] `--inspect pretty` 格式化 JSON 输出
- [ ] 与 `--cache-open` 组合: 从缓存恢复 IRModule 然后 inspect
