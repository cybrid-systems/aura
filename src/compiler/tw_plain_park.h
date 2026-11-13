// tw_plain_park.h — Issue #5274: a certifiable park for a PLAIN tree-walker.
//
// Both file-heap sweeps (Evaluator::reclaim_tw_unreachable_now and
// IRInterpreter::reclaim_unreachable_now) stand down while another thread is
// inside the tree walker without a registered fiber segment (#2738 / #4747 /
// #4768): its env frames, pinned results/vectors/spans live in that thread's
// default segment, which the collector has no way to cross-mark. A CLI
// std::thread fiber body (fiber:spawn fallback) is exactly that class, and a
// long-lived game keeps one parked in channel:recv forever — so no sweep ever
// ran, every env frame / closure / string stayed alive (RSS ~850 MB in 30 s)
// and every outermost mutate paid the full linear audit.
//
// A thread that is about to BLOCK can certify its segment: for the lifetime
// of this bracket it publishes the segment as parked (the marker a collector
// cross-marks), leaves the walker + body counters, and — on release — waits
// out any sweep that already decided to mark it before it touches the segment
// again. Only bracket a wait that reads and writes no EvalValue and no
// tree-walker frame (a condvar / poll / accept / recv); the bracket asserts
// that the segment is stable for its whole lifetime, which is exactly what a
// blocked primitive can promise and a running one cannot.
#pragma once

extern "C" {
// 1 = this thread's plain segment is parked for the caller; 0 = nothing to
// certify (not walking, no live frames, or a fiber segment is attached).
int aura_tw_plain_park_begin() noexcept;
// Undo exactly what begin() did. Nested begin/end pairs collapse onto the
// outermost one (only that pair moves the counters).
void aura_tw_plain_park_end(int parked) noexcept;
}

namespace aura::compiler {

// RAII face. Declare it BEFORE the lock guard of the wait it wraps so the
// lock is released before the segment is handed back (the unpark can wait out
// a sweep, and a peer must not queue behind that while we hold a hot mutex).
struct TwPlainPark {
    int parked = 0;
    TwPlainPark() noexcept
        : parked(aura_tw_plain_park_begin()) {}
    ~TwPlainPark() { aura_tw_plain_park_end(parked); }
    TwPlainPark(const TwPlainPark&) = delete;
    TwPlainPark& operator=(const TwPlainPark&) = delete;
    TwPlainPark(TwPlainPark&&) = delete;
    TwPlainPark& operator=(TwPlainPark&&) = delete;
};

} // namespace aura::compiler
