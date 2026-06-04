# evo-kv → Redis 路线图

> 目标：将 evo-kv-core 逐步演化为完整的 Redis 兼容 KV 系统。
> 每次迭代 = 实现一个核心能力 + 测试 + push。
> Grok 驱动代码演进，人做决策把关。

## 进度

```
R1  LRU 缓存层                          ✅ 已推送
R2  auto 驱动修复 + 集成                ✅ 已推送
R3  Sorted Set (ZSET)                   ✅ 已推送 (fix: flatten排序)
R4  逐出策略 + INFO                     ✅ 已推送
R5  Pub/Sub                             ✅ 已推送
R6  AOF 持久化                          ✅ 已推送
R7  Benchmark 套件                      ✅ 已推送
R8  RESP 协议层                         ✅ 已推送
R9  Replication / 集群                  ✅ 已推送
全模块集成测试                          ✅ 22 tests (R1-R4)
```

### R4（当前）

- `evo-kv-admin.aura` — 管理命令层
- CONFIG SET/GET: maxmemory, maxmemory-policy
- 逐出策略: noeviction / allkeys-lru / volatile-ttl
- INFO: 键值统计 + 操作计数
- MEMORY USAGE: 采样估算

## 各轮详情

### R3: Sorted Set — ZADD/ZRANGE/ZSCORE/ZRANK/ZREM

- 用 skiplist 或有序 vector 实现 score→member 映射
- 支持 ZADD（单 key 多条）、ZRANGE（by rank）、ZSCORE、ZRANK、ZREM、ZCARD
- 60 分起，慢速→优化

### R4: 逐出策略

- 当前: 死板的 LRU (100 entries)
- 进化: maxmemory + policy (allkeys-lru / volatile-ttl / noeviction)
- 与 TTL 联动：过期 key 按优先级逐出

### R5: Pub/Sub

- SUBSCRIBE channel → 邮件列表
- PUBLISH channel msg → 扇出
- 底层用 serve session 的 send/recv

### R6: AOF 持久化

- 当前: SAVE/LOAD 是全量快照
- AOF: 每个写操作 append 到日志文件
- BGREWRITEAOF: 压缩日志

### R7: Benchmark

- redis-benchmark 风格的 Aura 测试
- ops/s、p99 延迟、内存
- 自动对比各轮演进前后的性能

## 协作模式

```
Grok（我）                    Aura 运行时              你
  │                              │                      │
  ├─ 设计功能                    │                      │
  ├─ 写 evo-kv-xxx.aura         │                      │
  ├─ 测试 ─────────────────→ eval                      │
  ├─ 发现 gap ────────────→ 修 core                     │
  ├─ push                                            ←─┤─ 审查
  └─ 下一轮 ───────────────────────────────────────→ 确认
```

---

## Related: Broader Self-Evolving Infrastructure

For patterns applicable to caches, message queues, schedulers,
and other infrastructure components (not just KV stores), see
[`docs/design/self-evolving-infrastructure.md`](../../docs/design/self-evolving-infrastructure.md).
