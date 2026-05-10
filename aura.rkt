#! /usr/bin/env racket
#lang racket/base
;; Aura CLI — evaluate Aura source code, optionally output ABF binary
;; Usage: racket aura.rkt <file> [--abf]
;;        racket aura.rkt -e '(let ((x 10)) x)'

(require "lang/private/core.rkt"
         racket/port
         racket/cmdline)

(command-line
 #:program "aura"
 #:once-each
 [("-e" "--eval") expr "Evaluate expression"
                   (displayln (eval-expr (read (open-input-string expr)) (make-env)))]
 #:args files
 (for ([f files])
   (displayln (eval-expr (read (open-input-file f)) (make-env)))))
