# Aura API Reference

## 内建原语 (C++ 层)

### 语言核心
`+ - * / = < > <= >= abs` `and or not` `cons car cdr set-car! set-cdr!` `list pair? null?` `append reverse length map filter foldl` `equal? eq?` `display write newline` `read read-line` `eval` `apply` `error` `gensym` `lambda` `let letrec` `quote` `if cond` `begin` `define`

### 类型
`type?` `type-of` `number?` `integer?` `float?` `boolean?` `string?` `char?` `pair?` `symbol?` `procedure?` `void?` `eof-object?` `list?` `hash?` `vector?` `module?` `keyword?`

### 数学
`sin cos tan asin acos atan` `exp log log10 pow sqrt` `floor ceil round` `quotient remainder modulo` `max min` `gcd lcm` `inexact->exact` `pi` `rand rand-int`

### 字符串
`string-length string-ref string-append string<? string=?` `string->list list->string` `string-join string-split string-trim` `substring string-copy string-fill!` `string->number number->string` `char->integer integer->char` `char-alphabetic? char-numeric? char-whitespace? char-upcase char-downcase`

### 向量
`make-vector vector vector-length vector-ref vector-set! vector->list list->vector`

### Hash
`hash hash-set! hash-ref hash-remove! hash-keys hash-values hash-length hash-has-key?`

### I/O
`read-file write-file file-exists? file-size file-copy file-delete` `directory-list` `command-line` `shell command-output` `format`

### Module
`import load-module use module-get module-keys` `check-module-signature`

### 类型系统
`declare-type` `typecheck-current` `generate-type-sigs`

### 代码自修改 (EDSL)

> **实现状态提示**（最新）：C++ 层 EDSL 原语（query/mutate/ast/workspace）基本完整，详见各 `design/core/*` 的 `## 0. Implementation Status`。Aura std/ 层主要是便利 helper（例如 `std/query.aura` 只包少数组合）。**以 design/core/ §0 表格 + 本页 “EDSL Primitives Surface” 为准**。Serve 协议（`--serve` / `--serve-async`）是 Agent 使用 EDSL 的主要方式，支持 typed-mutate、mutation-log 等。

**代码加载**
`set-code` `current-source` `eval-current` `eval-current-output`

**查询**
`query:find` `query:children` `query:parent` `query:siblings` `query:root` `query:node` `query:node-type` `query:pattern` `query:where` `query:filter` `query:by-marker` `query:macro-introduced` `query:calls` `query:def-use` `query:reaches` `query:effects` `query:build-index` `query:index-stats`

**变异**
`mutate:rebind` `mutate:set-body` `mutate:replace-type` `mutate:replace-value` `mutate:splice` `mutate:wrap` `mutate:remove-node` `mutate:insert-child` `mutate:move-node` `mutate:tweak-literal` `mutate:replace-pattern` `mutate:query-and-replace` `mutate:record-patch` `mutate:rename-symbol` `mutate:extract-function` `mutate:inline-call`

**版本化**
`ast:snapshot` `ast:restore` `ast:list-snapshots` `ast:diff` `ast:summary` `mutation-count` `mutation-history` `rollback rollback-since`

**Workspace**
`workspace:create` `workspace:delete` `workspace:switch` `workspace:current` `workspace:list` `workspace:lock` `workspace:unlock` `workspace:discard` `workspace:merge` `workspace:can-write?` `workspace:sync-from` `workspace:memory-used` `workspace:memory-limit` 等

完整 Workspace P0（COW 分层、read-only、memory budget）+ 更高层状态见 [`design/core/workspace_layering.md`](design/core/workspace_layering.md)。

### EDSL Primitives Surface — 中央参考
此部分作为 **自修改核心的统一表面入口**，链接设计文档、实现代码、std 包装和测试。

**关键 EDSL 原语 + 引用**（完整列表见上；实现主要在 `src/compiler/evaluator_impl.cpp`）：

- **加载/执行**：`set-code` (~L5422), `eval-current` (~L5818), `eval-current-output` (~L5975) — 见 [`design/core/query_edsl.md`](design/core/query_edsl.md) §1 和 workspace_layering。
- **Query**：`query:find` (~L6036), `query:where` (~L6483), `query:pattern` (~L6725), `query:def-use` (~L9696) 等 — C++ 层 11+ 原语完整；Aura 包装有限（`std/query.aura`）。详见 [`design/core/query_edsl.md`](design/core/query_edsl.md) §0（Implementation Status 表）。
- **Mutate**：`mutate:rebind` (~L6200), `mutate:query-and-replace` (~L5154), `mutate:extract-function` (~L11319), `mutate:rename-symbol` (~L11176) 等 12+ — 原子 + rollback。见 [`design/core/mutate_api.md`](design/core/mutate_api.md) 和 [`typed_mutation.md`](design/core/typed_mutation.md)。
- **版本/AST**：`ast:snapshot` (~L10004), `ast:restore` (~L10226), `ast:defs` (~L10143) — 见 typed_mutation 和 query。
- **Workspace**：`workspace:create` (~L11689), `workspace:merge` (~L12075), `workspace:lock` 等 — 见 [`design/core/workspace_layering.md`](design/core/workspace_layering.md) §0。

**Serve 协议支持**：`--serve` / `--serve-async` 中的 `mutate` / `typed-mutate` / `mutation-log` / `rollback` / `query` 命令（main.cpp 实现）。Agent 主要交互入口。

**std/ 包装（Aura 层 helper）**：见下文 EDSL 模块。推荐直接用原语 + serve 协议进行复杂操作。

更多：测试覆盖在 `tests/suite/` 和 fuzzer；开发者扩展见 `docs/developer/evaluator.md`。

### Agent 编排

**注意**：高层 `agent:*`（spawn/ask/list/status/stop/restart）和 `orch:*`（define-role / pipeline / conduct / parallel / if / retry）主要由 `std/orchestrator.aura` 提供（纯 Aura 层，零 C++ 依赖）。底层 fiber / mailbox / send/recv 由 C++ 实现。

**Agent（高层推荐）**
`agent:spawn` `agent:ask` `agent:list` `agent:status` `agent:stop` `agent:restart`

**底层 / 内部**
`_agent:spawn` `_agent:list` `_agent`（通常不直接使用）

**Fiber**
`fiber:spawn` `fiber:join` `fiber:yield`

**Session**
`session:create` `session-active?` `send` `recv` `reply` `my-id` `mailbox-count`

完整编排模型与当前实装状态见 [`design/core/agent_orchestration.md`](design/core/agent_orchestration.md) 的 Implementation Status 表格。

### 合成
`synthesize:register-template` `synthesize:fill` `synthesize:list-templates` `synthesize:define` `synthesize:optimize`

### 反思
`compile:status` `coverage-report` `intend` `intend-analytics` `intend-history` `api-reference` `define-strategy` `strategy-field` `strategy-set-field!` `strategy-inspect` `register-strategy!` `diagnose`

### C FFI
`c-alloc` `c-free` `c-func` `c-load` `c-opaque` `c-opaque->int` `c-struct-ref` `c-struct-set!` `c-struct-size`

### 其他
`gc` `gc-freeze` `gc-heap` `gc-stats` `gc-temp` `getenv` `timestamp` `while` `for` `check` `check=` `check-success` `check-preconditions` `raise` `rollback-since` `json-parse` `json-encode` `json-get-string` `http-get` `http-post` `regex-match? regex-find regex-replace regex-split` `tcp-connect tcp-send tcp-recv tcp-close` `keyword? keyword->string`

---

## 标准库 (std/)

### 基础
| 模块 | 导出 |
|------|------|
| `std/list` | `foldr map for-each member? zip zip3 take skip take-while drop-while partition sort range sum product last flatten intersperse` |
| `std/string` | `string-split string-split-words string-join string-trim string-upcase string-downcase string-contains? string-prefix? string-suffix? string-replace string-pad-left string-pad-right string-reverse string-repeat string->chars chars->string string-take string-drop` |
| `std/math` | `square cube clamp avg atan2 positive? negative? zero? gcd lcm prime? factors fibonacci factorial expt nPr nCr sum product mean median mode variance stddev range percentile normalize min-list max-list` |
| `std/io` | `file-exists? file-size file-copy file-delete` |
| `std/fs` | `path-join path-dirname path-basename path-extname` |
| `std/json` | `json-parse json-stringify json-escape json-value` |
| `std/combinators` | `identity const flip compose thrush curry rcurry complement` |
| `std/encoding` | `hex-encode hex-decode base64-encode base64-decode` |
| `std/random` | `make-random random-next random-integer random-float random-range random-vector shuffle` |

### 数据结构
| 模块 | 导出 |
|------|------|
| `std/data` | `make-trie trie-insert trie-search trie-prefix? trie-keys` |
| `std/set` | `set set-add set-remove set-member? set-empty? set-union set-intersect set-difference set->list list->set set-size set-subset? set-equal?` |
| `std/hash` | `hash-set hash-ref hash-get hash-remove hash-empty? hash-merge frequencies` |
| `std/queue` | `enqueue dequeue queue-front queue-rest queue-empty? queue-length` |
| `std/stack` | `stack-push stack-pop stack-top stack-empty? stack-length` |
| `std/maybe` | `maybe? maybe-ref maybe-default map-maybe filter-maybe` |

### 算法
| 模块 | 导出 |
|------|------|
| `std/algorithm` | `sort-stable sort-by binary-search merge-sorted` |
| `std/iter` | `any? every? find find-index count occurrences group-by split-at hash-map hash-filter hash-vals hash-merge-into hash-keys-sorted vector-map vector-reverse vector-slice iterate unfold iota` |

### 网络
| 模块 | 导出 |
|------|------|
| `std/net` | `http-get-json http-post-json url-encode url-decode url-join fetch-try` |
| `std/socket` | `tcp-connect tcp-send tcp-recv tcp-close` |
| `std/regex` | `re-match? re-find re-replace re-split re-quote` |
| `std/uuid` | `uuid uuid-compact uuid-nil` |

### 时间
| 模块 | 导出 |
|------|------|
| `std/datetime` | `leap-year? days-in-month pad2 day-of-week time-diff month-name weekday-name format-time` |

### EDSL — 代码自修改（Aura 层 helper）
| 模块 | 导出 | 设计参考 |
|------|------|----------|
| `std/query` | `query:filter query:uncalled query:callers-of` | [`design/core/query_edsl.md`](design/core/query_edsl.md)（C++ 层更丰富） |
| `std/refactor` | `refactor:rename-var refactor:extract-function refactor:inline-function` | [`design/core/mutate_api.md`](design/core/mutate_api.md) |
| `std/workspace` | `ws:merge-symbols ws:diff` | [`design/core/workspace_layering.md`](design/core/workspace_layering.md)（P0 COW 已实装） |
| `std/rule` | `rule:define rule:apply rule:apply-all` | 见 notes/ 历史探索 |
| `std/ast-viz` | `ast:to-dot ast:to-dot-node mutation:trace mutation:trace-node` | 辅助工具 |
| `std/synthesize-v2` | `synthesize:test-driven synthesize:debug synthesize:project` | 合成管线 |
| `std/pipeline` | `synthesize:pipeline` | Agent 编排 |
| `std/verify` | `verify-output` | 测试相关 |
| `std/heal` | `heal` | 自愈相关 |

**注意**：std/ 包装是便利层；核心 EDSL 能力通过直接原语或 --serve 协议暴露（推荐用于复杂 Agent 循环）。完整状态见上文 "EDSL Primitives Surface" 及 `design/core/` §0。

### LLM & Agent
| 模块 | 导出 |
|------|------|
| `std/llm` | `aura-llm-call aura-verify` |
| `std/agent` | `auto-grow safe-eval llm-ask extract-code` |
| `std/orchestrator` | `orch:define-role orch:step orch:pipeline orch:conduct` |
| `std/ant` | `pheromone:init pheromone:update pheromone:rank pheromone:score pheromone:export` |
| `std/adaptive` | `measure-distance structured-diagnosis pid:analyze get-api-ref get-full-api-ref` |
| `std/evolve` | `evolve-strategy` |
| `std/bench` | `all-tasks task-count run-rounds aggregate print-report run-one run-parallel` |

### 工具
| 模块 | 导出 |
|------|------|
| `std/process` | `sh sh-ok? which` |
| `std/csv` | `csv-parse csv->rows csv->table csv-select csv-filter csv-header column-names` |
| `std/validate` | `validate valid? error-count` |
| `std/vector-math` | `vec:map vec:zip vec:fold vec:sum vec:prod vec:mean vec:dot vec:norm vec:normalize` |
| `std/prompt` | `build-sys-prompt get-api-ref-for-modules` |
| `std/capability` | `capability-stack capability? check-capability` |
| `std/extract` | `extract-code trim-str find-in-str` |
| `std/struct` | `define-struct` |
| `std/test` | test framework |
| `std/require` | module loader |

## Examples

For end-to-end runnable examples combining `query:pattern`,
`mutate:query-and-replace`, hygienic macros, and safe
snapshot/rollback, see
[`docs/examples/macro-edsl-transforms.md`](examples/macro-edsl-transforms.md).

The examples cover:

1. Simple constant substitution (`(+ x 1)` → `(+ x 2)`)
2. Function wrapping (`if` → `cond`)
3. Call-site rewriting (inline `+` → named `add`)
4. Macro-introduced code instrumentation
5. Snapshot → mutate → verify → rollback safety pattern
