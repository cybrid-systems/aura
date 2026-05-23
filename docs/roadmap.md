# Aura 路线图

**更新：2026-05-23**

---

## 🔴 开放 TODO

所有之前标记的 P0/P1/P2 项经逐一审查均已实现。当前无开放 issue。

---

## Aura 能力评估

### 能做的（生产级可用）

| 能力 | 说明 |
|------|------|
| 函数式编程 | 递归/lambda/let/letrec/宏 ✅ 85 任务通过 89-92% |
| 类型系统 | Sound Gradual: coercion / occurrence / let-poly / blame ✅ |
| ADT + match | (define-type) + 穷尽性检查 ✅ |
| M4 线性所有权 | move / borrow / drop 编译期 + 运行时 ✅ |
| 标准库 | 28 模块: string/list/hash/math/JSON/CSV/IO/socket/datetime ✅ |
| EDSL 自修改 | set-code → mutate → eval-current colony:search ✅ |
| C FFI | c-func dlopen/dlsym, Float/Int/String marshalling ✅ |
| TCP 网络 | tcp-connect/send/recv/close, http-get/post ✅ |
| 文件 I/O | read-file/write-file/file-exists?/file-copy/directory-list ✅ |
| 项目级 | try-catch / command-line / shell / command-output ✅ |
| 模块系统 | require/import 路径解析/缓存/循环检测/export 过滤/热重载 ✅ |
| 增量编译 | ArenaGroup + 磁盘缓存 + IR import + hot-swap ✅ |
| 编译期反射 | P2996 auto_to_json / auto_serialize / P1306 递归序列化 ✅ |
| 数值计算 | lib/std/vector-math.aura (向量/矩阵运算) ✅ |

### 不能做的（真正缺口）

| 缺口 | 影响 | 修复难度 |
|------|------|:--------:|
| **生成二进制** | 不能编译 standalone 可执行文件，只能 `./build/aura --serve` | 中 (LLVM) |
| **包管理** | 无包 registry/依赖声明 | 远用 |
| **IDE/LSP** | 无语法高亮/补全/诊断 | 远用 |
| **自举** | 编译器用 C++26 写，不用 Aura | 远用 |

### 关于生成二进制的思考

**现状：** Aura 以 `./build/aura --serve` 方式运行，代码通过 stdin JSON 传入。
没有 `./build/aura -o myapp input.aura` 这样的编译命令。

**需要什么：**
1. LLVM IR 输出到 `.o` 文件（已有 JIT codegen，38 条 opcode）→ 1-2d
2. 链接器调用（调用 `ld` / `gcc` 生成 ELF）→ 1d
3. 运行时环境（GC / stdlib 静态链接）→ 1d

**优先级：低。** 对 AI agent 场景（serve + REPL）没价值。
agent 不需要 standalone binary，但生产部署需要。

### 规模评估

| 场景 | 是否可行 | 说明 |
|------|:--------:|------|
| 50 行算法函数 | ✅ 非常稳 | Grok 91.8% 通过率 |
| 200 行单文件工具 | ✅ 可以 | 文件 I/O + CLI + try-catch 都有了 |
| 500 行多文件项目 | ⚠️ 勉强 | 模块系统有但缺 IDE 支持 |
| 1000+ 行编译器/服务器 | ❌ 痛苦 | 没 LSP/包管理/调试器 |

### 和同类对比

| | Aura | Scheme | Python | OCaml |
|--|:----:|:------:|:------:|:-----:|
| 类型系统 | Sound Gradual | Typed Racket | Dynamic | HM + module |
| EDSL 自修改 | ✅ 核心设计 | ❌ | ❌ | ❌ |
| 增量热更新 | ✅ | ❌ | ❌ | ❌ |
| stdlib | 够用 28 模块 | 全面 | 庞大 | 全面 |
| 项目级工具 | ❌ LSP/包管理 | ❌ 类似 | ✅ VS Code/PyPI | ✅ |
| 成熟度 | 6 个月 | 30 年 | 30 年 | 30 年 |

---

## 已解决（2026-05-23）

- **P0 缺陷 (5)** — blame / eval_flat / TypeAnnotation / parser / #\\<procedure\\>
- **P1 功能 (5)** — match 穷尽 / 模块类型 / M4 / parser / 增量检查
- **P2 增强 (5)** — --inspect / serve 超时 / P2996 / colony Phase 1-2 / tweak-literal  
- **P3 下沉 (2)** — pid:analyze / pure Aura colony:search
- **诊断 (4)** — FFI→stdout / ((: x Int)) lambda 参数 / closure warning→stdout / FFI 签名精确定位
- **原语 (3)** — command-line / shell / command-output
- **数值计算** — lib/std/vector-math.aura (217 行)
- **测试** — 47 条回归全绿 + 12 套测试套件 + 4 个新 suite 文件
