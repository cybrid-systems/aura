// mutation_audit_wal.hh — Issue #1567: append-only mutation audit WAL
// + crash recovery replay into the in-memory ring.
// Header form for evaluator TUs + tests. Does not change ring layout.

#ifndef AURA_CORE_MUTATION_AUDIT_WAL_HH
#define AURA_CORE_MUTATION_AUDIT_WAL_HH

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

namespace aura::core::audit_wal {

inline constexpr int kAuditWalPhase = 2;
inline constexpr int kAuditWalIssue = 1567;
inline constexpr char kAuditWalMagic[8] = {'A', 'U', 'R', 'A', 'W', 'A', 'L', '1'};
inline constexpr std::uint32_t kAuditWalVersion = 1;
// Default rotate after ~1 MiB of records (keeps segments small).
inline constexpr std::uint64_t kDefaultRotateBytes = 1ull << 20;

// On-disk / replay record — POD, fixed size for O(1) append + mmap-friendly layout.
// Mirrors MutationAuditEntry fields + epoch for full provenance chain.
#pragma pack(push, 1)
struct AuditWalRecord {
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    std::int64_t fiber_id = 0;
    std::uint32_t nodes_changed = 0;
    std::uint32_t epoch_delta = 0;
    std::uint32_t target_node = 0;
    char op[48]{};
    std::uint16_t effect_bits = 0;
    std::uint16_t reserved0 = 0;
    std::uint64_t tenant_id = 0;
    std::uint64_t provenance_mutation_id = 0;
    std::uint64_t epoch = 0; // bridge / provenance epoch at emit
    std::uint8_t effect_denied = 0;
    std::uint8_t reserved1[7]{};
};
#pragma pack(pop)

static_assert(sizeof(AuditWalRecord) == 8 + 8 + 8 + 4 + 4 + 4 + 48 + 2 + 2 + 8 + 8 + 8 + 1 + 7,
              "AuditWalRecord size stable for WAL format");

struct AuditWalMetrics {
    std::atomic<std::uint64_t> audit_record_persisted_total{0};
    std::atomic<std::uint64_t> audit_wal_replay_count{0};
    std::atomic<std::uint64_t> audit_crash_recovery_success{0};
    std::atomic<std::uint64_t> audit_wal_append_fail_total{0};
    std::atomic<std::uint64_t> audit_wal_rotate_total{0};
    std::atomic<std::uint64_t> audit_wal_bytes_written{0};
    std::atomic<std::uint64_t> audit_wal_enabled{0};
    std::atomic<std::uint64_t> audit_wal_segments{0};
};

inline AuditWalMetrics& g_audit_wal_metrics() noexcept {
    static AuditWalMetrics m;
    return m;
}

// Process-wide WAL controller (optional persist).
struct MutationAuditWal {
    std::mutex mtx;
    bool enabled = false;
    std::string dir;
    std::string current_path;
    std::FILE* fp = nullptr;
    std::uint64_t current_bytes = 0;
    std::uint64_t rotate_bytes = kDefaultRotateBytes;
    std::uint32_t segment_index = 0;
    std::uint64_t last_seq_persisted = 0;
    std::uint32_t unflushed = 0;
    // Batch fflush every N appends for <5% hot-path overhead (still
    // fflush on rotate/disable for crash recovery of recent batch).
    static constexpr std::uint32_t kFlushEvery = 32;

    ~MutationAuditWal() { close_unlocked(); }

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
        current_path = (fs::path(dir) / ("audit-" + std::to_string(seg) + ".wal")).string();
        // Append mode for crash-safe continue; create if missing.
        fp = std::fopen(current_path.c_str(), "ab+");
        if (!fp)
            return false;
        // Larger stdio buffer for hot-path append amortization (#1567 AC6).
        static char sbuf[64 * 1024];
        std::setvbuf(fp, sbuf, _IOFBF, sizeof(sbuf));
        // Measure current size
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            close_unlocked();
            return false;
        }
        const auto pos = std::ftell(fp);
        current_bytes = pos > 0 ? static_cast<std::uint64_t>(pos) : 0;
        // Write magic header on fresh file
        if (current_bytes == 0) {
            if (std::fwrite(kAuditWalMagic, 1, 8, fp) != 8) {
                close_unlocked();
                return false;
            }
            const std::uint32_t ver = kAuditWalVersion;
            if (std::fwrite(&ver, sizeof(ver), 1, fp) != 1) {
                close_unlocked();
                return false;
            }
            current_bytes = 8 + sizeof(ver);
            std::fflush(fp);
        }
        segment_index = seg;
        g_audit_wal_metrics().audit_wal_segments.store(seg + 1, std::memory_order_relaxed);
        return true;
    }

    void rotate_unlocked() noexcept {
        ++segment_index;
        g_audit_wal_metrics().audit_wal_rotate_total.fetch_add(1, std::memory_order_relaxed);
        (void)open_segment_unlocked(segment_index);
    }

    // Enable persist under `persist_dir`. Replays existing WAL into `out_records`
    // (last `max_replay` records, oldest→newest). Returns true if enabled.
    bool enable(std::string_view persist_dir, std::vector<AuditWalRecord>* out_replay = nullptr,
                std::size_t max_replay = 64) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        close_unlocked();
        dir.assign(persist_dir);
        enabled = false;
        if (dir.empty()) {
            g_audit_wal_metrics().audit_wal_enabled.store(0, std::memory_order_relaxed);
            return false;
        }
        // Discover highest segment index for append continuity.
        namespace fs = std::filesystem;
        std::error_code ec;
        std::uint32_t max_seg = 0;
        bool any = false;
        if (fs::exists(dir, ec)) {
            for (auto& ent : fs::directory_iterator(dir, ec)) {
                if (!ent.is_regular_file(ec))
                    continue;
                const auto name = ent.path().filename().string();
                // audit-N.wal
                if (name.rfind("audit-", 0) != 0 || !name.ends_with(".wal"))
                    continue;
                const auto mid = name.substr(6, name.size() - 6 - 4);
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
        // Replay all segments first (crash recovery).
        if (out_replay) {
            out_replay->clear();
            std::vector<AuditWalRecord> all;
            for (std::uint32_t s = 0; s <= max_seg && any; ++s) {
                const auto path =
                    (fs::path(dir) / ("audit-" + std::to_string(s) + ".wal")).string();
                auto part = read_segment_file(path);
                all.insert(all.end(), part.begin(), part.end());
            }
            // Keep last max_replay
            if (all.size() > max_replay)
                all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(max_replay));
            *out_replay = std::move(all);
            g_audit_wal_metrics().audit_wal_replay_count.fetch_add(1, std::memory_order_relaxed);
            if (!out_replay->empty())
                g_audit_wal_metrics().audit_crash_recovery_success.fetch_add(
                    1, std::memory_order_relaxed);
        }
        if (!open_segment_unlocked(any ? max_seg : 0)) {
            g_audit_wal_metrics().audit_wal_enabled.store(0, std::memory_order_relaxed);
            return false;
        }
        enabled = true;
        g_audit_wal_metrics().audit_wal_enabled.store(1, std::memory_order_relaxed);
        return true;
    }

    void disable() noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        close_unlocked();
        enabled = false;
        dir.clear();
        g_audit_wal_metrics().audit_wal_enabled.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_enabled() const noexcept { return enabled; }

    bool append(const AuditWalRecord& rec) noexcept {
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
            g_audit_wal_metrics().audit_wal_append_fail_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            return false;
        }
        current_bytes += sizeof(rec);
        last_seq_persisted = rec.seq;
        ++unflushed;
        // Batched flush: amortize syscall cost; full flush on rotate/close.
        if (unflushed >= kFlushEvery) {
            std::fflush(fp);
            unflushed = 0;
        }
        g_audit_wal_metrics().audit_record_persisted_total.fetch_add(1, std::memory_order_relaxed);
        g_audit_wal_metrics().audit_wal_bytes_written.fetch_add(sizeof(rec),
                                                                std::memory_order_relaxed);
        return true;
    }

    static std::vector<AuditWalRecord> read_segment_file(const std::string& path) noexcept {
        std::vector<AuditWalRecord> out;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return out;
        char magic[8]{};
        if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kAuditWalMagic, 8) != 0) {
            std::fclose(f);
            return out;
        }
        std::uint32_t ver = 0;
        if (std::fread(&ver, sizeof(ver), 1, f) != 1 || ver != kAuditWalVersion) {
            std::fclose(f);
            return out;
        }
        AuditWalRecord rec{};
        while (std::fread(&rec, 1, sizeof(rec), f) == sizeof(rec)) {
            out.push_back(rec);
        }
        std::fclose(f);
        return out;
    }

    void set_rotate_bytes(std::uint64_t n) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        if (n >= sizeof(AuditWalRecord) * 4)
            rotate_bytes = n;
    }

    void clear_for_test() noexcept {
        disable();
        auto& m = g_audit_wal_metrics();
        m.audit_record_persisted_total.store(0, std::memory_order_relaxed);
        m.audit_wal_replay_count.store(0, std::memory_order_relaxed);
        m.audit_crash_recovery_success.store(0, std::memory_order_relaxed);
        m.audit_wal_append_fail_total.store(0, std::memory_order_relaxed);
        m.audit_wal_rotate_total.store(0, std::memory_order_relaxed);
        m.audit_wal_bytes_written.store(0, std::memory_order_relaxed);
        m.audit_wal_enabled.store(0, std::memory_order_relaxed);
        m.audit_wal_segments.store(0, std::memory_order_relaxed);
        last_seq_persisted = 0;
        segment_index = 0;
        current_bytes = 0;
    }
};

inline MutationAuditWal& g_mutation_audit_wal() noexcept {
    static MutationAuditWal w;
    return w;
}

inline void reset_audit_wal_for_test() noexcept {
    g_mutation_audit_wal().clear_for_test();
}

struct AuditWalStatsSnapshot {
    std::uint64_t persisted = 0;
    std::uint64_t replay_count = 0;
    std::uint64_t crash_recovery_success = 0;
    std::uint64_t append_fail = 0;
    std::uint64_t rotate_total = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t enabled = 0;
    std::uint64_t segments = 0;
    std::uint64_t last_seq = 0;
    int phase = kAuditWalPhase;
    int issue = kAuditWalIssue;
};

[[nodiscard]] inline AuditWalStatsSnapshot snapshot_audit_wal_stats() noexcept {
    auto& m = g_audit_wal_metrics();
    auto& w = g_mutation_audit_wal();
    return AuditWalStatsSnapshot{
        m.audit_record_persisted_total.load(std::memory_order_relaxed),
        m.audit_wal_replay_count.load(std::memory_order_relaxed),
        m.audit_crash_recovery_success.load(std::memory_order_relaxed),
        m.audit_wal_append_fail_total.load(std::memory_order_relaxed),
        m.audit_wal_rotate_total.load(std::memory_order_relaxed),
        m.audit_wal_bytes_written.load(std::memory_order_relaxed),
        m.audit_wal_enabled.load(std::memory_order_relaxed),
        m.audit_wal_segments.load(std::memory_order_relaxed),
        w.last_seq_persisted,
        kAuditWalPhase,
        kAuditWalIssue,
    };
}

// Helper: fill disk record from ring-like fields.
inline AuditWalRecord make_record(std::uint64_t seq, std::uint64_t timestamp_ms,
                                  std::int64_t fiber_id, std::uint32_t nodes_changed,
                                  std::uint32_t epoch_delta, std::uint32_t target_node,
                                  std::string_view op, std::uint16_t effect_bits,
                                  std::uint64_t tenant_id, std::uint64_t provenance_mutation_id,
                                  std::uint64_t epoch, bool effect_denied) noexcept {
    AuditWalRecord r{};
    r.seq = seq;
    r.timestamp_ms = timestamp_ms;
    r.fiber_id = fiber_id;
    r.nodes_changed = nodes_changed;
    r.epoch_delta = epoch_delta;
    r.target_node = target_node;
    const auto n = std::min(op.size(), sizeof(r.op) - 1);
    std::memcpy(r.op, op.data(), n);
    r.op[n] = '\0';
    r.effect_bits = effect_bits;
    r.tenant_id = tenant_id;
    r.provenance_mutation_id = provenance_mutation_id;
    r.epoch = epoch;
    r.effect_denied = effect_denied ? 1 : 0;
    return r;
}

} // namespace aura::core::audit_wal

#endif // AURA_CORE_MUTATION_AUDIT_WAL_HH
