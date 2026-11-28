// ──────────────────────────────────────────────────────────────
//  capability.cpp — attenuation, grant tree, worldline ownership (#5124)
// ──────────────────────────────────────────────────────────────
#include "bus/capability.hh"

#include <algorithm>
#include <chrono>
#include <random>

#include <sys/random.h> // Issue #5505: unpredictable grant tokens

namespace aura::bus {
namespace {

    std::uint64_t audit_now_ms() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    bool subset_bits(std::uint64_t child, std::uint64_t parent) {
        return (child & ~parent) == 0;
    }

} // namespace

const char* capability_name(std::uint64_t bit) {
    switch (bit) {
        case kCapRead:
            return "read";
        case kCapPublish:
            return "publish";
        case kCapTrial:
            return "trial";
        case kCapKeep:
            return "keep";
        case kCapRollback:
            return "rollback";
        case kCapGrant:
            return "grant";
        case kCapConstraintWrite:
            return "constraint-write";
        case kCapApprove:
            return "approve";
    }
    return "unknown";
}

bool attenuates(const Capability& parent, const Capability& child, std::string* why) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    if (parent.workspace != child.workspace)
        return fail("workspace differs");
    if (!subset_bits(child.bits, parent.bits))
        return fail("adds a capability bit");
    if ((child.channels & ~parent.channels) != 0)
        return fail("adds a channel bit");
    if (parent.max_loans != 0 && (child.max_loans == 0 || child.max_loans > parent.max_loans))
        return fail("widens the loan quota");
    if (parent.expires_gen != 0 &&
        (child.expires_gen == 0 || child.expires_gen > parent.expires_gen))
        return fail("widens the expiry");
    if (parent.worldline != 0 && child.worldline != parent.worldline)
        return fail("changes the worldline scope");
    if (parent.task != 0 && child.task != parent.task)
        return fail("changes the task scope");
    return true;
}

Capability intersect(const Capability& a, const Capability& b) {
    Capability out{};
    out.bits = a.bits & b.bits;
    out.channels = a.channels & b.channels;
    out.workspace = a.workspace == b.workspace ? a.workspace : 0;
    if (a.max_loans == 0)
        out.max_loans = b.max_loans;
    else if (b.max_loans == 0)
        out.max_loans = a.max_loans;
    else
        out.max_loans = std::min(a.max_loans, b.max_loans);
    if (a.expires_gen == 0)
        out.expires_gen = b.expires_gen;
    else if (b.expires_gen == 0)
        out.expires_gen = a.expires_gen;
    else
        out.expires_gen = std::min(a.expires_gen, b.expires_gen);
    if (a.worldline == 0)
        out.worldline = b.worldline;
    else if (b.worldline == 0 || b.worldline == a.worldline)
        out.worldline = a.worldline;
    if (a.task == 0)
        out.task = b.task;
    else if (b.task == 0 || b.task == a.task)
        out.task = a.task;
    return out;
}

bool allows(const Capability& caps, std::uint64_t bit, std::uint64_t channel,
            std::uint64_t generation, std::uint64_t worldline, std::string* why) {
    if ((caps.bits & bit) == 0) {
        if (why)
            *why = std::string("missing capability: ") + capability_name(bit);
        return false;
    }
    if (channel >= 64 || (caps.channels & (1ull << channel)) == 0) {
        if (why)
            *why = "channel not in the capability mask";
        return false;
    }
    if (caps.expires_gen != 0 && generation > caps.expires_gen) {
        if (why)
            *why = "capability expired at generation " + std::to_string(caps.expires_gen);
        return false;
    }
    // The worldline scope narrows keep/rollback (worldline-targeted ops);
    // read/publish/subscribe/audit are not worldline-targeted (worldline
    // arg 0) and stay allowed under a scoped grant (#5377: the judge reads
    // the image and audits while holding scoped keep+rollback).
    const bool worldline_op = (bit == kCapKeep || bit == kCapRollback || bit == kCapTrial);
    if (worldline_op && caps.worldline != 0 && worldline != caps.worldline) {
        if (why)
            *why = "capability is scoped to another worldline";
        return false;
    }
    return true;
}

CapabilityTable::CapabilityTable() = default;

std::uint64_t CapabilityTable::mint_token_() {
    // Issue #5505: the tokens used to be (counter * K) ^ secret. Two tokens
    // then reveal the key and every other token in the table, so a session
    // could derive another session's root token. Draw from getrandom
    // instead and reject a collision.
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::uint64_t candidate = 0;
        if (::getrandom(&candidate, sizeof(candidate), 0) !=
            static_cast<ssize_t>(sizeof(candidate)))
            candidate = 0;
        if (candidate == 0) {
            std::random_device rd;
            candidate = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        }
        // Issue #5505: a token travels through Aura as a fixnum, so it must
        // fit make_int's 62-bit domain. 62 random bits are still far beyond
        // guessing and, unlike the old counter*K^secret shape, reveal
        // nothing about any other token.
        candidate &= (1ull << 62) - 1;
        if (candidate == 0)
            continue;
        const bool used =
            std::any_of(records_.begin(), records_.end(),
                        [candidate](const GrantRecord& r) { return r.token == candidate; });
        if (!used)
            return candidate;
    }
    // Practically unreachable: stay non-zero and unique enough.
    std::random_device rd;
    std::uint64_t fallback = (((static_cast<std::uint64_t>(rd()) << 32) ^ rd()) ^
                              ((records_.size() + 1) * 0x9e3779b97f4a7c15ull)) &
                             ((1ull << 62) - 1);
    return fallback == 0 ? 1 : fallback;
}

std::uint64_t CapabilityTable::issue_root(const Capability& caps) {
    GrantRecord rec{};
    rec.token = mint_token_();
    rec.caps = caps;
    records_.push_back(rec);
    return rec.token;
}

std::size_t CapabilityTable::depth_of(std::uint64_t token) const {
    std::size_t depth = 0;
    std::uint64_t current = token;
    // The tree is short by construction; the walk is bounded anyway.
    for (std::size_t guard = 0; guard < 64; ++guard) {
        const GrantRecord* rec = nullptr;
        for (const auto& candidate : records_) {
            if (candidate.token == current) {
                rec = &candidate;
                break;
            }
        }
        if (rec == nullptr || rec->parent == 0)
            return depth;
        ++depth;
        current = rec->parent;
    }
    return depth;
}

std::size_t CapabilityTable::fanout_of(std::uint64_t token) const {
    std::size_t count = 0;
    for (const auto& rec : records_) {
        // A revoked grant holds no authority, so it is not a child.
        if (rec.parent == token && !rec.revoked)
            ++count;
    }
    return count;
}

std::size_t CapabilityTable::session_grants(std::uint64_t root) const {
    // The subtree under the session's root token: the grants the
    // session may hand out.
    std::size_t count = 0;
    for (const auto& rec : records_) {
        if (rec.token == root || rec.revoked)
            continue;
        std::uint64_t current = rec.parent;
        for (std::size_t guard = 0; guard < 64 && current != 0; ++guard) {
            if (current == root) {
                ++count;
                break;
            }
            const GrantRecord* parent = nullptr;
            for (const auto& candidate : records_) {
                if (candidate.token == current) {
                    parent = &candidate;
                    break;
                }
            }
            if (parent == nullptr)
                break;
            current = parent->parent;
        }
    }
    return count;
}

std::optional<std::uint64_t> CapabilityTable::derive(std::uint64_t parent, const Capability& child,
                                                     std::string* why,
                                                     std::uint64_t owner_session) {
    Capability parent_caps{};
    if (!effective(parent, &parent_caps)) {
        if (why)
            *why = "parent grant is missing or revoked";
        return std::nullopt;
    }
    if ((parent_caps.bits & kCapGrant) == 0) {
        if (why)
            *why = "parent lacks the grant bit";
        return std::nullopt;
    }
    if (!attenuates(parent_caps, child, why))
        return std::nullopt;
    // #5396: the grant tree's limits. A grant may only attenuate, so a
    // deep or wide tree is either a mistake or an attempt to hand out
    // authority in bulk.
    if (depth_of(parent) + 1 > kMaxGrantDepth) {
        if (why)
            *why = "grant depth limit reached";
        return std::nullopt;
    }
    if (fanout_of(parent) >= kMaxGrantFanout) {
        if (why)
            *why = "grant fanout limit reached";
        return std::nullopt;
    }
    // The session that holds the parent may not hold an unbounded
    // number of grants.
    std::uint64_t root = parent;
    for (std::size_t guard = 0; guard < 64; ++guard) {
        const GrantRecord* rec = nullptr;
        for (const auto& candidate : records_) {
            if (candidate.token == root) {
                rec = &candidate;
                break;
            }
        }
        if (rec == nullptr || rec->parent == 0)
            break;
        root = rec->parent;
    }
    if (session_grants(root) >= kMaxGrantsPerSession) {
        if (why)
            *why = "grant limit reached for this session";
        return std::nullopt;
    }
    GrantRecord rec{};
    rec.token = mint_token_();
    rec.parent = parent;
    rec.caps = child;
    rec.owner_session = owner_session;
    records_.push_back(rec);
    return rec.token;
}

std::size_t CapabilityTable::revoke(std::uint64_t token) {
    bool changed = true;
    std::size_t count = 0;
    while (changed) {
        changed = false;
        for (auto& rec : records_) {
            if (rec.revoked)
                continue;
            if (rec.token == token ||
                (rec.parent != 0 &&
                 std::any_of(records_.begin(), records_.end(), [&](const GrantRecord& p) {
                     return p.token == rec.parent && p.revoked;
                 }))) {
                rec.revoked = true;
                ++count;
                changed = true;
            }
        }
    }
    return count;
}

bool CapabilityTable::effective(std::uint64_t token, Capability* out) const {
    const GrantRecord* rec = nullptr;
    for (const auto& candidate : records_) {
        if (candidate.token == token) {
            rec = &candidate;
            break;
        }
    }
    if (!rec || rec->revoked)
        return false;
    std::uint64_t parent = rec->parent;
    std::size_t hops = 0;
    while (parent != 0 && hops++ < records_.size()) {
        const GrantRecord* p = nullptr;
        for (const auto& candidate : records_) {
            if (candidate.token == parent) {
                p = &candidate;
                break;
            }
        }
        if (!p || p->revoked)
            return false;
        parent = p->parent;
    }
    if (out)
        *out = rec->caps;
    return true;
}

bool CapabilityTable::owned_by(std::uint64_t token, std::uint64_t session) const {
    for (const auto& candidate : records_) {
        if (candidate.token != token)
            continue;
        // Issue #5505: 0 = a coordinator root any session may activate (the
        // host hands those out of band); a derived grant belongs to the
        // session that asked for it.
        return candidate.owner_session == 0 || candidate.owner_session == session;
    }
    return false;
}

bool CapabilityTable::revoked(std::uint64_t token) const {
    for (const auto& rec : records_) {
        if (rec.token == token)
            return rec.revoked;
    }
    return true;
}

std::uint64_t WorldlineLedger::open_trial(std::uint64_t owner, std::uint64_t workspace,
                                          std::uint64_t base_generation, std::uint64_t task,
                                          std::uint32_t patch_begin) {
    TrialRecord rec{};
    rec.id = next_id_++;
    rec.owner = owner;
    rec.workspace = workspace;
    rec.base_generation = base_generation;
    rec.task = task;
    rec.patch_begin = patch_begin;
    rec.pending = true;
    rec.loan_outstanding = true;
    records_.push_back(rec);
    pending_.emplace(rec.owner, rec.id);
    return rec.id;
}

TrialRecord* WorldlineLedger::at(std::uint64_t id) {
    const auto it = std::lower_bound(
        records_.begin(), records_.end(), id,
        [](const TrialRecord& record, std::uint64_t value) { return record.id < value; });
    return it != records_.end() && it->id == id ? &*it : nullptr;
}

const TrialRecord* WorldlineLedger::at(std::uint64_t id) const {
    const auto it = std::lower_bound(
        records_.begin(), records_.end(), id,
        [](const TrialRecord& record, std::uint64_t value) { return record.id < value; });
    return it != records_.end() && it->id == id ? &*it : nullptr;
}

void WorldlineLedger::release_loan(std::uint64_t id) {
    if (TrialRecord* rec = at(id))
        rec->loan_outstanding = false;
}

std::size_t WorldlineLedger::pending_count() const {
    return pending_.size();
}

const TrialRecord* WorldlineLedger::find(std::uint64_t id) const {
    return at(id);
}

std::size_t WorldlineLedger::forget_settled_before(std::uint64_t horizon) {
    // A pending trial and a worldline under observation still change
    // state, so they stay whatever their id; the gaps do not cost a
    // lookup more than a bisection.
    const std::size_t before = records_.size();
    std::erase_if(records_, [horizon](const TrialRecord& record) {
        return record.id < horizon && !record.pending && !record.canary;
    });
    return before - records_.size();
}

bool WorldlineLedger::keep(std::uint64_t id, std::uint64_t owner, const Capability& caps,
                           std::string* why, bool applied_live, std::uint32_t patch_end) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    if ((caps.bits & kCapKeep) == 0)
        return fail("missing capability: keep");
    TrialRecord* rec = at(id);
    if (!rec)
        return fail("unknown worldline");
    if (rec->owner != owner && caps.worldline != id)
        return fail("worldline is owned by another session");
    if (rec->task != 0 || caps.task != 0) {
        if (rec->task != caps.task)
            return fail("capability is scoped to another task");
    }
    if (!rec->pending)
        return fail("worldline is not pending");
    if (!rec->loan_outstanding)
        return fail("trial loan was already released");
    rec->pending = false;
    rec->kept = true;
    rec->applied_live = applied_live;
    rec->patch_end = patch_end;
    pending_.erase({rec->owner, id});
    return true;
}

bool WorldlineLedger::rollback(std::uint64_t id, std::uint64_t owner, const Capability& caps,
                               std::string* why, std::uint32_t patch_end) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    if ((caps.bits & kCapRollback) == 0)
        return fail("missing capability: rollback");
    TrialRecord* rec = at(id);
    if (!rec)
        return fail("unknown worldline");
    if (rec->owner != owner && caps.worldline != id)
        return fail("worldline is owned by another session");
    if (rec->task != 0 || caps.task != 0) {
        if (rec->task != caps.task)
            return fail("capability is scoped to another task");
    }
    if (!rec->pending)
        return fail("worldline is not pending");
    rec->pending = false;
    rec->rolled_back = true;
    rec->patch_end = patch_end;
    pending_.erase({rec->owner, id});
    return true;
}

bool WorldlineLedger::rollback_kept(std::uint64_t id, std::uint64_t owner, const Capability& caps,
                                    std::string* why) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    if ((caps.bits & kCapRollback) == 0)
        return fail("missing capability: rollback");
    TrialRecord* rec = at(id);
    if (!rec)
        return fail("unknown worldline");
    if (rec->owner != owner && caps.worldline != id)
        return fail("worldline is owned by another session");
    if (rec->task != 0 || caps.task != 0) {
        if (rec->task != caps.task)
            return fail("capability is scoped to another task");
    }
    if (!rec->kept || rec->rolled_back)
        return fail("worldline is not kept");
    rec->kept = false;
    rec->canary = false;
    rec->rolled_back = true;
    return true;
}

// #5381: canary lifecycle. A kept trial enters observation; seal ends it
// as Kept, revert rolls it back. Ownership is not re-checked here: the
// coordinator gates the frame (scoped keep/rollback capability) first.
bool WorldlineLedger::enter_canary(std::uint64_t id, std::string* why) {
    TrialRecord* rec = at(id);
    if (!rec) {
        if (why)
            *why = "unknown worldline";
        return false;
    }
    if (!rec->kept || rec->rolled_back) {
        if (why)
            *why = "worldline is not kept";
        return false;
    }
    if (rec->canary) {
        if (why)
            *why = "worldline is already under canary observation";
        return false;
    }
    rec->canary = true;
    return true;
}

bool WorldlineLedger::seal_canary(std::uint64_t id, std::string* why) {
    TrialRecord* rec = at(id);
    if (!rec) {
        if (why)
            *why = "unknown worldline";
        return false;
    }
    if (!rec->canary) {
        if (why)
            *why = "worldline is not under canary observation";
        return false;
    }
    rec->canary = false;
    return true;
}

bool WorldlineLedger::revert_canary(std::uint64_t id, std::string* why) {
    TrialRecord* rec = at(id);
    if (!rec) {
        if (why)
            *why = "unknown worldline";
        return false;
    }
    if (!rec->canary) {
        if (why)
            *why = "worldline is not under canary observation";
        return false;
    }
    rec->canary = false;
    rec->kept = false;
    rec->rolled_back = true;
    return true;
}

std::vector<std::uint64_t> WorldlineLedger::end_session(std::uint64_t owner,
                                                        std::uint32_t patch_end) {
    std::vector<std::uint64_t> rolled;
    // Issue #5502: the record is looked up by id, never by id-1. The audit
    // window forgets settled records in the middle of the vector, so the
    // pending trial's record is not at its id offset any more; indexing
    // records_[id-1] wrote past the end once the window had slid.
    for (auto it = pending_.lower_bound({owner, 0}); it != pending_.end() && it->first == owner;) {
        TrialRecord* rec = at(it->second);
        if (rec == nullptr) {
            // A record forgotten under an unsettled pending entry cannot
            // happen while pending records are kept by forget_settled_before,
            // but never write through a missing one.
            it = pending_.erase(it);
            continue;
        }
        rec->pending = false;
        rec->rolled_back = true;
        rec->patch_end = patch_end;
        rolled.push_back(rec->id);
        it = pending_.erase(it);
    }
    return rolled;
}

void AuditLog::push(AuditEvent&& event) {
    events_.push_back(std::move(event));
    if (sink_)
        sink_(events_.back());
    if (events_.size() > kMaxEvents) {
        // Drop a quarter at a time: a single erase per event at the cap
        // would move the whole window for every audited publish.
        const std::size_t keep = kMaxEvents - kMaxEvents / 4;
        const std::size_t drop = events_.size() - keep;
        dropped_ += static_cast<std::uint64_t>(drop);
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

void AuditLog::add(AuditKind kind, std::uint64_t token, std::uint64_t worldline,
                   std::uint64_t detail) {
    AuditEvent event{};
    event.kind = kind;
    event.token = token;
    event.worldline = worldline;
    event.detail = detail;
    event.timestamp_ms = audit_now_ms();
    push(std::move(event));
}

void AuditLog::add_constraint(AuditKind kind, std::uint64_t token, std::uint64_t worldline,
                              std::uint64_t result, std::uint64_t version, std::string reason) {
    AuditEvent event{};
    event.kind = kind;
    event.token = token;
    event.worldline = worldline;
    event.result = result;
    event.constraint_version = version;
    event.timestamp_ms = audit_now_ms();
    event.reason = std::move(reason);
    push(std::move(event));
}

std::vector<AuditEvent> AuditLog::for_worldline(std::uint64_t worldline) const {
    std::vector<AuditEvent> out;
    for (const auto& event : events_) {
        if (event.worldline == worldline)
            out.push_back(event);
    }
    return out;
}

std::size_t AuditLog::count(AuditKind kind) const {
    return static_cast<std::size_t>(std::count_if(
        events_.begin(), events_.end(), [&](const AuditEvent& e) { return e.kind == kind; }));
}

void WorkspacePolicy::allow(std::uint32_t role, const Capability& max) {
    entries_.push_back({role, max});
}

Capability WorkspacePolicy::resolve(std::uint32_t role, const Capability& requested,
                                    std::uint64_t workspace, bool* denied) const {
    if (denied)
        *denied = false;
    for (const auto& entry : entries_) {
        if (entry.role != role)
            continue;
        Capability granted = intersect(entry.max, requested);
        granted.workspace = workspace;
        if (denied && (granted.bits != requested.bits || granted.channels != requested.channels ||
                       granted.max_loans != requested.max_loans ||
                       granted.expires_gen != requested.expires_gen))
            *denied = true;
        return granted;
    }
    if (denied)
        *denied = true;
    Capability none{};
    none.workspace = workspace;
    return none;
}

Capability clamp_preset(const Capability& caps, SandboxPreset preset) {
    Capability out = caps;
    switch (preset) {
        case SandboxPreset::Dev:
            break;
        case SandboxPreset::Ci:
            out.bits &= ~kCapConstraintWrite;
            break;
        case SandboxPreset::Prod:
            out.bits &= ~(kCapGrant | kCapConstraintWrite);
            break;
    }
    return out;
}

} // namespace aura::bus
