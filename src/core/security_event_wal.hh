// security_event_wal.hh — Issue #2225: durable side-car WAL for the
// unified SecurityEvent surface (Option 2 — non-breaking on-disk
// format, independent rotation; Option 1 (extend mutation_audit_wal
// record kind) would require WAL v2 bump + migration, deferred).
//
// Mirrors `mutation_audit_wal.hh` (file / segment / magic / version /
// batched flush pattern) but persists SecurityEvent fields rather
// than AuditWalRecord. Same env-driven directory resolution
// (`resolve_security_event_wal_dir()` falls through to the same dir
// as `resolve_mutation_audit_wal_dir()` so production defaults under
// multi-tenant / Strict (#2150) automatically cover both).
//
// Hot path cost: when disabled, `is_enabled()` is a single bool load
// — no syscalls, no heap. When enabled, append is one fwrite +
// amortized fflush every 32 records (matches mutation_audit_wal
// kFlushEvery), so steady-state overhead stays < 5%.
//
// Replay: on `enable(dir, &out_replay)`, reads the last `max_replay`
// records from disk in seq order; caller (Evaluator::enable_*) pushes
// them into the in-memory SecurityEventRing. seq monotonic across
// restart — `ring.seq` is set to max(replayed.seq) + 1 after replay
// so subsequent appends do not collide.

#ifndef AURA_CORE_SECURITY_EVENT_WAL_HH
#define AURA_CORE_SECURITY_EVENT_WAL_HH

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/mutation_audit_wal.hh" // resolve_mutation_audit_wal_dir fallthrough
#include "core/security_event.hh"

// Issue #3532: compiler-provided classifier (typed_mutation_audit_hooks.cpp).
// Weak so core-only TUs still link; null → leave reason unchanged.
extern "C" const char* aura_classify_mid0_se_reason(const char* reason) noexcept
    __attribute__((weak));
#include "core/wal_append_fail_slo.h" // #3056 shared append-fail SLO

namespace aura::core::security_event_wal {

// Re-export SecurityEvent types into this namespace so make_record /
// persist_security_event compile without fully-qualified names
// (security_event.hh keeps them under aura::core::security_event).
using ::aura::core::security_event::kSecurityEventRingSize;
using ::aura::core::security_event::SecurityEvent;
using ::aura::core::security_event::SecurityEventKind;

inline constexpr int kSecurityEventWalPhase = 1;
inline constexpr int kSecurityEventWalIssue = 2225;
inline constexpr char kSecurityEventWalMagic[8] = {'S', 'E', 'C', 'W', 'A', 'L', '1', '0'};
inline constexpr std::uint32_t kSecurityEventWalVersion = 1;
// Default rotate after ~1 MiB of records (matches mutation_audit_wal
// so segments rotate in lockstep under heavy audit).
inline constexpr std::uint64_t kDefaultRotateBytes = 1ull << 20;

// On-disk / replay record — POD, fixed size for O(1) append + mmap-friendly.
// Mirrors SecurityEvent fields with timestamp prefix for forensic sort.
#pragma pack(push, 1)
struct SecurityEventWalRecord {
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint8_t kind = 0; // SecurityEventKind (cast at boundary)
    std::uint8_t denied = 0;
    std::uint16_t effect_bits = 0;
    std::int64_t fiber_id = 0;
    std::uint64_t tenant_id = 0;
    std::uint64_t mutation_id = 0;
    std::uint64_t epoch = 0;
    char op[40]{};
    char reason[64]{};
    std::uint8_t reserved[8]{};
};
#pragma pack(pop)

static_assert(sizeof(SecurityEventWalRecord) == 8 + 8 + 1 + 1 + 2 + 8 + 8 + 8 + 8 + 40 + 64 + 8,
              "SecurityEventWalRecord size stable for WAL format");

// Issue #3109: production WAL append fail-closed option (SE + mutation
// audit trail integrity). Process-local overflow ring (capacity 256) that
// captures events lost to fwrite fail when wal_append_fail_closed_active()
// (#3109 env opt-in, #3302 force_wal default). Soft/Off: zero cost
// (overflow ring never written, AC1). Mid can join from overflow ring
// for in-process replay; it does NOT replace durable WAL replay
// (process-local only, crash recovery path unchanged per #2225).
inline constexpr std::uint32_t kWalOverflowRingCapacity = 256;

struct WalOverflowRecord {
    std::uint64_t mid;       // mutation_id (or SE id)
    std::uint32_t tenant_id; // tenant (or 0 for SE)
    std::uint64_t fiber_id;  // fiber / eval context
    std::uint64_t epoch;     // snapshot / audit epoch
    std::string op;          // operation name (e.g. "require_effect")
    std::string reason;      // failure reason (e.g. "fwrite_miss")
};

// Issue #4695: an enabled-WAL append miss used to store only the mechanical
// token (inject_fail / fwrite_miss). The forensic join then returned None
// for every deny that is not the capability-effect miss stamp
// (mutation_wal_append_miss). The existing reason field carries the denial
// class when the lost record is itself a deny and its reason is an
// agent-facing class token (hyphenated: isolation-deny:*, grant-mid-refused,
// hygiene-*). Placeholders such as "inject" stay on the mechanical token
// (#3178 exact equality). Allow rows are not relabeled. Mechanical tokens
// that happen to contain a hyphen (se-wal-enable-miss, wal-rotate-open-fail)
// are not a denial class — those arms are not this path.
[[nodiscard]] inline bool se_wal_reason_names_deny_class(std::string_view reason) noexcept {
    if (reason.empty() || reason == "inject_fail" || reason == "fwrite_miss" ||
        reason == "se-wal-enable-miss" || reason == "wal-rotate-open-fail")
        return false;
    return reason.find('-') != std::string_view::npos;
}

[[nodiscard]] inline std::string se_wal_overflow_miss_reason(const SecurityEventWalRecord& rec,
                                                             std::string_view mechanical) {
    if (rec.denied != 0) {
        const auto n = ::strnlen(rec.reason, sizeof(rec.reason));
        const std::string_view event_reason(rec.reason, n);
        if (se_wal_reason_names_deny_class(event_reason))
            return std::string(event_reason);
    }
    return std::string(mechanical);
}

inline WalOverflowRecord* wal_overflow_ring_storage() noexcept {
    // Process-local storage (header ODR-safe via inline). Sized at
    // kWalOverflowRingCapacity; head + count are atomic to allow push
    // from the WAL append path (under std::lock_guard) and read from
    // query:security-posture without synchronization. Pointer to the
    // static array so wal_overflow_ring_push can index ring[h % capacity].
    static WalOverflowRecord buf[kWalOverflowRingCapacity]{};
    return buf;
}

inline std::atomic<std::uint32_t>& wal_overflow_ring_head() noexcept {
    static std::atomic<std::uint32_t> h{0};
    return h;
}

inline std::atomic<std::uint32_t>& wal_overflow_ring_count() noexcept {
    static std::atomic<std::uint32_t> c{0};
    return c;
}

// Issue #3806: process-local wrap/overwrite counter. Bumped when push
// overwrites a live slot (count == capacity before store). Soft / WAL-off
// never call push (AC3 zero cost). Agent face: wal-overflow-wrap-total.
inline constexpr int kWalOverflowWrapIssue = 3806;
// Issue #3838: refuse-on-wrap under production fail-closed (residual of
// #3806 observability-only). Soft / fail-open still overwrites so Soft
// zero-cost / observe contract is unchanged.
inline constexpr int kWalOverflowWrapRefuseIssue = 3838;

inline std::atomic<std::uint64_t>& wal_overflow_ring_wrap_total() noexcept {
    // Named security_event_wal_overflow_wrap_total in the #3806 contract;
    // process-local atomic next to head/count (not SecurityEventWalMetrics —
    // Soft / WAL-off never touch this path).
    static std::atomic<std::uint64_t> security_event_wal_overflow_wrap_total{0};
    return security_event_wal_overflow_wrap_total;
}

inline std::atomic<std::uint64_t>& wal_overflow_ring_wrap_refuse_total() noexcept {
    // Named security_event_wal_overflow_wrap_refuse_total in the #3838
    // contract; bumped only when fail-closed refuses an overwrite.
    static std::atomic<std::uint64_t> security_event_wal_overflow_wrap_refuse_total{0};
    return security_event_wal_overflow_wrap_refuse_total;
}

// Push one record to the overflow ring. Production callers gate on
// wal_append_fail_closed_active() (fail-closed). Soft / WAL-off: push is
// never called today (zero cost). Returns false when #3838 refuse-on-wrap
// declines the store (live slots preserved). Thread-safe under WAL's
// std::lock_guard.
[[nodiscard]] inline bool wal_overflow_ring_push(const WalOverflowRecord& rec) noexcept {
    auto* ring = wal_overflow_ring_storage();
    auto& cnt = wal_overflow_ring_count();
    auto expected = cnt.load(std::memory_order_relaxed);
    // Issue #3806: bump wrap when overwriting a live slot (count already
    // at capacity before store). Overwritten mids vanish from
    // wal_overflow_find_by_mid — Agents need this face to distinguish
    // "never emitted" vs "overflow evicted".
    if (expected >= kWalOverflowRingCapacity)
        wal_overflow_ring_wrap_total().fetch_add(1, std::memory_order_relaxed);
    // Issue #3838: under production fail-closed, refuse silent overwrite —
    // bump refuse counter and return false so callers fail-closed rather
    // than lose older mids. Soft / fail-open keep overwrite.
    if (expected >= kWalOverflowRingCapacity &&
        ::aura::core::wal_slo::wal_append_fail_closed_active()) {
        wal_overflow_ring_wrap_refuse_total().fetch_add(1, std::memory_order_relaxed);
        // Issue #4005: compensating PostureObserve so a refused overflow
        // mid still joins the 1024 SE ring. Does not re-enter this push
        // (append_security_event is ring-only).
        using ::aura::core::security_event::append_security_event;
        using ::aura::core::security_event::g_security_event_ring;
        using ::aura::core::security_event::SecurityEventKind;
        append_security_event(
            g_security_event_ring(), SecurityEventKind::PostureObserve, rec.tenant_id, rec.mid,
            rec.epoch, /*effect_bits=*/0,
            rec.op.empty() ? std::string_view{"overflow-refuse"} : std::string_view{rec.op},
            "overflow-refuse", /*denied=*/false, static_cast<std::int64_t>(rec.fiber_id));
        return false;
    }
    const auto h = wal_overflow_ring_head().fetch_add(1, std::memory_order_relaxed);
    ring[h % kWalOverflowRingCapacity] = rec;
    // Cap count at capacity (head wraps but count saturates).
    if (expected < kWalOverflowRingCapacity)
        cnt.compare_exchange_strong(expected, expected + 1, std::memory_order_relaxed);
    return true;
}

[[nodiscard]] inline std::uint32_t wal_overflow_ring_depth() noexcept {
    const auto c = wal_overflow_ring_count().load(std::memory_order_relaxed);
    return c < kWalOverflowRingCapacity ? c : kWalOverflowRingCapacity;
}

[[nodiscard]] inline bool wal_overflow_ring_full() noexcept {
    return wal_overflow_ring_depth() >= kWalOverflowRingCapacity;
}

// Issue #4686: deterministic segment-open failure for the second-enable ACs.
// A read-only-but-readable segment is expressed with DAC permissions, but a
// root CI container bypasses them (CAP_DAC_OVERRIDE) and fopen("ab+") still
// succeeds — the runtime repro then never takes the open-fail branch. This
// test seam injects exactly that failure while leaving the existing bytes
// readable, so replay is still real and the open failure is deterministic
// on every uid.
inline std::atomic<bool>& security_event_segment_open_fail_for_test() noexcept {
    static std::atomic<bool> v{false};
    return v;
}

inline void set_security_event_segment_open_fail_for_test(bool on) noexcept {
    security_event_segment_open_fail_for_test().store(on, std::memory_order_relaxed);
}

inline void wal_overflow_ring_clear_for_test() noexcept {
    wal_overflow_ring_head().store(0, std::memory_order_relaxed);
    wal_overflow_ring_count().store(0, std::memory_order_relaxed);
    wal_overflow_ring_wrap_total().store(0, std::memory_order_relaxed);
    wal_overflow_ring_wrap_refuse_total().store(0, std::memory_order_relaxed);
}

// Issue #3734: join a lost mutation-WAL append by mid. Process-local
// (same ring as SE fwrite miss); does not replace durable WAL replay.
// Newest-first. Soft / empty ring: nullptr, no extra I/O.
[[nodiscard]] inline const WalOverflowRecord* wal_overflow_find_by_mid(std::uint64_t mid) noexcept {
    if (mid == 0)
        return nullptr;
    const auto n = wal_overflow_ring_depth();
    if (n == 0)
        return nullptr;
    const auto* ring = wal_overflow_ring_storage();
    const auto h = wal_overflow_ring_head().load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto idx = (h + kWalOverflowRingCapacity - 1 - i) % kWalOverflowRingCapacity;
        if (ring[idx].mid == mid)
            return &ring[idx];
    }
    return nullptr;
}
struct SecurityEventWalMetrics {
    std::atomic<std::uint64_t> security_event_persisted_total{0};
    std::atomic<std::uint64_t> security_event_wal_replay_count{0};
    std::atomic<std::uint64_t> security_event_crash_recovery_success{0};
    std::atomic<std::uint64_t> security_event_wal_append_fail_total{0};
    std::atomic<std::uint64_t> security_event_wal_rotate_total{0};
    std::atomic<std::uint64_t> security_event_wal_bytes_written{0};
    std::atomic<std::uint64_t> security_event_wal_enabled{0};
    std::atomic<std::uint64_t> security_event_wal_segments{0};
    // Issue #3338: segments unlinked under AURA_WAL_MAX_SEGMENTS.
    // Appended at struct END (#2906). Soft / unset retention never bump.
    std::atomic<std::uint64_t> security_event_wal_segment_prune_total{0};
    // Issue #4660: 1 when the last enable() replay stopped at max_replay
    // while older records were still on disk (unread segments or records
    // dropped from the last-read segment); 0 on a full replay. Post-restart
    // forensics reads this to tell "ring-miss = window truncated, use the
    // durable WAL path" apart from "no such record". Appended at struct END
    // (#2906). Stored by enable(); Soft / WAL-off never sets it.
    std::atomic<std::uint64_t> security_event_wal_replay_truncated{0};
};

inline SecurityEventWalMetrics& g_security_event_wal_metrics() noexcept {
    static SecurityEventWalMetrics m;
    return m;
}

// Process-wide side-car WAL controller (opt-in via enable()).
struct SecurityEventWal {
    mutable std::mutex mtx;
    bool enabled = false;
    std::string dir;
    std::string current_path;
    std::FILE* fp = nullptr;
    std::uint64_t current_bytes = 0;
    std::uint64_t rotate_bytes = kDefaultRotateBytes;
    std::uint32_t segment_index = 0;
    std::uint64_t last_seq_persisted = 0;
    std::uint32_t unflushed = 0;
    // Issue #4446: last rotate could not open the next segment. fp is
    // null while enabled stays true. Not a metric and not a query key.
    bool rotate_open_fail_ = false;
    // Issue #4520: prune high-water mark. Retention is on by default now,
    // so prune_old_segments_unlocked runs on every rotate; indices below
    // this were already unlinked in this dir session (enable resets it).
    std::uint32_t pruned_upto_ = 0;
    static constexpr std::uint32_t kFlushEvery = 32;

    ~SecurityEventWal() { close_unlocked(); }

    void close_unlocked() noexcept {
        if (fp) {
            std::fflush(fp);
            std::fclose(fp);
            fp = nullptr;
        }
        unflushed = 0;
    }

    [[nodiscard]] bool open_segment_unlocked(std::uint32_t seg) noexcept {
        close_unlocked();
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
            return false;
        current_path =
            (fs::path(dir) / ("security-event-" + std::to_string(seg) + ".wal")).string();
        // Issue #4686: see security_event_segment_open_fail_for_test().
        if (security_event_segment_open_fail_for_test().load(std::memory_order_relaxed))
            return false;
        fp = std::fopen(current_path.c_str(), "ab+");
        if (!fp)
            return false;
        static char sbuf[64 * 1024];
        std::setvbuf(fp, sbuf, _IOFBF, sizeof(sbuf));
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            close_unlocked();
            return false;
        }
        const auto pos = std::ftell(fp);
        current_bytes = pos > 0 ? static_cast<std::uint64_t>(pos) : 0;
        if (current_bytes == 0) {
            if (std::fwrite(kSecurityEventWalMagic, 1, 8, fp) != 8) {
                close_unlocked();
                return false;
            }
            const std::uint32_t ver = kSecurityEventWalVersion;
            if (std::fwrite(&ver, sizeof(ver), 1, fp) != 1) {
                close_unlocked();
                return false;
            }
            current_bytes = 8 + sizeof(ver);
            std::fflush(fp);
        }
        segment_index = seg;
        g_security_event_wal_metrics().security_event_wal_segments.store(seg + 1,
                                                                         std::memory_order_relaxed);
        return true;
    }

    void prune_old_segments_unlocked() noexcept {
        const auto cap = ::aura::core::wal_slo::wal_max_segments_retention();
        if (cap == 0)
            return;
        const std::uint64_t total = static_cast<std::uint64_t>(segment_index) + 1;
        if (total <= cap)
            return;
        const std::uint32_t first_keep = static_cast<std::uint32_t>(total - cap);
        // Issue #4520: high-water start — this runs on every rotate now
        // that retention is default-on; never re-stat pruned indices
        // (amortized O(1) per rotate instead of O(segment_index)).
        if (first_keep <= pruned_upto_)
            return;
        const std::uint32_t first = pruned_upto_;
        pruned_upto_ = first_keep;
        namespace fs = std::filesystem;
        std::error_code ec;
        for (std::uint32_t i = first; i < first_keep; ++i) {
            const auto p =
                (fs::path(dir) / ("security-event-" + std::to_string(i) + ".wal")).string();
            if (fs::remove(p, ec)) {
                g_security_event_wal_metrics().security_event_wal_segment_prune_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            ec.clear();
        }
    }

    void rotate_unlocked() noexcept {
        ++segment_index;
        g_security_event_wal_metrics().security_event_wal_rotate_total.fetch_add(
            1, std::memory_order_relaxed);
        // Issue #4446: keep the open result. A null fp with enabled still
        // true used to drop later appends with no fail accounting.
        rotate_open_fail_ = !open_segment_unlocked(segment_index);
        // Issue #3338: opt-in retention. Unset AURA_WAL_MAX_SEGMENTS → no-op.
        prune_old_segments_unlocked();
    }

    // Issue #4446: enabled but no segment file. Same counters and
    // fail-closed overflow capture as fwrite-miss. WAL-off never calls
    // this (`!enabled` returns first). Reason stays off the query surface.
    [[nodiscard]] bool note_rotate_open_fail_unlocked(const SecurityEventWalRecord& rec) noexcept {
        rotate_open_fail_ = true;
        g_security_event_wal_metrics().security_event_wal_append_fail_total.fetch_add(
            1, std::memory_order_relaxed);
        ::aura::core::wal_slo::note_wal_append_fail();
        if (::aura::core::wal_slo::wal_append_fail_closed_active()) {
            WalOverflowRecord ovr{};
            ovr.mid = rec.mutation_id;
            ovr.tenant_id = static_cast<std::uint32_t>(rec.tenant_id);
            ovr.fiber_id = static_cast<std::uint64_t>(rec.fiber_id);
            ovr.epoch = rec.epoch;
            ovr.op = rec.op[0] ? std::string(rec.op) : std::string("security_event_wal_append");
            ovr.reason = std::string("wal-rotate-open-fail");
            (void)wal_overflow_ring_push(ovr);
        }
        return false;
    }

    // Enable persist under `persist_dir`. Replays existing WAL into
    // `out_records` (last `max_replay` records, oldest→newest).
    // Returns true if enabled.
    bool enable(std::string_view persist_dir,
                std::vector<SecurityEventWalRecord>* out_replay = nullptr,
                std::size_t max_replay = 1024) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        close_unlocked();
        dir.assign(persist_dir);
        enabled = false;
        pruned_upto_ = 0;
        if (dir.empty()) {
            g_security_event_wal_metrics().security_event_wal_enabled.store(
                0, std::memory_order_relaxed);
            return false;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        std::uint32_t max_seg = 0;
        bool any = false;
        // Issue #4686: replay was non-empty, but recovery success is only
        // stamped once open_segment_unlocked succeeds — a second enable that
        // cannot open the existing (read-only) segment must not look like a
        // successful crash recovery to the caller.
        bool replayed_nonempty = false;
        // Issue #4695: hoisted so the recovery stamp below can see it.
        // A truncated window is not a complete history.
        bool replay_truncated = false;
        // Issue #4520: collected indices double as the enable-time upgrade
        // prune list (exact, hole-tolerant) — no extra directory pass.
        std::vector<std::uint32_t> seg_indices;
        if (fs::exists(dir, ec)) {
            for (auto& ent : fs::directory_iterator(dir, ec)) {
                if (!ent.is_regular_file(ec))
                    continue;
                const auto name = ent.path().filename().string();
                // security-event-N.wal
                if (name.rfind("security-event-", 0) != 0 || !name.ends_with(".wal"))
                    continue;
                const auto mid = name.substr(15, name.size() - 15 - 4); // strip prefix + ".wal"
                try {
                    const auto n = static_cast<std::uint32_t>(std::stoul(mid));
                    seg_indices.push_back(n);
                    if (!any || n > max_seg) {
                        max_seg = n;
                        any = true;
                    }
                } catch (...) {
                }
            }
        }
        if (out_replay) {
            out_replay->clear();
            // Issue #4520: bounded startup replay. Walk segments
            // newest→oldest and stop once max_replay records are
            // collected (every caller passes kSecurityEventRingSize);
            // the enable path must never read the whole directory — a
            // legacy default dir with 6655 segments (~6.5 GB) spun
            // restricted startup on `(display "hi")` past timeout 15.
            std::vector<SecurityEventWalRecord> newest_first;
            // Issue #4660: the loop index is hoisted so the post-loop value
            // marks the first UNREAD segment: after the scan stops, indices
            // s-1 .. 0 were never read (s==0 means every segment was read).
            std::uint32_t s = max_seg + 1;
            for (; any && newest_first.size() < max_replay && s > 0; --s) {
                const auto path =
                    (fs::path(dir) / ("security-event-" + std::to_string(s - 1) + ".wal")).string();
                auto part = read_segment_file(path);
                newest_first.insert(newest_first.end(), part.rbegin(), part.rend());
            }
            // Issue #4660: replay-window truncation marker. The window did
            // not reach the oldest record on disk when the cap dropped
            // records from the last-read segment, or when the scan hit
            // max_replay with an existing segment still unread (seg_indices
            // is the exact dirent list, so retention holes do not count).
            replay_truncated = newest_first.size() > max_replay;
            if (!replay_truncated && newest_first.size() >= max_replay) {
                for (const auto n : seg_indices) {
                    if (n < s) {
                        replay_truncated = true;
                        break;
                    }
                }
            }
            if (newest_first.size() > max_replay)
                newest_first.resize(max_replay);
            std::reverse(newest_first.begin(), newest_first.end());
            *out_replay = std::move(newest_first);
            g_security_event_wal_metrics().security_event_wal_replay_count.fetch_add(
                1, std::memory_order_relaxed);
            g_security_event_wal_metrics().security_event_wal_replay_truncated.store(
                replay_truncated ? 1 : 0, std::memory_order_relaxed);
            // Issue #4686: a non-empty replay is not crash recovery by
            // itself — the segment still has to open below. Defer the stamp.
            replayed_nonempty = !out_replay->empty();
        }
        if (!open_segment_unlocked(any ? max_seg : 0)) {
            g_security_event_wal_metrics().security_event_wal_enabled.store(
                0, std::memory_order_relaxed);
            return false;
        }
        // Issue #4686: stamp recovery success only after the segment really
        // opened. Evaluator::enable_mutation_audit_wal reads this counter's
        // delta as sidecar replay success; a phantom delta kept fail-closed
        // disarmed while the WAL was actually off.
        // Issue #4695: a truncated window still publishes
        // security_event_wal_replay_truncated. It must not also claim the
        // sidecar came back whole — that delta means the history is complete
        // and skips the mutation-WAL → SE rebuild. The #4686 nonempty gate
        // stays; the combined predicate withholds the stamp when truncated.
        const bool complete_window = replayed_nonempty && !replay_truncated;
        if (replayed_nonempty) {
            if (complete_window)
                g_security_event_wal_metrics().security_event_crash_recovery_success.fetch_add(
                    1, std::memory_order_relaxed);
        }
        // Issue #4520: upgrade path — a dir oversized by older versions
        // (retention defaulted to 0) is pruned to the cap on first
        // enable, from the dirent list collected above (exact indices,
        // tolerates holes) instead of being fully scanned by replay.
        {
            const auto cap = ::aura::core::wal_slo::wal_max_segments_retention();
            if (cap > 0 && seg_indices.size() > cap) {
                const std::uint64_t total = static_cast<std::uint64_t>(max_seg) + 1;
                const std::uint64_t floor64 = total > cap ? total - cap : 0;
                for (const auto n : seg_indices) {
                    if (static_cast<std::uint64_t>(n) >= floor64)
                        continue;
                    const auto p =
                        (fs::path(dir) / ("security-event-" + std::to_string(n) + ".wal")).string();
                    if (fs::remove(p, ec)) {
                        g_security_event_wal_metrics()
                            .security_event_wal_segment_prune_total.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                    ec.clear();
                }
            }
            const std::uint64_t total_now = static_cast<std::uint64_t>(segment_index) + 1;
            pruned_upto_ =
                cap > 0 && total_now > cap ? static_cast<std::uint32_t>(total_now - cap) : 0;
        }
        enabled = true;
        rotate_open_fail_ = false;
        g_security_event_wal_metrics().security_event_wal_enabled.store(1,
                                                                        std::memory_order_relaxed);
        return true;
    }

    void disable() noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        close_unlocked();
        enabled = false;
        rotate_open_fail_ = false;
        pruned_upto_ = 0;
        dir.clear();
        g_security_event_wal_metrics().security_event_wal_enabled.store(0,
                                                                        std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_enabled() const noexcept { return enabled; }

    [[nodiscard]] std::string directory() const noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        return dir;
    }

    // Issue #3970: live retained files (segment_index + 1). 0 when disabled.
    [[nodiscard]] std::uint32_t retained_segment_count() const noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        return enabled ? (segment_index + 1) : 0;
    }

    // Issue #3205: Agent mid point-query. Linear scan of current segment
    // plus at most (max_segments-1) prior rotate segments (default 2).
    // Issue #3338: production durable callers pass wal_mid_lookup_segments()
    // (default 8 / env). Explicit max_segments=2 stays #3205-compatible.
    // Newest-first. Disabled / mid==0 → nullopt, no I/O. Caller must
    // gate on production + :durable (Soft default never calls this).
    [[nodiscard]] std::optional<SecurityEventWalRecord>
    find_recent_by_mutation_id(std::uint64_t mid, std::uint32_t max_segments = 2) noexcept {
        if (mid == 0 || !enabled || max_segments == 0)
            return std::nullopt;
        std::lock_guard<std::mutex> lock(mtx);
        if (!enabled || dir.empty())
            return std::nullopt;
        if (fp)
            std::fflush(fp);
        namespace fs = std::filesystem;
        const std::uint32_t n = std::min(max_segments, segment_index + 1);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto seg = segment_index - i;
            const auto path =
                (fs::path(dir) / ("security-event-" + std::to_string(seg) + ".wal")).string();
            auto recs = read_segment_file(path);
            for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
                if (it->mutation_id == mid)
                    return *it;
            }
        }
        return std::nullopt;
    }

    // Issue #3970: production/Full explicit-mid continuation past the
    // cheap wal_mid_lookup_segments() window. find_recent_* already
    // scans min(max_segments, segment_index+1) newest-first;
    // kWalFullScanAllSegments therefore covers every retained file.
    // Do not hold mtx then reenter find_recent_* (this wrapper does not
    // take mtx). Disabled / mid==0 → nullopt, no I/O.
    [[nodiscard]] std::optional<SecurityEventWalRecord>
    find_by_mutation_id_scan_all_segments(std::uint64_t mid) noexcept {
        return find_recent_by_mutation_id(mid, ::aura::core::wal_slo::kWalFullScanAllSegments);
    }

    // Append one SecurityEvent to the side-car. Bumps ring_wrap_total
    // implicitly via the in-memory ring (this is just the durable
    // mirror). Batched flush mirrors mutation_audit_wal.
    bool append(const SecurityEventWalRecord& rec) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        // Issue #3056 AC1: WAL-off (`!enabled`) stays silent. Issue #4446:
        // enabled + null fp retries the segment once, then fail-closes
        // like fwrite-miss (append_fail, SLO, overflow).
        if (!enabled || !fp) {
            if (!enabled)
                return false;
            if (!open_segment_unlocked(segment_index))
                return note_rotate_open_fail_unlocked(rec);
            rotate_open_fail_ = false;
        }
        if (current_bytes >= rotate_bytes)
            rotate_unlocked();
        if (!fp)
            return note_rotate_open_fail_unlocked(rec);
        // Issue #3056: inject + fwrite miss share one fail path. WAL-off
        // never reaches here (AC1: `!enabled || !fp` above).
        if (::aura::core::wal_slo::consume_wal_inject_append_fail()) {
            g_security_event_wal_metrics().security_event_wal_append_fail_total.fetch_add(
                1, std::memory_order_relaxed);
            ::aura::core::wal_slo::note_wal_append_fail();
            // Issue #3109: same fail-closed overflow ring capture as fwrite_miss path.
            if (::aura::core::wal_slo::wal_append_fail_closed_active()) {
                WalOverflowRecord ovr{};
                // Issue #3178: stamp the forensic join key (mutation_id)
                // and full record context (tenant/fiber/epoch) so the
                // overflow entry is unjoinable with query:security-audit
                // [mutation-id=…], Typed trail, and
                // CapabilityGrant.bound_mutation_id. Previously stamped
                // rec.seq (WAL sequence number, NOT a join key) and
                // zeroed the context — that path was a residual hole
                // from #3109 that left Agents correlating the wrong trail
                // entry under fail-closed.
                ovr.mid = rec.mutation_id;
                ovr.tenant_id = static_cast<std::uint32_t>(rec.tenant_id);
                ovr.fiber_id = static_cast<std::uint64_t>(rec.fiber_id);
                ovr.epoch = rec.epoch;
                ovr.op = rec.op[0] ? std::string(rec.op) : std::string("security_event_wal_append");
                // Issue #4695: keep inject_fail unless the lost row's own
                // reason names the denial class (then the join can attribute it).
                ovr.reason = se_wal_overflow_miss_reason(rec, "inject_fail");
                (void)wal_overflow_ring_push(ovr);
            }
            return false;
        }
        const auto n = std::fwrite(&rec, 1, sizeof(rec), fp);
        if (n != sizeof(rec)) {
            g_security_event_wal_metrics().security_event_wal_append_fail_total.fetch_add(
                1, std::memory_order_relaxed);
            ::aura::core::wal_slo::note_wal_append_fail();
            // Issue #3109: if production + fail-closed env set, push the
            // lost record to the process-local overflow ring so mid can
            // join from ring replay (does NOT replace WAL replay — only
            // captures the trail for in-process recovery).
            // Issue #3178: stamp forensic join key + full record context
            // (see inject_fail branch above for the same fix).
            if (::aura::core::wal_slo::wal_append_fail_closed_active()) {
                WalOverflowRecord ovr{};
                ovr.mid = rec.mutation_id;
                ovr.tenant_id = static_cast<std::uint32_t>(rec.tenant_id);
                ovr.fiber_id = static_cast<std::uint64_t>(rec.fiber_id);
                ovr.epoch = rec.epoch;
                ovr.op = rec.op[0] ? std::string(rec.op) : std::string("security_event_wal_append");
                // Issue #4695: same class-carrying reason as the inject arm.
                ovr.reason = se_wal_overflow_miss_reason(rec, "fwrite_miss");
                (void)wal_overflow_ring_push(ovr);
            }
            return false;
        }
        ::aura::core::wal_slo::note_wal_append_ok();
        current_bytes += sizeof(rec);
        last_seq_persisted = rec.seq;
        ++unflushed;
        if (unflushed >= kFlushEvery) {
            std::fflush(fp);
            unflushed = 0;
        }
        g_security_event_wal_metrics().security_event_persisted_total.fetch_add(
            1, std::memory_order_relaxed);
        g_security_event_wal_metrics().security_event_wal_bytes_written.fetch_add(
            sizeof(rec), std::memory_order_relaxed);
        return true;
    }

    static std::vector<SecurityEventWalRecord> read_segment_file(const std::string& path) noexcept {
        std::vector<SecurityEventWalRecord> out;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return out;
        char magic[8]{};
        if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kSecurityEventWalMagic, 8) != 0) {
            std::fclose(f);
            return out;
        }
        std::uint32_t ver = 0;
        if (std::fread(&ver, sizeof(ver), 1, f) != 1 || ver != kSecurityEventWalVersion) {
            std::fclose(f);
            return out;
        }
        SecurityEventWalRecord rec{};
        while (std::fread(&rec, 1, sizeof(rec), f) == sizeof(rec)) {
            out.push_back(rec);
        }
        std::fclose(f);
        return out;
    }

    void set_rotate_bytes(std::uint64_t n) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        if (n >= sizeof(SecurityEventWalRecord) * 4)
            rotate_bytes = n;
    }

    void clear_for_test() noexcept {
        disable();
        auto& m = g_security_event_wal_metrics();
        m.security_event_persisted_total.store(0, std::memory_order_relaxed);
        m.security_event_wal_replay_count.store(0, std::memory_order_relaxed);
        m.security_event_crash_recovery_success.store(0, std::memory_order_relaxed);
        m.security_event_wal_append_fail_total.store(0, std::memory_order_relaxed);
        m.security_event_wal_rotate_total.store(0, std::memory_order_relaxed);
        m.security_event_wal_bytes_written.store(0, std::memory_order_relaxed);
        m.security_event_wal_enabled.store(0, std::memory_order_relaxed);
        m.security_event_wal_segments.store(0, std::memory_order_relaxed);
        m.security_event_wal_segment_prune_total.store(0, std::memory_order_relaxed);
        m.security_event_wal_replay_truncated.store(0, std::memory_order_relaxed);
        ::aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
        last_seq_persisted = 0;
        segment_index = 0;
        current_bytes = 0;
        rotate_open_fail_ = false;
        // Issue #4520: reset the full writer state — a stale tiny
        // rotate_bytes from a prior test used to leak into the next one,
        // which now also drags default retention pruning along.
        rotate_bytes = kDefaultRotateBytes;
        pruned_upto_ = 0;
    }
};

// Issue #2225: resolve the SecurityEventWAL directory. Falls through to
// the same env resolution as mutation_audit_wal (#2150) so production
// defaults under multi-tenant / Strict automatically cover both WALs.
// Returns empty string if the env path is empty (AURA_SANDBOX=off /
// dev builds skip the persist layer entirely).
[[nodiscard]] inline std::string resolve_security_event_wal_dir() noexcept {
    // Reuse mutation_audit_wal's resolution — AURA_MUTATION_AUDIT_WAL
    // → AURA_PERSIST_DIR → $TMPDIR/aura-audit. Side-car lives next to
    // the audit-N.wal files.
    return ::aura::core::audit_wal::resolve_mutation_audit_wal_dir();
}

inline SecurityEventWal& g_security_event_wal() noexcept {
    static SecurityEventWal w;
    return w;
}

inline void reset_security_event_wal_for_test() noexcept {
    set_security_event_segment_open_fail_for_test(false);
    g_security_event_wal().clear_for_test();
}


// Helper: fill disk record from a SecurityEvent + timestamp.
inline SecurityEventWalRecord make_record(const SecurityEvent& ev,
                                          std::uint64_t timestamp_ms) noexcept {
    SecurityEventWalRecord r{};
    r.seq = ev.seq;
    r.timestamp_ms = timestamp_ms;
    r.kind = static_cast<std::uint8_t>(ev.kind);
    r.denied = ev.denied ? 1 : 0;
    r.effect_bits = ev.effect_bits;
    r.fiber_id = ev.fiber_id;
    r.tenant_id = ev.tenant_id;
    r.mutation_id = ev.mutation_id;
    r.epoch = ev.epoch;
    const auto n_op = std::min(sizeof(r.op) - 1, sizeof(ev.op));
    std::memcpy(r.op, ev.op, n_op);
    r.op[n_op] = '\0';
    const auto n_reason = std::min(sizeof(r.reason) - 1, sizeof(ev.reason));
    std::memcpy(r.reason, ev.reason, n_reason);
    r.reason[n_reason] = '\0';
    return r;
}

// Copy one WAL row onto a ring slot and keep `seq` (the WAL seq, or the
// synthetic seq assigned to a pre-#4027 row). Does not advance ring.seq.
inline void fill_hydrated_security_event(SecurityEvent& slot, const SecurityEventWalRecord& rec,
                                         std::uint64_t seq) noexcept {
    slot = SecurityEvent{};
    slot.kind = static_cast<SecurityEventKind>(rec.kind);
    slot.seq = seq;
    slot.tenant_id = rec.tenant_id;
    slot.mutation_id = rec.mutation_id;
    slot.epoch = rec.epoch;
    slot.fiber_id = rec.fiber_id;
    slot.effect_bits = rec.effect_bits;
    slot.denied = rec.denied != 0;
    const auto n_op = strnlen(rec.op, sizeof(rec.op));
    if (n_op > 0) {
        const auto n = std::min(n_op, sizeof(slot.op) - 1);
        std::memcpy(slot.op, rec.op, n);
        slot.op[n] = '\0';
    }
    const auto n_reason = strnlen(rec.reason, sizeof(rec.reason));
    if (n_reason > 0) {
        const auto n = std::min(n_reason, sizeof(slot.reason) - 1);
        std::memcpy(slot.reason, rec.reason, n);
        slot.reason[n] = '\0';
    }
}

// Issue #3500: replay WAL records into the live SE ring. Shared by
// Evaluator::enable_security_event_wal and force_wal process-wide
// enable (security_defaults / apply_production_audit_defaults). Empty
// replay is a no-op (live ring stays as caller left it). Soft / WAL-off
// never calls this.
// Issue #4027: new persists stamp real ring_seq. A single rec.seq==0 is
// that first stamped event. More than one zero is a pre-#4027 segment.
// Issue #4445: stamped rows go to rec.seq % ring size with slot.seq kept,
// and ring.seq becomes max(rec.seq)+1. Newest-first and :since-seq then
// match the WAL, and the next append evicts the oldest replayed row.
// pre-#4027 rows renumber from base = max(0, n_total - n) so head and
// slot order still agree. No new query key. WAL format unchanged.
inline void hydrate_security_event_ring_from_wal_replay(
    const std::vector<SecurityEventWalRecord>& replayed) noexcept {
    if (replayed.empty())
        return;
    using ::aura::core::security_event::g_security_event_ring;
    using ::aura::core::security_event::kSecurityEventRingSize;
    using ::aura::core::security_event::reset_security_event_ring_for_test;
    reset_security_event_ring_for_test();

    const auto n_total = static_cast<std::uint64_t>(replayed.size());
    const auto ring_cap = static_cast<std::uint64_t>(kSecurityEventRingSize);
    std::uint64_t zero_count = 0;
    std::uint64_t max_plus = 0;
    std::uint64_t first_stamped = 0;
    bool any_stamped = false;
    for (const auto& rec : replayed) {
        if (rec.seq == 0) {
            ++zero_count;
            continue;
        }
        if (!any_stamped || rec.seq < first_stamped)
            first_stamped = rec.seq;
        any_stamped = true;
        if (rec.seq + 1 > max_plus)
            max_plus = rec.seq + 1;
    }

    auto& ring = g_security_event_ring();
    std::uint64_t next_seq = 0;
    if (!any_stamped) {
        // Every rec.seq is 0. Keep the newest ring-sized suffix.
        const auto n = n_total > ring_cap ? ring_cap : n_total;
        const auto base = n_total - n; // max(0, n_total - n)
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto seq = base + i;
            auto& slot = ring.ring[seq % kSecurityEventRingSize];
            fill_hydrated_security_event(slot, replayed[static_cast<std::size_t>(seq)], seq);
        }
        next_seq = n_total;
    } else if (zero_count > 1) {
        const auto base = first_stamped >= zero_count ? first_stamped - zero_count : 0;
        std::uint64_t zi = 0;
        for (const auto& rec : replayed) {
            const auto seq = rec.seq == 0 ? base + zi++ : rec.seq;
            auto& slot = ring.ring[seq % kSecurityEventRingSize];
            fill_hydrated_security_event(slot, rec, seq);
            if (seq + 1 > max_plus)
                max_plus = seq + 1;
        }
        next_seq = max_plus;
    } else {
        for (const auto& rec : replayed) {
            auto& slot = ring.ring[rec.seq % kSecurityEventRingSize];
            fill_hydrated_security_event(slot, rec, rec.seq);
        }
        next_seq = max_plus;
    }
    if (next_seq == 0)
        next_seq = n_total;
    ring.seq.store(next_seq, std::memory_order_relaxed);
    ring.total.store(n_total, std::memory_order_relaxed);
}

// Issue #4027: wall-clock ms for SE WAL forensic timeline. Soft/WAL-off
// never reaches make_record (persist short-circuits on is_enabled).
inline std::uint64_t security_event_wal_now_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Issue #2225: hot-path persist helper. Called from check_and_record_effect
// (allow + deny) and check_workspace_isolation (deny) immediately after the
// ring append. Short-circuits with a single bool load when WAL is disabled
// (no syscalls, no allocation, ~1 ns). When enabled, builds a transient
// SecurityEvent from the same args the ring call used and persists it.
//
// Building the record from local args (rather than reading back the ring
// slot at our seq) avoids the read-back race: a concurrent append can
// overwrite our slot at seq+size before we read it back, persisting a
// different event under our seq. The forensic loss is bounded to "WAL
// record with mismatched event data", not a missing record.
//
// Issue #4027: ring_seq + timestamp_ms fill join/sort keys on the WAL
// record; still no ring read-back. ring_seq defaults to 0 so direct
// persist helpers/tests and pre-#4027 segments keep #3500 max() hydrate.
inline bool persist_security_event(SecurityEventKind kind, std::uint64_t tenant_id,
                                   std::uint64_t mutation_id, std::uint64_t epoch,
                                   std::uint16_t effect_bits, std::string_view op,
                                   std::string_view reason, bool denied, std::int64_t fiber_id,
                                   std::uint64_t timestamp_ms,
                                   std::uint64_t ring_seq = 0) noexcept {
    if (!g_security_event_wal().is_enabled())
        return false;
    SecurityEvent ev{};
    ev.kind = kind;
    ev.seq = ring_seq; // #4027: stamp allocated ring seq (no ring read-back)
    ev.tenant_id = tenant_id;
    ev.mutation_id = mutation_id;
    ev.epoch = epoch;
    ev.fiber_id = fiber_id;
    ev.effect_bits = effect_bits;
    ev.denied = denied;
    if (!op.empty()) {
        const auto n = std::min(op.size(), sizeof(ev.op) - 1);
        std::memcpy(ev.op, op.data(), n);
        ev.op[n] = '\0';
    }
    if (!reason.empty()) {
        const auto n = std::min(reason.size(), sizeof(ev.reason) - 1);
        std::memcpy(ev.reason, reason.data(), n);
        ev.reason[n] = '\0';
    }
    auto rec = make_record(ev, timestamp_ms);
    return g_security_event_wal().append(rec);
}

// Issue #4646: per-thread count of ENABLED security-event WAL append
// misses. The capability dual-write (record_audit) persists the security
// event FIRST; when that append misses, the durable trail no longer
// matches the in-memory ring (the ring keeps the row, the segment does
// not). The evaluator snapshots the counter around the capability check so
// a miss inside it is attributable to THAT call and not to an earlier one.
// WAL-off never counts: persist_security_event short-circuits before the
// append, so the #3056 fail-open silence is unchanged.
inline std::uint64_t& se_wal_append_miss_counter() noexcept {
    static thread_local std::uint64_t n = 0;
    return n;
}

inline void note_se_wal_append_miss() noexcept {
    ++se_wal_append_miss_counter();
}

[[nodiscard]] inline std::uint64_t se_wal_append_miss_count() noexcept {
    return se_wal_append_miss_counter();
}

// Issue #2388: single dual-write entry for private capability / isolation
// audit rings → SecurityEvent ring + optional WAL. Prefer this over
// separate append + persist at each call site so Agents get one durable
// trail under wrap (ring 1024 + WAL). WAL off: persist short-circuits
// (~1 ns bool load, no syscalls — AC3).
inline void emit_security_event_durable(SecurityEventKind kind, std::uint64_t tenant_id,
                                        std::uint64_t mutation_id, std::uint64_t epoch,
                                        std::uint16_t effect_bits, std::string_view op,
                                        std::string_view reason, bool denied,
                                        std::int64_t fiber_id = 0) noexcept {
    using ::aura::core::security_event::append_security_event;
    using ::aura::core::security_event::g_security_event_ring;
    // Issue #3532: production/Full mid=0 must carry a canonical refuse
    // reason (mid-fallback-refused / grant-mid-refused) or be rewritten to
    // audit-mid-ssot-miss-not-refuse so Agent joins distinguish caller
    // misuse from resolve refuse. Hot path (mid != 0) is one compare.
    // Soft/Off: classifier returns the original reason (zero extra I/O).
    std::string_view use_reason = reason;
    char reason_buf[65];
    if (mutation_id == 0 && aura_classify_mid0_se_reason != nullptr) {
        const auto n = std::min(reason.size(), sizeof(reason_buf) - 1);
        if (n > 0)
            std::memcpy(reason_buf, reason.data(), n);
        reason_buf[n] = '\0';
        if (const char* c = aura_classify_mid0_se_reason(reason_buf))
            use_reason = c;
    }
    // Issue #4027: capture allocated ring seq; persist stamps it + real ts
    // (still builds from local params — no ring read-back / #2225 race).
    const auto ring_seq =
        append_security_event(g_security_event_ring(), kind, tenant_id, mutation_id, epoch,
                              effect_bits, op, use_reason, denied, fiber_id);
    const bool persisted =
        persist_security_event(kind, tenant_id, mutation_id, epoch, effect_bits, op, use_reason,
                               denied, fiber_id, security_event_wal_now_ms(), ring_seq);
    // Issue #4646: an ENABLED security-event WAL append miss lost the
    // durable audit row for this event (the in-memory ring still has it).
    // Record the miss so the evaluator can treat it like a mutation-WAL
    // miss for the same mutate (fail-closed deny with a compensating
    // EffectDeny) and can keep the #3211 SLO streak alive across the
    // paired mutation-WAL success. WAL-off stays #3056 fail-open:
    // persist_security_event short-circuits before the append, so
    // nothing is recorded and the mutate is not denied.
    if (!persisted && g_security_event_wal().is_enabled())
        note_se_wal_append_miss();
    // Issue #3965: production force_wal + SE sidecar off (enable-fail)
    // still fail-closes IsolationDeny/EffectDeny onto the overflow ring
    // so wrap of the 1024 SE ring is not the only trail. fwrite-miss
    // overflow is already pushed inside append(); this arm is WAL-off
    // only. Soft/Off: fail-closed inactive → one production_defaults load.
    if (!persisted && !g_security_event_wal().is_enabled() && denied &&
        ::aura::core::wal_slo::wal_append_fail_closed_active()) {
        WalOverflowRecord ovr{};
        ovr.mid = mutation_id;
        ovr.tenant_id = static_cast<std::uint32_t>(tenant_id);
        ovr.fiber_id = static_cast<std::uint64_t>(fiber_id);
        ovr.epoch = epoch;
        ovr.op = op.empty() ? std::string("se-wal-off") : std::string(op);
        ovr.reason = "se-wal-enable-miss";
        (void)wal_overflow_ring_push(ovr);
    }
}

struct SecurityEventWalStatsSnapshot {
    std::uint64_t persisted = 0;
    std::uint64_t replay_count = 0;
    std::uint64_t crash_recovery_success = 0;
    std::uint64_t append_fail = 0;
    std::uint64_t rotate_total = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t enabled = 0;
    std::uint64_t segments = 0;
    std::uint64_t last_seq = 0;
    int phase = kSecurityEventWalPhase;
    int issue = kSecurityEventWalIssue;
    // Issue #3338: prune count (struct end).
    std::uint64_t segment_prune_total = 0;
    // Issue #4660: replay-window truncation marker (struct end, #2906).
    std::uint64_t replay_truncated = 0;
};


[[nodiscard]] inline SecurityEventWalStatsSnapshot snapshot_security_event_wal_stats() noexcept {
    auto& m = g_security_event_wal_metrics();
    auto& w = g_security_event_wal();
    return SecurityEventWalStatsSnapshot{
        m.security_event_persisted_total.load(std::memory_order_relaxed),
        m.security_event_wal_replay_count.load(std::memory_order_relaxed),
        m.security_event_crash_recovery_success.load(std::memory_order_relaxed),
        m.security_event_wal_append_fail_total.load(std::memory_order_relaxed),
        m.security_event_wal_rotate_total.load(std::memory_order_relaxed),
        m.security_event_wal_bytes_written.load(std::memory_order_relaxed),
        m.security_event_wal_enabled.load(std::memory_order_relaxed),
        m.security_event_wal_segments.load(std::memory_order_relaxed),
        w.last_seq_persisted,
        kSecurityEventWalPhase,
        kSecurityEventWalIssue,
        m.security_event_wal_segment_prune_total.load(std::memory_order_relaxed),
        m.security_event_wal_replay_truncated.load(std::memory_order_relaxed),
    };
}

} // namespace aura::core::security_event_wal

// Issue #4118: the definitions live OUTSIDE security_event_wal so the
// qualified namespace extension is well-formed (it must appear in a scope
// that encloses aura::core::security_event).
namespace aura::core::security_event { // extends the decl namespace from
// core/security_event.hh (qualified form — an unqualified reopen inside
// security_event_wal creates a NEW inner ns, GCC 13/14 verified)

// Issue #4118: SE WAL / overflow join behind the #3877 forensic helpers
// (declarations in core/security_event.hh). Same mid semantics as
// query:security-audit's durable join (#3498/#3674/#3970): newest
// retained row via the wal_mid_lookup_segments() window, then
// scan-all retained segments, then the #3109/#3780 overflow ring.
// The newest row is the same row security-audit surfaces; #3879
// stamps the mutation_wal_append_miss Deny AFTER the effect row, so
// when a miss exists on the mid it is the newest row. mid==0 stays
// refuse-class ring-only (#3462/#3599) — the WAL never persisted it.
[[nodiscard]] inline ForensicEffectVerdict
forensic_effect_verdict_for_mid_full(std::uint64_t mid) noexcept {
    if (mid == 0)
        return ForensicEffectVerdict::None;
    auto& se_wal = ::aura::core::security_event_wal::g_security_event_wal();
    if (se_wal.is_enabled()) {
        const auto win = ::aura::core::wal_slo::wal_mid_lookup_segments();
        auto rec = se_wal.find_recent_by_mutation_id(mid, win);
        if (!rec)
            rec = se_wal.find_by_mutation_id_scan_all_segments(mid);
        if (rec) {
            if (std::string_view(rec->reason) == kMutationWalAppendMissReason)
                return ForensicEffectVerdict::Deny; // #3877 miss forces Deny
            const auto kind = static_cast<SecurityEventKind>(rec->kind);
            if (kind == SecurityEventKind::EffectDeny || rec->denied)
                return ForensicEffectVerdict::Deny;
            if (kind == SecurityEventKind::EffectAllow)
                return ForensicEffectVerdict::Allow;
            return ForensicEffectVerdict::None;
        }
    }
    // Overflow rows carry op/reason only (#3109). The #3879 miss stamp
    // is a Deny. Issue #4695: a denial-class reason on the same field
    // (isolation-deny:*, grant-mid-refused, hygiene-*) is also a Deny.
    // Mechanical tokens and se-wal-enable-miss stay None. In-memory
    // scan only, no I/O.
    if (const auto* ovr = ::aura::core::security_event_wal::wal_overflow_find_by_mid(mid)) {
        if (ovr->reason == kMutationWalAppendMissReason ||
            ::aura::core::security_event_wal::se_wal_reason_names_deny_class(ovr->reason))
            return ForensicEffectVerdict::Deny;
    }
    return ForensicEffectVerdict::None;
}

[[nodiscard]] inline bool forensic_mid_has_wal_append_miss_full(std::uint64_t mid) noexcept {
    if (mid == 0)
        return false;
    auto& se_wal = ::aura::core::security_event_wal::g_security_event_wal();
    if (se_wal.is_enabled()) {
        const auto win = ::aura::core::wal_slo::wal_mid_lookup_segments();
        auto rec = se_wal.find_recent_by_mutation_id(mid, win);
        if (!rec)
            rec = se_wal.find_by_mutation_id_scan_all_segments(mid);
        if (rec)
            return std::string_view(rec->reason) == kMutationWalAppendMissReason;
    }
    if (const auto* ovr = ::aura::core::security_event_wal::wal_overflow_find_by_mid(mid))
        return ovr->reason == kMutationWalAppendMissReason;
    return false;
}

} // namespace aura::core::security_event

#endif // AURA_CORE_SECURITY_EVENT_WAL_HH
