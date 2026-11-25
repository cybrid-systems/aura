// ──────────────────────────────────────────────────────────────
//  approval.hh — the human approval point (#5404)
//
//  A pending object (a task/contract id) has a deadline; an approval
//  or denial is recorded, and a pending object that runs out of time
//  becomes denied (fail-closed). The board is in-memory; the audit log
//  is the durable record.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_APPROVAL_HH
#define AURA_BUS_APPROVAL_HH

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aura::bus {

enum class ApprovalState : std::uint64_t {
    Pending = 0,
    Approved = 1,
    Denied = 2,
    Timeout = 3,
};

struct ApprovalRecord {
    std::uint64_t object = 0;
    ApprovalState state = ApprovalState::Pending;
    std::uint64_t deadline_ms = 0;
    std::uint64_t actor = 0;
    std::uint64_t decided_ms = 0;
};

class ApprovalBoard {
public:
    // #5460: decided approvals are a bounded window; the pending set is
    // an ordered index, so an expiry sweep only visits expired entries.
    static constexpr std::size_t kDefaultRetention = 4096;
    static constexpr std::size_t kMinRetention = 1;

    void set_retention(std::size_t count);

    // Create (or refresh) a pending approval. 0 timeout uses the default.
    bool request(std::uint64_t object, std::uint64_t actor, std::uint64_t timeout_ms,
                 std::uint64_t now_ms, ApprovalRecord* out, std::string* why);
    // decision: 1 = approve, 2 = deny. An absent record is created.
    bool decide(std::uint64_t object, std::uint64_t decision, std::uint64_t actor,
                std::uint64_t now_ms, ApprovalRecord* out, std::string* why);
    [[nodiscard]] bool get(std::uint64_t object, ApprovalRecord* out) const;
    [[nodiscard]] bool has_pending() const { return !pending_.empty(); }
    [[nodiscard]] std::vector<ApprovalRecord> list() const;
    // Mark expired pending records Timeout. One event per expiry.
    std::size_t expire(std::uint64_t now_ms, std::vector<ApprovalRecord>* events);
    // #5427: re-apply a persisted approval (checkpoint replay).
    void restore(const ApprovalRecord& record);
    // #5460: drop a pruned object's record (the task window shrank).
    void forget(std::uint64_t object);
    // #5460: retire old decided records, keeping at most keep. When
    // keep_object says an object is still live, stop at it (the caller's
    // task board protects its own gate state).
    void prune_decided(std::size_t keep,
                       const std::function<bool(std::uint64_t)>& keep_object = {});
    [[nodiscard]] const std::map<std::uint64_t, ApprovalRecord>& all() const { return records_; }
    [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }
    [[nodiscard]] std::size_t decided_count() const { return decided_live_; }

private:
    void remember_decision(std::uint64_t object);

    std::map<std::uint64_t, ApprovalRecord> records_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> pending_;         // (deadline, object)
    std::deque<std::pair<std::uint64_t, std::uint64_t>> decided_order_; // (seq, object)
    std::unordered_map<std::uint64_t, std::uint64_t> decided_seq_;
    std::uint64_t decision_seq_ = 0;
    std::size_t decided_live_ = 0;
    std::size_t retention_ = kDefaultRetention;
};

} // namespace aura::bus

#endif
