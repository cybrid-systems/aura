// ──────────────────────────────────────────────────────────────
//  round.cpp — the admission round state machine (#5393)
// ──────────────────────────────────────────────────────────────
#include "bus/round.hh"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aura::bus {
namespace {

    const auto fail = [](std::string* why, const char* text) {
        if (why != nullptr)
            *why = text;
        return false;
    };

} // namespace

const char* round_phase_name(RoundPhase phase) {
    switch (phase) {
        case RoundPhase::Idle:
            return "idle";
        case RoundPhase::Signal:
            return "signal";
        case RoundPhase::Trial:
            return "trial";
        case RoundPhase::Judge:
            return "judge";
        case RoundPhase::Canary:
            return "canary";
        case RoundPhase::Sealed:
            return "sealed";
    }
    return "unknown";
}

// ── The machine ───────────────────────────────────────────────

bool RoundMachine::open(std::uint64_t now_ms, bool canary_active, std::string* why) {
    if (active())
        return fail(why, "round-active");
    // #5381: an unsealed observation owns the workspace — a new round
    // must not stack on it.
    if (canary_active)
        return fail(why, "canary-active");
    if (cooling_down(now_ms))
        return fail(why, "round-cooldown");
    if (budget_.max_rounds != 0 && opened_count_ >= budget_.max_rounds)
        return fail(why, "round-budget-exhausted");
    ++id_;
    ++opened_count_;
    phase_ = RoundPhase::Signal;
    worldline_ = 0;
    owner_session_ = 0;
    judge_session_ = 0;
    started_ms_ = now_ms;
    ended_ms_ = 0;
    reason_.clear();
    return true;
}

bool RoundMachine::note_trial(std::uint64_t worldline, std::uint64_t owner_session,
                              std::string* why) {
    if (!active())
        return fail(why, "no-active-round");
    if (phase_ == RoundPhase::Trial && worldline_ == worldline)
        return true; // the same candidate again
    if (phase_ != RoundPhase::Signal)
        return fail(why, "round-busy");
    phase_ = RoundPhase::Trial;
    worldline_ = worldline;
    owner_session_ = owner_session;
    return true;
}

bool RoundMachine::note_judge(std::uint64_t session, std::string* why) {
    if (!active())
        return fail(why, "no-active-round");
    if (worldline_ == 0)
        return fail(why, "round-no-candidate");
    judge_session_ = session;
    return true;
}

bool RoundMachine::note_keep(std::string* why) {
    if (!active())
        return fail(why, "no-active-round");
    if (phase_ != RoundPhase::Trial)
        return fail(why, "round-busy");
    phase_ = RoundPhase::Judge;
    return true;
}

bool RoundMachine::note_seal(bool canary, std::uint64_t now_ms, std::string* why) {
    if (!active())
        return fail(why, "no-active-round");
    if (phase_ != RoundPhase::Judge)
        return fail(why, "round-not-judged");
    if (canary) {
        phase_ = RoundPhase::Canary;
        return true;
    }
    phase_ = RoundPhase::Sealed;
    ++sealed_count_;
    ended_ms_ = now_ms;
    return true;
}

bool RoundMachine::note_canary_seal(std::uint64_t now_ms, std::string* why) {
    if (phase_ != RoundPhase::Canary)
        return fail(why, "round-not-canary");
    phase_ = RoundPhase::Sealed;
    ++sealed_count_;
    ended_ms_ = now_ms;
    return true;
}

bool RoundMachine::roll(std::string_view reason, std::uint64_t now_ms, std::string* why) {
    if (!active())
        return fail(why, "no-active-round");
    phase_ = RoundPhase::Idle;
    worldline_ = 0;
    owner_session_ = 0;
    judge_session_ = 0;
    ended_ms_ = now_ms;
    reason_ = std::string(reason);
    return true;
}

bool RoundMachine::expired(std::uint64_t now_ms) const {
    if (!active() || budget_.wall_ms == 0)
        return false;
    return now_ms > started_ms_ + budget_.wall_ms;
}

bool RoundMachine::cooling_down(std::uint64_t now_ms) const {
    if (budget_.cooldown_ms == 0 || ended_ms_ == 0)
        return false;
    return now_ms < ended_ms_ + budget_.cooldown_ms;
}

// ── The checkpoint ─────────────────────────────────────────────
//
// The file is a short text log: a version line, then one line per
// round transition and per sealed candidate. It is append-only and
// flushed on every write: a host that dies mid-round leaves a
// readable checkpoint behind.

bool RoundCheckpoint::open(const std::string& path, std::string* why) {
    path_ = path;
    sealed_.clear();
    round_id_ = 0;
    round_phase_ = RoundPhase::Idle;
    round_worldline_ = 0;
    std::ifstream in(path_);
    if (!in.is_open())
        return true; // no checkpoint yet
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream stream(line);
        std::string tag;
        if (!(stream >> tag))
            continue;
        if (tag == "seal") {
            std::uint64_t fingerprint = 0;
            std::uint64_t worldline = 0;
            std::uint64_t timestamp = 0;
            if (!(stream >> fingerprint >> worldline >> timestamp))
                continue;
            if (!sealed(fingerprint))
                sealed_.push_back(fingerprint);
        } else if (tag == "round") {
            std::uint64_t id = 0;
            std::uint64_t phase = 0;
            std::uint64_t worldline = 0;
            std::uint64_t timestamp = 0;
            if (!(stream >> id >> phase >> worldline >> timestamp))
                continue;
            round_id_ = id;
            round_phase_ = static_cast<RoundPhase>(phase);
            round_worldline_ = worldline;
        }
    }
    (void)why;
    return true;
}

bool RoundCheckpoint::record_round(std::uint64_t id, RoundPhase phase, std::uint64_t worldline,
                                   std::uint64_t timestamp_ms, std::string* why) {
    round_id_ = id;
    round_phase_ = phase;
    round_worldline_ = worldline;
    if (path_.empty())
        return true;
    std::ofstream out(path_, std::ios::app);
    if (!out.is_open()) {
        if (why != nullptr)
            *why = "the round checkpoint cannot be written";
        return false;
    }
    out << "round " << id << " " << static_cast<std::uint64_t>(phase) << " " << worldline << " "
        << timestamp_ms << "\n";
    out.flush();
    return true;
}

bool RoundCheckpoint::record_seal(std::uint64_t fingerprint, std::uint64_t worldline,
                                  std::uint64_t timestamp_ms, std::string* why) {
    if (!sealed(fingerprint))
        sealed_.push_back(fingerprint);
    if (path_.empty())
        return true;
    std::ofstream out(path_, std::ios::app);
    if (!out.is_open()) {
        if (why != nullptr)
            *why = "the round checkpoint cannot be written";
        return false;
    }
    out << "seal " << fingerprint << " " << worldline << " " << timestamp_ms << "\n";
    out.flush();
    return true;
}

bool RoundCheckpoint::sealed(std::uint64_t fingerprint) const {
    for (const std::uint64_t item : sealed_) {
        if (item == fingerprint)
            return true;
    }
    return false;
}

} // namespace aura::bus
