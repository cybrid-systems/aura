# Shape Stability — History / Dominant / Deopt-Storm

## 背景

`src/compiler/shape_profiler.{h,cpp}` 的 `ShapeProfiler` 是 Aura JIT speculative optimization 的核心:
- 给每个 function 维护 `ShapeHistoryRing`(固定 capacity ring buffer,O(1) push)
- 检测 dominant shape(出现次数最多的 ShapeID)
- 判断 stability(`dominant_count / window_size >= stability_ratio`)
- 失效时 fire `ShapeDeoptHook`(由 `CompilerService::shape_deopt_hook_trampoline` 接住,转 `on_shape_deopt_hook` → dirty cache invalidation)

Issue #53 ship 了基础,Issue #570/#686 加了 deopt hook + dirty_scope,Issue #992 加了 profile cap + eviction。**调优策略(默认参数 + 高 mutation 工作负载适配 + AI 工作负载指标)留到 #1468**。

## 默认参数 + 3 个 Preset(#1468 AC1)

| Preset | window_size | stability_ratio | min_samples_for_stable | deopt_storm_window | deopt_storm_threshold |
| --- | --- | --- | --- | --- | --- |
| `kDefaultPreset` | 1000 | 0.90 | 100 | 256 | 4 |
| `kHighMutationPreset` | 2000 | 0.95 | 250 | 512 | 6 |
| `kLowMutationPreset` | 500 | 0.85 | 50 | 128 | 3 |

激活方式:

```cpp
shape::ShapeProfiler sp;
sp.apply_preset(shape::ShapeProfiler::kHighMutationPreset);
```

或者在 `(serve)` 通过 env `AURA_SHAPE_PRESET=high_mutation`(follow-up #1468.1 wire)。

**为什么需要 high-mutation preset**:AI Agent 多轮 mutation 下,shape churn 是正常的(不是 pathological)。Default 0.90 + 1000 window 太容易 deopt → deopt storm。High-mutation 0.95 + 2000 window + 250 samples 更保守,避免 false positive stability loss。

## Deopt-Storm 防护(#1468 AC2)

`_deopt_storm_active_` 是 atomic bool,触发条件:**最近 `deopt_storm_window_` 次 invalidate 事件数 ≥ `deopt_storm_threshold_`**(默认 4/256)。一旦触发,sticky(true 持续),只通过 `reset()` 清空。

Wire 点(已经 hook 进):
- `ShapeProfiler::invalidate()` 路径(stability loss + explicit invalidate 都算)
- `ShapeProfiler::record_shape()` 中 `is_stable=true → stability loss` 路径

**SpecJITController coordination**(follow-up #1468.2):
- SpecJITController 应该 `poll(shape_profiler.deopt_storm_active())` 后:
  - 把该 fn 降级到 generic(不 speculative)
  - 提高该 fn 的 threshold(`is_good_deopt_candidate` 配合)
  - 跟 #985 SpecJIT unbounded growth 协同:storm-active fn 不进 cache

## AI 工作负载指标(#1468 AC3)

4 个 atomic counter + 3 个 ratio accessor:

| Metric | 类型 | 含义 |
| --- | --- | --- |
| `mutation_induced_invalidations` | atomic counter | lifetime `invalidate_all()` 影响 profile 数 |
| `deopt_storm_total` | atomic counter | lifetime deopt-storm 激活次数 |
| `history_hit_count` | atomic counter | lifetime `record_shape` 调用数 |
| `history_miss_count` | atomic counter | lifetime miss 数(预留,目前 hit-only) |
| `shape_stable_ratio` | computed | 当前 stable profile / 总 profile |
| `deopt_rate_per_fn` | computed | 总 deopt_count / 总 profile |
| `history_hit_rate` | computed | hit / (hit + miss) |

暴露路径:
- 直接 C++:`shape_profiler.shape_stable_ratio()` 等
- Aura primitive:`(compile:shape-stability-stats)` — follow-up #1468.3 wire

## 测试(#1468 AC4)

`tests/test_issue_1468.cpp` 6 AC:
- AC1: Preset application(round-trip 3 个 preset)
- AC2: Deopt-storm detection(默认 preset 下 4 次 invalidate → storm active)
- AC3: 4 metric 初始状态(全 0)
- AC4: `invalidate_all()` bumps `mutation_induced_invalidations` by profile count
- AC5: `record_shape` bumps `history_hit_count` 每次
- AC6: SpecJITController coordination hook(storm active 可观测)

压力测试(high mutation rate + 多 fn + 多轮 invalidate)— follow-up #1468.4(需要 dedicated benchmark harness)。

## 集成点

- **`#1416 GuardShape`** — deopt-storm 时 GuardShape 应该提早失效,follow-up
- **`#985 SpecJIT unbounded growth`** — storm-active fn 不进 cache,follow-up
- **`#1241 SoAView + shape_id consult`** — SoAView 应该读 `shape_stable_ratio` 决定是否 shape-specialize,follow-up
- **`#1408 typed-mutate-atomic`** — atomic batch mutation 后 trigger `invalidate_all()`,已经在 wire 上

## Follow-ups(#1468 AC partial close)

- **#1468.1** — env var `AURA_SHAPE_PRESET` wire + `(serve)` 集成
- **#1468.2** — SpecJITController 真正读 `deopt_storm_active()` + 降级路径
- **#1468.3** — Aura primitive `(compile:shape-stability-stats)` 暴露
- **#1468.4** — 压力测试 + benchmark(release no regression check)
- **#1468.5** — `history_miss_count` 真正实现(区分 dominant match vs not)
- **#1468.6** — GuardShape 集成(#1416 协同)

## 相关

- #53 Shape-based Speculative JIT(基础)
- #570 / #686 deopt hook + dirty scope
- #992 profile cap + eviction
- #1416 GuardShape
- #985 SpecJIT unbounded growth
- #1241 SoAView
- #1408 typed-mutate-atomic
- #1468 本 issue

Refs: #1468