# CSV Query Tool — Aura 实战项目

一个用 Aura 写的 CSV 解析和查询工具。验证语言在实际数据处理场景中的表现。

## 文件

- `csv.aura` — 核心库（CSV 解析/列选择/行过滤/序列化）

## 使用

```bash
export AURA_PATH="../../lib:."
cd projects/csv-query
echo '{"cmd":"exec","code":"(import \"csv\") (csv-parse (read-file \"/tmp/test.csv\"))"}' | ../../build/aura --serve
```

## 测试数据

```bash
printf 'name,age,city\nalice,30,beijing\nbob,25,shanghai\ncarol,35,beijing\n' > /tmp/test.csv
```

## 支持的操作

- `(csv-parse text)` — 解析 CSV 字符串为行列表
- `(csv-headers data)` — 获取表头行
- `(csv-rows data)` — 获取数据行
- `(csv-select data col-names)` — 按列名选择
- `(csv-filter data col value)` — 按列值过滤行
- `(csv-unparse data)` — 转回 CSV 字符串

## 验证结果

全部操作 2026-05-16 验证通过 ✅
