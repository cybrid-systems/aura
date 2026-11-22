// ──────────────────────────────────────────────────────────────
//  round.hh — the admission round state machine (#5393)
//
//  One round at a time: idle → signal → trial → judge → canary →
//  sealed. The host decides; the peers propose and adjudicate. Every
//  transition is audited, and every failure fails closed: the round
//  rolls back and nothing is applied.
//
//  The budget is the host's: a round that runs out of wall clock is
//  closed automatically, a cooldown separates two rounds, and the
//  number of rounds is capped. The checkpoint remembers the sealed
//  candidates, so a restarted host does not apply one twice.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_ROUND_HH
#define AURA_BUS_ROUND_HH

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aura::bus {

// The phases of a round.
enum class RoundPhase : std::uint8_t {
    Idle = 0,   // no round is running
    Signal = 1, // a round was opened (the degradation signal)
    Trial = 2,  // the round's candidate was opened
    Judge = 3,  // the candidate is being adjudicated
    Canary = 4, // the candidate is under observation
    Sealed = 5, // the round is done and its worldline is kept
};

const char* round_phase_name(RoundPhase phase);

// The host's round budget.
struct RoundBudget {
    std::uint64_t wall_ms = 30000; // the per-round wall budget (0 = none)
    std::uint64_t cooldown_ms = 0; // the gap between rounds (0 = none)
    std::uint64_t max_rounds = 0;  // the rounds this host may run (0 = none)
    std::uint64_t version = 1;
};

// The round state machine. The coordinator owns exactly one.
class RoundMachine {
public:
    void configure(const RoundBudget& budget) { budget_ = budget; }
    [[nodiscard]] const RoundBudget& budget() const { return budget_; }
    [[nodiscard]] RoundPhase phase() const { return phase_; }
    [[nodiscard]] std::uint64_t id() const { return id_; }
    [[nodiscard]] std::uint64_t worldline() const { return worldline_; }
    [[nodiscard]] std::uint64_t owner_session() const { return owner_session_; }
    [[nodiscard]] std::uint64_t judge_session() const { return judge_session_; }
    [[nodiscard]] std::uint64_t opened_count() const { return opened_count_; }
    [[nodiscard]] std::uint64_t sealed_count() const { return sealed_count_; }
    [[nodiscard]] const std::string& reason() const { return reason_; }
    // True while a round is in flight (signal, trial, judge, canary).
    [[nodiscard]] bool active() const {
        return phase_ == RoundPhase::Signal || phase_ == RoundPhase::Trial ||
               phase_ == RoundPhase::Judge || phase_ == RoundPhase::Canary;
    }

    // The transitions. Each returns false with a reason when the
    // transition is refused; the caller turns a refusal into an error
    // frame and an audit event.
    bool open(std::uint64_t now_ms, bool canary_active, std::string* why);
    bool note_trial(std::uint64_t worldline, std::uint64_t owner_session, std::string* why);
    bool note_judge(std::uint64_t session, std::string* why);
    bool note_keep(std::string* why);
    bool note_seal(bool canary, std::uint64_t now_ms, std::string* why);
    bool note_canary_seal(std::uint64_t now_ms, std::string* why);
    // Fail closed: the round is abandoned and the machine is idle.
    bool roll(std::string_view reason, std::uint64_t now_ms, std::string* why);
    // The budget: a round that ran out of wall clock, a cooldown
    // between two rounds, and the round cap.
    [[nodiscard]] bool expired(std::uint64_t now_ms) const;
    [[nodiscard]] bool cooling_down(std::uint64_t now_ms) const;
    // Continue the round counter after a restart.
    void restore(std::uint64_t last_id) { id_ = last_id; }

private:
    RoundBudget budget_{};
    RoundPhase phase_ = RoundPhase::Idle;
    std::uint64_t id_ = 0;
    std::uint64_t worldline_ = 0;
    std::uint64_t owner_session_ = 0;
    std::uint64_t judge_session_ = 0;
    std::uint64_t opened_count_ = 0;
    std::uint64_t sealed_count_ = 0;
    std::uint64_t started_ms_ = 0;
    std::uint64_t ended_ms_ = 0;
    std::string reason_;
};

// The round checkpoint: the sealed candidates and the last round the
// host saw. It lives next to the socket, so a restarted host resumes
// from it instead of applying a sealed candidate twice.
class RoundCheckpoint {
public:
    // Load the checkpoint. A missing file is not an error.
    bool open(const std::string& path, std::string* why);
    // Record the current round (appended, flushed).
    bool record_round(std::uint64_t id, RoundPhase phase, std::uint64_t worldline,
                      std::uint64_t timestamp_ms, std::string* why);
    // Record a sealed candidate.
    bool record_seal(std::uint64_t fingerprint, std::uint64_t worldline, std::uint64_t timestamp_ms,
                     std::string* why);
    // True when the candidate was sealed before.
    [[nodiscard]] bool sealed(std::uint64_t fingerprint) const;
    [[nodiscard]] std::size_t sealed_count() const { return sealed_.size(); }
    [[nodiscard]] std::uint64_t round_id() const { return round_id_; }
    [[nodiscard]] RoundPhase round_phase() const { return round_phase_; }
    [[nodiscard]] std::uint64_t round_worldline() const { return round_worldline_; }
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
    std::vector<std::uint64_t> sealed_;
    std::uint64_t round_id_ = 0;
    RoundPhase round_phase_ = RoundPhase::Idle;
    std::uint64_t round_worldline_ = 0;
};

} // namespace aura::bus

#endif
