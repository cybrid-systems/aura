// ──────────────────────────────────────────────────────────────
//  approval.cpp — the human approval point (#5404)
// ──────────────────────────────────────────────────────────────
#include "bus/approval.hh"

#include <algorithm>

namespace aura::bus {
namespace {
    constexpr std::uint64_t kDefaultApprovalMs = 300000;
} // namespace

void ApprovalBoard::set_retention(std::size_t count) {
    retention_ = std::max<std::size_t>(count, kMinRetention);
}

void ApprovalBoard::remember_decision(std::uint64_t object) {
    const std::uint64_t seq = ++decision_seq_;
    decided_seq_[object] = seq;
    decided_order_.push_back({seq, object});
    ++decided_live_;
}

void ApprovalBoard::prune_decided(std::size_t keep,
                                  const std::function<bool(std::uint64_t)>& keep_object) {
    keep = std::max<std::size_t>(keep, kMinRetention);
    while (decided_live_ > keep && !decided_order_.empty()) {
        const auto [seq, object] = decided_order_.front();
        const auto current = decided_seq_.find(object);
        if (current == decided_seq_.end() || current->second != seq) {
            decided_order_.pop_front();
            continue;
        }
        if (keep_object && keep_object(object))
            break;
        decided_order_.pop_front();
        decided_seq_.erase(current);
        const auto record = records_.find(object);
        if (record != records_.end() && record->second.state != ApprovalState::Pending)
            records_.erase(record);
        --decided_live_;
    }
}

void ApprovalBoard::forget(std::uint64_t object) {
    const auto it = records_.find(object);
    if (it == records_.end())
        return;
    if (it->second.state == ApprovalState::Pending && it->second.deadline_ms != 0)
        pending_.erase({it->second.deadline_ms, object});
    const auto seq = decided_seq_.find(object);
    if (seq != decided_seq_.end()) {
        decided_seq_.erase(seq);
        --decided_live_;
    }
    records_.erase(it);
}

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
    if (record.state == ApprovalState::Pending) {
        if (record.deadline_ms != 0)
            pending_.erase({record.deadline_ms, object});
    } else {
        const auto seq = decided_seq_.find(object);
        if (seq != decided_seq_.end()) {
            decided_seq_.erase(seq);
            --decided_live_;
        }
    }
    record.object = object;
    record.state = ApprovalState::Pending;
    record.deadline_ms = now_ms + timeout_ms;
    record.actor = actor;
    record.decided_ms = 0;
    pending_.insert({record.deadline_ms, object});
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
    if (record.state == ApprovalState::Pending) {
        if (record.deadline_ms != 0)
            pending_.erase({record.deadline_ms, object});
    } else {
        const auto seq = decided_seq_.find(object);
        if (seq != decided_seq_.end()) {
            decided_seq_.erase(seq);
            --decided_live_;
        }
    }
    record.object = object;
    record.state = decision == 1 ? ApprovalState::Approved : ApprovalState::Denied;
    record.deadline_ms = 0;
    record.actor = actor;
    record.decided_ms = now_ms;
    remember_decision(object);
    if (out)
        *out = record;
    if (decided_live_ > retention_ * 2)
        prune_decided(retention_);
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
    while (!pending_.empty()) {
        const auto [deadline, object] = *pending_.begin();
        if (now_ms < deadline)
            break;
        pending_.erase(pending_.begin());
        const auto it = records_.find(object);
        if (it == records_.end() || it->second.state != ApprovalState::Pending ||
            it->second.deadline_ms != deadline)
            continue;
        it->second.state = ApprovalState::Timeout;
        it->second.decided_ms = now_ms;
        remember_decision(object);
        ++count;
        if (events)
            events->push_back(it->second);
    }
    if (decided_live_ > retention_ * 2)
        prune_decided(retention_);
    return count;
}

void ApprovalBoard::restore(const ApprovalRecord& record) {
    if (record.object == 0)
        return;
    forget(record.object);
    records_[record.object] = record;
    if (record.state == ApprovalState::Pending) {
        if (record.deadline_ms != 0)
            pending_.insert({record.deadline_ms, record.object});
    } else {
        remember_decision(record.object);
    }
    if (decided_live_ > retention_ * 2)
        prune_decided(retention_);
}

} // namespace aura::bus
