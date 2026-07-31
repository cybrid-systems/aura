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

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/mutation_audit_wal.hh" // resolve_mutation_audit_wal_dir fallthrough
#include "core/security_event.hh"

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

struct SecurityEventWalMetrics {
    std::atomic<std::uint64_t> security_event_persisted_total{0};
    std::atomic<std::uint64_t> security_event_wal_replay_count{0};
    std::atomic<std::uint64_t> security_event_crash_recovery_success{0};
    std::atomic<std::uint64_t> security_event_wal_append_fail_total{0};
    std::atomic<std::uint64_t> security_event_wal_rotate_total{0};
    std::atomic<std::uint64_t> security_event_wal_bytes_written{0};
    std::atomic<std::uint64_t> security_event_wal_enabled{0};
    std::atomic<std::uint64_t> security_event_wal_segments{0};
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

    void rotate_unlocked() noexcept {
        ++segment_index;
        g_security_event_wal_metrics().security_event_wal_rotate_total.fetch_add(
            1, std::memory_order_relaxed);
        (void)open_segment_unlocked(segment_index);
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
        if (dir.empty()) {
            g_security_event_wal_metrics().security_event_wal_enabled.store(
                0, std::memory_order_relaxed);
            return false;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        std::uint32_t max_seg = 0;
        bool any = false;
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
            std::vector<SecurityEventWalRecord> all;
            for (std::uint32_t s = 0; s <= max_seg && any; ++s) {
                const auto path =
                    (fs::path(dir) / ("security-event-" + std::to_string(s) + ".wal")).string();
                auto part = read_segment_file(path);
                all.insert(all.end(), part.begin(), part.end());
            }
            if (all.size() > max_replay)
                all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(max_replay));
            *out_replay = std::move(all);
            g_security_event_wal_metrics().security_event_wal_replay_count.fetch_add(
                1, std::memory_order_relaxed);
            if (!out_replay->empty())
                g_security_event_wal_metrics().security_event_crash_recovery_success.fetch_add(
                    1, std::memory_order_relaxed);
        }
        if (!open_segment_unlocked(any ? max_seg : 0)) {
            g_security_event_wal_metrics().security_event_wal_enabled.store(
                0, std::memory_order_relaxed);
            return false;
        }
        enabled = true;
        g_security_event_wal_metrics().security_event_wal_enabled.store(1,
                                                                        std::memory_order_relaxed);
        return true;
    }

    void disable() noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        close_unlocked();
        enabled = false;
        dir.clear();
        g_security_event_wal_metrics().security_event_wal_enabled.store(0,
                                                                        std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_enabled() const noexcept { return enabled; }

    [[nodiscard]] std::string directory() const noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        return dir;
    }

    // Append one SecurityEvent to the side-car. Bumps ring_wrap_total
    // implicitly via the in-memory ring (this is just the durable
    // mirror). Batched flush mirrors mutation_audit_wal.
    bool append(const SecurityEventWalRecord& rec) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        if (!enabled || !fp) {
            return false;
        }
        if (current_bytes >= rotate_bytes)
            rotate_unlocked();
        if (!fp)
            return false;
        const auto n = std::fwrite(&rec, 1, sizeof(rec), fp);
        if (n != sizeof(rec)) {
            g_security_event_wal_metrics().security_event_wal_append_fail_total.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
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
        last_seq_persisted = 0;
        segment_index = 0;
        current_bytes = 0;
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
inline bool persist_security_event(SecurityEventKind kind, std::uint64_t tenant_id,
                                   std::uint64_t mutation_id, std::uint64_t epoch,
                                   std::uint16_t effect_bits, std::string_view op,
                                   std::string_view reason, bool denied, std::int64_t fiber_id,
                                   std::uint64_t timestamp_ms) noexcept {
    if (!g_security_event_wal().is_enabled())
        return false;
    SecurityEvent ev{};
    ev.kind = kind;
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
    append_security_event(g_security_event_ring(), kind, tenant_id, mutation_id, epoch, effect_bits,
                          op, reason, denied, fiber_id);
    // Timestamp optional for forensic ordering; 0 is accepted when clock
    // not needed (WAL still stores fields). Avoid pulling <chrono> into
    // every capability include — 0 is fine for durability of content.
    (void)persist_security_event(kind, tenant_id, mutation_id, epoch, effect_bits, op, reason,
                                 denied, fiber_id, /*timestamp_ms=*/0);
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
    };
}

} // namespace aura::core::security_event_wal

#endif // AURA_CORE_SECURITY_EVENT_WAL_HH
