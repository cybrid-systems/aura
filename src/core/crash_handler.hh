// crash_handler.hh — Issue #4931 / #5155 / #5211: fatal-signal reporting.
//
// Extracted from src/main.cpp so tests (and non-main entry points) install
// the same handler the CLI uses. Async-signal-safe: raw write(2), no stdio
// or backtrace(3), and SA_ONSTACK runs it on the alternate stack installed
// here. Worker threads install their own alt stack (src/serve/worker.cpp).
#ifndef AURA_CORE_CRASH_HANDLER_HH
#define AURA_CORE_CRASH_HANDLER_HH

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#if defined(__linux__) && !defined(__APPLE__)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/ucontext.h>
#include <unistd.h>
#endif

namespace aura::core {

inline void install_fatal_signal_handlers() {
    // ── Crash handler: print backtrace on fatal signal ────────────
    // ── Crash handler: print backtrace on fatal signal ────────────
    // CI's --emit-binary path has been hitting 2 segfaults that don't
    // reproduce locally. This handler flushes a backtrace to stderr
    // before re-raising, so run-tests.sh's stderr capture (3ddf924)
    // will surface the failing instruction/frame. Skipped on macOS
    // (no execinfo.h); on Linux we use backtrace(3) from <execinfo.h>.
    //
    // CRITICAL: install an alternate signal stack (sigaltstack) so the
    // handler itself doesn't crash when the main stack is exhausted
    // (synthesize:optimize infinite recursion → stack overflow → SIGSEGV).
    // Without this, the handler would run on the main stack and re-fault
    // before backtrace() can capture frames, yielding only the header
    // line and "Backtrace:" with zero frame data.
#if defined(__linux__) && !defined(__APPLE__)
    static char alt_stack[65536]; // 64K alt stack (SIGSTKSZ is system-dependent)
    // Note: alt_stack must outlive the process; static-storage is fine.
    {
        stack_t ss{};
        ss.ss_sp = alt_stack;
        ss.ss_size = sizeof(alt_stack);
        ss.ss_flags = 0;
        if (::sigaltstack(&ss, nullptr) != 0) {
            (void)!::write(2, "aura: sigaltstack failed\n", 24);
        }
    }

    // Issue #4931: a SIGSEGV whose si_addr sits on (or just below) the
    // faulting thread's own stack is an overflow, not a wild pointer. The
    // old handler only consulted dladdr(), so a 512K-stack exhaustion in
    // FlatAST::assemble_nodeview reported "si_addr ... is outside any loaded
    // module (use-after-free or uninit ptr?)" and sent triage down the wrong
    // path. Read /proc/self/maps with raw syscalls (async-signal-safe, runs
    // on the alternate stack) and report the used depth instead.
    static auto report_stack_exhaustion = +[](const void* bad, const void* sp) -> bool {
        if (bad == nullptr || sp == nullptr)
            return false;
        static char maps_buf[262144];
        const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        long total = 0;
        while (total < static_cast<long>(sizeof(maps_buf)) - 1) {
            const long got =
                static_cast<long>(::read(fd, maps_buf + total, sizeof(maps_buf) - 1 - total));
            if (got <= 0)
                break;
            total += got;
        }
        ::close(fd);
        if (total <= 0)
            return false;
        maps_buf[total] = '\0';

        const auto sp_u = reinterpret_cast<unsigned long long>(sp);
        const auto bad_u = reinterpret_cast<unsigned long long>(bad);
        unsigned long long lo = 0, hi = 0;   // mapping that holds sp
        unsigned long long slo = 0, shi = 0; // the [stack] mapping
        long i = 0;
        while (i < total) {
            long j = i;
            while (j < total && maps_buf[j] != '\n')
                ++j;
            // "start-end perms ... [stack]"
            unsigned long long a = 0, b = 0;
            long k = i;
            auto hexval = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            while (k < j) {
                const int v = hexval(maps_buf[k]);
                if (v < 0)
                    break;
                a = (a << 4) | static_cast<unsigned long long>(v);
                ++k;
            }
            bool ok = k < j && maps_buf[k] == '-';
            if (ok)
                ++k;
            while (ok && k < j) {
                const int v = hexval(maps_buf[k]);
                if (v < 0)
                    break;
                b = (b << 4) | static_cast<unsigned long long>(v);
                ++k;
            }
            if (ok) {
                bool is_stack = false;
                for (long t = i; t + 6 <= j; ++t) {
                    if (maps_buf[t] == '[' && maps_buf[t + 1] == 's' && maps_buf[t + 2] == 't' &&
                        maps_buf[t + 3] == 'a' && maps_buf[t + 4] == 'c' &&
                        maps_buf[t + 5] == 'k') {
                        is_stack = true;
                        break;
                    }
                }
                if (is_stack && (slo == 0 || (sp_u >= a && sp_u < b))) {
                    slo = a;
                    shi = b;
                }
                if (sp_u >= a && sp_u < b) {
                    lo = a;
                    hi = b;
                }
            }
            i = j + 1;
        }
        // The faulting sp normally sits inside the thread's stack mapping.
        // An oversized frame (`sub sp, #N` past the guard) lands just BELOW
        // the mapping and the first store faults at sp+off, so accept an sp
        // in the guard window too.
        constexpr unsigned long long kGuardWindow = 1024ULL * 1024ULL;
        if (lo == 0 && slo != 0) {
            const bool sp_inside = sp_u >= slo && sp_u < shi;
            const bool sp_in_guard = sp_u < slo && (slo - sp_u) <= kGuardWindow;
            if (sp_inside || sp_in_guard) {
                lo = slo;
                hi = shi;
            }
        }
        if (lo == 0 || hi <= lo)
            return false;
        const unsigned long long stack_sz = hi - lo;
        const bool sp_below = sp_u < lo;
        // Used depth: bytes consumed from the stack top, including an
        // oversized frame that already spilled into the guard page.
        const unsigned long long used = sp_below ? (stack_sz + (lo - sp_u)) : (hi - sp_u);
        const bool in_stack = bad_u >= lo && bad_u < hi;
        const bool below_stack = bad_u < lo && (lo - bad_u) <= kGuardWindow;
        const unsigned long long dist = bad_u >= sp_u ? bad_u - sp_u : sp_u - bad_u;
        if (!((in_stack || below_stack) && dist <= (256ULL * 1024ULL)))
            return false;
        char line[512];
        const int n =
            std::snprintf(line, sizeof(line),
                          "si_addr %p is stack exhaustion: used %llu of %llu bytes of stack "
                          "[%p,%p) (sp %p, fault %lld bytes from sp)%s\n",
                          bad, used, stack_sz, reinterpret_cast<const void*>(lo),
                          reinterpret_cast<const void*>(hi), sp,
                          static_cast<long long>(bad_u) - static_cast<long long>(sp_u),
                          sp_below ? " [sp past the stack bottom: oversized frame]" : "");
        if (n > 0)
            (void)!::write(2, line, n);
        return true;
    };

    static auto crash_handler = +[](int sig, siginfo_t* info, void* ucontext) {
        // Use raw write(2) — stdio may be in undefined state.
        const char* name = sig == SIGSEGV   ? "SIGSEGV"
                           : sig == SIGABRT ? "SIGABRT"
                           : sig == SIGBUS  ? "SIGBUS"
                           : sig == SIGFPE  ? "SIGFPE"
                                            : "SIGNAL";
        // 1) Read the faulting instruction pointer directly from the kernel-
        //    provided ucontext. This is the most useful single piece of
        //    info: `addr2line -e aura -f -C <IP>` gives function + line.
        //    We deliberately do NOT call backtrace(3) here because it walks
        //    the (corrupt) frame pointer chain on the main stack, which can
        //    re-fault inside the handler and prevent any output. (We've seen
        //    this exact failure mode: handler writes header + "Backtrace:"
        //    then dies with no frames visible.)
        void* ip = nullptr;
        void* sp = nullptr;
        if (ucontext) {
#if defined(__x86_64__)
            // Linux x86_64: gregs[REG_RIP] / gregs[REG_RSP] (sys/ucontext.h).
            auto* mctx = &static_cast<ucontext_t*>(ucontext)->uc_mcontext;
            ip = reinterpret_cast<void*>(mctx->gregs[REG_RIP]);
            sp = reinterpret_cast<void*>(mctx->gregs[REG_RSP]);
#elif defined(__aarch64__)
            // Linux aarch64: mcontext_t has pc / sp (sys/ucontext.h).
            auto* mctx = &static_cast<ucontext_t*>(ucontext)->uc_mcontext;
            ip = reinterpret_cast<void*>(mctx->pc);
            sp = reinterpret_cast<void*>(mctx->sp);
#endif
        }

        // 2) Header: name, signal#, si_addr (the data address being accessed),
        //    si_code (SEGV_MAPERR=1, etc.), plus IP and SP from ucontext.
        char hdr[512];
        int n = 0;
        if (info) {
            n = std::snprintf(
                hdr, sizeof(hdr),
                "\n=== AURA CRASH: %s (signal %d) si_addr=%p si_code=%d ip=%p sp=%p ===\n", name,
                sig, info->si_addr, info->si_code, ip, sp);
        } else {
            n = std::snprintf(hdr, sizeof(hdr),
                              "\n=== AURA CRASH: %s (signal %d) ip=%p sp=%p ===\n", name, sig, ip,
                              sp);
        }
        if (n > 0)
            (void)!::write(2, hdr, n);

        // 3) Symbol-resolve the faulting IP (and si_addr) with dladdr.
        //    dladdr is async-signal-safe (POSIX) and works on the alt stack.
        //    Requires -rdynamic to see internal symbols; otherwise we still
        //    get dli_fname (the module that contains the address).
        if (ip) {
            Dl_info dlinfo;
            char line[512];
            int ln = 0;
            if (::dladdr(ip, &dlinfo) && dlinfo.dli_sname) {
                std::ptrdiff_t off =
                    static_cast<char*>(ip) - static_cast<const char*>(dlinfo.dli_saddr);
                ln =
                    std::snprintf(line, sizeof(line), "Faulting IP: %s+%td in %s\n",
                                  dlinfo.dli_sname, off, dlinfo.dli_fname ? dlinfo.dli_fname : "?");
            } else {
                ln = std::snprintf(line, sizeof(line),
                                   "Faulting IP: %p (no symbol; -rdynamic missing or stripped)\n",
                                   ip);
            }
            if (ln > 0)
                (void)!::write(2, line, ln);
        }
        // Issue #4931: classify a stack-overflow fault before falling back
        // to dladdr(), which cannot resolve a stack address and used to
        // report it as a use-after-free/wild pointer.
        // Only address-bearing faults carry a meaningful si_addr (for
        // SIGABRT/SIGFPE the union holds the signal's own payload).
        const bool stack_overflow = (sig == SIGSEGV || sig == SIGBUS) && info && info->si_addr &&
                                    report_stack_exhaustion(info->si_addr, sp);
        if (!stack_overflow && info && info->si_addr) {
            Dl_info dlinfo;
            char line[512];
            int ln = 0;
            void* bad = info->si_addr;
            if (::dladdr(bad, &dlinfo) && dlinfo.dli_sname) {
                std::ptrdiff_t off =
                    static_cast<char*>(bad) - static_cast<const char*>(dlinfo.dli_saddr);
                ln =
                    std::snprintf(line, sizeof(line), "si_addr in: %s+%td in %s\n",
                                  dlinfo.dli_sname, off, dlinfo.dli_fname ? dlinfo.dli_fname : "?");
            } else {
                ln = std::snprintf(
                    line, sizeof(line),
                    "si_addr %p is outside any loaded module (use-after-free or uninit ptr?)\n",
                    bad);
            }
            if (ln > 0)
                (void)!::write(2, line, ln);
        }
        (void)!::write(2, "Resolve with: addr2line -e aura -f -C <ip> <si_addr>\n", 54);
        (void)!::write(2, "=== END CRASH ===\n", 18);
        // Re-raise with default handler so the shell sees the signal
        // and core-dump machinery (ulimit -c) still kicks in.
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        ::sigaction(sig, &dfl, nullptr);
        ::raise(sig);
    };

    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // SA_ONSTACK → use alt stack
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
    ::sigaction(SIGFPE, &sa, nullptr);
#endif
}

} // namespace aura::core

#endif // AURA_CORE_CRASH_HANDLER_HH
