# JIT Exception Bridge (Issue #811)

## Classification

| Path | Mechanism | Allowed |
|------|-----------|---------|
| Guest-language `Raise` / OpRaise | `aura_throw_exception` + personality | **Yes** — language feature |
| Internal runtime / bridge errors | `AuraResult` / Diagnostic | **No throw** |

## Observability

- `(query:jit-exception-bridge-stats)` schema **811**
  - `guest-exception-bridge` — count of guest EH bridge entries
  - `internal-aura-result-path` — internal Result-style error path samples
  - `guest-only-policy-active` — 1 when policy is documented/enforced

## Notes

Hot IR executor paths should remain `noexcept` once migrated to `AuraResult`.
Guest exceptions intentionally use structured EH so Aura `Raise` can unwind
JIT frames without poisoning the Agent control loop's Result surface.
