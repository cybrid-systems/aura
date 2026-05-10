#lang racket/base
;; Aura Phase 0 — minimal Lisp core evaluator

(provide eval-expr make-env extend-env lookup-env)

;; ─── Mutable cell for letrec ───
(struct cell (value) #:mutable
  #:transparent)

;; ─── Environment ───

(define (make-env)
  (list (cons '+ +) (cons '- -) (cons '* *) (cons '/ /)
        (cons '= =) (cons '< <) (cons '> >) (cons '<= <=) (cons '>= >=)
        (cons 'eq? eq?) (cons 'equal? equal?)
        (cons 'not not) (cons 'cons cons) (cons 'car car) (cons 'cdr cdr)
        (cons 'null? null?) (cons 'pair? pair?)
        (cons 'display display) (cons 'displayln displayln)
        (cons 'number? number?) (cons 'symbol? symbol?)
        (cons 'string? string?) (cons 'boolean? boolean?)))

(define (extend-env env name val)
  (cons (cons name val) env))

;; lookup returns value, auto-unwrapping cells
(define (lookup-env env name)
  (let ([v (raw-lookup env name)])
    (if (cell? v) (cell-value v) v)))

;; raw lookup returns the stored value (cell or not)
(define (raw-lookup env name)
  (cond [(null? env) (error 'lookup-env "unbound variable: ~a" name)]
        [(equal? (caar env) name) (cdar env)]
        [else (raw-lookup (cdr env) name)]))

;; ─── Core evaluator ───

(define (eval-expr expr env)
  (cond
    [(number? expr) expr] [(string? expr) expr]
    [(boolean? expr) expr] [(null? expr) expr]
    [(symbol? expr) (lookup-env env expr)]
    [(pair? expr)
     (case (car expr)
       [(quote) (cadr expr)]
       [(if) (if (eval-expr (cadr expr) env)
                 (eval-expr (caddr expr) env)
                 (eval-expr (cadddr expr) env))]
       [(lambda)
        (let ([params (cadr expr)] [body (caddr expr)])
          (lambda vals
            (eval-expr body (extend-multi env params vals))))]
       [(let)
        (eval-expr (caddr expr) (eval-bindings env (cadr expr)))]
       [(letrec)
        (let* ([names (map car (cadr expr))]
               [val-exprs (map cadr (cadr expr))]
               [body (caddr expr)]
               ;; Create mutable cells in the env
               [cell-env (extend-multi env names
                           (map (lambda _ (cell 0)) names))])
          ;; Fill cells by evaluating in cell-env (self-refs see current cells)
          (for-each (lambda (n ve)
                      (set-cell-value! (raw-lookup cell-env n)
                                       (eval-expr ve cell-env)))
                    names val-exprs)
          (eval-expr body cell-env))]
       [(define) (eval-expr (caddr expr) env)]
       [else
        (let ([proc (eval-expr (car expr) env)]
              [args (map (lambda (e) (eval-expr e env)) (cdr expr))])
          (apply proc args))])]
    [else (error 'eval-expr "unknown expression: ~a" expr)]))

;; ─── Helpers ───

(define (extend-multi env params vals)
  (if (null? params) env
      (extend-multi (extend-env env (car params) (car vals))
                    (cdr params) (cdr vals))))

(define (eval-bindings env bindings)
  (if (null? bindings) env
      (let* ([b (car bindings)]
             [val (eval-expr (cadr b) env)])
        (eval-bindings (extend-env env (car b) val) (cdr bindings)))))
