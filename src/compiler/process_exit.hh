// Issue #5436: the exit status a CLI run has decided on, for the teardown.
//
// A fiber body that is still running when its Evaluator is destroyed cannot
// be joined: the drain detaches it (Issue #3394) and the teardown then frees
// the heaps under it. When the process is exiting anyway there is nothing
// left to tear down: the drain ends the process here, with the status the
// run had decided on, before any evaluator memory is freed.
//
// extern "C" so the note can be written from the CLI module and read from
// the evaluator module through one global-module declaration.
#ifndef AURA_COMPILER_PROCESS_EXIT_HH
#define AURA_COMPILER_PROCESS_EXIT_HH

// The process is about to exit with `code` once the CompilerService that
// ran the program goes out of scope. -1: no exit pending (tests, hosts that
// destroy an evaluator mid-process); the drain then keeps its bounded
// detach.
extern "C" void aura_note_process_exit(int code) noexcept;
extern "C" int aura_pending_process_exit() noexcept;

namespace aura::compiler {

// The status of a CLI run. `return rc;` constructs it while the service is
// still alive, so the drain in the service destructor knows the status.
class CliExitStatus {
public:
    // Implicit on purpose: `return rc;` is the whole point.
    CliExitStatus(int status) noexcept
        : code_(status) {
        aura_note_process_exit(status);
    }
    [[nodiscard]] operator int() const noexcept { return code_; }

private:
    int code_;
};

} // namespace aura::compiler

#endif // AURA_COMPILER_PROCESS_EXIT_HH
