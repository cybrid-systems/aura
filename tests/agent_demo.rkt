#lang racket/base
;; Aura Agent Auto-Fix Demo (v3 — E2E ABF pipeline via #lang aura)
;;
;; Pipeline:  #lang aura file → racket -l aura -- --abf → ./build/aura --abf

(require racket/port racket/string racket/match racket/format racket/system)

(define AURA (string-append (getenv "HOME") "/code/aura/build/aura"))
(define TMP "/tmp/aura-demo")

(define (prepare!)
  (with-handlers ([(λ (e) (directory-exists? TMP)) (λ (e) (void))])
    (make-directory TMP)))

(define (write-file path content)
  (call-with-output-file path
    (λ (p) (display content p) (newline p))
    #:exists 'replace))

;; ── E2E ABF pipeline ──────────────────────────────────────────
(define (aura-eval code)
  (prepare!)
  (define src-path (string-append TMP "/source.aura"))
  (write-file src-path code)
  (define cmd (string-append "cat " src-path " | "
                             "racket -l aura -- --abf | "
                             AURA " --abf 2>/dev/null"))
  (string-trim (with-output-to-string (λ () (system cmd)))))

;; ── --serve: submit code, get JSON error response ─────────────
(define (aura-serve code)
  (prepare!)
  (define in-path  (string-append TMP "/serve-stdin.txt"))
  (define out-path (string-append TMP "/serve-stdout.txt"))
  (write-file in-path code)
  (define cmd (string-append "cat " in-path " | "
                             AURA " --serve > " out-path " 2>/dev/null"))
  (system cmd)
  (string-trim (with-output-to-string
                 (λ () (call-with-input-file out-path port->string)))))

;; ── Demo ───────────────────────────────────────────────────────
(define (run-demo)
  (printf "=== Aura E2E ABF Pipeline Demo (v3) ===\n\n")

  (printf "--- Step 1: Basic eval via ABF pipeline ---\n")
  (define demo-exprs
    '("#lang aura\n(+ 1 2)"
      "#lang aura\n(let ((x 10)) x)"
      "#lang aura\n((lambda (x) (* x 2)) 5)"
      "#lang aura\n(let ((x 1) (y 2) (z 3)) (+ x (+ y z)))"
      "#lang aura\n(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))"))
  (for ([expr demo-exprs])
    (printf "  ~a → ~a\n" (cadr (string-split expr "\n")) (aura-eval expr)))

  (printf "\n--- Step 2: Auto-fix via --serve ---\n")
  (printf "  Input: (+ x 1)\n")
  (define serve-out (aura-serve "(+ x 1)"))
  (printf "  Output:\n")
  (for ([line (in-list (string-split serve-out "\n"))]
        #:when (> (string-length (string-trim line)) 0))
    (printf "    ~a\n" line))

  (printf "\n--- Step 3: E2E file -> ABF -> C++ eval ---\n")
  (printf "  demo.aura:\n")
  (printf "    #lang aura\n")
  (printf "    (letrec ((fact ...)) (fact 10))\n")
  (printf "  Result: ~a\n" (aura-eval "#lang aura
(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))))
  (fact 10))"))

  (printf "\n=== E2E pipeline demo complete! ===\n"))

(run-demo)
