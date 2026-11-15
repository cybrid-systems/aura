// runtime_config.hh — Issue #5245: one typed snapshot for AURA_* runtime knobs.
//
// Before #5245 every module called getenv("AURA_…") at its own call site (297
// reads, 202 names). This header is the single parse point: the table in
// runtime_config_table.inc names every knob with a type, a default and a doc
// string, and the snapshot below is built once (process start or an explicit
// reload) and then read by all modules.
//
// Semantics are unchanged: the snapshot keeps the raw string exactly as
// getenv returned it, so a site that compares "off"/"hard"/"1" keeps doing so.
// Typed accessors (flag/integer/real/size/is_set) are offered on top of the
// same slot for code whose comparison they express exactly.
//
// Tests that must change the environment mid-process go through
// test_setenv()/test_unsetenv(), which set the variable and refresh the same
// snapshot — no second silent getenv path.
//
// Header-only on purpose: every target that reads an AURA_* knob already
// includes headers from src/, and inline variables give the whole process one
// snapshot without adding a link dependency to test binaries.

#ifndef AURA_CORE_RUNTIME_CONFIG_HH
#define AURA_CORE_RUNTIME_CONFIG_HH

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace aura::config {

enum class Kind : unsigned char {
    Flag,    // on/off switch
    Integer, // signed integer
    Real,    // floating point
    Text,    // free-form string
    Path,    // filesystem path
    Size,    // non-negative count / byte size
    Duration // milliseconds unless the name says otherwise
};

enum class Key : unsigned short {
#define AURA_CONFIG_ENTRY(key, name, kind, dflt, doc) key,
#include "runtime_config_table.inc"
#undef AURA_CONFIG_ENTRY
};

struct Entry {
    Key key;
    const char* name;
    Kind kind;
    const char* default_value;
    const char* doc;
};

inline constexpr Entry kEntries[] = {
#define AURA_CONFIG_ENTRY(key, name, kind, dflt, doc) {Key::key, name, Kind::kind, dflt, doc},
#include "runtime_config_table.inc"
#undef AURA_CONFIG_ENTRY
};

inline constexpr std::size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

struct Slot {
    std::string text; // empty when unset
    bool set = false;
    long long integer = 0;
    double real = 0.0;
    bool flag = false;
};

struct Snapshot {
    Slot slots[kEntryCount];
};

namespace detail {

    inline std::atomic<std::shared_ptr<const Snapshot>>& published() noexcept {
        static std::atomic<std::shared_ptr<const Snapshot>> snap;
        return snap;
    }

    inline std::mutex& build_mutex() noexcept {
        static std::mutex m;
        return m;
    }

    // Kept alive so a pointer returned by raw() stays valid for a while even if
    // another thread reloads immediately afterwards.
    inline std::shared_ptr<const Snapshot>* retired() noexcept {
        static std::shared_ptr<const Snapshot> ring[4];
        return ring;
    }

    inline std::size_t& retired_next() noexcept {
        static std::size_t next = 0;
        return next;
    }

    inline std::string& override_slot(Key k) noexcept {
        static std::string overrides[kEntryCount];
        return overrides[static_cast<std::size_t>(k)];
    }

    inline bool& override_set(Key k) noexcept {
        static bool flags[kEntryCount] = {};
        return flags[static_cast<std::size_t>(k)];
    }

    inline void parse_into(Slot& s, const char* v) noexcept {
        s.set = v != nullptr;
        s.text = v ? v : "";
        s.integer = v ? std::strtoll(v, nullptr, 10) : 0;
        s.real = v ? std::strtod(v, nullptr) : 0.0;
        // Canonical on/off reading: set, non-empty, and not "0".
        s.flag = v != nullptr && v[0] != '\0' && v[0] != '0';
    }

    inline std::shared_ptr<const Snapshot> build() {
        auto snap = std::make_shared<Snapshot>();
        for (std::size_t i = 0; i < kEntryCount; ++i) {
            const Key k = kEntries[i].key;
            const char* v = nullptr;
            if (override_set(k))
                v = override_slot(k).c_str();
            else
                v = std::getenv(kEntries[i].name);
            parse_into(snap->slots[i], v);
        }
        return snap;
    }

    inline std::shared_ptr<const Snapshot> current() {
        auto snap = published().load(std::memory_order_acquire);
        if (snap)
            return snap;
        std::lock_guard<std::mutex> lock(build_mutex());
        snap = published().load(std::memory_order_acquire);
        if (!snap) {
            snap = build();
            published().store(snap, std::memory_order_release);
        }
        return snap;
    }

    inline const Slot& slot(Key k) {
        // Keep the snapshot alive across the caller's use of the returned pointer.
        thread_local std::shared_ptr<const Snapshot> held;
        held = current();
        return held->slots[static_cast<std::size_t>(k)];
    }

} // namespace detail

// Parses every AURA_* knob once. Call at process start (main / embed entry
// point); the accessors also initialize lazily so library users that never
// call it still see one consistent snapshot.
inline void initialize() {
    (void)detail::current();
}

// Re-reads the environment into a fresh snapshot; the explicit reload point
// after a deliberate environment change (test fixtures, wrapper setup).
inline void reload() {
    std::lock_guard<std::mutex> lock(detail::build_mutex());
    auto next = detail::build();
    auto prev = detail::published().load(std::memory_order_acquire);
    if (prev) {
        auto* ring = detail::retired();
        auto& idx = detail::retired_next();
        ring[idx % 4] = prev;
        ++idx;
    }
    detail::published().store(next, std::memory_order_release);
}

inline const Entry& entry(Key k) noexcept {
    return kEntries[static_cast<std::size_t>(k)];
}

inline std::span<const Entry> entries() noexcept {
    return kEntries;
}

// Raw value as getenv would return it: nullptr when unset, exact text
// otherwise. Existing comparisons against "off"/"hard"/"1" keep working.
inline const char* raw(Key k) {
    const Slot& s = detail::slot(k);
    return s.set ? s.text.c_str() : nullptr;
}

inline bool is_set(Key k) {
    return detail::slot(k).set;
}

// True when set, non-empty and not starting with '0'.
inline bool flag(Key k) {
    return detail::slot(k).flag;
}

inline long long integer(Key k) {
    return detail::slot(k).integer;
}

inline std::size_t size(Key k) {
    const long long v = detail::slot(k).integer;
    return v > 0 ? static_cast<std::size_t>(v) : 0;
}

inline double real(Key k) {
    return detail::slot(k).real;
}

inline std::string_view text(Key k) {
    return detail::slot(k).text;
}

// Controlled override for embedders/tests that cannot touch the environment.
inline void set_override(Key k, std::string_view value) {
    {
        std::lock_guard<std::mutex> lock(detail::build_mutex());
        detail::override_slot(k).assign(value.begin(), value.end());
        detail::override_set(k) = true;
    }
    reload();
}

inline void clear_override(Key k) {
    {
        std::lock_guard<std::mutex> lock(detail::build_mutex());
        detail::override_slot(k).clear();
        detail::override_set(k) = false;
    }
    reload();
}

// Test fixtures: set/unset the variable and refresh the same snapshot, so a
// test that flips AURA_SANDBOX per case keeps the single parse path.
inline int test_setenv(const char* name, const char* value, int overwrite) {
    const int rc =
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        ::setenv(name, value ? value : "", overwrite);
#endif
    reload();
    return rc;
}

inline int test_unsetenv(const char* name) {
    const int rc =
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        ::unsetenv(name);
#endif
    reload();
    return rc;
}

} // namespace aura::config

#endif // AURA_CORE_RUNTIME_CONFIG_HH
