#!/usr/bin/env python3
"""Issue #3870 — production fold purity is declared, not inferred.

The #3870 concept tighten makes ProductionPipelinePass require PureWrapPass
(const-run PureAnalysisPass discipline or an explicit kPureWrap author
annotation). The annotation itself stays author trust — C++ concepts cannot
type-check the run() body — so this linter is the soft audit arm:

  AC1  concept-declared:  the ProductionPipelinePass concept definition in
      src/core/concept_constraints.ixx requires PureWrapPass<P>.
  AC2  no-lock-thread:    no kPureWrap-marked class/struct body in the
      production pass surfaces uses lock / mutex / thread primitives.
  AC3  no-heap-fs:        no kPureWrap-marked body uses C-heap alloc
      (malloc/calloc/realloc) or filesystem / process escape (fopen /
      system / popen).
  AC4  stub-pins:         the DirtyAware-no-marker negative stub and the
      marked positive stub static_asserts exist in concept_constraints.ixx.

Exit 0 only when all ACs pass. `--self-test` runs the token scanner over an
inline impure fixture and exits 0 only when the fixture is detected.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONCEPTS = ROOT / "src" / "core" / "concept_constraints.ixx"
SCAN_FILES = [
    CONCEPTS,
    ROOT / "src" / "compiler" / "optimization_passes.ixx",
    ROOT / "src" / "compiler" / "pass_impls.ixx",
    ROOT / "src" / "compiler" / "service.ixx",
]
LOCK_TOKENS = [
    "std::mutex",
    "std::lock_guard",
    "std::unique_lock",
    "std::shared_lock",
    "std::scoped_lock",
    "std::shared_mutex",
    "std::thread",
    "std::jthread",
]
ESCAPE_TOKENS = [
    "malloc(",
    "calloc(",
    "realloc(",
    "fopen(",
    "std::system",
    "popen(",
]
DECL_RE = re.compile(r"^(?:export )?(?:class|struct)\s+(\w+)", re.MULTILINE)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def class_blocks(text: str):
    """Yield (name, block_text) for each top-level class/struct region.

    DECL_RE anchors at line start, so nested (indented) type declarations do
    not split the enclosing block.
    """
    decls = [(m.start(), m.group(1)) for m in DECL_RE.finditer(text)]
    decls.append((len(text), "<eof>"))
    for (start, name), (end, _) in zip(decls, decls[1:], strict=False):
        yield name, text[start:end]


def purity_bodies():
    for path in SCAN_FILES:
        text = path.read_text()
        for name, block in class_blocks(text):
            if "kPureWrap = true" in block:
                yield path, name, LINE_COMMENT_RE.sub("", block)


def scan(tokens, label):
    hits = []
    for path, name, code in purity_bodies():
        for tok in tokens:
            if tok in code:
                hits.append(f"{path.name}:{name} contains {tok!r}")
    print(f"AC({label}): " + ("FAIL" if hits else "PASS"))
    for hit in hits:
        print(f"  {hit}")
    return not hits


def ac1_concept_declared():
    text = CONCEPTS.read_text()
    match = re.search(r"concept\s+ProductionPipelinePass\s*=[^;]+;", text)
    ok = bool(match) and "PureWrapPass<P>" in match.group(0)
    print("AC(concept-declared): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_stub_pins():
    text = CONCEPTS.read_text()
    ok = (
        "DirtyAwareNoPurityMarkerStub" in text
        and "!ProductionPipelinePass<pass_purity_detail::DirtyAwareNoPurityMarkerStub>" in text
        and "ProductionPipelinePass<pass_purity_detail::MarkedPureWrapStub>" in text
    )
    print("AC(stub-pins): " + ("PASS" if ok else "FAIL"))
    return ok


def main() -> int:
    if "--self-test" in sys.argv:
        fixture = (
            "struct FakeWrap {\n"
            "    void run(aura::ir::IRModule&) {}\n"
            "    bool has_error() const { return false; }\n"
            '    std::string_view name() const { return "fake"; }\n'
            "    bool uses_soa_view() const { return true; }\n"
            "    std::mutex m_;\n"
            "    static constexpr bool kPureWrap = true;\n"
            "};\n"
        )
        code = LINE_COMMENT_RE.sub("", fixture)
        detected = [tok for tok in LOCK_TOKENS if tok in code]
        print("self-test: " + ("PASS — impure fixture detected" if detected else "FAIL"))
        return 0 if detected else 1
    results = [
        ac1_concept_declared(),
        scan(LOCK_TOKENS, "no-lock-thread-in-kPureWrap"),
        scan(ESCAPE_TOKENS, "no-heap-fs-in-kPureWrap"),
        ac4_stub_pins(),
    ]
    print(f"check_pass_purity_effect_3870: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
