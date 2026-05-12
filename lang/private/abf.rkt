#lang racket/base
(provide serialize-expr serialize-delta
         ABF-MAGIC tag-for-expr)

(define ABF-MAGIC #"ABF2")
(define ABF-VERSION 2)

(define TAG-LITERAL-INT  #x01)
(define TAG-VARIABLE     #x02)
(define TAG-CALL         #x03)
(define TAG-IF          #x04)
(define TAG-LAMBDA      #x05)
(define TAG-LET         #x06)
(define TAG-LETREC      #x07)
(define TAG-DEFINE      #x08)
(define TAG-BEGIN       #x09)
(define TAG-SET         #x0A)
(define TAG-QUOTE       #x0B)
(define TAG-STRING      #x0D)
(define TAG-COND       #x0C)
(define TAG-TYPE-ANNOTATION #x0F)
(define PHASE-PARSED 0)

(define (tag-for-expr expr)
  (cond [(number? expr) TAG-LITERAL-INT]
        [(string? expr) TAG-STRING]
        [(boolean? expr) TAG-LITERAL-INT]
        [(symbol? expr) TAG-VARIABLE]
        [(pair? expr)
         (case (car expr)
           [(quote) TAG-QUOTE]
           [(string) TAG-STRING]
           [(if) TAG-IF]
           [(lambda) TAG-LAMBDA]
           [(let) TAG-LET]
           [(letrec) TAG-LETREC]
           [(define) TAG-DEFINE]
           [(begin) TAG-BEGIN]
           [(cond) TAG-COND]
           [(set!) TAG-SET]
           [(:) TAG-TYPE-ANNOTATION]
           [else TAG-CALL])]
        [else (error 'tag-for-expr "unknown expr: ~a" expr)]))

(define (encode-varint n)
  (let loop ([n n] [acc '()])
    (if (< n 128)
        (list->bytes (cons n (reverse acc)))
        (loop (arithmetic-shift n -7)
              (cons (bitwise-ior (bitwise-and n 127) 128) acc)))))

(define (make-buf) (open-output-bytes))
(define (get-bytes buf) (get-output-bytes buf))
(define (put! buf b) (write-byte b buf))
(define (put-bytes! buf b) (write-bytes b buf))
(define (put-varint! buf n) (put-bytes! buf (encode-varint n)))

(define (serialize-expr expr [phase-id PHASE-PARSED])
  (define buf (make-buf))
  (put-bytes! buf ABF-MAGIC)
  (put-varint! buf ABF-VERSION)
  (put-varint! buf phase-id)
  (write-node buf expr phase-id)
  (get-bytes buf))

(define (write-node buf expr phase-id)
  (define tag (tag-for-expr expr))
  (put-varint! buf tag)
  (put-varint! buf phase-id)
  (put-varint! buf 0)  ; ExtensionLength = 0
  (case tag
    [(#x01) (write-literal-int buf expr)]
    [(#x02) (write-variable buf expr)]
    [(#x03) (write-call buf expr phase-id)]
    [(#x04) (write-if buf expr phase-id)]
    [(#x0C) (write-cond buf expr phase-id)]
    [(#x05) (write-lambda buf expr phase-id)]
    [(#x06) (write-let buf expr phase-id #f)]
    [(#x07) (write-let buf expr phase-id #t)]
    [(#x08) (write-define buf expr phase-id)]
    [(#x09) (write-begin buf expr phase-id)]
    [(#x0A) (write-set buf expr phase-id)]
    [(#x0B) (write-quote buf expr phase-id)]
    [(#x0D) (write-string buf expr phase-id)]
    [(#x0F) (write-type-annotation buf expr phase-id)]))

(define (write-literal-int buf expr)
  (define val (if (integer? expr) expr 0))
  ;; Write int64 as 8-byte big-endian
  (let loop ([i 56] [v (if (negative? val) (+ val (expt 2 64)) val)])
    (when (>= i 0)
      (put! buf (bitwise-and (arithmetic-shift v (- i)) 255))
      (loop (- i 8) v))))

(define (write-variable buf expr)
  (define name (symbol->string expr))
  (define name-bytes (string->bytes/utf-8 name))
  (put-varint! buf (bytes-length name-bytes))
  (put-bytes! buf name-bytes))

(define (write-call buf expr phase-id)
  ;; Write function
  (write-node buf (car expr) phase-id)
  ;; Write arg count + args
  (define args (cdr expr))
  (put-varint! buf (length args))
  (for ([arg args])
    (write-node buf arg phase-id)))

(define (write-if buf expr phase-id)
  (write-node buf (cadr expr) phase-id)   ; cond
  (write-node buf (caddr expr) phase-id)  ; then
  (write-node buf (cadddr expr) phase-id)) ; else

(define (write-lambda buf expr phase-id)
  (define params (cadr expr))
  (define body (caddr expr))
  (put-varint! buf (length params))
  (for ([p params])
    (define name (symbol->string p))
    (define name-bytes (string->bytes/utf-8 name))
    (put-varint! buf (bytes-length name-bytes))
    (put-bytes! buf name-bytes))
  (write-node buf body phase-id))

(define (write-let buf expr phase-id is-rec)
  (define bindings (cadr expr))
  (define body (caddr expr))
  (put-varint! buf (length bindings))
  (for ([b bindings])
    (define name (symbol->string (car b)))
    (define name-bytes (string->bytes/utf-8 name))
    (put-varint! buf (bytes-length name-bytes))
    (put-bytes! buf name-bytes)
    (write-node buf (cadr b) phase-id))
  (write-node buf body phase-id))

(define (write-define buf expr phase-id)
  (define name (symbol->string (cadr expr)))
  (define name-bytes (string->bytes/utf-8 name))
  (put-varint! buf (bytes-length name-bytes))
  (put-bytes! buf name-bytes)
  (write-node buf (caddr expr) phase-id))

(define (write-begin buf expr phase-id)
  (define exprs (cdr expr))
  (put-varint! buf (length exprs))
  (for ([e exprs])
    (write-node buf e phase-id)))

(define (write-set buf expr phase-id)
  (define name (symbol->string (cadr expr)))
  (define name-bytes (string->bytes/utf-8 name))
  (put-varint! buf (bytes-length name-bytes))
  (put-bytes! buf name-bytes)
  (write-node buf (caddr expr) phase-id))


(define (write-cond buf expr phase-id)
  ;; Desugar (cond (t1 v1) (t2 v2) ... (else en))
  ;; Write as sequence: test1 val1 IF_tag test2 val2 IF_tag ... else_val
  ;; read_cond reads: test1, val1, then loop: if next=0x04 read next pair
  (define clauses (cdr expr))
  (define (write-pair test val)
    (write-node buf test phase-id)
    (write-node buf val phase-id))
  (define (write-clauses cls)
    (unless (null? cls)
      (write-pair (caar cls) (cadar cls))
      (let ([rest (cdr cls)])
        (unless (null? rest)
          ;; Mark continuation with IF tag
          (put-varint! buf TAG-IF)
          (put-varint! buf phase-id)
          (put-varint! buf 0)
          (write-clauses rest)))))
  (write-clauses clauses))

(define (write-string buf expr phase-id)
  ;; expr is the string literal itself
  (define name-bytes (string->bytes/utf-8 expr))
  (put-varint! buf (bytes-length name-bytes))
  (put-bytes! buf name-bytes))

(define (write-quote buf expr phase-id)
  ;; quote wraps a single expression — write it directly
  (write-node buf (cadr expr) phase-id))

(define (write-type-annotation buf expr phase-id)
  ;; (: x Int) → tag 0x0F, type name string, inner expression
  (define type-name (symbol->string (caddr expr)))
  (define inner-expr (cadr expr))
  (define name-bytes (string->bytes/utf-8 type-name))
  (put-varint! buf (bytes-length name-bytes))
  (put-bytes! buf name-bytes)
  (write-node buf inner-expr phase-id))

(define (serialize-delta expr base-version)
  (serialize-expr expr))
