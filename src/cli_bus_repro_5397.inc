// cli_bus_repro_5397.inc — Issue #5397: `aura bus-repro FILE`.
//
// The reproduction package of a KEEP is one JSON line per worldline (the
// `bus:repro` verb emits it). This command re-runs the admission
// decision from the package alone — no coordinator, no live bus — and
// reports whether the recomputed verdict matches the recorded one. A
// nightly archives the packages; this is the offline check that they
// still decide the same thing.
//
// usage: aura bus-repro FILE [--json]
//
// Exit code 0 when every package reproduces, 1 when one does not, 2 on a
// malformed file.

#include "bus/gate.hh"

namespace bus_repro_5397 {

// One package: the evidence five-tuple, the held-out windows, the
// policy snapshot and the verdict the coordinator recorded.
struct Package {
    bool have_five = false;
    std::uint64_t worldline = 0;
    std::uint64_t baseline_gen = 0;
    std::uint64_t candidate_hash = 0;
    std::uint64_t workload_sym = 0;
    std::uint64_t seed = 0;
    std::uint64_t verifier_version = 0;
    std::uint64_t round_id = 0;
    std::uint64_t fold_count = 0;
    std::uint64_t threshold_ppm = 0;
    std::uint64_t control = 0;
    std::vector<std::uint64_t> windows; // the raw (biased) window slots
    aura::bus::GatePolicy policy{};
    aura::bus::GateVerdictRecord verdict{};
    bool have_verdict = false;
};

// The JSON is ours: a flat object of numbers and one array of numbers.
// The evo log's parser reads a whole line, so the package is read the
// same way.
static bool field_u64(const evo_cli_4910::J& obj, const char* name, std::uint64_t* out) {
    const evo_cli_4910::J* v = obj.get(name);
    if (v == nullptr)
        return false;
    // The numbers are exact integers; parse the raw text so a 62-bit
    // hash survives (a double would not).
    const std::string& raw = v->raw;
    if (raw.empty())
        return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw.c_str(), &end, 10);
    if (end == raw.c_str())
        return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
}

static bool parse_package(const evo_cli_4910::J& obj, Package* out) {
    if (!field_u64(obj, "worldline", &out->worldline))
        return false;
    out->have_five = field_u64(obj, "baseline_gen", &out->baseline_gen) &&
                     field_u64(obj, "candidate_hash", &out->candidate_hash) &&
                     field_u64(obj, "workload_sym", &out->workload_sym) &&
                     field_u64(obj, "seed", &out->seed) &&
                     field_u64(obj, "verifier_version", &out->verifier_version);
    if (!field_u64(obj, "round_id", &out->round_id))
        return false;
    if (!field_u64(obj, "fold_count", &out->fold_count))
        return false;
    if (!field_u64(obj, "threshold_ppm", &out->threshold_ppm))
        return false;
    if (!field_u64(obj, "control", &out->control))
        return false;
    const evo_cli_4910::J* wins = obj.get("windows");
    if (wins == nullptr || wins->t != evo_cli_4910::J::T::Arr)
        return false;
    for (const evo_cli_4910::J& w : wins->arr) {
        char* end = nullptr;
        const long long value = std::strtoll(w.raw.c_str(), &end, 10);
        if (end == w.raw.c_str())
            return false;
        // The package stores the signed delta; the wire body carries it
        // in a u64 slot as two's complement.
        out->windows.push_back(static_cast<std::uint64_t>(value));
    }
    const evo_cli_4910::J* pol = obj.get("policy");
    if (pol == nullptr)
        return false;
    if (!field_u64(*pol, "threshold_ppm", &out->policy.threshold_ppm))
        return false;
    std::uint64_t min_windows = 0;
    std::uint64_t budget_window = 0;
    std::uint64_t tighten = 0;
    std::uint64_t version = 0;
    if (!field_u64(*pol, "min_windows", &min_windows))
        return false;
    if (!field_u64(*pol, "budget_window", &budget_window))
        return false;
    if (!field_u64(*pol, "tighten_factor", &tighten))
        return false;
    if (!field_u64(*pol, "version", &version))
        return false;
    out->policy.min_windows = static_cast<std::uint32_t>(min_windows);
    out->policy.budget_window = static_cast<std::uint32_t>(budget_window);
    out->policy.tighten_factor = static_cast<std::uint32_t>(tighten);
    out->policy.version = version;
    const evo_cli_4910::J* ver = obj.get("verdict");
    if (ver == nullptr)
        return false;
    // #5397: the package says whether the coordinator recorded a
    // verdict at all. A worldline kept without the statistical gate
    // has none: the offline re-run verifies the recorded inputs, it
    // does not compare a verdict that was never made.
    std::uint64_t has_verdict = 0;
    if (!field_u64(obj, "has_verdict", &has_verdict))
        has_verdict = 1; // a package written before the marker
    if (has_verdict == 0) {
        out->have_verdict = false;
        return true;
    }
    std::uint64_t value = 0;
    const auto read = [&](const char* name, std::uint64_t* slot) {
        if (!field_u64(*ver, name, &value))
            return false;
        *slot = value;
        return true;
    };
    if (!read("verdict", &out->verdict.verdict))
        return false;
    if (!read("family", &out->verdict.family))
        return false;
    if (!read("observed_ppm", reinterpret_cast<std::uint64_t*>(&out->verdict.observed_ppm)))
        return false;
    if (!read("adjusted_ppm", reinterpret_cast<std::uint64_t*>(&out->verdict.adjusted_ppm)))
        return false;
    if (!read("haircut_ppm", &out->verdict.haircut_ppm))
        return false;
    if (!read("threshold_ppm", &out->verdict.threshold_ppm))
        return false;
    if (!read("fingerprint", &out->verdict.fingerprint))
        return false;
    if (!read("policy_version", &out->verdict.policy_version))
        return false;
    if (!read("min_windows", &out->verdict.min_windows))
        return false;
    if (!read("tighten_factor", &out->verdict.tighten_factor))
        return false;
    if (!read("windows", &out->verdict.windows))
        return false;
    out->have_verdict = true;
    return true;
}

// Re-run the decision from the package and compare it with the verdict
// the coordinator recorded.
static bool reproduces(const Package& pkg, std::string* why) {
    aura::bus::GateEvidenceBody evidence{};
    evidence.worldline = pkg.worldline;
    evidence.round_id = pkg.round_id;
    evidence.workload_sym = pkg.workload_sym;
    evidence.seed = pkg.seed;
    evidence.verifier_version = pkg.verifier_version;
    evidence.fold_count = pkg.fold_count;
    evidence.threshold_ppm = pkg.threshold_ppm;
    evidence.control = pkg.control;
    for (std::size_t i = 0; i < pkg.windows.size() && i < aura::bus::kGateMaxWindows; ++i)
        evidence.windows[i] = pkg.windows[i];
    std::string reason;
    const aura::bus::GateVerdictRecord rec =
        aura::bus::gate_admit(evidence, pkg.verdict.family, pkg.policy, &reason);
    // No recorded verdict: the coordinator kept the worldline without
    // the statistical gate, so there is nothing to compare. The
    // package's own inputs are still verified below.
    if (!pkg.have_verdict)
        return true;
    if (rec.verdict != pkg.verdict.verdict) {
        if (why)
            *why = "verdict " + std::to_string(rec.verdict) + " != recorded " +
                   std::to_string(pkg.verdict.verdict);
        return false;
    }
    if (rec.observed_ppm != pkg.verdict.observed_ppm) {
        if (why)
            *why = "observed " + std::to_string(rec.observed_ppm) + " != recorded " +
                   std::to_string(pkg.verdict.observed_ppm);
        return false;
    }
    if (rec.adjusted_ppm != pkg.verdict.adjusted_ppm) {
        if (why)
            *why = "adjusted " + std::to_string(rec.adjusted_ppm) + " != recorded " +
                   std::to_string(pkg.verdict.adjusted_ppm);
        return false;
    }
    if (rec.haircut_ppm != pkg.verdict.haircut_ppm) {
        if (why)
            *why = "haircut " + std::to_string(rec.haircut_ppm) + " != recorded " +
                   std::to_string(pkg.verdict.haircut_ppm);
        return false;
    }
    if (rec.fingerprint != pkg.verdict.fingerprint) {
        if (why)
            *why = "fingerprint " + std::to_string(rec.fingerprint) + " != recorded " +
                   std::to_string(pkg.verdict.fingerprint);
        return false;
    }
    if (rec.policy_version != pkg.verdict.policy_version) {
        if (why)
            *why = "policy version " + std::to_string(rec.policy_version) + " != recorded " +
                   std::to_string(pkg.verdict.policy_version);
        return false;
    }
    if (rec.min_windows != pkg.verdict.min_windows) {
        if (why)
            *why = "min windows " + std::to_string(rec.min_windows) + " != recorded " +
                   std::to_string(pkg.verdict.min_windows);
        return false;
    }
    if (rec.tighten_factor != pkg.verdict.tighten_factor) {
        if (why)
            *why = "tighten factor " + std::to_string(rec.tighten_factor) + " != recorded " +
                   std::to_string(pkg.verdict.tighten_factor);
        return false;
    }
    return true;
}

static int run(int argc, char** argv) {
    if (argc < 3) {
        std::println(std::cerr, "usage: aura bus-repro FILE [--json]");
        return 2;
    }
    bool as_json = false;
    std::string path;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--json") {
            as_json = true;
            continue;
        }
        path = arg;
    }
    if (path.empty()) {
        std::println(std::cerr, "error: bus-repro needs a file");
        return 2;
    }
    std::ifstream in(path);
    if (!in) {
        std::println(std::cerr, "error: cannot read ", path);
        return 2;
    }
    std::size_t total = 0;
    std::size_t failed = 0;
    std::size_t malformed = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        ++total;
        evo_cli_4910::Parser ps{line};
        const evo_cli_4910::J value = ps.value();
        if (!ps.ok || value.t != evo_cli_4910::J::T::Obj) {
            ++malformed;
            continue;
        }
        Package pkg;
        if (!parse_package(value, &pkg)) {
            ++malformed;
            continue;
        }
        std::string why;
        if (!reproduces(pkg, &why)) {
            ++failed;
            if (!as_json)
                std::println("MISMATCH wl={} {}", pkg.worldline, why);
        }
    }
    if (as_json) {
        const std::string json = "{\"packages\":" + std::to_string(total) + ",\"failed\":" +
                                 std::to_string(failed) + ",\"malformed\":" +
                                 std::to_string(malformed) + ",\"ok\":" +
                                 (failed == 0 && malformed == 0 ? "true" : "false") + "}";
        std::println("{}", json);
    } else {
        std::println("bus-repro: {} package(s), {} mismatch(es), {} malformed", total, failed,
                     malformed);
    }
    if (malformed != 0)
        return 2;
    return failed == 0 ? 0 : 1;
}

} // namespace bus_repro_5397

// The evo log's JSON reader is reused to parse the package.
static int aura_bus_repro_5397(int argc, char** argv) {
    using namespace evo_cli_4910;
    return bus_repro_5397::run(argc, argv);
}
