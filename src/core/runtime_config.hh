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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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

enum class Key : std::uint8_t {
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

inline constexpr auto kEntries = std::to_array<Entry>({
#define AURA_CONFIG_ENTRY(key, name, kind, dflt, doc) {Key::key, name, Kind::kind, dflt, doc},
#include "runtime_config_table.inc"
#undef AURA_CONFIG_ENTRY
});

inline constexpr std::size_t kEntryCount = kEntries.size();

struct Slot {
    std::string text; // empty when unset
    long long integer = 0;
    double real = 0.0;
    bool set = false;
    bool flag = false;
    char explicit_padding[6] = {};
};

struct Snapshot {
    std::array<Slot, kEntryCount> slots;
};

namespace detail {

    inline std::atomic<std::shared_ptr<const Snapshot>>& published() noexcept {
        static std::atomic<std::shared_ptr<const Snapshot>> snap;
        return snap;
    }

    inline std::mutex& build_mutex() noexcept {
        static std::mutex build_lock;
        return build_lock;
    }

    // Kept alive so a pointer returned by raw() stays valid for a while even if
    // another thread reloads immediately afterwards.
    inline std::array<std::shared_ptr<const Snapshot>, 4>& retired() noexcept {
        // NOLINTNEXTLINE(misc-const-correctness): reload() rotates this ring.
        static std::array<std::shared_ptr<const Snapshot>, 4> ring;
        return ring;
    }

    inline std::size_t& retired_next() noexcept {
        static std::size_t next = 0;
        return next;
    }

    inline std::string& override_slot(Key key) noexcept {
        static std::array<std::string, kEntryCount> overrides;
        return overrides.at(static_cast<std::size_t>(key));
    }

    inline bool& override_set(Key key) noexcept {
        static std::array<bool, kEntryCount> flags{};
        return flags.at(static_cast<std::size_t>(key));
    }

    inline void parse_into(Slot& slot, const char* value) noexcept {
        slot.set = value != nullptr;
        slot.text = value != nullptr ? value : "";
        slot.integer = value != nullptr ? std::strtoll(value, nullptr, 10) : 0;
        slot.real = value != nullptr ? std::strtod(value, nullptr) : 0.0;
        // Canonical on/off reading: set, non-empty, and not "0".
        slot.flag = value != nullptr && *value != '\0' && *value != '0';
    }

    inline std::shared_ptr<const Snapshot> build() {
        auto snap = std::make_shared<Snapshot>();
        for (std::size_t i = 0; i < kEntryCount; ++i) {
            const Key key = kEntries.at(i).key;
            const char* value = nullptr;
            if (override_set(key))
                value = override_slot(key).c_str();
            else
                value = std::getenv(kEntries.at(i).name);
            parse_into(snap->slots.at(i), value);
        }
        return snap;
    }

    inline std::shared_ptr<const Snapshot> current() {
        auto snap = published().load(std::memory_order_acquire);
        if (snap)
            return snap;
        std::lock_guard<std::mutex> const lock(build_mutex());
        snap = published().load(std::memory_order_acquire);
        if (!snap) {
            snap = build();
            published().store(snap, std::memory_order_release);
        }
        return snap;
    }

    inline const Slot& slot(Key key) {
        // Keep the snapshot alive across the caller's use of the returned pointer.
        thread_local std::shared_ptr<const Snapshot> held;
        held = current();
        return held->slots.at(static_cast<std::size_t>(key));
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
    std::lock_guard<std::mutex> const lock(detail::build_mutex());
    auto const next = detail::build();
    auto prev = detail::published().load(std::memory_order_acquire);
    if (prev) {
        auto& ring = detail::retired();
        auto& idx = detail::retired_next();
        ring.at(idx % 4) = std::move(prev);
        ++idx;
    }
    detail::published().store(next, std::memory_order_release);
}

inline const Entry& entry(Key key) noexcept {
    return kEntries.at(static_cast<std::size_t>(key));
}

inline std::span<const Entry> entries() noexcept {
    return std::span<const Entry, kEntryCount>(kEntries);
}

// Raw value as getenv would return it: nullptr when unset, exact text
// otherwise. Existing comparisons against "off"/"hard"/"1" keep working.
inline const char* raw(Key key) {
    const Slot& slot_value = detail::slot(key);
    return slot_value.set ? slot_value.text.c_str() : nullptr;
}

inline bool is_set(Key key) {
    return detail::slot(key).set;
}

// True when set, non-empty and not starting with '0'.
inline bool flag(Key key) {
    return detail::slot(key).flag;
}

inline long long integer(Key key) {
    return detail::slot(key).integer;
}

inline std::size_t size(Key key) {
    const long long value = detail::slot(key).integer;
    return value > 0 ? static_cast<std::size_t>(value) : 0;
}

inline double real(Key key) {
    return detail::slot(key).real;
}

inline std::string_view text(Key key) {
    return detail::slot(key).text;
}

// Controlled override for embedders/tests that cannot touch the environment.
inline void set_override(Key key, std::string_view value) {
    {
        std::lock_guard<std::mutex> const lock(detail::build_mutex());
        detail::override_slot(key).assign(value.begin(), value.end());
        detail::override_set(key) = true;
    }
    reload();
}

inline void clear_override(Key key) {
    {
        std::lock_guard<std::mutex> const lock(detail::build_mutex());
        detail::override_slot(key).clear();
        detail::override_set(key) = false;
    }
    reload();
}

// Test fixtures: set/unset the variable and refresh the same snapshot, so a
// test that flips AURA_SANDBOX per case keeps the single parse path.
inline int test_setenv(const char* name, const char* value, int overwrite) {
    const int result_code =
#if defined(_WIN32)
        _putenv_s(name, value != nullptr ? value : "");
#else
        ::setenv(name, value != nullptr ? value : "", overwrite);
#endif
    reload();
    return result_code;
}

inline int test_unsetenv(const char* name) {
    const int result_code =
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        ::unsetenv(name);
#endif
    reload();
    return result_code;
}

} // namespace aura::config

#endif // AURA_CORE_RUNTIME_CONFIG_HH
