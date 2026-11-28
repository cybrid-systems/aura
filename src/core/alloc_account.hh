#pragma once

// Issue #5636: snapshot counters for subsystems that have no enumerable
// slab. Bumps are sharded (one cache line per thread) and sit off the
// quiet allocate_checked path. load() is exact. Bytes are monotonic:
// a free on another thread cannot subtract from a uint64 shard.

#include "core/sharded_counter.h"

namespace aura::core::alloc_account {

[[nodiscard]] inline ShardedCounter& flat_nodes() noexcept {
    static ShardedCounter c;
    return c;
}

[[nodiscard]] inline ShardedCounter& pcv_bytes() noexcept {
    static ShardedCounter c;
    return c;
}

[[nodiscard]] inline ShardedCounter& ir_insns() noexcept {
    static ShardedCounter c;
    return c;
}

[[nodiscard]] inline ShardedCounter& fiber_bytes() noexcept {
    static ShardedCounter c;
    return c;
}

} // namespace aura::core::alloc_account
