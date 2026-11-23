// ──────────────────────────────────────────────────────────────
//  constraint.cpp — constraint gate (#5126)
// ──────────────────────────────────────────────────────────────
#include "bus/constraint.hh"

#include "bus/image.hh"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace aura::bus {
namespace {

    // Comma/space separated unsigned integers. An empty string is a valid
    // empty list; a token that is not a number is not.
    std::optional<std::vector<std::uint64_t>> parse_u64_list(std::string_view source) {
        std::vector<std::uint64_t> out;
        std::size_t i = 0;
        while (i < source.size()) {
            while (i < source.size() &&
                   (source[i] == ',' || source[i] == ' ' || source[i] == '\t' ||
                    source[i] == '\n' || source[i] == '\r'))
                ++i;
            if (i >= source.size())
                break;
            const std::size_t begin = i;
            while (i < source.size() && source[i] >= '0' && source[i] <= '9')
                ++i;
            if (i == begin)
                return std::nullopt;
            std::uint64_t value = 0;
            const auto [ptr, ec] = std::from_chars(source.data() + begin, source.data() + i, value);
            if (ec != std::errc() || ptr != source.data() + i)
                return std::nullopt;
            out.push_back(value);
            while (i < source.size() && source[i] != ',' && source[i] != ' ' && source[i] != '\t' &&
                   source[i] != '\n' && source[i] != '\r')
                return std::nullopt; // a suffix like "12x" is not a number
        }
        return out;
    }

    std::optional<std::uint64_t> parse_u64(std::string_view source) {
        std::size_t i = 0;
        while (i < source.size() && (source[i] == ' ' || source[i] == '\t'))
            ++i;
        const std::size_t begin = i;
        while (i < source.size() && source[i] >= '0' && source[i] <= '9')
            ++i;
        if (i == begin)
            return std::nullopt;
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(source.data() + begin, source.data() + i, value);
        if (ec != std::errc() || ptr != source.data() + i)
            return std::nullopt;
        while (i < source.size() &&
               (source[i] == ' ' || source[i] == '\t' || source[i] == '\n' || source[i] == '\r'))
            ++i;
        if (i != source.size())
            return std::nullopt;
        return value;
    }

} // namespace

ConstraintPredicate target_allowlist_predicate(std::string_view source) {
    const auto allowed = parse_u64_list(source);
    if (!allowed) {
        return [](const ConstraintContext&, std::string* why) {
            if (why)
                *why = "target-allowlist source is not a node-id list";
            return false;
        };
    }
    return [allowed = *allowed](const ConstraintContext& ctx, std::string* why) {
        for (const PatchRecord& patch : ctx.patches) {
            if (patch.field_offset != kPatchSourceField)
                continue;
            if (std::find(allowed.begin(), allowed.end(), patch.node) == allowed.end()) {
                if (why)
                    *why = "source patch target " + std::to_string(patch.node) +
                           " is not in target-allowlist";
                return false;
            }
        }
        return true;
    };
}

ConstraintPredicate metric_floor_predicate(std::string_view source) {
    const auto floor = parse_u64(source);
    if (!floor) {
        return [](const ConstraintContext&, std::string* why) {
            if (why)
                *why = "metric-floor-bound has no numeric floor";
            return false;
        };
    }
    return [floor = *floor](const ConstraintContext& ctx, std::string* why) {
        if (ctx.new_value < floor) {
            if (why)
                *why = "metric " + std::to_string(ctx.new_value) + " is below the floor " +
                       std::to_string(floor);
            return false;
        }
        return true;
    };
}


GateResult ConstraintStore::check(const ConstraintContext& ctx, AuditLog* audit,
                                  std::uint64_t token) {
    GateResult result{};
    result.pass = true;
    result.version = current_.version;
    for (const auto& rule : current_.rules) {
        std::string why;
        if (rule.check(ctx, &why))
            continue;
        result.pass = false;
        result.code = 1;
        result.reason = rule.name.empty() ? why : rule.name + ": " + why;
        audit->add_constraint(AuditKind::ConstraintResult, token, ctx.worldline, 0,
                              current_.version, result.reason);
        return result;
    }
    audit->add_constraint(AuditKind::ConstraintResult, token, ctx.worldline, 1, current_.version,
                          {});
    return result;
}

bool ConstraintStore::install(ConstraintSet set, const Capability& caps, AuditLog* audit,
                              std::uint64_t token, std::string* why) {
    if ((caps.bits & kCapConstraintWrite) == 0) {
        if (why)
            *why = "missing capability: constraint-write";
        return false;
    }
    // A rule-only install keeps the current allow policy; a set carrying a
    // policy replaces it atomically with the rules.
    if (set.policy.empty())
        set.policy = current_.policy;
    // #5396: the install may not drop a rule another session installed.
    std::string relax;
    for (const auto& [name, installer] : current_.installers) {
        if (installer == token)
            continue;
        const bool kept =
            std::any_of(set.rules.begin(), set.rules.end(),
                        [&](const ConstraintRule& rule) { return rule.name == name; });
        if (!kept)
            relax = name;
    }
    if (!relax.empty()) {
        if (why)
            *why = "the install would relax the protection set (" + relax + ")";
        return false;
    }
    set.version = current_.version + 1;
    for (auto& rule : set.rules)
        set.installers.emplace(rule.name, token);
    history_.push_back(current_);
    current_ = std::move(set);
    audit->add(AuditKind::ConstraintChange, token, 0, current_.version);
    return true;
}

bool ConstraintStore::rollback(const Capability& caps, AuditLog* audit, std::uint64_t token,
                               std::string* why) {
    if ((caps.bits & kCapConstraintWrite) == 0) {
        if (why)
            *why = "missing capability: constraint-write";
        return false;
    }
    if (history_.empty()) {
        if (why)
            *why = "no previous constraint set";
        return false;
    }
    // #5396: the rollback may not drop a rule another session
    // installed — a rollback must not relax the gate behind the owner's
    // back.
    const ConstraintSet& target = history_.back();
    for (const auto& [name, installer] : current_.installers) {
        if (installer == token)
            continue;
        const bool kept =
            std::any_of(target.rules.begin(), target.rules.end(),
                        [&](const ConstraintRule& rule) { return rule.name == name; });
        if (!kept) {
            if (why)
                *why = "the rollback would relax the protection set (" + name + ")";
            return false;
        }
    }
    current_ = target;
    history_.pop_back();
    audit->add(AuditKind::ConstraintRollback, token, 0, current_.version);
    return true;
}

} // namespace aura::bus
