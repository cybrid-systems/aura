# Aura 模块系统 + ABF v2 序列化设计

**版本**: v1.0
**状态**: 设计阶段
**对应**: M3e — ABF v2 + M4 — 模块系统

---

## 1. 模块系统设计

### 1.1 核心概念

```
一个模块 = 一个 .aura 文件
一个库 = 一组模块的集合 (一个目录)

.aura 文件:
  (define pi 3.14159)
  (define square (lambda (x) (* x x)))
  
  (define circle-area (lambda (r) (* pi (square r))))
```

模块边界定义：**一个文件就是一个模块**。没有显式的 `module`/`import` 声明——文件内的 define 就是该模块的公共 API。

### 1.2 模块解析和 import

```
import 机制 (语法级):

  (import "math.aura")           ← 相对路径
  (import "/usr/lib/aura/io.aura") ← 绝对路径
  (import (std "io"))             ← 标准库命名
  (import (lib "json"))           ← 包管理器命名
  
展开后:
  (let ((imported-module (load-module "math.aura")))
    ;; 使用 imported-module 中定义的函数
    (imported-module 'square 5))
```

实际上，import 是宏：

```
(defmacro import (path)
  `(let ((__module__ (load-module ,(resolve-path path))))
     __module__))
```

`load-module` 是一个内建函数（primitive），它：
1. 检查模块缓存 (.aura_cache 文件)
2. 如果缓存存在且未过期 → mmap 加载
3. 如果缓存不存在 → parse + compile + cache
4. 返回一个模块对象（实际是环境/闭包表）

### 1.3 模块缓存管线

```
source.aura                     ← 源代码
    │
    ├── 首次: parse → compile → cache
    │                           │
    │                      .aura_cache    ← mmap-ready 二进制
    │                           │
    └── 以后: → 检查 mtime → mmap → 直接用
    
    
.aura_cache 包含:
  - FlatAST 数据 (列式 SoA dump)
  - 字符串池 (StringPool dump)
  - IR 缓存 (已编译的函数)
  - 元数据 (编译时间、源文件 mtime、依赖列表)
```

### 1.4 模块间依赖

```
math.aura ──→ circle.aura ──→ app.aura
    │              │              │
    ▼              ▼              ▼
  .aura_cache   .aura_cache    .aura_cache

当 math.aura 变化时:
  → circle.aura 的缓存标记为 stale
  → app.aura 的缓存标记为 stale
  → 下次使用时重新编译 (利用增量编译的依赖追踪)
```

### 1.5 命名空间和标准库

```
查找路径:
  1. 当前目录
  2. AURA_PATH 环境变量 (类似 PYTHONPATH)
  3. 标准库路径 ~/.aura/std/

标准库布局:
  std/
    io.aura          ← read-file, write-file, display
    list.aura        ← map, filter, fold, zip
    math.aura        ← sin, cos, sqrt, pi
    json.aura        ← parse, stringify
```

---

## 2. ABF v2 设计

### 2.1 设计目标

```
1. 零拷贝加载: mmap → 直接使用
2. 列式布局: SoA 的每列连续存储
3. 版本校验: 格式版本号 + 内容 hash
4. 增量更新: 只重写变化的列
5. 跨平台: 固定字节序 (小端)
```

### 2.2 文件格式

```
┌──────────────────────────┐
│ Header (64 bytes)         │
│   magic: "AURACACHE\0"    │
│   version: 2              │
│   num_nodes: uint32       │
│   num_strings: uint32     │
│   num_functions: uint32   │
│   node_data_offset: uint64│
│   string_data_offset: u64 │
│   ir_data_offset: uint64  │
│   source_mtime: uint64    │
│   content_hash: uint64    │
│   padding: 8 bytes        │
├──────────────────────────┤
│ Node Data (列式)           │
│   tag: uint8[1/节点]      │
│   padding: 7 bytes         │
│   int_val: int64[8/节点]   │
│   sym_id: uint32[4/节点]   │
│   child_begin: uint32[...] │
│   child_count: uint32[...] │
│   ... 每列连续存储          │
├──────────────────────────┤
│ String Data               │
│   offset: uint32[1/字符串] │
│   length: uint32[1/字符串] │
│   data: char[...]          │
├──────────────────────────┤
│ IR Cache Data             │
│   num_functions: uint32   │
│   function_data[...]       │
└──────────────────────────┘
```

### 2.3 头结构

```cpp
// 文件头, 64 字节对齐
struct CacheHeader {
    char     magic[8];           // "AURACACHE"
    uint32_t version;            // 2
    uint32_t num_nodes;          // FlatAST 节点数
    uint32_t num_strings;        // 字符串池大小
    uint32_t num_functions;      // IR 缓存函数数
    
    uint64_t node_offset;        // 节点数据偏移
    uint64_t string_offset;      // 字符串数据偏移
    uint64_t ir_offset;          // IR 缓存偏移
    
    uint64_t source_mtime;       // 源文件修改时间
    uint64_t content_hash;       // 内容哈希 (排除 header)
    uint8_t  reserved[8];        // 保留
};
// 总大小: 64 字节
```

### 2.4 节点数据布局

FlatAST 的 SoA 列直接 dump：

```cpp
struct NodeData {
    // 固定大小列 (每节点固定字节数)
    std::vector<uint8_t>   tags;        // 1 字节/节点
    std::vector<int64_t>   int_vals;    // 8 字节/节点
    std::vector<uint32_t>  sym_ids;     // 4 字节/节点
    std::vector<uint32_t>  child_begins; // 4 字节/节点
    std::vector<uint32_t>  child_counts; // 4 字节/节点
    std::vector<uint32_t>  param_begins; // 4 字节/节点
    std::vector<uint32_t>  param_counts; // 4 字节/节点
    std::vector<uint32_t>  type_ids;    // 4 字节/节点
    std::vector<uint32_t>  lines;       // 4 字节/节点
    std::vector<uint32_t>  cols;        // 4 字节/节点
    
    // 变长列 (总大小 = 最后一个偏移 + 最后一个 count)
    std::vector<NodeId>    child_data;  // 4 字节/子节点
    std::vector<SymId>     param_data;  // 4 字节/参数
};
```

写文件时：

```cpp
void write_node_data(std::ofstream& f, const FlatAST& flat) {
    // 1. 写入固定大小列 (连续排列)
    write_vector(f, flat.tags_);      // 列 0
    write_vector(f, flat.int_val_);   // 列 1
    write_vector(f, flat.sym_id_);    // 列 2
    // ... 直接 memcpy
    
    // 2. 写入变长列 (child_data, param_data)
    write_vector(f, flat.child_data_);
    write_vector(f, flat.param_data_);
}
```

### 2.5 加载：零拷贝

```cpp
struct MappedCache {
    const CacheHeader* header;  // mmap 头部
    const uint8_t*     tags;     // 直接指向 mmap 区域
    const int64_t*     int_vals;
    const SymId*       sym_ids;
    // ...
    
    static std::optional<MappedCache> open(const std::string& path) {
        int fd = open(path.c_str(), O_RDONLY);
        struct stat st;
        fstat(fd, &st);
        
        void* data = mmap(nullptr, st.st_size, PROT_READ,
                          MAP_PRIVATE, fd, 0);
        close(fd);
        
        auto* header = static_cast<const CacheHeader*>(data);
        // 校验 magic + version
        
        MappedCache cache;
        cache.header = header;
        
        // 指针直接指向 mmap 区域
        auto* nd = static_cast<const uint8_t*>(data) + header->node_offset;
        cache.tags     = nd;
        cache.int_vals = reinterpret_cast<const int64_t*>(nd + flat_size * sizeof(uint8_t) + pad);
        cache.sym_ids  = ...;  // 每列按偏移寻址
        
        return cache;
    }
    
    // 直接读节点, 不需 parse
    NodeView get(NodeId id) const {
        return NodeView{
            .tag       = static_cast<NodeTag>(tags[id]),
            .int_value = int_vals[id],
            .sym_id    = sym_ids[id],
            .line      = lines[id],
            .col       = cols[id],
            .children  = span(child_data + child_begins[id], child_counts[id]),
            .params    = span(param_data + param_begins[id], param_counts[id]),
        };
    }
};
```

### 2.6 FlatAST 序列化 API

```cpp
// 在 compiler/types 或新模块 aura.compiler.cache 中

// 将 FlatAST + 缓存写为 .aura_cache 文件
bool write_cache(const std::string& path,
                 const FlatAST& flat,
                 const StringPool& pool,
                 uint64_t source_mtime);

// 从 .aura_cache 文件加载 (mmap, 零拷贝)
// 返回 FlatAST 兼容的 NodeView 访问器
std::optional<MappedCache> open_cache(const std::string& path);
```

### 2.7 与增量编译的集成

```
编译管线 + 缓存:
  
  source.aura ──parse──→ FlatAST
      │                       │
      │                  lower_to_ir
      │                       │
      │                   IR Cache
      │                       │
      └──→ write_cache ──→ .aura_cache
  
  加载管线:
  
  .aura_cache ──mmap──→ MappedCache ──→ NodeView (直接读)
      │                                        │
      │                                   typecheck / lower
      │                                        │
      └──→ IR Cache (已包含) ─────────→ exec
```

### 2.8 过期检测

```cpp
// 当源文件修改时, 缓存过期
bool cache_is_fresh(const std::string& cache_path, 
                    const std::string& source_path) {
    struct stat cache_st, source_st;
    stat(cache_path.c_str(), &cache_st);
    stat(source_path.c_str(), &source_st);
    
    // 比较 mtime: 缓存必须比源文件新
    return cache_st.st_mtime >= source_st.st_mtime;
}
```

---

## 3. 编译器集成

### 3.1 新模块和文件

```
src/compiler/
├── cache.ixx              ← ABF v2 序列化/反序列化
├── cache_impl.cpp         ← 实现
├── evaluator.ixx          ← 树遍历求值器 (已改名)
├── ir_lowering.cpp        ← IR 降低 (已改名)
├── ir_executor.ixx        ← IR 执行 (已改名)
└── ...
```

### 3.2 CLI

```bash
# 编译 + 缓存
echo '(define pi 3.14)' | ./aura --cache math.aura

# 使用缓存
echo '(import "math.aura")' | ./aura

# 查看缓存信息
./aura --cache-info math.aura.cache
```

### 3.3 CompilerService 集成

```cpp
class CompilerService {
    // 现有的增量编译缓存
    std::unordered_map<std::string, IRCacheEntry> ir_cache_;
    
    // 新增: 模块缓存
    std::unordered_map<std::string, MappedCache> module_cache_;
    
    // 加载模块 (优先 mmap 缓存)
    MappedCache* load_module(const std::string& path) {
        auto cache_path = path + ".aura_cache";
        
        // 检查缓存是否新鲜
        if (cache_is_fresh(cache_path, path)) {
            auto mc = open_cache(cache_path);
            if (mc) {
                module_cache_[path] = *mc;
                return &module_cache_[path];
            }
        }
        
        // 缓存不存在或过期: 解析 + 编译 + 缓存
        auto alloc = arena_.allocator();
        StringPool pool(alloc);
        FlatAST flat(alloc);
        auto pr = parse_to_flat(read_file(path), flat, pool);
        
        // 编译
        auto ir_mod = lower_to_ir(flat, pool, arena_);
        
        // 写缓存
        write_cache(cache_path, flat, pool, source_mtime(path));
        
        // mmap 加载
        module_cache_[path] = *open_cache(cache_path);
        return &module_cache_[path];
    }
};
```

---

## 4. 与现有系统的关系

```
现有组件                      模块系统中的作用
────────────────────────────────────────────────
incremental_caas.md          Phase 3 设计文档
ir_cache_                    模块 IR 缓存
dep_graph_                   模块间依赖追踪
try_extract_define           模块的公共 API 提取
unparse_node                 模块导出 S-表达式 (调试用)
ABF v2 (本设计)              模块的二进制缓存格式
build.py                     cmake --build + cache
```

---

## 5. 实现计划

### Phase 1: FlatAST 列式写入 (1d)

```
- 实现 write_cache: FlatAST 列 dump → 二进制文件
- 实现 open_cache: mmap → MappedCache
- 测试: write → mmap → read → compare NodeView
```

### Phase 2: 字符串池 + IR 缓存 (0.5d)

```
- 序列化 StringPool
- 序列化 IRFunction 缓存
- 测试: 全量 roundtrip
```

### Phase 3: 模块加载 (0.5d)

```
- load_module primitive
- import 宏
- 过期检测
- 依赖追踪跨模块
```

### Phase 4: CLI 集成 (0.5d)

```
- --cache 标志
- --cache-info 标志
- serve 协议扩展
```

---

## 6. 设计决策

### 为什么是列式而不是行式？

SoA FlatAST 是列式的。按列写 = 连续 memcpy = 最接近零拷贝。
按行写 = 需要 walk 树 + 序列化每个节点 = 有转换开销。

### 为什么 mmap 而不是 read + parse？

mmap:
- 不需要解析
- 操作系统中止时自动回收
- 多个进程共享同一映射
- 页面按需加载（大文件不需要全读入）

### 为什么保留源文件 mtime 而不是用 hash？

mtime 检查是 O(1)。hash 是 O(file size)。对于频繁的"编译/运行"循环，mtime 足够。

### 固定字节序？

小端。x86 和 ARM 都是小端。如果未来需要支持大端平台，加一个字节序标志。

---

## 7. 未解决的问题

1. **模块间交叉引用**：模块 A 使用模块 B 的类型/函数，如何保证类型一致性？
2. **循环依赖**：A 依赖 B, B 依赖 A — 如何检测和拒绝？
3. **标准库升级**：`std/io.aura` 更新后，所有依赖它的模块都需要重编译，如何保证平滑？
4. **安全**：加载外部 .aura_cache 文件时，如何防止恶意构造的数据造成缓冲区溢出？
