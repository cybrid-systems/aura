#! /usr/bin/env racket
#lang racket/base
;; Aura CLI — evaluate Aura source code, optionally output ABF binary
;;
;; Usage:
;;   racket aura.rkt -e '(+ 1 2)'              # eval, print result
;;   racket aura.rkt -e '(+ 1 2)' --abf         # eval → ABF → stdout
;;   racket aura.rkt source.aura                # eval file
;;   echo '(+ 1 2)' | racket aura.rkt           # stdin eval
;;   echo '(+ 1 2)' | racket aura.rkt --abf     # stdin → ABF → stdout

(require "lang/private/core.rkt"
         "lang/private/abf.rkt"
         racket/port
         racket/cmdline)

(define abf-mode? #f)
(define exprs '())

(define (run! exprs)
  (for ([e (in-list exprs)])
    (if abf-mode?
        (begin (write-bytes (serialize-expr e))
               (flush-output))
        (displayln (eval-expr e (make-env))))))

(command-line
 #:program "aura"
 #:once-each
 [("-e" "--eval") expr "Evaluate expression"
                   (set! exprs (append exprs (list (read (open-input-string expr)))))]
 ["--abf" "Output ABF binary instead of text"
                   (set! abf-mode? #t)]
 #:args files
 (cond
   [(pair? exprs)
    ;; -e given: evaluate those expressions
    (run! exprs)]
   [(null? files)
    ;; No files, no -e: read stdin
    (let ([expr (read (current-input-port))])
      (unless (eof-object? expr)
        (run! (list expr))))]
   [else
    ;; File mode
    (for ([f files])
      (run! (list (read (open-input-file f)))))]))
