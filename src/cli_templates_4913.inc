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

A program that evolves itself: each step proposes new `params`, rewrites the
live AST with `mutate:rebind`, keeps the change only if `gate.aura` says it is
better, and otherwise rolls back with `ast:restore`.

    aura run     # writes out/report.html and out/audit.tsv
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
    {"evolve", "evolve.aura", R"AURA(;; evolve.aura — propose → ast:snapshot → mutate:rebind → gate → KEEP or ast:restore
(define seed 1)
(define (rand3!)
  (set! seed (modulo (+ (* seed 1103515245) 12345) 2147483648))
  (- (modulo (quotient seed 65536) 3) 1))
(define (propose p) (list (+ (car p) (rand3!)) (+ (cadr p) (rand3!))))
(define (params->src p) (format "(quote (~a ~a))" (car p) (cadr p)))

;; Returns a list of rows (step decision params fitness), newest first.
(define (evolve! steps)
  (let loop ((i 1) (rows '()))
    (if (> i steps)
        rows
        (let* ((cur (live-params))
               (cand (propose cur))
               (snap (ast:snapshot (format "step-~a" i)))
               (ok (mutate:rebind "params" (params->src cand) "evolve")))
          (eval-current)
          (if (and (eq? ok #t) (keep? cur cand))
              (loop (+ i 1) (cons (list i "KEEP" cand (fitness cand)) rows))
              (begin
                (ast:restore snap)
                (eval-current)
                (loop (+ i 1) (cons (list i "ROLLBACK" cand (fitness cand)) rows))))))))
)AURA"},
    {"evolve", "gate.aura", R"AURA(;; gate.aura — what counts as progress. Edit fitness / keep? to change it.
;; Target: params near (3 -2). Fitness is the negated squared distance.
(define (fitness p)
  (let ((a (car p)) (b (cadr p)))
    (- 0 (+ (* (- a 3) (- a 3)) (* (+ b 2) (+ b 2))))))
(define (keep? cur cand) (> (fitness cand) (fitness cur)))
)AURA"},
    {"evolve", "main.aura", R"AURA(;; main.aura — live-AST self-evolution with a gate and an HTML report (aura run).
(require "gate")
(require "evolve")
(require "report")

;; The evolving program lives in the live AST; mutate:rebind rewrites it.
(set-code "(define params (quote (0 0)))")
(eval-current)
(define (live-params) params)

(define rows (reverse (evolve! 40)))
(define keeps (length (filter (lambda (r) (equal? (cadr r) "KEEP")) rows)))
(write-report! "out" rows (live-params))
(display (format "evolved ~a steps: ~a KEEP, ~a ROLLBACK\n" (length rows) keeps (- (length rows) keeps)))
(display (format "params ~a fitness ~a\n" (live-params) (fitness (live-params))))
(display "report: out/report.html (audit: out/audit.tsv)\n")
)AURA"},
    {"evolve", "report.aura", R"AURA(;; report.aura — out/report.html and out/audit.tsv
(define (row->tsv r) (format "~a\t~a\t~a\t~a\n" (car r) (cadr r) (caddr r) (list-ref r 3)))
(define (row->html r)
  (format "<tr class=~a><td>~a</td><td>~a</td><td>~a</td><td>~a</td></tr>\n"
          (cadr r) (car r) (cadr r) (caddr r) (list-ref r 3)))
(define (join strs) (if (null? strs) "" (string-append (car strs) (join (cdr strs)))))
(define (write-report! dir rows final)
  (make-directory dir)
  (write-file (string-append dir "/audit.tsv")
              (string-append "step\tdecision\tparams\tfitness\n" (join (map row->tsv rows))))
  (write-file (string-append dir "/report.html")
              (string-append
               "<!doctype html><meta charset=utf-8><title>evolution report</title>"
               "<style>body{font-family:sans-serif}td{padding:2px 8px}"
               ".KEEP{background:#dfd}.ROLLBACK{color:#999}</style>"
               (format "<h1>evolution report</h1><p>final params ~a, fitness ~a</p>"
                       final (fitness final))
               "<table><tr><th>step</th><th>decision</th><th>params</th><th>fitness</th></tr>\n"
               (join (map row->html rows)) "</table>\n")))
)AURA"},
    {"evolve", "tests/gate_test.aura", R"AURA((require "gate")
(display (list (fitness (list 3 -2)) (keep? (list 0 0) (list 1 0)) (keep? (list 3 -2) (list 4 -2))))
(newline)
)AURA"},
    {"evolve", "tests/gate_test.golden", R"AURA((0 #t #f)
)AURA"},
    {"agent", ".gitignore", R"AURA(out/
)AURA"},
    {"agent", "README.md", R"AURA(# {{name}}

An orch workflow: `workflow.aura` defines the stages, `main.aura` runs them.

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
    {"agent", "main.aura", R"AURA(;; main.aura — an orch workflow (aura run). Stages run in order; the tasks
;; inside one stage run in parallel, each on its own region key (keys must not
;; overlap, otherwise the stage is refused as "missing-or-overlap-keys").
(require "workflow")

(define r
  (orch:run-workflow
   (vector (hash "tasks" (vector (lambda () (square 1)) (lambda () (square 2)) (lambda () (square 3)))
                 "region-keys" (vector 1 2 3))
           (hash "tasks" (vector (lambda () (summarize (list 1 4 9))))))))
(display (format "stages ok ~a, failed ~a\n" (hash-ref r "stages-ok") (hash-ref r "stages-failed")))
)AURA"},
    {"agent", "tests/workflow_test.aura", R"AURA((require "workflow")
(define r (orch:run-workflow (vector (hash "tasks" (vector (lambda () (square 2)) (lambda () (square 3)))
                                           "region-keys" (vector 1 2)))))
(display (list (hash-ref r "stages-ok") (hash-ref r "stages-failed") (summarize (list 1 2))))
(newline)
)AURA"},
    {"agent", "tests/workflow_test.golden", R"AURA((1 0 2 results)
)AURA"},
    {"agent", "workflow.aura", R"AURA(;; workflow.aura — the work each task does.
(define (square n) (* n n))
(define (summarize xs) (format "~a results" (length xs)))
)AURA"},
};
