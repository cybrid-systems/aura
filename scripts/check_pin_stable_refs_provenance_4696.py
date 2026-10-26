#!/usr/bin/env python3
"""Issue #4696: the production pin table must not launder a raw NodeId.

Hole: collect_pin_handles() built a zero-provenance StableNodeRef from a
bare int (id only, gen == 0, wrap_epoch == 0). Both pin-granting prims
then promoted that handle with make_stamped_ref -- which stamps the CURRENT
generation / wrap / COW of whatever occupies the slot NOW -- and validated
the freshly stamped ref against itself. A deleted node's id therefore
became a valid, pinned cross-round handle for a DIFFERENT node that had
since taken the slot. The sibling faces already refuse a bare int on the
production face (query:as-stable-ref #3425, resolve_mutate_node_arg
#3395) and already refuse to occupancy-remake an expired-gen packed ref
(#3661), so the pin table was the asymmetric survivor.

Fix shape (reuse the existing gates; no new primitive, no new query key):
  - a bare NodeId argument is refused with stale-ref on the production face
    BEFORE any pin is registered (same shape as query:as-stable-ref
    #3425), so a mixed call cannot leave a partial pin behind;
  - a packed v2 handle is validated, not occupancy-remade, on the
    production face (#3661 parity with resolve_mutate_node_arg): an
    expired gen must fail closed instead of being remade onto the slot's
    current occupant;
  - the Soft face keeps the historical promote-and-refresh body (#2186
    legacy), and unpin-stable-refs keeps accepting a raw NodeId (it
    releases a pin, it never mints a handle).

Contract (one row per AC):
  AC1  pin-stable-refs refuses a raw NodeId with stale-ref on the
       production face, before any pin is registered
  AC2  with-pinned refuses a raw NodeId with stale-ref on the production
       face, before the body runs
  AC3  both prims validate a packed handle without occupancy-remake on
       the production face (auto_refresh = !production_face)
  AC4  the Soft face keeps the legacy promote-and-refresh path
       (make_stamped_ref for a zero-provenance handle, auto_refresh true)
  AC5  the collector flags a bare int (saw_raw_id) instead of each prim
       re-deriving is_int on the argument
  AC6  the production gate is production_defaults_active() -- the same
       predicate the sibling faces use -- not the broader hard-face gate
  AC7  runtime evidence: the #4696 ACs live in
       tests/serve/test_stable_ref_provenance_fiber_cow.cpp and are
       dispatched from main(); no docs/design/4696-* and no
       tests/core/test_issue_4696.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUT = "src/compiler/evaluator_primitives_mutate.cpp"
TEST = "tests/serve/test_stable_ref_provenance_fiber_cow.cpp"
NL = chr(10)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _sq(s: str) -> str:
    """Whitespace-free form, so a check survives clang-format reflow."""
    return "".join(s.split())


def _prim_body(text: str, name: str) -> str:
    """Body of one add("<name>", ...) prim registration, up to the next add()."""
    anchor = 'add("' + name + '"'
    i = text.find(anchor)
    if i < 0:
        return ""
    j = text.find(NL + '    add("', i)
    if j < 0:
        j = len(text)
    return text[i:j]


def _lambda_span(text: str, decl: str) -> str:
    """A lambda declared with decl, from its declaration through its body.

    The out-parameter of a lambda lives in the declaration, which sits
    BEFORE the body brace, so a body-only slice would miss it.
    """
    i = text.find(decl)
    if i < 0:
        return ""
    j = text.find("{", i)
    if j < 0:
        return ""
    depth = 0
    for k in range(j, len(text)):
        c = text[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[i : k + 1]
    return text[i:]


def _lambda_body(text: str, decl: str) -> str:
    """Body of a lambda declared with decl, up to that lambda's closing brace."""
    span = _lambda_span(text, decl)
    j = span.find("{")
    return span[j:] if j >= 0 else ""


def main() -> int:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if _sq(needle) not in _sq(hay):
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if _sq(needle) in _sq(hay):
            fails.append(f"{label}: forbidden {needle!r}")

    mut = _read(MUT)
    if not mut:
        print(f"check_pin_stable_refs_provenance_4696: {MUT} missing")
        return 1

    pin = _prim_body(mut, "pin-stable-refs")
    wp = _prim_body(mut, "with-pinned")
    unpin = _prim_body(mut, "unpin-stable-refs")
    collector = _lambda_span(mut, "auto collect_pin_handles")

    if not pin:
        fails.append("AC1: no pin-stable-refs registration found")
    if not wp:
        fails.append("AC2: no with-pinned registration found")
    if not collector:
        fails.append("AC5: no collect_pin_handles lambda found")

    # AC1 pin-stable-refs refuses a raw NodeId on the production face.
    must("saw_raw_id", "AC1", pin)
    must("production_defaults_active()", "AC1", pin)
    must('mev("stale-ref"', "AC1", pin)
    must("raw node-id rejected under production", "AC1", pin)
    must("Issue #4696", "AC1", pin)
    must("bump_raw_nodeid_usage_in_primitives_count()", "AC1", pin)
    if (
        _sq('mev("stale-ref"') in _sq(pin)
        and "pin_stable_ref_for_cow_boundary" in pin
        and _sq(pin).find('mev("stale-ref"') > _sq(pin).find("pin_stable_ref_for_cow_boundary")
    ):
        fails.append("AC1: the raw-NodeId refuse must precede pin registration")

    # AC2 with-pinned refuses a raw NodeId on the production face.
    must("saw_raw_id", "AC2", wp)
    must("production_defaults_active()", "AC2", wp)
    must('mev("stale-ref"', "AC2", wp)
    must("raw node-id rejected under production", "AC2", wp)
    must("Issue #4696", "AC2", wp)
    must("bump_raw_nodeid_usage_in_primitives_count()", "AC2", wp)
    if (
        _sq('mev("stale-ref"') in _sq(wp)
        and "eval_flat" in wp
        and _sq(wp).find('mev("stale-ref"') > _sq(wp).find("eval_flat")
    ):
        fails.append("AC2: the raw-NodeId refuse must precede the body evaluation")

    # AC3 production validates a packed handle without occupancy-remake.
    for label, body in (("AC3", pin), ("AC3", wp)):
        must("auto_refresh=*/!production_face", label, body)
        must_not("auto_refresh=*/true", label, body)

    # AC4 the Soft face keeps the legacy promote-and-refresh path.
    for label, body in (("AC4", pin), ("AC4", wp)):
        must("make_stamped_ref(ref.id)", label, body)
    if unpin:
        # unpin releases a pin, it never mints a handle: a raw NodeId stays
        # accepted there.
        must_not('mev("stale-ref"', "AC4", unpin)

    # AC5 the collector flags a bare int.
    must("bool& saw_raw_id", "AC5", collector)
    must("saw_raw_id = true;", "AC5", collector)
    must("is_int(v)", "AC5", collector)

    # AC6 the production gate matches the sibling faces.
    must_not("production_hard_face_active()", "AC6", pin)
    must_not("production_hard_face_active()", "AC6", wp)

    # AC7 runtime evidence.
    test = _read(TEST)
    if not test:
        print(f"check_pin_stable_refs_provenance_4696: {TEST} missing")
        return 1
    for fn in (
        "ac4696_1_prod_raw_id_pin_rejected",
        "ac4696_2_prod_with_pinned_raw_id_rejected",
        "ac4696_3_prod_packed_ref_still_pins",
        "ac4696_4_soft_raw_id_still_promotes",
        "ac4696_5_prod_expired_packed_ref_not_remade",
        "ac4696_6_no_docs_linter_wired",
    ):
        must(f"static void {fn}(", "AC7", test)
        must(f"{fn}();", "AC7", test)
    must("Issue #4696", "AC7", test)
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for md in design.glob("4696-*"):
            fails.append(f"AC7: forbidden docs/design/{md.name}")
    if (ROOT / "tests" / "core" / "test_issue_4696.cpp").exists():
        fails.append("AC7: forbidden tests/core/test_issue_4696.cpp")
    must("check_pin_stable_refs_provenance_4696", "AC7", _read("build.py"))
    must(
        "check_pin_stable_refs_provenance_4696.py",
        "AC7",
        _read("scripts/coverage/root_check_allowlist.txt"),
    )

    if fails:
        print("Issue #4696 pin stable-ref provenance linter FAILED:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("Issue #4696 pin stable-ref provenance linter OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
