// ──────────────────────────────────────────────────────────────
//  approval.cpp — the human approval point (#5404)
// ──────────────────────────────────────────────────────────────
#include "bus/approval.hh"

namespace aura::bus {
namespace {
    constexpr std::uint64_t kDefaultApprovalMs = 300000;
} // namespace

bool ApprovalBoard::request(std::uint64_t object, std::uint64_t actor, std::uint64_t timeout_ms,
                            std::uint64_t now_ms, ApprovalRecord* out, std::string* why) {
    if (object == 0) {
        if (why)
            *why = "approval object is zero";
        return false;
    }
    if (timeout_ms == 0)
        timeout_ms = kDefaultApprovalMs;
    ApprovalRecord& record = records_[object];
    record.object = object;
    record.state = ApprovalState::Pending;
    record.deadline_ms = now_ms + timeout_ms;
    record.actor = actor;
    record.decided_ms = 0;
    if (out)
        *out = record;
    return true;
}

bool ApprovalBoard::decide(std::uint64_t object, std::uint64_t decision, std::uint64_t actor,
                           std::uint64_t now_ms, ApprovalRecord* out, std::string* why) {
    if (object == 0) {
        if (why)
            *why = "approval object is zero";
        return false;
    }
    if (decision != 1 && decision != 2) {
        if (why)
            *why = "approval decision must be 1 (approve) or 2 (deny)";
        return false;
    }
    ApprovalRecord& record = records_[object];
    record.object = object;
    record.state = decision == 1 ? ApprovalState::Approved : ApprovalState::Denied;
    record.deadline_ms = 0;
    record.actor = actor;
    record.decided_ms = now_ms;
    if (out)
        *out = record;
    return true;
}

bool ApprovalBoard::get(std::uint64_t object, ApprovalRecord* out) const {
    const auto it = records_.find(object);
    if (it == records_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

bool ApprovalBoard::has_pending() const {
    for (const auto& [object, record] : records_) {
        (void)object;
        if (record.state == ApprovalState::Pending)
            return true;
    }
    return false;
}

std::vector<ApprovalRecord> ApprovalBoard::list() const {
    std::vector<ApprovalRecord> out;
    out.reserve(records_.size());
    for (const auto& [object, record] : records_) {
        (void)object;
        out.push_back(record);
    }
    return out;
}

std::size_t ApprovalBoard::expire(std::uint64_t now_ms, std::vector<ApprovalRecord>* events) {
    std::size_t count = 0;
    for (auto& [object, record] : records_) {
        (void)object;
        if (record.state != ApprovalState::Pending || record.deadline_ms == 0 ||
            now_ms < record.deadline_ms)
            continue;
        record.state = ApprovalState::Timeout;
        record.decided_ms = now_ms;
        ++count;
        if (events)
            events->push_back(record);
    }
    return count;
}

} // namespace aura::bus
