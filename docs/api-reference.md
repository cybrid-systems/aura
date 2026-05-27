# Aura Standard Library API Reference

## Modules Overview

| Module                         |  Size | Description |
| ------------------------------ | ----- | ---------------------------------------- |
| std/adaptive                   | 18841 | lib/std/adaptive.aura -- adaptive intend decision module
| std/agent                      |  3993 | agent.aura — Self-growing AI agent library
| std/algorithm                  |  3467 | Aura standard algorithm library
| std/ant                        |  9428 | lib/std/ant.aura -- ant colony pheromone system + colony:search
| std/bench                      | 10498 | ── bench.aura — Aura Native EDSL Benchmark ───
| std/combinators                |   921 | combinators.aura — Functional programming utilities
| std/csv                        |  1605 | csv.aura — CSV parsing and generation
| std/data                       |  1866 | Aura standard data structures library
| std/datetime                   |  6496 | datetime.aura — Date and time utilities
| std/encoding                   |  5396 | lib/std/encoding.aura — Encoding utilities
| std/evolve                     |  1901 | evolve.aura -- Strategy evolution from intend analytics
| std/fs                         |  2887 | lib/std/fs.aura — File system utilities
| std/hash                       |  1210 | hash.aura -- Hash table operations
| std/heal                       |  2264 | ── std/heal.aura — Self-healing code execution loop (鸿渐于陆) ──
| std/io                         |  1506 | Aura standard I/O library
| std/iter                       |  5031 | iter.aura — Iterator and collection utilities
| std/json                       |  1558 | Aura JSON library -- delegates to built-in json-parse / json-encode
| std/list                       |  2379 | Aura standard list library
| std/llm                        |   827 | Aura std/llm -- LLM 交互模块
| std/math                       |  6606 | Aura standard math library
| std/maybe                      |   472 | maybe.aura — Maybe/Option type
| std/net                        |  3047 | lib/std/net.aura — Network client library
| std/pipeline                   |  2017 | Aura std/pipeline — 代码生成管线
| std/process                    |   851 | lib/std/process.aura — Process management
| std/queue                      |  1050 | queue.aura — FIFO queue (pair-based, immutable)
| std/random                     |  2522 | random.aura — Simple pseudo-random number generator
| std/regex                      |  1783 | lib/std/regex.aura — Regular expression utilities
| std/require                    |   584 | Aura module loader — require
| std/rule                       | 10290 | Aura std/rule — 代码规范系统 (P2)
| std/set                        |  1360 | set.aura — Set data structure (built on hash tables)
| std/socket                     |   436 | socket.aura — TCP socket library
| std/stack                      |   597 | stack.aura — LIFO stack (pair-based)
| std/string                     |  5141 | Aura standard string library
| std/struct                     |  1195 | Aura struct library — define-struct as a macro
| std/synthesize-v2              | 13562 | lib/std/synthesize-v2.aura -- Synthesize Pipeline v2
| std/test                       |   639 | Aura Testing Framework
| std/uuid                       |   740 | lib/std/uuid.aura — UUID v4 generation
| std/validate                   |  4416 | Aura JSON Schema Validator — 纯函数式
| std/vector-math                |  6124 | lib/std/vector-math.aura — Numerical vector operations
| std/verify                     |   763 | verify.aura — Output-verified code checking

---
Total: 40 modules


---
## std/adaptive.aura

lib/std/adaptive.aura -- adaptive intend decision module

Returns lists, not hashes (avoids cross-module hash mutation bug).
Uses cond, not if (avoids cross-module if-in-begin bug).
== Primitives ==
== measure-distance ==
Returns (list phase-string ratio-number diagnosis-string)
== structured-diagnosis ==

**Exports:** `string-index`, `string-contains?`, `string-trim`, `measure-distance`, `structured-diagnosis`, `pid:analyze`, `get-api-ref`, `get-full-api-ref`

---
## std/agent.aura

agent.aura — Self-growing AI agent library
Implements the complete auto-fix loop within Aura.
(require std/agent) → auto-grow, llm-ask, safe-eval
── Configuration ──────────────────────────────────────────────
Defaults
── LLM API call ──────────────────────────────────────────────
── Extract code from LLM response ────────────────────────────
Strip markdown code blocks

**Exports:** `auto-grow`, `safe-eval`, `llm-ask`, `extract-code`

**Functions:**
  - `(llm-ask prompt)`
  - `(extract-code response)`
  - `(safe-eval code)`
  - `(auto-grow task . max-tries)`
  - `(edsl-fix task source)`

---
## std/algorithm.aura

Aura standard algorithm library
Import with: (import "std/algorithm")
Requires: std/list
── Sorting extensions ──────────────────────────────
── Merge ────────────────────────────────────────────
── Search ─────────────────────────────────────────
── Set operations (on sorted lists) ────────────────
── Aggregations ───────────────────────────────────

**Exports:** `sort-stable`, `sort-by`, `binary-search`, `merge-sorte`

**Functions:**
  - `(sorted? lst)`
  - `(sort-by lst key)`
  - `(sort-stable lst)`
  - `(merge-sorted a b)`
  - `(binary-search target lst)`
  - `(unique lst)`
  - `(min-by lst key)`
  - `(max-by lst key)`
  - `(permutations lst)`
  - `(combinations n k)`

---
## std/ant.aura

lib/std/ant.aura -- ant colony pheromone system + colony:search

── Local utility functions (no external deps) ───────────────────
── Pheromone system ──────────────────────────────────────────────
── Colony Search (pure Aura, Phase 2) ───────────────────────────

(colony:search expected-str max-variants)
Returns (found? output debug-msg) list.

**Exports:** `pheromone:init`, `pheromone:update`, `pheromone:rank`, `pheromone:score`, `pheromone:expor`

**Functions:**
  - `(sort-descending lst)`

---
## std/bench.aura

── bench.aura — Aura Native EDSL Benchmark ───
── API reference for LLM system prompt ──
── extract-code from LLM response ──
```lisp ... ``` → content
``` ... ``` → content
(define ...) or (display ...) → inline
else → raw trimmed
── Load tasks from JSON ──

**Exports:** `all-tasks`, `task-count`, `run-rounds`, `aggregate`, `print-report`, `extract-code`, `run-one`

**Functions:**
  - `(extract-code response)`
  - `(run-one task model max-a)`
  - `(generator g)`
  - `(check-success actual expects-list)`
  - `(verifier code)`
  - `(fixer code err g)`
  - `(run-rounds tasks model max-a rounds)`
  - `(loop r results)`
  - `(aggregate tasks round-results)`
  - `(count-task ti)`
  - `(iter ti all-p all-f stats)`
  - `(print-report tasks results)`

---
## std/combinators.aura


**Functions:**
  - `(identity x)`
  - `(const x)`
  - `(flip f)`
  - `(compose-internal fns)`
  - `(compose f . rest)`
  - `(thrush-internal x fns)`
  - `(thrush x . fns)`
  - `(curry f a)`
  - `(rcurry f a)`
  - `(partial2 f a)`
  - `(complement pred)`

---
## std/csv.aura


**Exports:** `csv-parse`, `csv->rows`, `csv->table`, `csv-select`, `csv-filter`, `csv-header`, `column-names`

**Functions:**
  - `(split-lines text)`
  - `(csv-split-line line)`
  - `(csv-parse text)`
  - `(csv-select cols rows)`
  - `(csv-filter pred rows)`
  - `(csv-header rows)`
  - `(column-names rows)`

---
## std/data.aura

Aura standard data structures library
Import with: (import "std/data")
═══════════════════════════════════════════════════════════
Trie (prefix tree)
═══════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════

**Exports:** `make-trie`, `trie-insert`, `trie-search`, `trie-prefix?`, `trie-keys`

**Functions:**
  - `(trie-insert trie word)`
  - `(trie-search trie word)`
  - `(trie-prefix? trie prefix)`
  - `(trie-keys trie)`

---
## std/datetime.aura

datetime.aura — Date and time utilities
Import with: (import "std/datetime")

Depends on: (current-time) primitive (seconds since Unix epoch)

All functions are pure (no side effects) except timestamp.
Core
Formatting

**Functions:**
  - `(leap-year? y)`
  - `(days-in-month y m)`
  - `(pad2 n)`
  - `(day-of-week y m d)`
  - `(time-diff t1 t2)`
  - `(month-name m)`
  - `(weekday-name wd)`
  - `(format-time fmt t)`

---
## std/encoding.aura

lib/std/encoding.aura — Encoding utilities

(require "std/encoding" all:)

API:
  (hex-encode data)       → hex string  (lowercase)
  (hex-decode hex-str)    → string of decoded bytes | ""
  (base64-encode data)    → base64 string

**Exports:** `hex-encode`, `hex-decode`, `base64-encode`, `base64-decode`

**Functions:**
  - `(hex-encode data)`
  - `(hex-decode hex)`
  - `(base64-encode data)`
  - `(base64-decode b64)`

---
## std/evolve.aura

evolve.aura -- Strategy evolution from intend analytics

(require "std/evolve" all:)
(evolve-strategy name) → new-variant-name
  reads (intend-analytics), adjusts strategy body,
  registers new version as "<name>-v<N>"
Build new body -- single begin block avoids Aura if/set! interaction bug
Find next version number

**Exports:** `evolve-strategy`

**Functions:**
  - `(evolve-strategy name)`

---
## std/fs.aura

lib/std/fs.aura — File system utilities

(require "std/fs" all:)

API:
  (path-join parts...)       → combined path string
  (path-dirname path)        → directory part
  (path-basename path)       → filename part

**Exports:** `path-join`, `path-dirname`, `path-basename`, `path-extnam`

**Functions:**
  - `(path-join . parts)`
  - `(path-dirname path)`
  - `(path-basename path)`
  - `(path-extname path)`
  - `(file-read path)`
  - `(file-write path content)`
  - `(file-exists? path)`
  - `(file-size path)`
  - `(dir-list path)`
  - `(dir-files path)`
  - `(dir-dirs path)`

---
## std/hash.aura


**Functions:**
  - `(for-each f lst)`
  - `(hash-set h k v)`
  - `(hash-ref h k)`
  - `(hash-get h k)`
  - `(hash-remove h k)`
  - `(hash-empty? h)`
  - `(hash-merge a b)`
  - `(frequencies lst)`

---
## std/heal.aura

── std/heal.aura — Self-healing code execution loop (鸿渐于陆) ──

heal: Run code, auto-fix common errors, retry until success or max attempts.
Uses set-code + eval-current-output for correct stdout capture.

Usage:
  (require "std/heal" all:)
  (heal "(for-each display (list 1 2))" (list "1" "2") 3)

**Exports:** `heal`

**Functions:**
  - `(heal code expected max-attempts)`

---
## std/io.aura

Aura standard I/O library
Import with: (import "std/io")

Wraps raw C++ I/O primitives into a consistent API.
Primitive names (file-exists?, read-file, write-file, etc.)
are available globally without import; this module provides
convenience aliases and structured readers.
── Raw primitives (re-exported) ────────────────────

**Exports:** `file-exists?`, `file-size`, `file-copy`, `file-delet`

**Functions:**
  - `(file-read path)`
  - `(file-write path content)`

---
## std/iter.aura

iter.aura — Iterator and collection utilities
Import with: (import "std/iter")

Provides generic operations over lists, vectors, and hash tables.
(iterate f x n) — generate n elements: x, f(x), f(f(x)), ...
(unfold p f g seed) — generate list until p holds
(iota n) — generate list 0..n-1

**Functions:**
  - `(any? pred lst)`
  - `(every? pred lst)`
  - `(find pred lst)`
  - `(find-index pred lst)`
  - `(count pred lst)`
  - `(occurrences eq? lst)`
  - `(group-by keyfn lst)`
  - `(frequencies lst)`
  - `(split-at n lst)`
  - `(hash-map f h)`
  - `(hash-filter pred h)`
  - `(hash-keys-filter f h)`
  - `(hash-vals h)`
  - `(hash-merge-into h . sources)`
  - `(hash-keys-sorted h)`
  - `(vector-map f v)`
  - `(vector-reverse v)`
  - `(vector-slice v start end)`
  - `(vector-find pred v)`
  - `(iterate f x n)`
  - `(unfold p f g seed)`
  - `(iota n)`

---
## std/json.aura

Aura JSON library -- delegates to built-in json-parse / json-encode
Built-in versions are O(n), no recursion depth limit.
(require "std/json" all:) -- json-parse + json-stringify
── Delegated to built-in primitives ──────────────────────────
── json-escape -- string → JSON-safe string ───────────────────
── json-value -- value → JSON string (uses builtin json-encode) ─
── json-arr-items / json-obj-items -- kept for backward compat ──

**Exports:** `json-parse`, `json-stringify`, `json-escape`, `json-value`, `c2s`, `json-arr-items`, `json-obj-items`

**Functions:**
  - `(c2s c)`
  - `(json-parse s)`
  - `(json-stringify v)`
  - `(json-escape s)`
  - `(esc-char c)`
  - `(escape-all cs)`
  - `(json-value v)`
  - `(json-arr-items lst json-fn)`
  - `(json-obj-items h keys json-fn)`

---
## std/list.aura


**Exports:** `foldr`, `map`, `for-each`, `member?`, `zip`, `zip3`, `take`, `skip`, `take-while`, `drop-while`, `partition`, `sort`, `range`, `sum`, `product`, `last`, `flatten`, `intersperse`

**Functions:**
  - `(foldr f init lst)`
  - `(for-each f lst)`
  - `(member? elem lst)`
  - `(zip a b)`
  - `(zip3 a b c)`
  - `(take-while pred lst)`
  - `(drop-while pred lst)`
  - `(partition pred lst)`
  - `(sort lst)`
  - `(range start end)`
  - `(take n lst)`
  - `(skip n lst)`
  - `(sum lst)`
  - `(product lst)`
  - `(last lst)`
  - `(flatten lst)`
  - `(intersperse sep lst)`

---
## std/llm.aura


**Exports:** `aura-llm-call`, `aura-verify`

---
## std/math.aura

Aura standard math library
Import with: (import "std/math")
Constants
Basic
Rounding
Trigonometry
Number theory
Combinatorics

**Functions:**
  - `(square x)`
  - `(cube x)`
  - `(clamp x lo hi)`
  - `(avg a b)`
  - `(atan2 y x)`
  - `(positive? n)`
  - `(negative? n)`
  - `(zero? n)`
  - `(gcd a b)`
  - `(lcm a b)`
  - `(prime? n)`
  - `(factors n)`
  - `(fibonacci n)`
  - `(factorial n)`
  - `(expt b n)`
  - `(nPr n r)`
  - `(nCr n r)`
  - `(sum lst)`
  - `(product lst)`
  - `(mean lst)`
  - `(median lst)`
  - `(mode lst)`
  - `(variance lst)`
  - `(stddev lst)`
  - `(range lst)`
  - `(percentile lst p)`
  - `(normalize lst)`
  - `(min-list lst)`
  - `(max-list lst)`

---
## std/maybe.aura


**Functions:**
  - `(maybe? v)`
  - `(maybe-ref m)`
  - `(maybe-default d m)`
  - `(map-maybe f m)`
  - `(filter-maybe pred lst)`

---
## std/net.aura

lib/std/net.aura — Network client library

(require "std/net" all:)

API:
  (http-get-json url)  → parsed JSON | ()
  (http-post-json url body) → parsed JSON | ()
  (url-encode s) → percent-encoded string

**Exports:** `http-get-json`, `http-post-json`, `url-encode`, `url-decode`, `url-join`, `fetch-try`

**Functions:**
  - `(http-get-json url)`
  - `(http-post-json url body)`
  - `(url-encode s)`
  - `(url-decode s)`
  - `(url-join base path)`
  - `(fetch-try url max-a)`

---
## std/pipeline.aura

Aura std/pipeline — 代码生成管线

Pipeline 把多个生成步骤编排成一个顺序管线：
  (synthesize:pipeline "name"
    step1 step2 ...)

每步可以是：
  - (synthesize:fill "template-name" args...)

**Exports:** `synthesize:pipeline`

---
## std/process.aura

lib/std/process.aura — Process management

(require "std/process" all:)


**Exports:** `sh`, `sh-ok?`, `which`

**Functions:**
  - `(sh cmd)`
  - `(sh-ok? cmd)`
  - `(string-trim s)`
  - `(which program)`

---
## std/queue.aura

queue.aura — FIFO queue (pair-based, immutable)
Import with: (import "std/queue")

Enqueue at front, dequeue from back. Inefficient for large queues
but fine for small ones (~100 elements). For production, use
ring-buffer or list reversal pattern.

Alternative: use enqueue/dequeue-of-list pattern:

**Functions:**
  - `(enqueue q elem)`
  - `(dequeue q)`
  - `(queue-front q)`
  - `(queue-rest q)`
  - `(queue-empty? q)`
  - `(queue-length q)`

---
## std/random.aura

random.aura — Simple pseudo-random number generator
Import with: (import "std/random")

Linear Congruential Generator (LCG).
Uses integer arithmetic only — no external deps.
Default seed: 123456789
(make-random) → generator (a pair (seed . state))
(make-random seed) → generator with given seed

**Functions:**
  - `(make-random . rest)`
  - `(random-next gen)`
  - `(random-integer gen)`
  - `(random-float gen)`
  - `(random-range lo hi gen)`
  - `(random-vector n gen)`
  - `(shuffle lst gen)`
  - `(remove-at lst i)`
  - `(list-ref lst i)`
  - `(random-seed gen)`

---
## std/regex.aura

lib/std/regex.aura — Regular expression utilities

(require "std/regex" all:)


**Exports:** `re-match?`, `re-find`, `re-replace`, `re-split`, `re-quote`

**Functions:**
  - `(re-match? pattern str)`
  - `(re-find pattern str)`
  - `(re-replace pattern str replacement)`
  - `(string-index haystack needle start)`
  - `(re-split pattern str)`
  - `(re-quote str)`

---
## std/rule.aura

Aura std/rule — 代码规范系统 (P2)

(require "std/rule" all:)

规则 = query:pattern + 相应 mutate 的组合。
P2 新增：:scope 支持、rule:save / rule:load、出口过滤
── 规则存储 ──────────────────────────────────────────────
── 内部：查找规则 ─────────────────────────────────────────

**Exports:** `rule:define`, `rule:apply`, `rule:apply-al`

**Functions:**
  - `(find-rule name)`
  - `(rule-name r)`
  - `(rule-pattern r)`
  - `(rule-replace r)`
  - `(rule-condition r)`
  - `(rule-desc r)`
  - `(rule-enabled? r)`
  - `(rule-scope r)`
  - `(check-condition cond-fn node-id)`
  - `(in-scope? scope node-id)`
  - `(apply-rule r)`

---
## std/set.aura


**Exports:** `set`, `set-add`, `set-remove`, `set-member?`, `set-empty?`, `set-union`, `set-intersect`, `set-difference`, `set->list`, `list->set`, `set-size`, `set-subset?`, `set-equal?`

**Functions:**
  - `(for-each f lst)`
  - `(set . items)`
  - `(set-add s x)`
  - `(set-remove s x)`
  - `(set-member? s x)`
  - `(set-empty? s)`
  - `(set-size s)`
  - `(set-union a b)`
  - `(set-intersect a b)`
  - `(set-difference a b)`
  - `(set-subset? a b)`
  - `(set-equal? a b)`

---
## std/socket.aura

socket.aura — TCP socket library
(require std/socket) → tcp-connect, tcp-send, tcp-recv, tcp-close

Uses native C++ socket primitives (not FFI).

**Exports:** `tcp-connect`, `tcp-send`, `tcp-recv`, `tcp-close`

**Functions:**
  - `(tcp-connect host port)`
  - `(tcp-send socket data)`
  - `(tcp-recv socket maxlen)`
  - `(tcp-close socket)`

---
## std/stack.aura

stack.aura — LIFO stack (pair-based)
Import with: (import "std/stack")

Most efficient collection: push is O(1), pop is O(1).

**Functions:**
  - `(stack-push s elem)`
  - `(stack-pop s)`
  - `(stack-top s)`
  - `(stack-empty? s)`
  - `(stack-length s)`

---
## std/string.aura

Aura standard string library
Import with: (import "std/string")
Note: string.aura intentionally avoids requiring std/list internally.
Module evaluation runs in isolated env; internal imports would pollute top_.
Split string by a separator
sep can be: a string (uses first char), or a char code (integer)
Split by any whitespace (space, tab, newline)
Join list of strings with separator

**Exports:** `string-split`, `string-split-words`, `string-join`, `string-tri`

**Functions:**
  - `(string-split s sep)`
  - `(iter i current result)`
  - `(string-split-words s)`
  - `(word-char? c)`
  - `(iter i current result)`
  - `(string-join strs sep)`
  - `(string-trim s)`
  - `(drop-leading lst)`
  - `(string-upcase s)`
  - `(up c)`
  - `(string-downcase s)`
  - `(down c)`
  - `(string-contains? haystack needle)`
  - `(string-prefix? s prefix)`
  - `(string-suffix? s suffix)`
  - `(string-replace s old new)`
  - `(string-pad-left s width)`
  - `(string-pad-right s width)`
  - `(make-spaces n)`
  - `(string-reverse s)`
  - `(string-repeat s n)`
  - `(string-take s n)`
  - `(string-drop s n)`

---
## std/synthesize-v2.aura

lib/std/synthesize-v2.aura -- Synthesize Pipeline v2

── string-trim ──
── eval-test ──
── run-tests ──
── build-diagnostic ──
── build-llm-prompt ──
── call-llm ──

**Exports:** `synthesize:test-driven`, `synthesize:debug`, `synthesize:projec`

**Functions:**
  - `(string-trim str)`
  - `(build-diagnostic goal-name code passed total failures)`
  - `(build-llm-prompt goal-name code feedback model . extra)`
  - `(call-llm goal-name code feedback model . extra)`
  - `(extract-code response)`
  - `(parse-key key val)`

---
## std/uuid.aura

lib/std/uuid.aura — UUID v4 generation

(require "std/uuid" all:)


**Exports:** `uuid`, `uuid-compact`, `uuid-nil`

**Functions:**
  - `(rnd n)`
  - `(hs n)`

---
## std/validate.aura

Aura JSON Schema Validator — 纯函数式
(import "std/validate")
用法: (validate schema data) → 错误列表 (空=有效)
── 单值验证（全函数式，无 set!）─────────────────────────────
Property schemas
Main

**Exports:** `json-type`, `err`, `check`, `validate`, `valid?`, `error-count`

**Functions:**
  - `(json-type v)`
  - `(err path msg errors)`
  - `(check val schema path)`
  - `(check-type val type path errs)`
  - `(check-range val min max path errs)`
  - `(check-str-len s min max path errs)`
  - `(check-arr val items min max path errs)`
  - `(check-obj val props required path errs)`
  - `(check-enum val options path errs)`
  - `(validate schema data)`
  - `(valid? errors)`
  - `(error-count errors)`

---
## std/vector-math.aura

lib/std/vector-math.aura — Numerical vector operations

Uses existing (vector ...) type for O(1) random access.
All operations are pure (return new vectors) unless marked !.

Import: (import "std/vector-math")

── Construction ──────────────────────────────────────────────

**Exports:** `vec:map`, `vec:zip`, `vec:fold`, `vec:sum`, `vec:prod`, `vec:mean`, `vec:dot`, `vec:norm`, `vec:normaliz`

---
## std/verify.aura


**Exports:** `verify-output`

**Functions:**
  - `(verify-output code . expected)`