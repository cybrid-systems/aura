#lang racket/base
;; Aura #lang — ABF output mode
;;
;; Same as lang/expander.rkt but outputs ABF binary instead of text.
;; Used by: #lang aura --abf
;;
;; This is a separate module to avoid conditional logic in the expander.

(require "private/core.rkt"
         "private/abf.rkt")

(provide (rename-out [my-module-begin #%module-begin])
         (rename-out [my-datum #%datum])
         (rename-out [my-top #%top])
         (rename-out [my-app #%app]))

(define-syntax-rule (my-module-begin expr)
  (#%plain-module-begin
    (write-bytes (serialize-expr 'expr))
    (flush-output)))

(define-syntax-rule (my-datum . x) x)
(define-syntax-rule (my-top . x) x)
(define-syntax-rule (my-app proc arg ...) (proc arg ...))
