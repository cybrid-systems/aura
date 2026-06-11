# Pipeline 组合策略 — Design & Implementation

**Status**: Design + Implemented (2026-05-25)

## 1. 问题

单个策略有局限：
- **Template**：仅适合已知结构，无法处理未知逻辑
- **LLM**：一次生成长代码容易遗漏细节，且成本高
- **Genetic**：需要明确的 fitness 函数

**Pipeline 方案**：把代码生成拆成多步骤，每步用最合适的策略，步骤之间传递 workspace 状态。

## 2. 场景

### API 客户端（标准管线）
```
Step 1 (Template) → 骨架
Step 2 (LLM) → 核心逻辑
Step 3 (LLM Fixer) → 错误处理
```

### 重构管线
```
Step 1 (Template) → 添加 benchmark
Step 2 (LLM) → 优化版本
Step 3 (Verifier) → 回归测试
Step 4 (LLM Fixer) → 修复失败
```

### 多 Agent 协作
```
Agent A (Template) → 骨架 → workspace:create-child
Agent B (LLM) → 填充 → workspace:switch
Agent C (Verifier) → 测试 → messaging
```

## 3. EDSL API

```
(synthesize:pipeline name step1 step2 ...)
  → 依次执行每个 step
  → 每步结果写入 workspace（供下一步使用）
  → 失败时 ast:restore 回退
  → 返回 #t / #f
```

每个 step 是任意可 eval 的表达式，返回 #t 或字符串（源码）。

## 4. 实现

实现在 `lib/std/pipeline.aura`，纯 Aura，不需要 C++ 改动。

```lisp
(define (synthesize:pipeline name . steps)
  (display "=== Pipeline: ")(display name)(display " ===\n")
  (define sid (ast:snapshot name))
  (let loop ((remaining steps) (n 1))
    (if (null? remaining)
      (begin (display "✅ Complete!\n") #t)
      (let* ((step (car remaining))
             (ok (try
                  (let ((r (eval step)))
                    (if r (begin (display "  Step ")(display n)(" ✅\n") #t)
                        (begin (display "  Step ")(display n)(" ❌\n") #f)))
                  (catch (e)
                    (display "  Step ")(display n)(" ❌: ")
                    (display e)(newline)
                    #f)))))
        (if ok
          (loop (cdr remaining) (+ n 1))
          (begin
            (ast:restore sid)
            #f))))))
```
