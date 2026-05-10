#lang racket/base
(require "private/core.rkt")
(provide #%module-begin #%datum #%top #%app)

(define-syntax-rule (#%module-begin expr)
  (#%plain-module-begin
    (displayln (eval-expr 'expr (make-env)))))

(define-syntax-rule (#%datum . x) x)
(define-syntax-rule (#%top . x) x)
(define-syntax-rule (#%app proc arg ...) (proc arg ...))
