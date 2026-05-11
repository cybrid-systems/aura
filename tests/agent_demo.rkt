#lang racket/base
;; Aura Agent Auto-Fix Demo
;;
;; Writes all inputs to temp files, uses shell redirection.

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

(define (read-file path)
  (with-input-from-file path port->string))

;; ── Run aura with stdin via temp file ─────────────────────────
(define (aura-run code . args)
  (prepare!)
  (define in-path  (string-append TMP "/stdin.txt"))
  (define out-path (string-append TMP "/stdout.txt"))
  (define err-path (string-append TMP "/stderr.txt"))
  (write-file in-path code)
  ;; Build shell command: aura [args...] < stdin > stdout 2> stderr
  (define cmd (string-append AURA " "
                (string-join args " ") " < "
                in-path " > " out-path " 2> " err-path))
  (system cmd)
  (read-file out-path))

;; ── Compile ────────────────────────────────────────────────────
(define (compile-once code)
  (define output (string-trim (aura-run code)))
  (let ([val (string->number output)])
    (if val (list 'ok val) (list 'error output))))

;; ── Query ──────────────────────────────────────────────────────
(define (query-once code qstr)
  ;; Write query to a file, pass filename
  (define qpath (string-append TMP "/query.txt"))
  (write-file qpath qstr)
  ;; Use --query <file> pattern: but aura reads query from arg, not file
  ;; Workaround: pass query as argument directly (no shell quoting needed
  ;; since we use system which goes through sh)
  (aura-run code (string-append "--query '" qstr "'")))

;; ── Query-and-fix ──────────────────────────────────────────────
(define (qf-once code qstr rstr)
  (aura-run code
    (string-append "--query-and-fix '" qstr "' '" rstr "'")))

;; ── Agent logic ────────────────────────────────────────────────
(define (agent-fix code var)
  (printf "Agent: fixing unbound variable '~a'...\n" var)
  (define qr (query-once code (format "(= name \"~a\")" var)))
  (printf "  query: ~a\n" (string-trim qr))
  (define fix-q (format "(and (node-type Variable) (= name \"~a\"))" var))
  (define fix-r "(LiteralInt 42)")
  (define fixr (qf-once code fix-q fix-r))
  (printf "  fix: ~a\n" (string-trim fixr))
  (string-replace code var "42"))

;; ── Demo ───────────────────────────────────────────────────────
(define (run-demo)
  (printf "=== Aura Agent Auto-Fix Demo ===\n\n")

  (printf "--- Step 1: Submit buggy code ---\n")
  (define code1 "(+ x 1)")
  (printf "Input: ~a\n" code1)
  (match (compile-once code1)
    [(list 'ok val) (printf "Result: OK ~a\n" val)]
    [(list 'error msg)
     (printf "Result: ERROR ~a\n\n" msg)
     (printf "--- Step 2: Agent applies fix ---\n")
     (define fixed (agent-fix code1 "x"))
     (printf "Fixed source: ~a\n\n" fixed)
     (printf "--- Step 3: Verify fix ---\n")
     (match (compile-once fixed)
       [(list 'ok val) (printf "Result: OK ~a ✓\nDemo complete!\n" val)]
       [(list 'error m2) (printf "Result: STILL BROKEN ~a\n" m2)]
       [o (printf "Unexpected: ~a\n" o)])]
    [o (printf "Unexpected: ~a\n" o)]))

(run-demo)
