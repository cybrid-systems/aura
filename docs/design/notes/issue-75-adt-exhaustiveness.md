# Design: Issue #75 — ADT match exhaustiveness: scope-aware + bare-identifier patterns

## Current state

ADT exhaustiveness checking has **two** implementations (one in the type-checker synth path,
one in the tree-walker eval path), and both have the same bugs:

### Bug A: bare-identifier patterns are ignored

`src/parser/parser_impl.cpp:1192-1210` (in `parse_match`):

```cpp
aura::ast::MatchClauseInfo minfo;
for (auto& c : clauses) {
    auto pv = flat_.get(c.pattern);
    if (pv.tag == NodeTag::Variable) {
        auto pname = pool_.resolve(pv.sym_id);
        if (pname == "_" || (pname.size() > 1 && pname[0] == '_')) {
            minfo.has_wildcard = true;
        }
    } else if (pv.tag == NodeTag::Call && !pv.children.empty()) {
        // Constructor pattern: (Ctor args...) -> callee is constructor name
        auto callee_id = pv.child(0);
        auto callee_v = flat_.get(callee_id);
        if (callee_v.tag == NodeTag::Variable)
            minfo.used_constructors.push_back(callee_v.sym_id);
    }
}
```

Only **Call** patterns (e.g. `(Cons h t)`, `(None)`, `(Red)`) are added to
`used_constructors`. A **bare-identifier** pattern (e.g. `Nil` in `(match xs ((Cons h t) …) (Nil 0))`)
is parsed by `parse_val` → `add_variable(Nil)` → `NodeTag::Variable`, so the
`Variable` branch is taken — and since `Nil` is not `_` and doesn't start with `_`,
it falls through silently. The pattern becomes a let binding (`let ((Nil tmp)) 0`)
but is **never** added to `used_constructors`.

Concrete repro (from `tests/suite/typesystem.aura` test 7):

```aura
(define-type (List a) (Nil) (Cons a (List a)))
(let ((xs (Cons 1 (Cons 2 (Cons 3 Nil)))))
  (match xs
    ((Cons h t) (begin (display h) (display (+ h 1))))
    (Nil 0)))
```

Expected: no warning (Cons + Nil both covered).
Actual: `match warning: unhandled constructor 'Nil' in List`.
Result: `12` (output is still correct, but the warning is wrong).

### Bug B: iterates over all ADTs, picks the first

`src/compiler/type_checker_impl.cpp:1993-2066` (in `synthesize_flat_let`) and the
parallel block in `src/compiler/evaluator_impl.cpp:14605-14640` (in eval `let`):

```cpp
for (std::size_t i = 0; i < reg_.size(); ++i) {
    auto tid = TypeId{static_cast<std::uint32_t>(i), 1};
    auto* ctors = reg_.get_adt_constructors(tid);
    if (!ctors) continue;
    ...
    if (!missing.empty()) { ... diag_.report(...); }
    break; // Only process the first ADT found
}
```

The loop iterates **every type in the registry** and breaks on the **first ADT
it finds**. It does not look at the actual type of the subject expression. If the
user defines multiple ADTs, the check is fundamentally about a different type
than the one being matched on. The check is only correct by accident when the
ADT being matched is the first one registered.

### Bug C: "recursive" function never recurses

The helper is named `check_adt_recursive` with `this const auto& self` and a
`depth` parameter, but the body just iterates `ctor_names` and pushes missing
entries — it never calls `self` on nested ADTs. For a recursive ADT like
`(List a) → Nil | Cons a (List a)`, if the user writes
`(match xs ((Cons h (Cons h2 Nil)) _) (Nil 0))` (a single pattern that
recursively destructures), the check would not notice. (We don't fix recursive
nesting in this issue — see §3 below.)

### Bug D: duplicate implementations

The same logic lives in two places, which drift. `type_checker_impl.cpp` runs in
synth mode (compile-time, when the value is type-known); `evaluator_impl.cpp`
runs in the tree-walker eval path (runtime, no static type info). The latter
guesses the ADT by looking for the **first used constructor** in the registry.

## The fix

### Phase 1: Parser — collect bare-identifier patterns as candidate constructors

Augment `MatchClauseInfo` with a new field `candidate_constructors` (SymId vector).
A **bare-identifier pattern** is ambiguous: it could be a constructor or a
variable binding. We collect it as a *candidate*; the type-checker resolves
ambiguity by checking against the actual type.

`src/core/ast.ixx`:

```cpp
export struct MatchClauseInfo {
    std::vector<SymId> used_constructors;     // (Ctor ...) patterns — definite
    std::vector<SymId> candidate_constructors; // bare-id patterns — may be ctor
    bool has_wildcard = false;
};
```

`src/parser/parser_impl.cpp:1192`:

```cpp
for (auto& c : clauses) {
    auto pv = flat_.get(c.pattern);
    if (pv.tag == NodeTag::Variable) {
        auto pname = pool_.resolve(pv.sym_id);
        if (pname == "_" || (pname.size() > 1 && pname[0] == '_')) {
            minfo.has_wildcard = true;
        } else {
            // Bare identifier: could be a constructor or a variable binding.
            // Record as candidate; type checker resolves.
            minfo.candidate_constructors.push_back(pv.sym_id);
        }
    } else if (pv.tag == NodeTag::Call && !pv.children.empty()) {
        auto callee_id = pv.child(0);
        auto callee_v = flat_.get(callee_id);
        if (callee_v.tag == NodeTag::Variable)
            minfo.used_constructors.push_back(callee_v.sym_id);
    }
}
```

### Phase 2: Type checker — use the actual subject type, resolve candidates

`src/compiler/type_checker_impl.cpp:1993-2066` (in `synthesize_flat_let`):

We already have `val_type` — the inferred type of the value bound to
`__match_tmp`. Look up its ADT constructors (if any) and check coverage.

```cpp
if (let_name == "__match_tmp" && !v.children.empty()) {
    auto* scan_minfo = flat.get_match_info(node_id);
    if (!scan_minfo || scan_minfo->has_wildcard)
        return; // wildcard = exhaustive

    // Look up the actual ADT type of the subject value
    auto val_norm = cs_.normalize(val_type);
    auto* adt_ctors = reg_.get_adt_constructors(val_norm);
    if (!adt_ctors) {
        // Subject is not an ADT (e.g. Int, String, Bool, or unknown).
        // Don't try to enforce exhaustiveness on non-ADT.
        return;
    }

    // Build the set of *effective* used constructors
    std::vector<std::string> used_eff;
    auto mark_used = [&](SymId sid) {
        used_eff.push_back(std::string(pool.resolve(sid)));
    };
    for (auto sid : scan_minfo->used_constructors) mark_used(sid);
    for (auto sid : scan_minfo->candidate_constructors) {
        auto cname = std::string(pool.resolve(sid));
        // Only count as used if it's a known ctor of this ADT
        if (std::find(adt_ctors->begin(), adt_ctors->end(), cname) != adt_ctors->end()) {
            used_eff.push_back(cname);
        }
    }

    // Find missing
    std::vector<std::string> missing;
    for (auto& cname : *adt_ctors) {
        if (std::find(used_eff.begin(), used_eff.end(), cname) == used_eff.end()) {
            missing.push_back(cname);
        }
    }

    if (!missing.empty()) {
        auto type_name = std::string(reg_.name_of(val_norm));
        std::string msg = "match: ";
        if (missing.size() == 1) {
            msg += "missing constructor '" + missing[0] + "'";
        } else {
            msg += "missing constructors: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) msg += ", ";
                msg += "'" + missing[i] + "'";
            }
        }
        msg += " in " + type_name;
        if (missing.size() == 1) {
            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                .with_suggestion("add clause for '" + missing[0] + "' pattern"));
        } else {
            std::string suggest = "add clauses for ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) suggest += ", ";
                suggest += "'" + missing[i] + "'";
            }
            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                .with_suggestion(suggest));
        }
    }
    return;
}
```

The key change: **only check the actual subject type** (`val_norm`), no more
iterating the entire registry. Candidates that aren't real constructors of
that ADT are silently ignored (so a `(let ((x 5)) (match x (a a) (_ 0)))`
where `a` is just a variable binding doesn't false-positive).

### Phase 3: Tree-walker eval path — same fix

`src/compiler/evaluator_impl.cpp:14605-14640` has the parallel implementation.
The fix is identical, but we have **no static type** at runtime. We resolve
candidates by finding the ADT whose first used-ctor matches, then look up
candidates against that ADT's constructor list.

```cpp
if (!rec && type_registry_ && f->has_match_info(current_id)) {
    auto* minfo = f->get_match_info(current_id);
    if (!minfo || minfo->has_wildcard) return;
    if (minfo->used_constructors.empty() && minfo->candidate_constructors.empty())
        return;

    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    // Pick ADT: first used_ctor that's a known ctor of some ADT
    const std::vector<std::string>* target_ctors = nullptr;
    std::string target_name;
    for (auto sid : minfo->used_constructors) {
        auto cname = std::string(p->resolve(sid));
        for (std::size_t ti = 0; ti < treg.size(); ++ti) {
            auto tid = aura::core::TypeId{static_cast<std::uint32_t>(ti), 1};
            auto* ctors = treg.get_adt_constructors(tid);
            if (!ctors) continue;
            if (std::find(ctors->begin(), ctors->end(), cname) != ctors->end()) {
                target_ctors = ctors;
                target_name = treg.name_of(tid);
                break;
            }
        }
        if (target_ctors) break;
    }
    if (!target_ctors) return;

    // Build effective used set
    std::vector<std::string> used_eff;
    auto mark = [&](aura::ast::SymId sid) {
        used_eff.push_back(std::string(p->resolve(sid)));
    };
    for (auto sid : minfo->used_constructors) mark(sid);
    for (auto sid : minfo->candidate_constructors) {
        auto cname = std::string(p->resolve(sid));
        if (std::find(target_ctors->begin(), target_ctors->end(), cname) != target_ctors->end()) {
            used_eff.push_back(cname);
        }
    }

    for (auto& expected : *target_ctors) {
        if (std::find(used_eff.begin(), used_eff.end(), expected) == used_eff.end()) {
            std::println(std::cerr,
                "match warning: unhandled constructor '{}' in {}",
                expected, target_name);
        }
    }
    return;
}
```

### Phase 4: clean up service.ixx collect_match_info

`src/compiler/service.ixx:collect_match_info` re-derives match info on the
**expanded** flat (post-macro-expansion). It currently only extracts from the
outer if's then-branch by looking at the first child of a let. This misses
wildcards at the top level and may double-count. For this issue, we leave it
as a no-op (the parser's match_info is what the type-checker and eval use);
a follow-up can rewire or remove it.

## Out of scope (deferred)

- **Recursive ADT structural exhaustiveness**: e.g. `match xs ((Cons h Nil) _)`
  is exhaustive for `List` because every `Cons` chain ends in `Nil`. Implementing
  this needs a proper algebraic check; punt to a follow-up.
- **GADT pattern refinement**: type-changing patterns in dependent-style matches.
- **Pattern guards** (`(match x ((Some n) when (> n 0)) ...)`).

## Test plan

Add tests covering:

1. `(match xs ((Cons h t) …) (Nil 0))` — bare-identifier pattern, no warning.
2. `(match xs ((Cons h t) …))` — Nil missing, warning.
3. `(match xs (Nil 0))` — Cons missing, warning.
4. `(match xs ((Cons h t) …) (_ 0))` — wildcard, no warning.
5. `(match n (1 'one) (_ 'other))` — non-ADT subject, no warning.
6. `(match c ((Red) 1) ((Green) 2) ((Blue) 3))` — multi-ctor covered, no warning.
7. `tests/suite/typesystem.aura` test 7 must now produce no warning.

## Affected files

- `src/core/ast.ixx` — add `candidate_constructors` field.
- `src/parser/parser_impl.cpp` — collect bare-identifier candidates.
- `src/compiler/type_checker_impl.cpp` — use `val_type` instead of iterating registry.
- `src/compiler/evaluator_impl.cpp` — same fix in tree-walker path.
- `tests/suite/typesystem.aura` — verify existing tests pass.
- `tests/test_regression.py` — add a few new cases.
- `tests/test_ir.cpp` — add a `match` exhaustiveness section to type-checker tests.
