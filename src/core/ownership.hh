// ownership.hh — Issue #4936: the ownership vocabulary of docs/cpp-style.md R1/R2.
//
// * gsl::owner<T*>   marks the few raw pointers that own (deleters, C APIs).
//                    It is the Core Guidelines alias, defined here so the code
//                    needs no GSL dependency; clang-tidy's
//                    cppcoreguidelines-owning-memory recognises it by name.
// * c_alloc/c_free   the C heap shared with JIT code and C ABI callers.
// * opaque_ptr       a type-erased owner for pimpl members.
// * unique_file      std::FILE* that fcloses itself. open_file() never throws.
// * NoCopy           a member that pins an aggregate RAII guard in place.
// * immortal<T>      a function-local singleton that is constructed on first
//                    use and never destroyed, for state fibers or detached
//                    threads may still touch during static destruction. It
//                    replaces `static auto* p = new T;` without a heap
//                    allocation and without a leak report.
#ifndef AURA_CORE_OWNERSHIP_HH
#define AURA_CORE_OWNERSHIP_HH

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdio.h> // popen / pclose (POSIX)
#include <memory>
#include <type_traits>
#include <utility>

namespace gsl {
template <class T>
    requires std::is_pointer_v<T>
using owner = T;
} // namespace gsl

namespace aura::core {

struct FileCloser {
    void operator()(gsl::owner<std::FILE*> stream) const noexcept {
        if (stream != nullptr) {
            static_cast<void>(std::fclose(stream));
        }
    }
};

using unique_file = std::unique_ptr<std::FILE, FileCloser>;

// popen(3) stream; pclose waits for the child.
struct PipeCloser {
    void operator()(gsl::owner<std::FILE*> stream) const noexcept {
        if (stream != nullptr) {
            static_cast<void>(::pclose(stream));
        }
    }
};

using unique_pipe = std::unique_ptr<std::FILE, PipeCloser>;

// The C heap. Memory that crosses the JIT / C ABI boundary (FlatHashTable,
// pair slabs, FFI buffers) is malloc/free by contract, because emitted code
// and C callers free it. These are the only malloc-family calls in the
// tree; everything else owns through unique_ptr, a container or an arena.
[[nodiscard]] inline void* c_alloc(std::size_t bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): C-heap entry
    // point
    return std::malloc(bytes);
}
[[nodiscard]] inline void* c_calloc(std::size_t count, std::size_t bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): C-heap entry
    // point
    return std::calloc(count, bytes);
}
[[nodiscard]] inline void* c_realloc(void* ptr, std::size_t bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): C-heap entry
    // point
    return std::realloc(ptr, bytes);
}
inline void c_free(void* ptr) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): C-heap exit
    // point
    std::free(ptr);
}

// Owns a C-heap pointer handed out by a C API (realpath, getcwd, strdup).
struct CFree {
    void operator()(void* ptr) const noexcept { c_free(ptr); }
};
template <class T> using unique_c_ptr = std::unique_ptr<T, CFree>;

// A type-erased owner for a pimpl member whose type the declaring module
// interface cannot name (Evaluator's TypeChecker / InferenceEngine). The
// deleter is bound by adopt_opaque<T>, so reset() runs ~T.
struct OpaqueDelete {
    void (*destroy)(void*) noexcept = nullptr;
    void operator()(void* ptr) const noexcept {
        if (destroy != nullptr) {
            destroy(ptr);
        }
    }
};
using opaque_ptr = std::unique_ptr<void, OpaqueDelete>;

template <class T> void destroy_opaque(void* ptr) noexcept {
    const std::unique_ptr<T> owned{static_cast<T*>(ptr)};
}

template <class T> [[nodiscard]] opaque_ptr adopt_opaque(std::unique_ptr<T> owned) noexcept {
    return opaque_ptr{owned.release(), OpaqueDelete{&destroy_opaque<T>}};
}

[[nodiscard]] inline unique_file open_file(const char* path, const char* mode) noexcept {
    return unique_file{std::fopen(path, mode)};
}

// Flushes and closes `file`; reports whether every write reached the OS. The
// flush is where a buffered write fails, so this matches fclose's result.
[[nodiscard]] inline bool close_file(unique_file& file) noexcept {
    if (!file) {
        return true;
    }
    const bool flushed = std::fflush(file.get()) == 0 && std::ferror(file.get()) == 0;
    file.reset();
    return flushed;
}

// NoCopy: a member that makes an aggregate RAII guard neither copyable nor
// movable without giving it a constructor (a constructor would end aggregate
// initialisation). Use it as `[[no_unique_address]] NoCopy no_copy{};` after
// the data members (cpp-style R4).
struct NoCopy {
    NoCopy() = default;
    NoCopy(const NoCopy&) = delete;
    NoCopy& operator=(const NoCopy&) = delete;
    NoCopy(NoCopy&&) = delete;
    NoCopy& operator=(NoCopy&&) = delete;
    ~NoCopy() = default;
};

template <class T> class immortal {
public:
    template <class... Args>
        requires std::constructible_from<T, Args...>
    constexpr explicit immortal(Args&&... args)
        : value_(std::forward<Args>(args)...) {}

    immortal(const immortal&) = delete;
    immortal(immortal&&) = delete;
    immortal& operator=(const immortal&) = delete;
    immortal& operator=(immortal&&) = delete;
    // Deliberately does not destroy value_: the object outlives static
    // destruction so late fibers and detached threads never see a dead object.
    constexpr ~immortal() {}

    [[nodiscard]] constexpr T& get() noexcept { return value_; }
    [[nodiscard]] constexpr const T& get() const noexcept { return value_; }
    [[nodiscard]] constexpr T& operator*() noexcept { return value_; }
    [[nodiscard]] constexpr const T& operator*() const noexcept { return value_; }
    [[nodiscard]] constexpr T* operator->() noexcept { return &value_; }
    [[nodiscard]] constexpr const T* operator->() const noexcept { return &value_; }

private:
    union {
        T value_;
    };
};

} // namespace aura::core

#endif // AURA_CORE_OWNERSHIP_HH
