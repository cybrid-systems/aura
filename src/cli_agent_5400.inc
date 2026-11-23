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
    // #5422: the supervisor's execution layer.
    bool spawn = false;
    bool spawn_once = false;
    std::uint64_t interval_ms = 1000;
    std::uint64_t spawn_batch = 4;
    std::uint64_t drain_ms = 10000;
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
    "  --spawn            supervisor: start/stop worker processes\n"
    "  --spawn-once       supervisor: one spawn cycle, wait, exit (CI)\n"
    "  --interval-ms N    supervisor spawn loop interval (default 1000)\n"
    "  --spawn-batch N    max workers started per cycle (default 4)\n"
    "  --drain-ms N       graceful drain before SIGTERM (default 10000)\n"
    "\n"
    "Roles: 4 planner posts tasks, 5 worker claims/executes them,\n"
    "6 supervisor asks the host how many workers its real CPU/memory\n"
    "slack holds (no fixed cap). With --spawn it also starts the workers\n"
    "and audits spawn/shrink decisions.\n";

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
        } else if (a == "--spawn") {
            out->spawn = true;
        } else if (a == "--spawn-once") {
            out->spawn = true;
            out->spawn_once = true;
        } else if (a == "--interval-ms") {
            std::string v;
            if (!value("--interval-ms", &v))
                return false;
            if (!parse_u64_5400(v, &out->interval_ms) || out->interval_ms < 50 ||
                out->interval_ms > 3600000) {
                *error = "--interval-ms must be between 50 and 3600000";
                return false;
            }
        } else if (a == "--spawn-batch") {
            std::string v;
            if (!value("--spawn-batch", &v))
                return false;
            if (!parse_u64_5400(v, &out->spawn_batch) || out->spawn_batch == 0 ||
                out->spawn_batch > 64) {
                *error = "--spawn-batch must be between 1 and 64";
                return false;
            }
        } else if (a == "--drain-ms") {
            std::string v;
            if (!value("--drain-ms", &v))
                return false;
            if (!parse_u64_5400(v, &out->drain_ms) || out->drain_ms > 3600000) {
                *error = "--drain-ms must be between 0 and 3600000";
                return false;
            }
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
    if (out->spawn && out->role != "supervisor") {
        *error = "--spawn is only valid with --role supervisor";
        return false;
    }
    if (out->spawn && !out->mock && out->prompt.empty()) {
        *error = "spawned workers need --prompt FILE (or use --mock)";
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

// #5422: the supervisor's execution layer. The plan comes from the host
// (bus:supervise); this loop owns the worker processes. It is opt-in
// (--spawn) and defaults to one-shot only in tests (--spawn-once).
namespace spawn_5422 {

inline volatile std::sig_atomic_t g_stop = 0;

inline void on_signal(int) {
    g_stop = 1;
}

inline bool query_plan(aura::bus::Attach& att, aura::bus::SupervisorReplyBody* out,
                       std::string* why) {
    aura::bus::SupervisorQueryBody req{};
    if (!aura::bus::send_frame(
            att.sock, aura::bus::FrameType::SupervisorQuery, att.session,
            {reinterpret_cast<const std::byte*>(&req), sizeof(req)})) {
        *why = "supervise query send failed";
        return false;
    }
    aura::bus::FrameHeader hdr{};
    std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
    aura::bus::TransportError err;
    if (!aura::bus::recv_frame(att.sock, &hdr, std::span<std::byte>(body), &err)) {
        *why = err.message.empty() ? "supervise query reply lost" : err.message;
        return false;
    }
    if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
        aura::bus::ErrorBody eb{};
        std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
        *why = eb.message;
        return false;
    }
    if (hdr.len < sizeof(aura::bus::SupervisorReplyBody)) {
        *why = "malformed supervise reply";
        return false;
    }
    std::memcpy(out, body.data(), sizeof(*out));
    return true;
}

inline bool send_note(aura::bus::Attach& att, std::uint64_t spawned, std::uint64_t stopped,
                      std::uint64_t alive) {
    aura::bus::SupervisorNoteBody note{};
    note.spawned = spawned;
    note.stopped = stopped;
    note.alive = alive;
    return aura::bus::send_frame(
        att.sock, aura::bus::FrameType::SupervisorNote, att.session,
        {reinterpret_cast<const std::byte*>(&note), sizeof(note)});
}

inline void reap(std::vector<pid_t>* children) {
    for (auto it = children->begin(); it != children->end();) {
        int status = 0;
        const pid_t r = ::waitpid(*it, &status, WNOHANG);
        if (r == *it || (r == -1 && errno == ECHILD))
            it = children->erase(it);
        else
            ++it;
    }
}

inline pid_t spawn_worker(const Options& opts, const std::string& self, std::string* why) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        *why = std::string("fork: ") + std::strerror(errno);
        return -1;
    }
    if (pid == 0) {
#ifdef __linux__
        // The worker must not outlive a killed supervisor.
        (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        std::vector<std::string> args;
        args.push_back(self);
        args.push_back("agent");
        args.push_back("--role");
        args.push_back("worker");
        args.push_back("--workspace");
        args.push_back(std::to_string(opts.workspace));
        if (!opts.path.empty()) {
            args.push_back("--path");
            args.push_back(opts.path);
        }
        args.push_back("--lease-ms");
        args.push_back(std::to_string(opts.lease_ms));
        args.push_back("--preset");
        args.push_back(opts.preset);
        if (!opts.requirement.empty()) {
            args.push_back("--requirement");
            args.push_back(opts.requirement);
        }
        if (opts.mock) {
            args.push_back("--mock");
        } else {
            args.push_back("--prompt");
            args.push_back(opts.prompt);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        ::execv(self.c_str(), argv.data());
        _exit(127);
    }
    return pid;
}

inline void terminate_children(std::vector<pid_t>* children) {
    for (const pid_t pid : *children)
        ::kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!children->empty() && std::chrono::steady_clock::now() < deadline) {
        reap(children);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (const pid_t pid : *children)
        ::kill(pid, SIGKILL);
    for (const pid_t pid : *children) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    children->clear();
}

inline void wait_children(std::vector<pid_t>* children) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!children->empty() && std::chrono::steady_clock::now() < deadline) {
        reap(children);
        if (!children->empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!children->empty())
        terminate_children(children);
}

inline bool fatal_attach(const std::string& why) {
    return why.starts_with("attach rejected");
}

inline void sleep_reaping(std::uint64_t ms, std::vector<pid_t>* children) {
    for (std::uint64_t waited = 0; waited < ms && !g_stop; waited += 100) {
        const auto slice = std::min<std::uint64_t>(100, ms - waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        reap(children);
    }
}

// Graceful drain: let active workers finish, then terminate the rest.
inline void drain_children(std::vector<pid_t>* children, std::uint64_t ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!children->empty() && std::chrono::steady_clock::now() < deadline) {
        reap(children);
        if (!children->empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!children->empty())
        terminate_children(children);
}

inline int run(const Options& opts, int* exit_code) {
    *exit_code = 1;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string path =
        opts.path.empty() ? aura::bus::default_socket_path(opts.workspace) : opts.path;
    const std::string self = ::aura::compiler::paths::resolve_self_executable(nullptr);
    if (self.empty()) {
        std::println(std::cerr, "error: cannot resolve the aura executable for workers");
        return 1;
    }

    std::vector<pid_t> children;
    std::uint64_t total_spawned = 0;
    std::uint64_t last_target = ~std::uint64_t{0};
    bool query_ok = true;
    bool connected = false;
    std::uint64_t reconnect_ms = 1000;
    aura::bus::Attach att;

    while (!g_stop) {
        if (!connected) {
            aura::bus::Attach fresh;
            aura::bus::TransportError err;
            aura::bus::Client client;
            if (!client.connect_to(path, opts.workspace, 6, 255, &fresh, &err)) {
                if (fatal_attach(err.message)) {
                    std::println(std::cerr, "error: supervisor attach rejected: {}", err.message);
                    query_ok = false;
                    break;
                }
                std::println("supervisor-reconnect-wait={} reason={}", reconnect_ms, err.message);
                std::fflush(stdout);
                sleep_reaping(reconnect_ms, &children);
                reconnect_ms = std::min<std::uint64_t>(reconnect_ms * 2, 30000);
                continue;
            }
            att = std::move(fresh);
            connected = true;
            reconnect_ms = 1000;
            std::println("supervisor-attached=1");
            std::fflush(stdout);
        }

        reap(&children);
        aura::bus::SupervisorReplyBody plan{};
        std::string why;
        if (!query_plan(att, &plan, &why)) {
            std::println(std::cerr, "warning: host query failed: {}", why);
            connected = false;
            att = aura::bus::Attach{};
            sleep_reaping(reconnect_ms, &children);
            reconnect_ms = std::min<std::uint64_t>(reconnect_ms * 2, 30000);
            continue;
        }

        const std::uint64_t alive = children.size();
        // Capacity alone must not spawn an idle loop: start workers only
        // for open work. A live worker may already hold a claim, so the
        // target is bounded by alive + open tasks.
        const std::uint64_t target =
            std::min<std::uint64_t>(plan.workers, alive + plan.open_tasks);
        std::uint64_t spawned = 0;
        std::uint64_t stopped = 0;
        if (target > alive) {
            const std::uint64_t want =
                std::min<std::uint64_t>(target - alive, opts.spawn_batch);
            for (std::uint64_t i = 0; i < want; ++i) {
                std::string spawn_why;
                const pid_t pid = spawn_worker(opts, self, &spawn_why);
                if (pid < 0) {
                    std::println(std::cerr, "error: cannot spawn worker: {}", spawn_why);
                    break;
                }
                children.push_back(pid);
                ++spawned;
                ++total_spawned;
            }
        } else if (target < alive && target < last_target) {
            // A shrink is a decision, not a kill: active workers finish
            // their task and exit; the supervisor stops replacing them.
            stopped = alive - target;
        }
        if (spawned > 0 || stopped > 0) {
            if (!send_note(att, spawned, stopped, children.size())) {
                std::println(std::cerr, "warning: supervisor note was not delivered");
                connected = false;
                att = aura::bus::Attach{};
            }
        }
        std::println(
            "supervisor-desired={} open-tasks={} target={} alive={} spawned={} stopped={} "
            "cpu-free-pct={} mem-free-mb={} reason={}",
            plan.workers, plan.open_tasks, target, children.size(), spawned, stopped,
            plan.cpu_free_pct, plan.mem_free_mb, plan.reason);
        std::fflush(stdout);
        last_target = target;
        if (opts.spawn_once)
            break;
        sleep_reaping(opts.interval_ms, &children);
    }

    if (!opts.spawn_once)
        drain_children(&children, opts.drain_ms);
    else
        wait_children(&children);

    std::println("supervisor-done spawned={} alive={}", total_spawned, children.size());
    std::fflush(stdout);
    if (g_stop)
        std::println("supervisor-stopped=1");
    *exit_code = query_ok ? 0 : 1;
    return *exit_code;
}

} // namespace spawn_5422

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
    // #5422: the supervisor owns worker processes; it does not run the
    // Aura runner itself. --spawn is opt-in.
    if (opts.spawn) {
        (void)spawn_5422::run(opts, exit_code);
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
