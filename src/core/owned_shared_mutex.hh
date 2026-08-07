// owned_shared_mutex.hh — Issue #222 / #300: copyable shared_mutex wrapper.
// Extracted from ast.ixx for FlatAST decomposition (step 1 of 4).
//
// Used by FlatAST (flatast_mutex_ / structural_mtx_ / metadata_mtx_ /
// dirty_column_mtx_) and StringPool. Not a public module export — header
// form for the aura.core.ast GMF only.

#ifndef AURA_CORE_OWNED_SHARED_MUTEX_HH
#define AURA_CORE_OWNED_SHARED_MUTEX_HH

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace aura::ast {

// std::shared_mutex is neither copyable nor movable, so a class
// containing it directly has its implicit copy/move ctors
// deleted. This wrapper stores the mutex inline and defines the
// copy/move semantics we want for FlatAST + StringPool:
//
//   - Copy ctor: placement-new a FRESH shared_mutex. Each copy
//     gets its own mutex (independent mutation isolation).
//   - Copy assign: no-op (the destination keeps its own mutex;
//     only the data members are overwritten).
//   - Move ctor / move assign: destination gets a fresh mutex;
//     the source keeps its own until destroyed (lock state is
//     not transferred — same discipline as the old unique_ptr
//     move, which left the moved-from FlatAST with nullptr).
//
// Issue #300 follow-up #1: the previous unique_ptr<std::shared_mutex>
// allocated the mutex on the heap. ~FlatAST ran ~OwnedSharedMutex
// (freeing that heap block) before ~children_, and ASAN caught a
// PCV shared_ptr control block UAF — the freed mutex block was
// reused with a corrupted use_count. Inline storage removes the
// extra heap free entirely.
class OwnedSharedMutex {
public:
    OwnedSharedMutex() noexcept { construct(); }
    ~OwnedSharedMutex() { destroy(); }

    // Copy: fresh mutex (independent isolation).
    OwnedSharedMutex(const OwnedSharedMutex&) noexcept { construct(); }
    // Move: fresh mutex in the destination; source keeps its own.
    OwnedSharedMutex(OwnedSharedMutex&&) noexcept { construct(); }
    // Copy-assign: keep our own mutex (the data being copied
    // doesn't include the mutex state).
    OwnedSharedMutex& operator=(const OwnedSharedMutex&) noexcept { return *this; }
    // Move-assign: keep our own mutex.
    OwnedSharedMutex& operator=(OwnedSharedMutex&&) noexcept { return *this; }

    std::shared_mutex& get() noexcept { return *mutex_ptr(); }
    const std::shared_mutex& get() const noexcept { return *mutex_ptr(); }
    // Like get() but returns a non-const reference even through
    // a const OwnedSharedMutex. Needed because shared_lock /
    // unique_lock require a non-const mutex reference to acquire
    // (the lock state is part of the mutex). The const_cast is
    // safe here because acquiring a lock is a "logical const"
    // operation: it doesn't modify the protected data.
    std::shared_mutex& mutable_get() const noexcept {
        return *const_cast<std::shared_mutex*>(mutex_ptr());
    }

private:
    alignas(std::shared_mutex) std::byte storage_[sizeof(std::shared_mutex)];

    std::shared_mutex* mutex_ptr() noexcept {
        return std::launder(reinterpret_cast<std::shared_mutex*>(storage_));
    }
    const std::shared_mutex* mutex_ptr() const noexcept {
        return std::launder(reinterpret_cast<const std::shared_mutex*>(storage_));
    }
    void construct() { std::construct_at(mutex_ptr()); }
    void destroy() { std::destroy_at(mutex_ptr()); }
};

} // namespace aura::ast

#endif // AURA_CORE_OWNED_SHARED_MUTEX_HH
