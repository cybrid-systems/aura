// handoff_token_stash.hh — Issue #3729: per-Evaluator bounded HandoffToken
// staging. Not AgentRegistry. Process-level g_handoff_token_stash is a
// hash→Evaluator* route so import/join on another Evaluator can find the
// source stash given the returned string (unguessable). Tokens live here.
#ifndef AURA_COMPILER_HANDOFF_TOKEN_STASH_HH
#define AURA_COMPILER_HANDOFF_TOKEN_STASH_HH

#include "orch/orch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace aura::compiler {

inline constexpr int kHandoffTokenStashIssue = 3729;
inline constexpr std::size_t kHandoffTokenStashCap = 64;
// Join-via-token is observe-only and must not pin forever (#3729).
inline constexpr std::uint64_t kHandoffJoinTtlNs = 5'000'000'000ull;

struct HandoffStashEntry {
    aura::orch::HandoffToken tok;
    std::uint64_t expire_steady_ns = 0; // 0 = not yet join-stamped
};

[[nodiscard]] inline std::uint64_t handoff_steady_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] inline std::string make_handoff_token_hash() {
    static std::atomic<std::uint64_t> seq{1};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    const auto r = static_cast<std::uint64_t>(std::random_device{}());
    char buf[48];
    std::snprintf(buf, sizeof(buf), "handoff:%016llx-%llu", static_cast<unsigned long long>(r),
                  static_cast<unsigned long long>(n));
    return std::string(buf);
}

struct HandoffTokenStash {
    struct InsertResult {
        std::string hash;
        std::string evicted; // empty if nothing dropped
    };

    InsertResult insert(aura::orch::HandoffToken tok) {
        std::lock_guard<std::mutex> lock(mu_);
        InsertResult out;
        if (fifo_.size() >= kHandoffTokenStashCap && !fifo_.empty()) {
            out.evicted = fifo_.front();
            fifo_.pop_front();
            by_hash_.erase(out.evicted);
        }
        out.hash = make_handoff_token_hash();
        HandoffStashEntry e;
        e.tok = std::move(tok);
        by_hash_[out.hash] = std::move(e);
        fifo_.push_back(out.hash);
        return out;
    }

    bool take(const std::string& hash, aura::orch::HandoffToken& out) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = by_hash_.find(hash);
        if (it == by_hash_.end())
            return false;
        out = std::move(it->second.tok);
        by_hash_.erase(it);
        for (auto q = fifo_.begin(); q != fifo_.end(); ++q) {
            if (*q == hash) {
                fifo_.erase(q);
                break;
            }
        }
        return true;
    }

    bool peek(const std::string& hash, aura::orch::HandoffToken& out) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = by_hash_.find(hash);
        if (it == by_hash_.end())
            return false;
        const auto now = handoff_steady_ns();
        if (it->second.expire_steady_ns != 0 && now >= it->second.expire_steady_ns) {
            by_hash_.erase(it);
            for (auto q = fifo_.begin(); q != fifo_.end(); ++q) {
                if (*q == hash) {
                    fifo_.erase(q);
                    break;
                }
            }
            return false;
        }
        if (it->second.expire_steady_ns == 0)
            it->second.expire_steady_ns = now + kHandoffJoinTtlNs;
        out = it->second.tok;
        return true;
    }

    [[nodiscard]] bool contains(const std::string& hash) const {
        std::lock_guard<std::mutex> lock(mu_);
        return by_hash_.find(hash) != by_hash_.end();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return by_hash_.size();
    }

    [[nodiscard]] std::vector<std::string> hashes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return {fifo_.begin(), fifo_.end()};
    }

    std::vector<std::string> drain_hashes() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> out(fifo_.begin(), fifo_.end());
        fifo_.clear();
        by_hash_.clear();
        return out;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, HandoffStashEntry> by_hash_;
    std::deque<std::string> fifo_;
};

// Issue #3729 / #3089: process hash→Evaluator* route (not a token
// registry, not AgentRegistry). Bounded by per-Evaluator cap. Identifier
// retained for #3148/#3273 linters.
struct HandoffRouteTable {
    std::mutex mu;
    std::unordered_map<std::string, void*> by_hash; // Evaluator*
    std::deque<std::string> fifo;

    void route(const std::string& hash, void* ev) {
        std::lock_guard<std::mutex> lock(mu);
        by_hash[hash] = ev;
        fifo.push_back(hash);
    }
    void unroute(const std::string& hash) {
        std::lock_guard<std::mutex> lock(mu);
        by_hash.erase(hash);
        for (auto q = fifo.begin(); q != fifo.end(); ++q) {
            if (*q == hash) {
                fifo.erase(q);
                break;
            }
        }
    }
    void unroute_all(const std::vector<std::string>& hashes) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& h : hashes) {
            by_hash.erase(h);
        }
        if (!hashes.empty()) {
            std::deque<std::string> kept;
            for (auto& h : fifo) {
                if (by_hash.find(h) != by_hash.end())
                    kept.push_back(std::move(h));
            }
            fifo.swap(kept);
        }
    }
    [[nodiscard]] void* find(const std::string& hash) {
        std::lock_guard<std::mutex> lock(mu);
        auto it = by_hash.find(hash);
        return it == by_hash.end() ? nullptr : it->second;
    }
};

inline HandoffRouteTable g_handoff_token_stash{};

} // namespace aura::compiler

#endif
