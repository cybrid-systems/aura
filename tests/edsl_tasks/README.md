# EDSL Benchmark Tasks

每个 `.aura` 文件定义一个基准测试任务。元数据写在 `;;` 注释中：

```scheme
;; task: arith-basic        ; 任务名（默认从文件名取）
;; goal: Write (+ 1 2 3 4 5)  ; LLM 的提示（必填）
;; expect: 15                ; 期望输出中含有的字符串
;; expect: 6                 ; （可多个）
;; depend: std/list          ; 依赖的标准库模块
;; hint: --- WORKING CODE ---  ; 针对性提示（可选，嵌入 system prompt）
;; hint: (display (+ 1 2 3 4 5))
```

文件主体是可选的参考代码（不会被 benchmark 使用，仅供人类阅读）。
