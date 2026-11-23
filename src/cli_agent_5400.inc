// cli_agent_5400.inc — Issue #5400: `aura agent`.
//
// A thin launcher for the bus peer runner (lib/std/bus_agent_cli.aura).
// The CLI owns the option surface and resolves the runner; the loop
// itself lives in Aura so it uses the same std/bus and std/llm a user
// script does. No prompt body is hardcoded here: --prompt names a file
// and --mock replaces the model with a deterministic answer.
//
// usage: aura agent --role planner|worker|supervisor [options]
//
//   --role NAME        planner (4), worker (5) or supervisor (6)
//   --workspace N      the bus workspace id (default 8900)
//   --path PATH        the socket path (default: the workspace default)
//   --lease-ms N       a worker lease in milliseconds (default 30000)
//   --tasks N          planner: how many tasks to post (default 4)
//   --contract         planner: attach a contract (needs approval) to each task
//   --requirement TEXT planner: the requirement text (no prompt file needed in mock)
//   --prompt FILE      the role prompt file (real LLM mode)
//   --mock             deterministic mock model; no network, no key
//   --preset P         sandbox preset for the runner (default dev)
//
// The runner attaches as the role, then loops: claim -> model (or mock)
// -> heartbeat -> complete/fail. The supervisor asks the host for its
// scaling plan; the host measures its own CPU/memory slack and audits
// each decision.

namespace agent_cli_5400 {

struct Options {
    std::string role = "worker";
    std::uint32_t role_id = 5;
    std::uint64_t workspace = 8900;
    std::string path;
    std::uint64_t lease_ms = 30000;
    std::uint64_t tasks = 4;
    bool contract = false;
    std::string requirement;
    std::string prompt;
    bool mock = false;
    std::string preset = "dev";
};

inline constexpr std::string_view kUsage5400 =
    "usage: aura agent --role planner|worker|supervisor [options]\n"
    "\n"
    "Attach a bus peer and run its role loop. The runner is\n"
    "lib/std/bus_agent_cli.aura; it uses std/bus and std/llm.\n"
    "\n"
    "  --role NAME        planner (4), worker (5) or supervisor (6)\n"
    "  --workspace N      bus workspace id (default 8900)\n"
    "  --path PATH        socket path (default: the workspace default)\n"
    "  --lease-ms N       worker lease in ms (default 30000)\n"
    "  --tasks N          planner: tasks to post (default 4)\n"
    "  --contract         planner: attach an approval-gated contract\n"
    "  --requirement TEXT planner: the requirement text\n"
    "  --prompt FILE      role prompt file (real LLM mode)\n"
    "  --mock             deterministic mock model; no network, no key\n"
    "  --preset P         sandbox preset (default dev)\n"
    "\n"
    "Roles: 4 planner posts tasks, 5 worker claims/executes them,\n"
    "6 supervisor asks the host how many workers its real CPU/memory\n"
    "slack holds (no fixed cap) and prints the audited decision.\n";

inline bool parse_u64_5400(std::string_view s, std::uint64_t* out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    const std::string text(s);
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
        return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

inline std::optional<std::filesystem::path> runner_5400() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> roots;
    if (const char* explicit_path = std::getenv("AURA_AGENT_SCRIPT");
        explicit_path && *explicit_path)
        roots.emplace_back(explicit_path);
    // The dev build sits in build/ next to lib/; the installed layout
    // puts the runner in <prefix>/lib/std next to <prefix>/bin/aura.
    if (const char* self = ::aura::config::raw(::aura::config::Key::AURA_SELF);
        self && *self) {
        const fs::path bin(self);
        roots.push_back(bin.parent_path() / "lib" / "std" / "bus_agent_cli.aura");
        roots.push_back(bin.parent_path() / ".." / "lib" / "std" / "bus_agent_cli.aura");
    }
    if (const char* stdlib = ::aura::config::raw(::aura::config::Key::AURA_STDLIB_DIR);
        stdlib && *stdlib)
        roots.emplace_back(fs::path(stdlib) / "bus_agent_cli.aura");
    roots.emplace_back("lib/std/bus_agent_cli.aura");
    roots.emplace_back("../lib/std/bus_agent_cli.aura");
    for (const auto& p : roots) {
        if (fs::is_regular_file(p, ec))
            return fs::absolute(p, ec);
    }
    return std::nullopt;
}

// Parse options. On error, *error is set and the caller returns 1;
// --help is reported through *help so the caller prints and returns 0.
inline bool parse_5400(int argc, char** argv, Options* out, bool* help, std::string* error) {
    for (int i = 2; i < argc; ++i) {
        const std::string_view a(argv[i]);
        auto value = [&](const char* name, std::string* dst) {
            if (i + 1 >= argc) {
                *error = std::string("option ") + name + " needs a value";
                return false;
            }
            *dst = argv[++i];
            return true;
        };
        if (a == "--help" || a == "-h") {
            *help = true;
            return true;
        }
        if (a == "--mock") {
            out->mock = true;
        } else if (a == "--contract") {
            out->contract = true;
        } else if (a == "--role") {
            if (!value("--role", &out->role))
                return false;
        } else if (a == "--workspace") {
            std::string v;
            if (!value("--workspace", &v))
                return false;
            if (!parse_u64_5400(v, &out->workspace)) {
                *error = "--workspace must be a non-negative integer";
                return false;
            }
        } else if (a == "--path") {
            if (!value("--path", &out->path))
                return false;
        } else if (a == "--lease-ms") {
            std::string v;
            if (!value("--lease-ms", &v))
                return false;
            if (!parse_u64_5400(v, &out->lease_ms) || out->lease_ms == 0) {
                *error = "--lease-ms must be a positive integer";
                return false;
            }
        } else if (a == "--tasks") {
            std::string v;
            if (!value("--tasks", &v))
                return false;
            if (!parse_u64_5400(v, &out->tasks) || out->tasks == 0 || out->tasks > 64) {
                *error = "--tasks must be between 1 and 64";
                return false;
            }
        } else if (a == "--requirement") {
            if (!value("--requirement", &out->requirement))
                return false;
        } else if (a == "--prompt") {
            if (!value("--prompt", &out->prompt))
                return false;
        } else if (a == "--preset") {
            if (!value("--preset", &out->preset))
                return false;
        } else {
            *error = std::string("unknown option '") + std::string(a) + "'";
            return false;
        }
    }
    if (out->role == "planner")
        out->role_id = 4;
    else if (out->role == "worker")
        out->role_id = 5;
    else if (out->role == "supervisor")
        out->role_id = 6;
    else {
        *error = "unknown role '" + out->role +
                 "'; expected planner, worker or supervisor (roles 4/5/6)";
        return false;
    }
    if (!out->mock && out->role != "supervisor" && out->prompt.empty()) {
        *error = "a real planner/worker run needs --prompt FILE (or use --mock)";
        return false;
    }
    if (out->role == "planner" && !out->mock && out->requirement.empty()) {
        *error = "planner needs --requirement TEXT";
        return false;
    }
    return true;
}

} // namespace agent_cli_5400

// Prepare the runner invocation. Returns the argv for rewrite(); the
// caller exits with *exit_code when it is non-zero.
inline std::optional<std::vector<std::string>>
agent_argv_5400(int argc, char** argv, int* exit_code) {
    using namespace agent_cli_5400;
    Options opts;
    bool help = false;
    std::string error;
    if (!parse_5400(argc, argv, &opts, &help, &error)) {
        std::println(std::cerr, "error: {}", error);
        std::print(std::cerr, "{}", kUsage5400);
        *exit_code = 1;
        return std::nullopt;
    }
    if (help) {
        std::print("{}", kUsage5400);
        *exit_code = 0;
        return std::nullopt;
    }
    const auto runner = runner_5400();
    if (!runner) {
        std::println(std::cerr, "error: cannot find lib/std/bus_agent_cli.aura");
        std::println(std::cerr,
                     "hint: run from the aura tree, or set AURA_AGENT_SCRIPT to the runner");
        *exit_code = 1;
        return std::nullopt;
    }
    auto set_env = [](const char* name, const std::string& value) {
        ::setenv(name, value.c_str(), 1);
    };
    set_env("AURA_AGENT_ROLE", std::to_string(opts.role_id));
    set_env("AURA_AGENT_WORKSPACE", std::to_string(opts.workspace));
    set_env("AURA_AGENT_BUS_PATH", opts.path);
    set_env("AURA_AGENT_LEASE_MS", std::to_string(opts.lease_ms));
    set_env("AURA_AGENT_TASKS", std::to_string(opts.tasks));
    set_env("AURA_AGENT_CONTRACT", opts.contract ? "1" : "0");
    set_env("AURA_AGENT_REQUIREMENT", opts.requirement);
    set_env("AURA_AGENT_PROMPT", opts.prompt);
    set_env("AURA_AGENT_MOCK", opts.mock ? "1" : "0");
    set_env("AURA_AGENT_VERSION", "0.3.3");
    ::aura::config::reload();
    *exit_code = 0;
    return std::vector<std::string>{argv[0], "--preset", opts.preset, runner->string()};
}
