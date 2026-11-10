// Issue #5117 — (engine:metrics) after a workspace mutation aborted under
// the restricted sandbox. The group snapshot passed aot_last_reemit_mid
// (bit 63 set by the reemit) to make_int, and the production hot contract
// called abort. try/catch cannot see that. These cases spawn the aura CLI.
//
//   AC1: set-code + eval-current + (engine:metrics) under --preset dev
//   AC2: the same program under --preset ci
//   AC3: mutate:rebind + eval-current + (engine:metrics) under --preset dev
//   AC4: the set-code program under AURA_SANDBOX=off still returns the hash

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_failed = 0;

struct CliRun {
    int status = -1;
    std::string out;
    std::string err;
};

std::string aura_binary() {
    if (const char* e = std::getenv("AURA_BIN")) {
        if (::access(e, X_OK) == 0)
            return e;
    }
    char buf[4096];
    const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string dir(buf);
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            auto cand = dir.substr(0, slash + 1) + "aura";
            if (::access(cand.c_str(), X_OK) == 0)
                return cand;
        }
    }
    for (const char* c : {"./aura", "./build/aura", "../aura", "../build/aura"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return {};
}

// The issue runner exports AURA_SANDBOX=off. Drop it so --preset dev
// actually arms the restricted sandbox.
CliRun run_aura(const std::string& bin, const std::string& path, const char* preset,
                bool sandbox_off) {
    CliRun r;
    const std::string outp = "/tmp/aura-5117-out.txt";
    const std::string errp = "/tmp/aura-5117-err.txt";
    std::string cmd = "timeout 30 env -u AURA_SANDBOX -u AURA_HOT_HARDEN ";
    if (sandbox_off)
        cmd += "AURA_SANDBOX=off ";
    cmd += "'" + bin + "' --preset " + preset + " --load '" + path + "' >'" + outp + "' 2>'" +
           errp + "'";
    r.status = std::system(cmd.c_str());
    {
        std::ifstream in(outp);
        r.out.assign(std::istreambuf_iterator<char>(in), {});
    }
    {
        std::ifstream in(errp);
        r.err.assign(std::istreambuf_iterator<char>(in), {});
    }
    return r;
}

void write_file(const char* path, const char* body) {
    std::ofstream f(path);
    f << body;
}

bool survived(const CliRun& r) {
    return r.status != -1 && WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0 &&
           r.err.find("AURA CRASH") == std::string::npos &&
           r.err.find("SIGABRT") == std::string::npos;
}

void expect_metrics_hash(const CliRun& r, const char* label) {
    const bool aborted = r.status != -1 && WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGABRT;
    if (aborted || !survived(r) || r.out.find("#t") == std::string::npos ||
        (r.out.find("\n2\n") == std::string::npos && r.out.find("2\n") == std::string::npos)) {
        std::cerr << "  FAIL: " << label << " status " << r.status << " out [" << r.out << "] err ["
                  << r.err << "]\n";
        ++g_failed;
        return;
    }
    std::cout << "  PASS: " << label << "\n";
}

} // namespace

int main() {
    const auto bin = aura_binary();
    if (bin.empty()) {
        std::cerr << "FAIL: aura binary not found\n";
        return 1;
    }
    const char* set_code = "/tmp/aura-5117-set-code.aura";
    write_file(set_code, "(set-code \"(define g (lambda () 1))\")\n"
                         "(eval-current)\n"
                         "(display (hash? (engine:metrics)))\n"
                         "(newline)\n"
                         "(display (hash-ref (engine:metrics) \"schema\"))\n"
                         "(newline)\n");
    const char* rebind = "/tmp/aura-5117-rebind.aura";
    write_file(rebind, "(define g (lambda () 1))\n"
                       "(mutate:rebind \"g\" \"(lambda () 2)\")\n"
                       "(eval-current)\n"
                       "(display (hash? (engine:metrics)))\n"
                       "(newline)\n"
                       "(display (hash-ref (engine:metrics) \"schema\"))\n"
                       "(newline)\n");

    std::cout << "=== #5117 engine:metrics after a workspace mutation ===\n";
    expect_metrics_hash(run_aura(bin, set_code, "dev", false), "AC1 set-code preset dev");
    expect_metrics_hash(run_aura(bin, set_code, "ci", false), "AC2 set-code preset ci");
    expect_metrics_hash(run_aura(bin, rebind, "dev", false), "AC3 mutate:rebind preset dev");
    expect_metrics_hash(run_aura(bin, set_code, "dev", true), "AC4 AURA_SANDBOX=off");
    if (g_failed != 0) {
        std::cerr << g_failed << " failed\n";
        return 1;
    }
    std::cout << "OK-5117\n";
    return 0;
}
