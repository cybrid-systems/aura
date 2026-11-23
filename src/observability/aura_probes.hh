// aura_probes.hh — zero-cost USDT probes for external system tools (#5409).
//
// Each probe emits one patched NOP plus a .note.stapsdt descriptor. With no
// probe attached the runtime cost is one NOP; `perf`, `bpftrace` and other
// SystemTap-compatible tools can attach to the provider/name without any
// Aura source changes. On non-Linux / unsupported targets the macros are
// empty, so the same code builds everywhere.
#ifndef AURA_OBSERVABILITY_AURA_PROBES_HH
#define AURA_OBSERVABILITY_AURA_PROBES_HH

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) &&                         \
    !defined(AURA_DISABLE_USDT)
#define AURA_SDT_PROBE(provider, name)                                                             \
    do {                                                                                           \
        __asm__ __volatile__("990: nop\n\t"                                                        \
                             ".pushsection .note.stapsdt,\"?\",@note\n\t"                          \
                             ".balign 4\n\t"                                                       \
                             ".4byte 992f-991f, 994f-993f, 3\n\t"                                  \
                             "991: .asciz \"stapsdt\"\n\t"                                         \
                             "992: .balign 4\n\t"                                                  \
                             "993: .8byte 990b\n\t"                                                \
                             ".8byte 0\n\t"                                                        \
                             ".8byte 0\n\t"                                                        \
                             ".asciz \"" provider "\"\n\t"                                         \
                             ".asciz \"" name "\"\n\t"                                             \
                             ".asciz \"\"\n\t"                                                     \
                             "994: .balign 4\n\t"                                                  \
                             ".popsection\n\t" ::                                                  \
                                 : "memory");                                                      \
    } while (0)
#else
#define AURA_SDT_PROBE(provider, name)                                                             \
    do {                                                                                           \
    } while (0)
#endif

namespace aura::observability {

// RAII so every exit path (including early returns and exceptions) emits the
// matching end probe. The objects are tiny and empty on non-probe targets.
struct SdtFiberSpawn {
    SdtFiberSpawn() noexcept { AURA_SDT_PROBE("aura", "fiber__spawn"); }
};
struct SdtFiberYield {
    SdtFiberYield() noexcept { AURA_SDT_PROBE("aura", "fiber__yield"); }
};
struct SdtFiberResume {
    SdtFiberResume() noexcept { AURA_SDT_PROBE("aura", "fiber__resume"); }
};
struct SdtChannelWait {
    SdtChannelWait() noexcept { AURA_SDT_PROBE("aura", "channel__wait_enter"); }
    ~SdtChannelWait() noexcept { AURA_SDT_PROBE("aura", "channel__wait_exit"); }
};
struct SdtGcCollect {
    SdtGcCollect() noexcept { AURA_SDT_PROBE("aura", "gc__start"); }
    ~SdtGcCollect() noexcept { AURA_SDT_PROBE("aura", "gc__end"); }
};
struct SdtHttpPost {
    SdtHttpPost() noexcept { AURA_SDT_PROBE("aura", "http_post__start"); }
    ~SdtHttpPost() noexcept { AURA_SDT_PROBE("aura", "http_post__end"); }
};
struct SdtRequireEval {
    SdtRequireEval() noexcept { AURA_SDT_PROBE("aura", "require_eval__start"); }
    ~SdtRequireEval() noexcept { AURA_SDT_PROBE("aura", "require_eval__end"); }
};

} // namespace aura::observability

#endif // AURA_OBSERVABILITY_AURA_PROBES_HH
