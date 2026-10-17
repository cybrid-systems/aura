// type_feedback.h — Issue #4568 per-site type feedback.
//
// Side table, not a field on IRFunction (that struct is copied, and
// std::atomic is not). Rows are published after the site array is
// sized and never grow. Writes are relaxed monotone CAS on single
// bytes or target words. No ShapeProfiler shard lock.
//
// Intentionally relaxed hint bytes. Concurrent fibers may race these
// atomics; that is the feedback protocol. Do not no_sanitize them.
// A TSan run of the same join/record functions must stay clean.
//
// Recording stops once a site lattice is Other or a call cache is
// megamorphic, and once the function's back-edge or entry budget is
// spent (cooled). Defaults let the #4566 warmup (fib 8, sumi 20) cool
// before the timed call. Override with AURA_FEEDBACK_BACK_EDGES and
// AURA_FEEDBACK_ENTRIES.

#ifndef AURA_COMPILER_TYPE_FEEDBACK_H
#define AURA_COMPILER_TYPE_FEEDBACK_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aura::compiler::feedback {

inline constexpr std::uint8_t kNone = 0;
inline constexpr std::uint8_t kFixnum = 1;
inline constexpr std::uint8_t kFloat = 2;
inline constexpr std::uint8_t kOther = 3;

inline constexpr std::uint8_t kKindNone = 0;
inline constexpr std::uint8_t kKindArg = 1;
inline constexpr std::uint8_t kKindArith = 2;
inline constexpr std::uint8_t kKindCmp = 3;
inline constexpr std::uint8_t kKindBranch = 4;
inline constexpr std::uint8_t kKindCall = 5;
inline constexpr std::uint8_t kKindCar = 6;
inline constexpr std::uint8_t kKindCdr = 7;
inline constexpr std::uint8_t kKindHash = 8;

inline constexpr std::uint8_t kRecvPair = 1;
inline constexpr std::uint8_t kRecvVector = 2;
inline constexpr std::uint8_t kRecvHash = 3;
inline constexpr std::uint8_t kRecvOther = 4;

// JIT shape_map bytes (aura_jit.h). Not ShapeID (SHAPE_INT is 2 there).
inline constexpr std::uint8_t kJitInt = 1;
inline constexpr std::uint8_t kJitFloat = 2;

inline constexpr std::uint32_t kMaxUserArgs = 8;
inline constexpr std::uint32_t kMaxSites = 4096;
inline constexpr std::uint32_t kMaxBlocks = 64;
inline constexpr std::uint32_t kEmptyTarget = 0xffffffffu;
inline constexpr std::uint32_t kDefaultBackEdges = 8;
inline constexpr std::uint32_t kDefaultEntries = 64;

struct Site {
    // Relaxed feedback bytes (#4568). Monotone CAS. Not a sanitizer hide.
    std::atomic<std::uint8_t> lattice{kNone};
    std::atomic<std::uint8_t> overflow{0};
    std::atomic<std::uint8_t> taken{0};
    std::atomic<std::uint8_t> not_taken{0};
    std::atomic<std::uint32_t> target[4];
    std::atomic<std::uint8_t> ntargets{0};
    std::atomic<std::uint8_t> mega{0};
    std::atomic<std::uint8_t> kind{kKindNone};

    Site() noexcept {
        for (auto& t : target)
            t.store(kEmptyTarget, std::memory_order_relaxed);
    }
    Site(const Site&) = delete;
    Site& operator=(const Site&) = delete;
};

struct Row {
    std::string name;
    std::uint32_t free_count = 0;
    std::uint32_t arg_count = 0;
    std::uint32_t site_count = 0;
    std::uint64_t fingerprint = 0;
    std::unique_ptr<Site[]> sites;
    std::atomic<std::uint8_t> arg_lat[kMaxUserArgs]{};
    std::atomic<std::uint32_t> entries{0};
    std::atomic<std::uint32_t> back_edges{0};
    std::atomic<std::uint8_t> cooled{0};
};

struct Recorder {
    std::shared_ptr<Row> hold;
    Row* row = nullptr;
    bool on = false;
    std::uint32_t backs = 0;
    std::uint32_t free_count = 0;
    std::uint32_t base[kMaxBlocks]{};
    std::uint32_t nblocks = 0;

    [[nodiscard]] std::uint32_t site(std::uint32_t block, std::uint32_t ii) const noexcept {
        if (!on || block >= nblocks)
            return kEmptyTarget;
        return base[block] + ii;
    }
};

struct EnterInfo {
    const void* key = nullptr;
    const char* name = nullptr;
    std::uint32_t name_len = 0;
    std::uint32_t free_count = 0;
    std::uint32_t arg_count = 0;
    std::uint32_t site_count = 0;
    std::uint64_t fingerprint = 0;
    std::uint32_t nblocks = 0;
    std::uint32_t bases[kMaxBlocks]{};
};

struct ArgSlot {
    std::uint32_t dest = 0;
    std::uint32_t arg_index = 0;
};

namespace detail {

    struct Table {
        std::mutex mu;
        std::atomic<std::uint64_t> epoch{1};
        std::unordered_map<std::string, std::shared_ptr<Row>> rows;
    };

    struct Tls {
        const void* key = nullptr;
        std::uint64_t epoch = 0;
        std::shared_ptr<Row> row;
        std::uint32_t bases[kMaxBlocks]{};
        std::uint32_t nblocks = 0;
        std::uint32_t free_count = 0;
        bool known_off = false;
    };

    // One row map for the interpreter, the tree walker, and query:.
    // A function-local static in this header splits across C++ modules.
    // ir_executor_impl.cpp owns the production definition.
    // AURA_TYPE_FEEDBACK_OWN_TABLE is the standalone TSan driver, which
    // does not link that file.
#ifdef AURA_TYPE_FEEDBACK_OWN_TABLE
    // Standalone TSan driver. It does not link ir_executor_impl.cpp.
    inline Table& table() noexcept {
        static Table t;
        return t;
    }
#elif defined(AURA_TYPE_FEEDBACK_DEFINE_TABLE)
    // Emitted once, from ir_executor_impl.cpp's include. A handwritten
    // definition in that file's global module fragment is ill-formed.
    Table& table() noexcept {
        static Table t;
        return t;
    }
#else
    Table& table() noexcept;
#endif

    inline Tls& tls_slot() noexcept {
        static thread_local Tls t;
        return t;
    }

    inline std::uint32_t read_budget(const char* env, std::uint32_t fallback) noexcept {
        const char* v = std::getenv(env);
        if (!v || !*v)
            return fallback;
        char* end = nullptr;
        long n = std::strtol(v, &end, 10);
        if (end == v || n <= 0 || n > 1000000)
            return fallback;
        return static_cast<std::uint32_t>(n);
    }

    inline std::uint32_t back_edge_budget() noexcept {
        static const std::uint32_t v = read_budget("AURA_FEEDBACK_BACK_EDGES", kDefaultBackEdges);
        return v;
    }

    inline std::uint32_t entry_budget() noexcept {
        static const std::uint32_t v = read_budget("AURA_FEEDBACK_ENTRIES", kDefaultEntries);
        return v;
    }

    inline bool public_name(std::string_view n) noexcept {
        if (n.empty() || n == "__lambda__" || n == "__self_placeholder__" || n == "__top__")
            return false;
        if (n.starts_with("__lambda__"))
            return false;
        return true;
    }

    inline bool name_matches(std::string_view row, std::string_view q) noexcept {
        if (q.empty())
            return false;
        if (row == q)
            return true;
        return row.size() > q.size() && row.starts_with(q) && row[q.size()] == '#';
    }

    inline std::string map_key(const EnterInfo& in) {
        std::string_view n(in.name ? in.name : "", in.name_len);
        if (public_name(n))
            return std::string(n);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "#%lx",
                      static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(in.key)));
        return std::string(buf);
    }

    inline std::uint8_t joined(std::uint8_t cur, std::uint8_t obs) noexcept {
        if (obs == kNone || cur == obs || cur == kOther)
            return cur;
        if (cur == kNone)
            return obs;
        return kOther;
    }

    inline void join_atomic(std::atomic<std::uint8_t>& cell, std::uint8_t obs) noexcept {
        std::uint8_t cur = cell.load(std::memory_order_relaxed);
        while (cur != kOther) {
            const std::uint8_t next = joined(cur, obs);
            if (next == cur)
                return;
            if (cell.compare_exchange_weak(cur, next, std::memory_order_relaxed,
                                           std::memory_order_relaxed))
                return;
        }
    }

    inline void bump_sat(std::atomic<std::uint8_t>& cell) noexcept {
        std::uint8_t v = cell.load(std::memory_order_relaxed);
        while (v < 255 &&
               !cell.compare_exchange_weak(v, static_cast<std::uint8_t>(v + 1),
                                           std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    inline const char* lat_word(std::uint8_t lat) noexcept {
        switch (lat) {
            case kFixnum:
                return "fixnum";
            case kFloat:
                return "float";
            case kOther:
                return "other";
            default:
                return "none";
        }
    }

    inline std::shared_ptr<Row> make_row(const EnterInfo& in, const std::string& key) {
        auto row = std::make_shared<Row>();
        std::string_view n(in.name ? in.name : "", in.name_len);
        row->name = public_name(n) ? std::string(n) : key;
        row->free_count = in.free_count;
        row->arg_count = in.arg_count;
        row->fingerprint = in.fingerprint;
        row->site_count = in.site_count > kMaxSites ? kMaxSites : in.site_count;
        if (row->site_count)
            row->sites = std::make_unique<Site[]>(row->site_count);
        return row;
    }

    inline void fill_rec(Recorder& r, const std::shared_ptr<Row>& row, const std::uint32_t* bases,
                         std::uint32_t nblocks, std::uint32_t free_count) noexcept {
        r.hold = row;
        r.row = row.get();
        r.free_count = free_count;
        r.nblocks = nblocks;
        for (std::uint32_t i = 0; i < nblocks && i < kMaxBlocks; ++i)
            r.base[i] = bases[i];
        const auto n = row->entries.fetch_add(1, std::memory_order_relaxed);
        const bool cooled = row->cooled.load(std::memory_order_relaxed) != 0;
        r.on = !cooled && n < entry_budget();
        if (!r.on)
            tls_slot().known_off = true;
    }

    inline void publish_tls(const EnterInfo& in, const std::shared_ptr<Row>& row) {
        auto& tls = tls_slot();
        tls.key = in.key;
        tls.epoch = table().epoch.load(std::memory_order_relaxed);
        tls.row = row;
        tls.nblocks = in.nblocks;
        tls.free_count = in.free_count;
        tls.known_off = false;
        for (std::uint32_t i = 0; i < in.nblocks && i < kMaxBlocks; ++i)
            tls.bases[i] = in.bases[i];
    }

    inline void drop_anonymous_over_cap(Table& t) {
        if (t.rows.size() <= 256)
            return;
        for (auto it = t.rows.begin(); it != t.rows.end();) {
            if (!it->first.empty() && it->first[0] == '#')
                it = t.rows.erase(it);
            else
                ++it;
        }
    }

} // namespace detail

inline bool known_off(const void* key) noexcept {
    auto& tls = detail::tls_slot();
    if (!tls.known_off || tls.key != key || !tls.row)
        return false;
    return tls.epoch == detail::table().epoch.load(std::memory_order_acquire);
}

inline bool fast_hit(const void* key) noexcept {
    auto& tls = detail::tls_slot();
    if (!tls.row || tls.key != key)
        return false;
    return tls.epoch == detail::table().epoch.load(std::memory_order_acquire);
}

inline Recorder enter_fast() noexcept {
    Recorder r;
    auto& tls = detail::tls_slot();
    if (!tls.row || tls.epoch != detail::table().epoch.load(std::memory_order_acquire))
        return r;
    detail::fill_rec(r, tls.row, tls.bases, tls.nblocks, tls.free_count);
    if (!r.on)
        r.nblocks = 0;
    return r;
}

inline Recorder enter_slow(const EnterInfo& in) {
    Recorder r;
    auto& t = detail::table();
    std::shared_ptr<Row> row;
    {
        std::lock_guard lock(t.mu);
        detail::drop_anonymous_over_cap(t);
        const std::string key = detail::map_key(in);
        auto it = t.rows.find(key);
        if (it == t.rows.end() || it->second->fingerprint != in.fingerprint ||
            it->second->site_count != (in.site_count > kMaxSites ? kMaxSites : in.site_count)) {
            row = detail::make_row(in, key);
            t.rows[key] = row;
            t.epoch.fetch_add(1, std::memory_order_release);
        } else {
            row = it->second;
        }
    }
    detail::publish_tls(in, row);
    detail::fill_rec(r, row, in.bases, in.nblocks, in.free_count);
    if (!r.on)
        r.nblocks = 0;
    return r;
}

// Tree walker: one synthetic site per named closure. The key is the
// closure id (plus one, so id 0 is not a null key). Rebind mints a new
// id, so a cooled TLS slot does not hide the new body. Empty and
// anonymous names are not rows — query: looks them up by define name.
inline Recorder enter_named(const void* key, std::string_view name, std::uint32_t argc) {
    Recorder r;
    if (!key || !detail::public_name(name))
        return r;
    if (known_off(key))
        return r;
    if (fast_hit(key)) {
        r = enter_fast();
        if (r.row)
            return r;
    }
    EnterInfo info;
    info.key = key;
    info.name = name.data();
    info.name_len = static_cast<std::uint32_t>(name.size());
    info.arg_count = argc;
    info.site_count = 1;
    info.fingerprint = 1;
    info.nblocks = 1;
    info.bases[0] = 0;
    return enter_slow(info);
}

inline void note_arg(Row* row, std::uint32_t site, std::uint32_t arg_index,
                     std::uint32_t free_count, std::uint8_t lat) noexcept {
    if (!row)
        return;
    if (site < row->site_count && row->sites &&
        row->sites[site].lattice.load(std::memory_order_relaxed) != kOther) {
        Site& s = row->sites[site];
        s.kind.store(kKindArg, std::memory_order_relaxed);
        detail::join_atomic(s.lattice, lat);
    }
    if (arg_index < free_count)
        return;
    const std::uint32_t user = arg_index - free_count;
    if (user < kMaxUserArgs && row->arg_lat[user].load(std::memory_order_relaxed) != kOther)
        detail::join_atomic(row->arg_lat[user], lat);
}

inline void note_arith(Row* row, std::uint32_t site, std::uint8_t la, std::uint8_t lb,
                       bool overflow) noexcept {
    if (!row || site >= row->site_count || !row->sites)
        return;
    Site& s = row->sites[site];
    if (overflow)
        s.overflow.store(1, std::memory_order_relaxed);
    if (s.lattice.load(std::memory_order_relaxed) == kOther)
        return;
    s.kind.store(kKindArith, std::memory_order_relaxed);
    detail::join_atomic(s.lattice, detail::joined(la, lb));
}

inline void note_cmp(Row* row, std::uint32_t site, std::uint8_t la, std::uint8_t lb) noexcept {
    note_arith(row, site, la, lb, false);
    if (row && site < row->site_count && row->sites &&
        row->sites[site].lattice.load(std::memory_order_relaxed) != kOther)
        row->sites[site].kind.store(kKindCmp, std::memory_order_relaxed);
}

inline void note_branch(Row* row, std::uint32_t site, bool taken) noexcept {
    if (!row || site >= row->site_count || !row->sites)
        return;
    Site& s = row->sites[site];
    s.kind.store(kKindBranch, std::memory_order_relaxed);
    detail::bump_sat(taken ? s.taken : s.not_taken);
}

inline void note_back(Recorder& fb) noexcept {
    if (!fb.on || !fb.row)
        return;
    fb.row->back_edges.fetch_add(1, std::memory_order_relaxed);
    if (++fb.backs >= detail::back_edge_budget()) {
        fb.on = false;
        fb.row->cooled.store(1, std::memory_order_relaxed);
    }
}

inline void note_call(Row* row, std::uint32_t site, std::uint32_t target_id) noexcept {
    if (!row || site >= row->site_count || !row->sites)
        return;
    Site& s = row->sites[site];
    if (s.mega.load(std::memory_order_relaxed))
        return;
    s.kind.store(kKindCall, std::memory_order_relaxed);
    for (int spin = 0; spin < 8; ++spin) {
        bool placed = false;
        for (int i = 0; i < 4; ++i) {
            std::uint32_t cur = s.target[i].load(std::memory_order_relaxed);
            if (cur == target_id)
                return;
            if (cur != kEmptyTarget)
                continue;
            if (s.target[i].compare_exchange_weak(cur, target_id, std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
                s.ntargets.fetch_add(1, std::memory_order_relaxed);
                placed = true;
                break;
            }
        }
        if (placed)
            return;
        bool full = true;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t cur = s.target[i].load(std::memory_order_relaxed);
            if (cur == target_id)
                return;
            if (cur == kEmptyTarget)
                full = false;
        }
        if (full) {
            s.mega.store(1, std::memory_order_relaxed);
            return;
        }
    }
}

// Caller row, before enter_named overwrites TLS. One site is enough
// for the polymorphic query (two targets, or the megamorphic bit).
inline void note_call_current(std::uint32_t target) noexcept {
    auto& tls = detail::tls_slot();
    if (!tls.row || tls.known_off)
        return;
    if (tls.epoch != detail::table().epoch.load(std::memory_order_acquire))
        return;
    if (tls.row->cooled.load(std::memory_order_relaxed)) {
        tls.known_off = true;
        return;
    }
    note_call(tls.row.get(), 0, target);
}

// Special-form while. The tree walker does not inline the loop into
// IR Jump/Branch, so the back-edge budget lives here. After the budget
// this is a TLS flag check.
inline void note_loop_edge() noexcept {
    auto& tls = detail::tls_slot();
    if (!tls.row || tls.known_off)
        return;
    if (tls.epoch != detail::table().epoch.load(std::memory_order_acquire))
        return;
    if (tls.row->cooled.load(std::memory_order_relaxed)) {
        tls.known_off = true;
        return;
    }
    const auto n = tls.row->back_edges.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n >= detail::back_edge_budget()) {
        tls.row->cooled.store(1, std::memory_order_relaxed);
        tls.known_off = true;
    }
}

inline void note_recv(Row* row, std::uint32_t site, std::uint8_t kind, std::uint8_t recv) noexcept {
    if (!row || site >= row->site_count || !row->sites)
        return;
    Site& s = row->sites[site];
    s.kind.store(kind, std::memory_order_relaxed);
    std::uint8_t cur = s.overflow.load(std::memory_order_relaxed);
    if (cur == 0) {
        s.overflow.compare_exchange_strong(cur, recv, std::memory_order_relaxed,
                                           std::memory_order_relaxed);
        return;
    }
    if (cur != recv)
        s.overflow.store(kRecvOther, std::memory_order_relaxed);
}

inline void reset_name(std::string_view name) {
    if (name.empty())
        return;
    auto& t = detail::table();
    std::lock_guard lock(t.mu);
    bool dropped = false;
    for (auto it = t.rows.begin(); it != t.rows.end();) {
        if (detail::name_matches(it->first, name) || detail::name_matches(it->second->name, name)) {
            it = t.rows.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    if (dropped)
        t.epoch.fetch_add(1, std::memory_order_release);
}

inline std::string render(std::string_view name) {
    std::vector<std::shared_ptr<Row>> hits;
    {
        auto& t = detail::table();
        std::lock_guard lock(t.mu);
        for (auto& [k, row] : t.rows) {
            if (!row)
                continue;
            if (detail::name_matches(k, name) || detail::name_matches(row->name, name))
                hits.push_back(row);
        }
    }
    std::string out;
    out.reserve(256);
    const auto bb = detail::back_edge_budget();
    const auto eb = detail::entry_budget();
    if (hits.empty()) {
        out += "fn=";
        out.append(name);
        out += " entries=0 back-edges=0 cooled=0 budget-back=";
        out += std::to_string(bb);
        out += " budget-entry=";
        out += std::to_string(eb);
        out += "\narg0=none\ncall-targets=\nmegamorphic=0\n";
        return out;
    }
    for (const auto& row : hits) {
        out += "fn=";
        out += row->name;
        out += " entries=";
        out += std::to_string(row->entries.load(std::memory_order_relaxed));
        out += " back-edges=";
        out += std::to_string(row->back_edges.load(std::memory_order_relaxed));
        out += " cooled=";
        out += row->cooled.load(std::memory_order_relaxed) ? "1" : "0";
        out += " budget-back=";
        out += std::to_string(bb);
        out += " budget-entry=";
        out += std::to_string(eb);
        out += " sites=";
        out += std::to_string(row->site_count);
        out += "\n";
        const std::uint32_t nargs = row->arg_count == 0 ? 1 : row->arg_count;
        const std::uint32_t shown = nargs < kMaxUserArgs ? nargs : kMaxUserArgs;
        for (std::uint32_t i = 0; i < shown; ++i) {
            out += "arg";
            out += std::to_string(i);
            out += "=";
            out += detail::lat_word(row->arg_lat[i].load(std::memory_order_relaxed));
            out += "\n";
        }
        std::vector<std::uint32_t> targets;
        bool mega = false;
        if (row->sites) {
            for (std::uint32_t s = 0; s < row->site_count; ++s) {
                Site& site = row->sites[s];
                if (site.kind.load(std::memory_order_relaxed) != kKindCall)
                    continue;
                if (site.mega.load(std::memory_order_relaxed))
                    mega = true;
                for (int i = 0; i < 4; ++i) {
                    const auto id = site.target[i].load(std::memory_order_relaxed);
                    if (id != kEmptyTarget)
                        targets.push_back(id);
                }
            }
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        out += "call-targets=";
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (i)
                out += ",";
            out += std::to_string(targets[i]);
        }
        out += "\nmegamorphic=";
        out += mega ? "1" : "0";
        out += "\n";
    }
    return out;
}

// Fills JIT shape codes for monomorphic user arguments. Macro-origin
// functions stay generic (#1610). An Other arg refuses the map.
// Empty feedback returns false (no synthetic SHAPE_INT seed).
inline bool shape_map_from_feedback(std::string_view fn_name, std::uint8_t source_marker,
                                    std::uint32_t local_count, const ArgSlot* args,
                                    std::uint32_t nargs, std::vector<std::uint8_t>& storage) {
    storage.clear();
    if (source_marker == 1 || fn_name.empty() || !args || nargs == 0)
        return false;
    std::shared_ptr<Row> row;
    {
        auto& t = detail::table();
        std::lock_guard lock(t.mu);
        auto it = t.rows.find(std::string(fn_name));
        if (it == t.rows.end()) {
            for (auto& [k, cand] : t.rows) {
                if (detail::name_matches(k, fn_name) ||
                    (cand && detail::name_matches(cand->name, fn_name))) {
                    row = cand;
                    break;
                }
            }
        } else {
            row = it->second;
        }
    }
    if (!row)
        return false;
    storage.assign(local_count, 0);
    bool any = false;
    for (std::uint32_t i = 0; i < nargs; ++i) {
        const auto idx = args[i].arg_index;
        if (idx < row->free_count)
            continue;
        const std::uint32_t user = idx - row->free_count;
        if (user >= kMaxUserArgs)
            continue;
        const auto lat = row->arg_lat[user].load(std::memory_order_relaxed);
        if (lat == kOther) {
            storage.clear();
            return false;
        }
        if (lat != kFixnum && lat != kFloat)
            continue;
        if (args[i].dest >= local_count)
            continue;
        storage[args[i].dest] = lat == kFixnum ? kJitInt : kJitFloat;
        any = true;
    }
    if (!any)
        storage.clear();
    return any;
}

} // namespace aura::compiler::feedback

#endif
