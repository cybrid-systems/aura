# Data Pipeline Tool Test

## 测试结果

```
=== Data Pipeline ===
Headers: (product price quantity city date)
Rows: 8
--- Numeric Column Stats ---
price: (10.5 25 142 8 17.75 7.25)
quantity: (50 200 905 8 113 52.8488)
---
Written to /tmp/output.csv
```

✅ 8 行数据处理完成
✅ numeric columns 统计正确
✅ CSV 输出正确
✅ 所有原语正常工作（regex-split, string-trim, read-file, write-file, foldl, map, filter）

## 发现的问题

1. `string->number` 对 "2024-01-15" 返回 2024（部分解析）——需要 stricter parsing？
2. `foldl` 参数顺序 (acc elem) 容易混淆——LLM 常写反
3. Agent 处理 >5 函数项目时 LLM 调用易超时
