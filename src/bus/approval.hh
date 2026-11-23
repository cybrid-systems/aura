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

#include <cstdint>
#include <map>
#include <string>
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
    // Create (or refresh) a pending approval. 0 timeout uses the default.
    bool request(std::uint64_t object, std::uint64_t actor, std::uint64_t timeout_ms,
                 std::uint64_t now_ms, ApprovalRecord* out, std::string* why);
    // decision: 1 = approve, 2 = deny. An absent record is created.
    bool decide(std::uint64_t object, std::uint64_t decision, std::uint64_t actor,
                std::uint64_t now_ms, ApprovalRecord* out, std::string* why);
    [[nodiscard]] bool get(std::uint64_t object, ApprovalRecord* out) const;
    [[nodiscard]] bool has_pending() const;
    [[nodiscard]] std::vector<ApprovalRecord> list() const;
    // Mark expired pending records Timeout. One event per expiry.
    std::size_t expire(std::uint64_t now_ms, std::vector<ApprovalRecord>* events);

private:
    std::map<std::uint64_t, ApprovalRecord> records_;
};

} // namespace aura::bus

#endif
