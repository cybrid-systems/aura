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
it is better, and otherwise rolls back with `ast:restore`. The terminal shows
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
         (snap (ast:snapshot (format "step-~a" i)))
         (ok (mutate:rebind "guess" src "evolve")))
    (eval-current)
    (if (and (eq? ok #t) (keep? live-value cand))
        (display (format "~a ~a~a   live ~a\n" i (pad "KEEP" 10) (pad src 19) (live)))
        (begin
          (ast:restore snap)
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

(orch:run-workflow
 (vector
  (stage (lambda () (propose 0 out)))
  (stage (lambda () (check 0 out)))
  (stage (lambda () (propose 1 out)))
  (stage (lambda () (check 1 out)))))

;; Stages may interleave their sends; collect them and print in order.
(define rows
  (let loop ((acc '()))
    (let ((m (channel:try-recv out)))
      (if (equal? m "") (reverse acc) (loop (cons m acc))))))

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
(mutate:rebind "double" (cadr kept) "keep")
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
