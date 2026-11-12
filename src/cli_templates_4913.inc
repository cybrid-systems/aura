// Generated from the `aura new` templates (#4913). Edit the templates, not this file.
struct TemplateFile4913 {
    std::string_view tpl;
    std::string_view path;
    std::string_view body;
};
inline constexpr TemplateFile4913 kTemplates4913[] = {
    {"app", ".gitignore", R"AURA(out/
)AURA"},
    {"app", "README.md", R"AURA(# {{name}}

    aura run     # runs main.aura with the settings in aura.toml
    aura test    # runs tests/*.aura and compares stdout with tests/*.golden
)AURA"},
    {"app", "aura.toml", R"AURA(name = "{{name}}"
entry = "main.aura"
preset = "dev"
out = "out"

# The aura release this project was made with; .github/workflows/ci.yml runs
# the ghcr.io/cybrid-systems/aura image with the same tag. require paths
# resolve from the project root, std/* ships with aura.
[dependencies]
aura = "{{channel}}"
)AURA"},
    {"app", ".github/workflows/ci.yml", R"AURA(name: ci
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/cybrid-systems/aura:{{channel}}
    steps:
      - uses: actions/checkout@v4
      - run: aura test
      - run: aura run
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: out
          path: out/
          if-no-files-found: ignore
)AURA"},
    {"app", "lib/greet.aura", R"AURA(;; lib/greet.aura — a module; every top-level define is visible to requirers; module paths are project-root relative.
(define (greet who) (string-append "hello, " who))
(define (sum-to n) (if (= n 0) 0 (+ n (sum-to (- n 1)))))
)AURA"},
    {"app", "main.aura", R"AURA(;; main.aura — entry point (aura run). require paths are relative to the project root.
(require "lib/greet")

(display (greet "world"))
(newline)
(display (format "sum 1..10 = ~a\n" (sum-to 10)))
)AURA"},
    {"app", "tests/greet_test.aura", R"AURA((require "lib/greet")
(display (greet "aura"))
(newline)
(display (sum-to 4))
(newline)
)AURA"},
    {"app", "tests/greet_test.golden", R"AURA(hello, aura
10
)AURA"},
    {"evolve", ".gitignore", R"AURA(out/
)AURA"},
    {"evolve", "README.md", R"AURA(# {{name}}

A program that evolves itself: each step proposes a new definition, rewrites
the live AST with `mutate:rebind`, keeps the change only if `gate.aura` says
it is better, and otherwise rolls back with `mutate :restore`. The terminal shows
each KEEP and ROLLBACK with the live source after the decision.

    aura run
    aura test
)AURA"},
    {"evolve", "aura.toml", R"AURA(name = "{{name}}"
entry = "main.aura"
preset = "dev"
out = "out"

# The aura release this project was made with; .github/workflows/ci.yml runs
# the ghcr.io/cybrid-systems/aura image with the same tag. require paths
# resolve from the project root, std/* ships with aura.
[dependencies]
aura = "{{channel}}"
)AURA"},
    {"evolve", ".github/workflows/ci.yml", R"AURA(name: ci
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/cybrid-systems/aura:{{channel}}
    steps:
      - uses: actions/checkout@v4
      - run: aura test
      - run: aura run
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: out
          path: out/
          if-no-files-found: ignore
)AURA"},
    {"evolve", "gate.aura", R"AURA(;; gate.aura — what counts as progress. Edit fitness / keep? to change it.
;; The demo asks for 42: a candidate must be closer to it than the live
;; definition, and it must land on a multiple of 7.
(define (fitness guess) (abs (- guess 42)))
(define (keep? live cand)
  (and (< (fitness cand) (fitness live)) (= 0 (modulo cand 7))))
)AURA"},
    {"evolve", "main.aura", R"AURA(;; main.aura — one live definition, four fixed candidates. Each step snapshots
;; the live AST, rebinds `guess`, evaluates, then keeps the change or restores
;; the snapshot. Open this file to see the whole loop; gate.aura holds the rule.
(require "gate")

(set-code "(define (guess) 0)")
(eval-current)

(define (live) (current-source :workspace :define-fn-sugar))

;; Pad a column so the record lines up like a table.
(define spaces "                    ")
(define (pad s n)
  (let ((k (- n (string-length s))))
    (if (<= k 0) s (string-append s (substring spaces 0 k)))))

(define (step i cand)
  (let* ((live-value (guess))
         (src (format "(define (guess) ~a)" cand))
         (snap (mutate :snapshot (format "step-~a" i)))
         (ok (mutate :rebind "guess" src "evolve")))
    (eval-current)
    (if (and (eq? ok #t) (keep? live-value cand))
        (display (format "~a ~a~a   live ~a\n" i (pad "KEEP" 10) (pad src 19) (live)))
        (begin
          (mutate :restore snap)
          (eval-current)
          (display (format "~a ~a~a   live ~a\n" i (pad "ROLLBACK" 10) (pad src 19) (live)))))))

(for-each (lambda (p) (step (car p) (cadr p)))
          (list (list 1 10) (list 2 7) (list 3 2) (list 4 42)))
(display (format "(guess) => ~a\n" (guess)))
)AURA"},
    {"evolve", "tests/gate_test.aura", R"AURA((require "gate")
(display (list (fitness 42) (keep? 0 7) (keep? 7 2) (keep? 7 42)))
(newline)
)AURA"},
    {"evolve", "tests/gate_test.golden", R"AURA((0 #t #f #t)
)AURA"},
    {"evolve-moo", ".gitignore", R"AURA(out/
)AURA"},
    {"evolve-moo", "README.md", R"AURA(# {{name}}

A multi-objective + quality-diversity search, ready to run with no LLM and no
network (the proposer is deterministic from the seed).

    aura run                       # 240 mock proposals, Pareto front + QD map
    aura evo report --html out/report
    aura test

`aura run` prints the evaluated count, the Pareto front size and the QD
coverage, and writes `out/pareto.svg` and `out/qd.svg`. The evo log records
every candidate's objectives and behaviour, so `aura evo report` adds the
Pareto scatter, the hypervolume curve, the QD heat map and the lineage tree.
Replace `lib/moo.aura`'s evaluator or the proposer in `main.aura` to make it
your own problem.
)AURA"},
    {"evolve-moo", "aura.toml", R"AURA(name = "{{name}}"
entry = "main.aura"
preset = "dev"
out = "out"

# The aura release this project was made with; .github/workflows/ci.yml runs
# the ghcr.io/cybrid-systems/aura image with the same tag. require paths
# resolve from the project root, std/* ships with aura.
[dependencies]
aura = "{{channel}}"
)AURA"},
    {"evolve-moo", ".github/workflows/ci.yml", R"AURA(name: ci
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/cybrid-systems/aura:{{channel}}
    steps:
      - uses: actions/checkout@v4
      - run: aura test
      - run: aura run
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: out
          path: out/
          if-no-files-found: ignore
)AURA"},
    {"evolve-moo", "lib/moo.aura", R"AURA(;; lib/moo.aura — the evaluator this example evolves against. Two objectives
;; that pull in opposite directions, plus a 2-D behaviour descriptor for QD.
(require "std/evo-core" all:)

(export moo:unit moo:evaluate)

;; Map the mock proposer's integer genome into [0, 1).
(define (moo:unit x) (/ (modulo x 1000) 1000.0))

(define (moo:evaluate c)
  (let* ((g (evo:candidate-genome c))
         (x (moo:unit (car g)))
         (y (moo:unit (car (cdr g))))
         (dx (- x 0.8))
         (dy (- y 0.2)))
    (hash "objectives" (hash "loss" (+ (* dx dx) (* dy dy))
                             "cost" (+ x (* 0.5 y)))
          "constraints" (hash "inside" 0.0)
          "behavior" (hash "x" x "y" y))))
)AURA"},
    {"evolve-moo", "main.aura", R"AURA(;; evolve-moo — multi-objective search plus quality diversity, with the mock
;; proposer (no LLM, no network). Each candidate is evaluated against two
;; conflicting objectives; every evaluated candidate is appended to the evo
;; log (objectives + behaviour), so the report can draw the Pareto scatter,
;; the HV curve, the QD heat map and the lineage.
;;
;;   aura run
;;   aura evo report --html out/report
;;
;; The search is a plain value (evo:search); see gate.aura for the objectives.
(require "lib/moo")
(require "std/evo-core" all:)
(require "std/pareto" all:)
(require "std/qd" all:)
(require "std/fs" all:)

;; Ask/tell loop, mock proposals, 240 evaluations, archive every measurement so
;; the QD map sees the whole run.
(define s (evo:search (hash "algo" "moo-mock" "seed" 7 "budget" 240 "pop" 8
                            "archive" 512
                            "objectives" (list (cons "loss" "min")
                                               (cons "cost" "min")))))
(evo:run! s moo:evaluate)
(define rep (evo:report s))
(define arch (hash-ref rep "archive"))

;; Objective-space archive: the non-dominated front over the two objectives.
(define a (pareto:archive (hash "dirs" (list "min" "min") "cap" 64)))
(for-each (lambda (m)
            (pareto:insert! a m (list (evo:objective m "loss")
                                      (evo:objective m "cost"))))
          arch)
(define front (pareto:front a))

;; Behaviour-space archive: MAP-Elites over (x, y), quality = 1/(1+loss).
(define grid (qd:map-elites (hash "descriptors" (list (list "x" 0.0 1.0 8)
                                                      (list "y" 0.0 1.0 8))
                                 "quality" "loss")))
(for-each (lambda (m)
            (let ((b (evo:candidate-behavior m)))
              (qd:insert! grid m (/ 1.0 (+ 1.0 (evo:objective m "loss"))) b)))
          arch)

(display (format "evaluated ~a  front ~a  qd coverage ~a  qd-score ~a\n"
                 (hash-ref rep "evaluated") (length front)
                 (qd:coverage grid) (qd:qd-score grid)))
(make-directory "out")
(file-write "out/pareto.svg" (pareto:report-svg front (hash "dirs" (list "min" "min"))))
(file-write "out/qd.svg" (qd:heatmap-svg grid (hash)))
(display "wrote out/pareto.svg and out/qd.svg\n")
(display "next: aura evo report --html out/report\n")
)AURA"},
    {"evolve-moo", "tests/moo_test.aura", R"AURA((require "lib/moo")
(require "std/evo-core" all:)

(display (list (moo:unit 1234) (moo:unit 500)))
(newline)
(define e (moo:evaluate (evo:candidate (list 250 750))))
(display (list (evo:objective e "loss") (evo:objective e "cost")))
(newline)
)AURA"},
    {"evolve-moo", "tests/moo_test.golden", R"AURA((0.234 0.5)
(0.6050000000000001 0.625)
)AURA"},
    {"agent", ".gitignore", R"AURA(out/
)AURA"},
    {"agent", "README.md", R"AURA(# {{name}}

Two roles in an orch workflow: `workflow.aura` holds the proposer and the
checker, `main.aura` wires them stage by stage and prints the dialogue. The
checker's rejected definition never reaches the live AST.

    aura run
    aura test
)AURA"},
    {"agent", "aura.toml", R"AURA(name = "{{name}}"
entry = "main.aura"
preset = "dev"
out = "out"

# The aura release this project was made with; .github/workflows/ci.yml runs
# the ghcr.io/cybrid-systems/aura image with the same tag. require paths
# resolve from the project root, std/* ships with aura.
[dependencies]
aura = "{{channel}}"
)AURA"},
    {"agent", ".github/workflows/ci.yml", R"AURA(name: ci
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/cybrid-systems/aura:{{channel}}
    steps:
      - uses: actions/checkout@v4
      - run: aura test
      - run: aura run
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: out
          path: out/
          if-no-files-found: ignore
)AURA"},
    {"agent", "main.aura", R"AURA(;; main.aura — run the two roles as workflow stages, collect their lines, then
;; install the definition the checker kept into the live AST.
(require "workflow")

(set-code "(define (double x) 0)")
(eval-current)

(define out (channel:create 16))
(define (stage thunk) (hash "tasks" (vector thunk)))

;; Stages run in order; each stage here has one task. (#5097 retired the
;; orch:run-workflow primitive, so drive the stage thunks directly.)
(for-each
 (lambda (s) ((vector-ref (hash-ref s "tasks") 0)))
 (list
  (stage (lambda () (propose 0 out)))
  (stage (lambda () (check 0 out)))
  (stage (lambda () (propose 1 out)))
  (stage (lambda () (check 1 out)))))

;; Stages may interleave their sends; collect them and print in order.
(define rows
  (let loop ((acc '()))
    (let ((m (channel:try-recv out)))
      (if m (loop (cons m acc)) (reverse acc)))))

(for-each
 (lambda (row)
   (if (equal? (caddr row) "propose")
       (display (format "propose  ~a\n" (cadr row)))
       (display (format "check    (double 21) => ~a\n" (list-ref row 3)))))
 rows)

;; The rejected definition never reaches the live AST; install the kept one.
(define kept
  (car (filter (lambda (row) (and (equal? (caddr row) "check")
                                  (string-index (list-ref row 3) "KEEP")))
               rows)))
(mutate :rebind "double" (cadr kept) "keep")
(eval-current)
(display (format "live     ~a\n" (current-source :workspace :define-fn-sugar)))
)AURA"},
    {"agent", "tests/workflow_test.aura", R"AURA((require "workflow")
(define out (channel:create 8))
(propose 0 out)
(check 0 out)
(define p (channel:try-recv out))
(define c (channel:try-recv out))
(display (list (cadr p) (caddr c) (list-ref c 3)))
(newline)
)AURA"},
    {"agent", "tests/workflow_test.golden", R"AURA(((define (double x) (+ x x)) check 42  KEEP)
)AURA"},
    {"agent", "workflow.aura", R"AURA(;; workflow.aura — the two roles in the dialogue.
;;
;; The proposer writes candidate definitions; the checker runs each one
;; against (double 21) and says whether it may stay.
(define (candidate i)
  (if (= i 0)
      (list "(define (double x) (+ x x))" (lambda (x) (+ x x)))
      (list "(define (double x) (+ x 1))" (lambda (x) (+ x 1)))))

(define (propose i out)
  (channel:send out (list i (car (candidate i)) "propose" "")))

(define (check i out)
  (let* ((src (car (candidate i)))
         (got ((cadr (candidate i)) 21)))
    (channel:send out (list i src "check"
                           (format "~a  ~a" got (if (= got 42) "KEEP" "REJECT"))))))
)AURA"},
};
