# Double Arena — Conservative vs Live Defrag

## 背景

`src/core/arena.ixx` 的 `ASTArena`(`pmr::monotonic_buffer_resource` 上 + `SmallObjectPool` 三 tier)是 Aura 整个 FlatAST + closure + Env 生命周期的主要承载者。长时间运行的 AI Agent 多轮 mutation 之后,arena buffer 的 fragmentation ratio 会缓慢上升,直接影响:
- Cache locality(`tag_[id]` / `int_val_[id]` / `sym_id_[id]` 等 SoA 列访问跨页)
- Peak RSS(`buffer_.size()` 不会自动 shrink,即使 `stats_.used` 很低)
- 长 session 的最终 OOM 风险

Issue #187 已经 ship 了 conservative `compact()`(只 reclaim unused tail,不移动 live objects)。Issue #300 ship 了 `defrag()` 基础 + 计数器,Phase 3 ship 了 `request_defrag()` flag。**Foundation 已就位**,完整 live-object-moving defrag 是 #1467 的目标。

## 两条路径的语义区别

| 维度 | conservative `compact()` / `defrag()` | live `live_defrag()` (#1467) |
| --- | --- | --- |
| 移动 live objects? | ❌ 不移动 | ⚠️ Phase 1 mark-only(只数,不 copy);Phase 2 才真 copy |
| 改 `buffer_.size()`? | ✅ 缩到 `used + 25% headroom` | Phase 1 不改;Phase 2 缩 + 重映射 |
| Safe to call any time? | ✅ 是(除了 fiber 中需 safepoint) | ⚠️ Phase 1 是;Phase 2 必须 stop-the-world |
| PCV / gap_buffer 协调? | 不需要(没动 data pointer) | ⚠️ Phase 2 需要 compact gap 到 end + 更新 `data()` 指针 |
| Shape invalidation? | 触发 `invoke_compact_hook_` | 同(已经触发)+ Phase 2 后所有 ShapeID 进 arena 的要 invalidate |
| NodeId 稳定性? | ✅ 保持 | ⚠️ Phase 1 保持;Phase 2 不保持(需 StableNodeRef 重映射) |
| `defrag_attempted_count` 增加? | ✅ 是 | ✅ 是(独立 counter `live_defrag_attempted_count`) |

## Issue #1467 Phase 1 已 ship(Plan A,foundation-only)

本次 commit 只 ship **mark 阶段**(`live_objects_marked_total` 计数器真的能更新),真正 copy + 指针重映射留 follow-up。代码路径:

1. `live_defrag()` 入口 → `defrag_impl(false)`(先跑 conservative trim)
2. 遍历 `classes_`(SmallObjectPool 的 3 tier):`(tier.bump - tier.start) / tier.obj_sz` = 该 tier 的 live object 数
3. `stats_.live_defrag_attempted_count++` + `stats_.live_objects_marked_total += marked`
4. 私有 atomic mirror 同步 bump(给 `(arena:live-defrag-stats)` primitive 读)
5. `invoke_compact_hook_()` 触发 shape invalidation

返回:本 pass 数到的 live objects 数(=`saved` 的 live 版本)。

## 后续 Phase 2 / 3 / 4(#1467 follow-ups)

按 issue AC 拆的 research-grade work,**不能在今晚完成**:

- **Phase 2 — copy + 指针重映射**
  - 在 `buffer_` 头部重新 layout(留 prefix 段给新 alloc)
  - 遍历所有 live 对象 → 复制到新位置 → 维护 old → new 映射
  - 重写 StableNodeRef / Closure::env / FlatAST::children_ / Env::parent_ / PersistentChildVector::data_ / gap_buffer::ptr_ 等所有指向原 buffer 内部的指针
  - 校验 ASan / TSan clean(必须,否则 ship 不了)
- **Phase 3 — PCV / gap_buffer 协调**
  - PCV:先 compact 它的 gap 到 end,再 defrag 外层 arena
  - gap_buffer:同上
  - 或反过来:Phase 2 期间记录所有 data 指针,defrag 完后统一 patch
- **Phase 4 — Fiber / safepoint 集成**
  - 只能在 safepoint 或显式 `(arena:defrag-now)` 触发
  - 跟 #1464 auto-policy 集成:阈值超过 + 当前在 safepoint → 自动调 `live_defrag()`
  - 估计要改 `aura::gc_hooks::safepoint_check` 让它在 defrag mark 阶段 pin fiber
- **Phase 5 — stress test + benchmark**
  - 长时 allocation + mutation 压力(几千轮 mutation + 多 million 对象)
  - `frag_ratio → 接近 0`,无 dangling
  - ASan / TSan / UBSan clean

## 何时调 live_defrag() vs compact() / defrag()

| 场景 | 用什么 |
| --- | --- |
| 普通维护(每 N 次 alloc 一次) | `compact()` |
| fragmentation ratio > 阈值 + 短任务 | `defrag()`(conservative) |
| fragmentation ratio > 阈值 + 长 session + idle | `live_defrag()`(Phase 1 today;Phase 2 后才能真搬) |
| 用户显式 `(arena:defrag-now)` | `defrag()` today;Phase 4 后改 `live_defrag()` |

## 观测

- `(arena:defrag-stats)` — `defrag_attempted_count` / `last_defrag_saved`(Issue #300 已有)
- `(arena:live-defrag-stats)` — `live_defrag_attempted_count` / `live_objects_marked_total`(Phase 1 暴露;Phase 2 加 `live_objects_moved_total` + `live_objects_relocated_bytes`)

## 相关

- #187 — conservative compaction(已 ship)
- #300 — defrag 基础 + 计数器(已 ship)
- #1464 — auto-policy(可集成 `live_defrag` 触发)
- #1467 — 本 issue
- Task 4 Review「Arena + DOD/SoA 内存布局」差距

Refs: #1467