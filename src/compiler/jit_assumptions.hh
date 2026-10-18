// Issue #4569: assumption-based JIT invalidation.
//
// A compiled function records the assumptions it was built under instead of
// one process-global mutation epoch:
//   * a content fingerprint of its IR (opcodes, operands, type/shape stamps,
//     referenced string-pool text, escape map, dirty bits) — the code is
//     reusable only for the same IR, so an unrelated edit that re-lowers the
//     module keeps it valid;
//   * DefineVersion(sym) for every name its IR references — bumped by
//     invalidate_function(sym) (rebind / set-code cascade / rollback), so a
//     rebind of A invalidates A and the functions that name A, nothing else.
// The global epoch predicate stays available as a coarse safety net behind
// AURA_JIT_GLOBAL_EPOCH_INVALIDATE=1 (or set_global_epoch_mode_for_test).
//
// Version words are never freed (stable pointers), so a guard can later
// acquire-load one directly without a lock (multi-fiber requirement).
#ifndef AURA_COMPILER_JIT_ASSUMPTIONS_HH
#define AURA_COMPILER_JIT_ASSUMPTIONS_HH

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aura::compiler::jit_assume {

enum class Kind : std::uint8_t { Define = 1, Fingerprint = 2, GlobalEpoch = 3 };

struct VersionWord {
    std::atomic<std::uint64_t> v{1};
};

struct Event {
    std::string fn;
    std::string key;
    Kind kind = Kind::Define;
    std::uint64_t old_version = 0;
    std::uint64_t new_version = 0;
    std::uint64_t mutation_epoch = 0;
};

struct Stats {
    std::atomic<std::uint64_t> bumps{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> define_invalidations{0};
    std::atomic<std::uint64_t> fingerprint_misses{0};
    std::atomic<std::uint64_t> epoch_misses{0};
    std::atomic<std::uint64_t> torn_publishes{0};
    // Issue #4600 §7: JIT Apply prologues compiled with the assumption-word
    // gate (one acquire load; epoch C calls only on the cold moved path).
    std::atomic<std::uint64_t> prologue_word_armed{0};
    // Issue #4600 §4/§5: car/cdr guard slow-path deopt notes (per access that
    // fell back to the locked generic call) + prologue deopt attributions.
    std::atomic<std::uint64_t> guard_deopt_notes{0};
    // Issue #4600 §4: fns marked stormed (deopt count reached the threshold).
    std::atomic<std::uint64_t> storm_marks{0};
};

inline Stats& stats() noexcept {
    static Stats s;
    return s;
}

// Bumped on every DefineVersion change. A publish whose IR was sampled
// before a bump it cannot attribute is stored fail-closed (torn).
inline std::atomic<std::uint64_t>& define_clock() noexcept {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<int>& global_epoch_override() noexcept {
    static std::atomic<int> o{-1}; // -1 = env, 0 = off, 1 = on
    return o;
}

[[nodiscard]] inline bool global_epoch_mode() noexcept {
    const int o = global_epoch_override().load(std::memory_order_relaxed);
    if (o >= 0)
        return o != 0;
    static const bool env = [] {
        const char* e = std::getenv("AURA_JIT_GLOBAL_EPOCH_INVALIDATE");
        return e && *e && std::strcmp(e, "0") != 0;
    }();
    return env;
}

inline void set_global_epoch_mode_for_test(int mode) noexcept {
    global_epoch_override().store(mode, std::memory_order_relaxed);
}

class DefineVersions {
public:
    static DefineVersions& instance() noexcept {
        static DefineVersions d;
        return d;
    }
    // Stable word for `name` (created at version 1).
    VersionWord* word(std::string_view name) {
        auto& sh = shard(name);
        {
            std::shared_lock rl(sh.mu);
            if (auto it = sh.map.find(std::string(name)); it != sh.map.end())
                return it->second.get();
        }
        std::unique_lock wl(sh.mu);
        auto& slot = sh.map[std::string(name)];
        if (!slot)
            slot = std::make_unique<VersionWord>();
        return slot.get();
    }
    [[nodiscard]] std::uint64_t peek(std::string_view name) {
        auto& sh = shard(name);
        std::shared_lock rl(sh.mu);
        auto it = sh.map.find(std::string(name));
        return it == sh.map.end() ? 1 : it->second->v.load(std::memory_order_acquire);
    }
    // Release-store the next version; returns it.
    std::uint64_t bump(std::string_view name) {
        auto* w = word(name);
        const auto nv = w->v.fetch_add(1, std::memory_order_acq_rel) + 1;
        define_clock().fetch_add(1, std::memory_order_acq_rel);
        stats().bumps.fetch_add(1, std::memory_order_relaxed);
        return nv;
    }

private:
    struct Shard {
        std::shared_mutex mu;
        std::unordered_map<std::string, std::unique_ptr<VersionWord>> map;
    };
    static constexpr std::size_t kShards = 16;
    std::array<Shard, kShards> shards_;
    Shard& shard(std::string_view name) noexcept {
        return shards_[std::hash<std::string_view>{}(name) % kShards];
    }
};

class EventLog {
public:
    static EventLog& instance() noexcept {
        static EventLog l;
        return l;
    }
    void record(Event e) {
        std::lock_guard g(mu_);
        if (ring_.size() >= kCap)
            ring_.pop_front();
        ring_.push_back(std::move(e));
        ++total_;
    }
    [[nodiscard]] std::vector<Event> snapshot() const {
        std::lock_guard g(mu_);
        return {ring_.begin(), ring_.end()};
    }
    [[nodiscard]] std::uint64_t total() const {
        std::lock_guard g(mu_);
        return total_;
    }
    void clear_for_test() {
        std::lock_guard g(mu_);
        ring_.clear();
        total_ = 0;
    }

private:
    static constexpr std::size_t kCap = 64;
    mutable std::mutex mu_;
    std::deque<Event> ring_;
    std::uint64_t total_ = 0;
};

// Issue #4600 §4: deopt-storm policy state. Prologue deopts
// (aura_jit_deopt_to_interpreter) and car/cdr guard slow-path deopts
// (aura_jit_note_guard_deopt) count per function name; once the count
// reaches the threshold the fn is "stormed" for the session and the
// service side drops its shape speculation (ShapeProfiler megamorphic
// mark → version bump → shape-specialized cache entries miss → generic
// recompile). Threshold: AURA_JIT_DEOPT_STORM_N (default 3, min 1).
class DeoptStorms {
public:
    static DeoptStorms& instance() noexcept {
        static DeoptStorms d;
        return d;
    }
    void note(std::string_view fn) {
        std::lock_guard g(mu_);
        const auto n = ++counts_[std::string(fn)];
        if (n >= threshold() && stormed_.insert(std::string(fn)).second)
            stats().storm_marks.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool stormed(std::string_view fn) const {
        std::lock_guard g(mu_);
        return stormed_.count(std::string(fn)) != 0;
    }
    [[nodiscard]] std::uint64_t count(std::string_view fn) const {
        std::lock_guard g(mu_);
        const auto it = counts_.find(std::string(fn));
        return it == counts_.end() ? 0 : it->second;
    }
    void clear_for_test() {
        std::lock_guard g(mu_);
        counts_.clear();
        stormed_.clear();
    }
    static std::uint32_t threshold() noexcept {
        static const std::uint32_t t = [] {
            const char* e = std::getenv("AURA_JIT_DEOPT_STORM_N");
            const long v = e && *e ? std::strtol(e, nullptr, 10) : 3;
            return v > 0 ? static_cast<std::uint32_t>(v) : 3u;
        }();
        return t;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::uint64_t> counts_;
    std::unordered_set<std::string> stormed_;
};

// One recorded dependency: the stable word plus the version observed when
// the IR was sampled. version 0 never matches (fail-closed publish).
struct Dep {
    VersionWord* word = nullptr;
    std::uint64_t version = 0;
    std::string name;
};

// Returns the first dependency whose version moved, or nullptr.
[[nodiscard]] inline const Dep* first_broken(const std::vector<Dep>& deps) noexcept {
    for (const auto& d : deps)
        if (!d.word || d.word->v.load(std::memory_order_acquire) != d.version)
            return &d;
    return nullptr;
}

// FNV-1a 64 helpers for the IR fingerprint.
struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
    }
    void u64(std::uint64_t v) noexcept { bytes(&v, sizeof v); }
    void str(std::string_view s) noexcept {
        u64(s.size());
        bytes(s.data(), s.size());
    }
    [[nodiscard]] std::uint64_t done() const noexcept { return h == 0 ? 1 : h; }
};

[[nodiscard]] inline std::string format_stats() {
    auto& s = stats();
    std::string out = "jit-assume: mode=";
    out += global_epoch_mode() ? "global-epoch" : "assumption";
    auto kv = [&out](const char* k, std::uint64_t v) {
        out += ' ';
        out += k;
        out += '=';
        out += std::to_string(v);
    };
    kv("bumps", s.bumps.load(std::memory_order_relaxed));
    kv("hits", s.hits.load(std::memory_order_relaxed));
    kv("define_invalidations", s.define_invalidations.load(std::memory_order_relaxed));
    kv("fingerprint_misses", s.fingerprint_misses.load(std::memory_order_relaxed));
    kv("epoch_misses", s.epoch_misses.load(std::memory_order_relaxed));
    kv("torn_publishes", s.torn_publishes.load(std::memory_order_relaxed));
    kv("prologue_word_armed", s.prologue_word_armed.load(std::memory_order_relaxed));
    kv("guard_deopt_notes", s.guard_deopt_notes.load(std::memory_order_relaxed));
    kv("storm_marks", s.storm_marks.load(std::memory_order_relaxed));
    kv("events", EventLog::instance().total());
    return out;
}

// Most recent events, newest last: "fn kind key old->new @epoch".
[[nodiscard]] inline std::string format_events() {
    std::string out;
    for (const auto& e : EventLog::instance().snapshot()) {
        if (!out.empty())
            out += '\n';
        out += e.fn;
        out += e.kind == Kind::Define        ? " define "
               : e.kind == Kind::Fingerprint ? " fingerprint "
                                             : " global-epoch ";
        out += e.key.empty() ? "-" : e.key;
        out += ' ';
        out += std::to_string(e.old_version);
        out += "->";
        out += std::to_string(e.new_version);
        out += " @";
        out += std::to_string(e.mutation_epoch);
    }
    return out;
}

} // namespace aura::compiler::jit_assume

#endif // AURA_COMPILER_JIT_ASSUMPTIONS_HH
