#lang racket/base
;; Aura #lang expander — evaluate single top-level expressions
;;
;; This module is the expansion target of lang/reader.rkt.
;; It provides #%module-begin and friends to make #lang aura work.

(require "private/core.rkt")

(provide (rename-out [my-module-begin #%module-begin])
         (rename-out [my-datum #%datum])
         (rename-out [my-top #%top])
         (rename-out [my-app #%app]))

(define-syntax-rule (my-module-begin expr)
  (#%plain-module-begin
    (displayln (eval-expr 'expr (make-env)))))

(define-syntax-rule (my-datum . x) x)
(define-syntax-rule (my-top . x) x)
(define-syntax-rule (my-app proc arg ...) (proc arg ...))
