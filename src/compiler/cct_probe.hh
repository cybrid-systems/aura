// ──────────────────────────────────────────────────────────────
//  cct_probe.hh — live calling-context tree recorder (#5122)
//
//  The tree-walker's apply_closure calls this probe. Recording is off
//  unless AURA_BUS_CCT=1, so the default hot path is one TLS bool load.
//  Each closure call creates/updates a node keyed by (parent context,
//  callee identity); samples and exclusive self time accumulate.
//
//  The recorder is thread-local: bus:project-cct publishes the calling
//  thread's tree. A multi-threaded producer should publish per fiber.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_COMPILER_CCT_PROBE_HH
#define AURA_COMPILER_CCT_PROBE_HH

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::cct {

inline constexpr std::uint32_t kMaxNodes = 256;
inline constexpr std::uint32_t kMaxDepth = 64;
inline constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;

struct Node {
    std::uint64_t mid = 0;  // callee identity (body node or name hash)
    std::uint64_t node = 0; // AST node aligned with the callee
    std::uint64_t samples = 0;
    std::uint64_t self_ns = 0;
    std::uint32_t parent = kNoParent;
};

struct Frame {
    std::uint32_t node = 0;
    std::uint64_t child_ns = 0;
    std::uint64_t start_ns = 0;
};

struct Recorder {
    std::array<Node, kMaxNodes> nodes{};
    std::uint32_t count = 0;
    std::array<Frame, kMaxDepth> stack{};
    std::uint32_t depth = 0;
    std::uint64_t dropped = 0; // nodes beyond kMaxNodes / frames beyond kMaxDepth
};

inline bool enabled() {
    static thread_local const bool on = [] {
        const char* v = std::getenv("AURA_BUS_CCT");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

inline Recorder& recorder() {
    static thread_local Recorder r;
    return r;
}

inline std::uint64_t mix_name(std::string_view name) {
    std::uint64_t h = 14695981039346656037ull;
    for (const char c : name) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ull;
    }
    return h;
}

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

inline bool enter(std::uint64_t mid, std::uint64_t node) {
    Recorder& r = recorder();
    if (mid == 0)
        return false;
    if (r.depth >= kMaxDepth) {
        ++r.dropped;
        return false;
    }
    const std::uint32_t parent = r.depth == 0 ? kNoParent : r.stack[r.depth - 1].node;
    std::uint32_t index = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < r.count; ++i) {
        if (r.nodes[i].parent == parent && r.nodes[i].mid == mid) {
            index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (r.count >= kMaxNodes) {
            ++r.dropped;
            return false;
        }
        index = r.count++;
        Node& n = r.nodes[index];
        n.mid = mid;
        n.node = node;
        n.parent = parent;
    }
    ++r.nodes[index].samples;
    r.stack[r.depth++] = Frame{index, 0, now_ns()};
    return true;
}

inline void leave() {
    Recorder& r = recorder();
    if (r.depth == 0)
        return;
    const Frame frame = r.stack[--r.depth];
    const std::uint64_t total = now_ns() - frame.start_ns;
    const std::uint64_t self = total >= frame.child_ns ? total - frame.child_ns : 0;
    r.nodes[frame.node].self_ns += self;
    if (r.depth > 0)
        r.stack[r.depth - 1].child_ns += total;
}

// RAII bracket for one closure call. A zero mid (unresolved closure) is a
// no-op, so failed lookups do not pollute the tree.
class Scope {
public:
    Scope(std::uint64_t mid, std::uint64_t node) {
        if (enabled() && mid != 0)
            entered_ = enter(mid, node);
    }
    ~Scope() {
        if (entered_)
            leave();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool entered_ = false;
};

} // namespace aura::cct

#endif
