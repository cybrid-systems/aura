#lang racket/base
;; Test: can #%module-begin be defined in a separate module?

(module test-lang racket/base
  (provide #%module-begin #%datum #%top #%app)
  (define-syntax-rule (#%module-begin expr)
    (#%plain-module-begin
      (println expr))))

(module test-mod test-lang
  #%module-begin
  42)

(require 'test-mod)
