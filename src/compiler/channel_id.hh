// channel_id.hh — Issue #4854: tagged channel ids.
//
// channel:create used to return the raw slot index (0, 1, 2, ...), which
// made an unreferenced channel indistinguishable from an ordinary fixnum
// during a GC mark. channel ids now live in a sparse high range so a
// mark/sweep can tell "some live Aura value still holds this channel" from
// "the evaluator's channel table is the only owner left". The id remains a
// fixnum (the documented API: pass it to channel:send/recv/close). It is
// still not a generation handle: a closed slot is recycled for the next
// channel:create, so a stale id can name a newer channel (same contract as
// before this change).

#ifndef AURA_COMPILER_CHANNEL_ID_HH
#define AURA_COMPILER_CHANNEL_ID_HH

#include <cstddef>
#include <cstdint>

namespace aura::compiler::channel_id {

// 2^40 keeps channel handles far away from loop counters, indices and
// ordinary user arithmetic. A collision only over-retains (an unrelated
// fixnum whose value happens to equal a live id), never frees early.
inline constexpr std::int64_t kBase = std::int64_t{1} << 40;
inline constexpr std::int64_t kMaxSlot = (std::int64_t{1} << 30) - 1;

inline constexpr std::int64_t encode(std::size_t slot) noexcept {
    return kBase + static_cast<std::int64_t>(slot);
}

inline constexpr bool decode(std::int64_t id, std::size_t& slot) noexcept {
    if (id < kBase)
        return false;
    const auto delta = id - kBase;
    if (delta > kMaxSlot)
        return false;
    slot = static_cast<std::size_t>(delta);
    return true;
}

} // namespace aura::compiler::channel_id

#endif // AURA_COMPILER_CHANNEL_ID_HH
