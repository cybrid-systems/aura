#lang racket/base
;; Aura ABF v2 serialization — output AST as binary for C++ Compiler Service
;; Format: docs/aura_serialization.md §4

(provide serialize-expr serialize-delta
         ABF-MAGIC tag-for-expr)

;; ─── ABF v2 constants ───

(define ABF-MAGIC #"ABF2")
(define ABF-VERSION 2)

;; Tag values matching src/core/ast.ixx NodeTag
(define TAG-LITERAL-INT  #x01)
(define TAG-VARIABLE     #x02)
(define TAG-CALL         #x03)
(define TAG-IF          #x04)
(define TAG-LAMBDA      #x05)
(define TAG-LET         #x06)

;; Phase IDs matching src/core/ast.ixx ParsedPhase::id
(define PHASE-PARSED 0)

;; ─── Tag dispatcher ───

(define (tag-for-expr expr)
  (cond [(number? expr) TAG-LITERAL-INT]
        [(string? expr) TAG-LITERAL-INT]  ;; strings treated as literals for now
        [(boolean? expr) TAG-LITERAL-INT]
        [(symbol? expr) TAG-VARIABLE]
        [(pair? expr)
         (case (car expr)
           [(quote) TAG-LITERAL-INT]
           [(if) TAG-IF]
           [(lambda) TAG-LAMBDA]
           [(let) TAG-LET]
           [(letrec) TAG-LET]
           [else TAG-CALL])]
        [else (error 'tag-for-expr "unknown expression: ~a" expr)]))

;; ─── Varint encoding ───

(define (encode-varint n)
  (let loop ([n n] [acc '()])
    (if (< n 128)
        (list->bytes (cons n (reverse acc)))
        (loop (arithmetic-shift n -7)
              (cons (bitwise-ior (bitwise-and n 127) 128) acc)))))

;; ─── Bytes helpers ───

(define (append-bytes! buf b)
  (for ([byte (in-bytes b)])
    (write-byte byte buf)))

(define (make-byte-buffer)
  (open-output-bytes))

(define (get-bytes buf)
  (get-output-bytes buf))

;; ─── Serialize an expression to ABF v2 binary ───

(define (serialize-expr expr [phase-id PHASE-PARSED])
  (define buf (make-byte-buffer))
  ;; Write header
  (write-bytes ABF-MAGIC buf)
  (append-bytes! buf (encode-varint ABF-VERSION))
  (append-bytes! buf (encode-varint phase-id))
  ;; Write node
  (write-node buf expr phase-id)
  (get-bytes buf))

;; Serialize a single node
(define (write-node buf expr phase-id)
  (define tag (tag-for-expr expr))
  ;; Tag (varint)
  (append-bytes! buf (encode-varint tag))
  ;; Extension ID (varint) — ParsedPhase has no extension data
  (append-bytes! buf (encode-varint phase-id))
  ;; Extension length (varint) — 0 for ParsedPhase
  (append-bytes! buf (encode-varint 0))
  ;; Core payload
  (write-core-payload buf expr tag phase-id))

;; Write core payload based on node tag
(define (write-core-payload buf expr tag phase-id)
  (case tag
    [(#x01) ; LiteralInt
     (when (number? expr)
       ;; Write int64 as 8-byte big-endian signed integer
       (let ([val (if (integer? expr) expr 0)])
         (define (int64->bytes n)
           (let loop ([i 7] [n n] [acc '()])
             (if (< i 0)
                 (list->bytes acc)
                 (loop (sub1 i) (arithmetic-shift n -8)
                       (cons (bitwise-and n 255) acc)))))
         (write-bytes (int64->bytes (if (negative? val) (+ val (expt 2 64)) val)) buf)))]
    [(#x02) ; Variable
     (let* ([name (symbol->string expr)]
            [name-bytes (string->bytes/utf-8 name)]
            [len (bytes-length name-bytes)])
       (append-bytes! buf (encode-varint len))
       (write-bytes name-bytes buf))]
    [(#x03) ; Call
     (write-node buf (car expr) phase-id)     ; function
     (for ([arg (cdr expr)])                   ; args
       (write-node buf arg phase-id))]
    [(#x04) ; If
     (write-node buf (cadr expr) phase-id)     ; condition
     (write-node buf (caddr expr) phase-id)    ; then
     (write-node buf (cadddr expr) phase-id)]  ; else
    [(#x05) ; Lambda
     (let* ([params (cadr expr)]
            [body (caddr expr)]
            [param-count (length params)])
       (append-bytes! buf (encode-varint param-count))
       (for ([p params])
         (let* ([name (symbol->string p)]
                [name-bytes (string->bytes/utf-8 name)])
           (append-bytes! buf (encode-varint (bytes-length name-bytes)))
           (write-bytes name-bytes buf)))
       (write-node buf body phase-id))]
    [(#x06) ; Let
     (let* ([bindings (cadr expr)]
            [body (caddr expr)]
            [binding-count (length bindings)])
       (append-bytes! buf (encode-varint binding-count))
       (for ([b bindings])
         (let* ([name (symbol->string (car b))]
                [name-bytes (string->bytes/utf-8 name)])
           (append-bytes! buf (encode-varint (bytes-length name-bytes)))
           (write-bytes name-bytes buf)
           (write-node buf (cadr b) phase-id)))
       (write-node buf body phase-id))]
    [else
     (error 'write-core-payload "unknown tag: ~a" tag)]))

;; ─── Delta serialization (stub) ───

(define (serialize-delta expr base-version)
  ;; For now, just serialize the full expression
  (serialize-expr expr))
