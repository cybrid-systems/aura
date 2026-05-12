#lang racket/base
;; Aura — entry point for `racket -l aura`
;;
;; Usage:
;;   echo '(+ 1 2)' | racket -l aura              # stdin eval → text
;;   racket -l aura source.aura                    # #lang file eval → text
;;   echo '#lang aura ...' | racket -l aura -- --abf | ./aura --abf  # ABF
;;   racket -l aura -- --abf source.aura | ./aura --abf  # file → ABF

(require racket/port
         racket/list
         racket/string
         "lang/private/core.rkt"
         "lang/private/abf.rkt")

(define args (filter (λ (a) (not (equal? a "---")))
               (vector->list (current-command-line-arguments))))
(define flags (filter (λ (a) (string-prefix? a "--")) args))
(define files (filter (λ (a) (not (string-prefix? a "--"))) args))
(define abf-mode? (member "--abf" flags))

(define (run expr)
  (if abf-mode?
      (begin (write-bytes (serialize-expr expr)) (flush-output))
      (displayln (eval-expr expr (make-env)))))

(define (read-sexpr-from-file path)
  (call-with-input-file path
    (λ (p)
      (read-line p)  ;; skip #lang line
      (read p))))

(define (read-stdin-sexpr)
  (define all-text (port->string (current-input-port)))
  (define parts (regexp-split #px"\n" all-text))
  (define src (if (and (pair? parts) (string-prefix? (car parts) "#lang"))
                  (string-join (cdr parts) "\n")
                  all-text))
  (read (open-input-string src)))

(cond
  [(pair? files)
   (if abf-mode?
       (for ([f (in-list files)])
         (run (read-sexpr-from-file f)))
       (for ([f (in-list files)])
         (dynamic-require (list 'file f) #f)))]
  [else
   (define expr (read-stdin-sexpr))
   (unless (eof-object? expr)
     (run expr))])
