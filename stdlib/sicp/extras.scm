;;; sicp/extras -- the `#lang sicp` extras SRFI 216 leaves out.
;;;
;;; Racket's `#lang sicp` provides inc, dec, identity and amb, and course
;;; materials written against it assume them.  SRFI 216 is a finished spec,
;;; so they live here instead.  amb-reset! is ours: Racket's amb has no way to
;;; drop the last search's choice points, and exhausting a second search would
;;; otherwise resume the first.
;;;
;;; amb's choice points are one top-level variable, so they are per program,
;;; not per thread: two threads searching at once share them (Racket's are
;;; the same).
;;;
;;; docs/archive/r7rs-srfi-18-216-sicp-plan.md, D4 and stage T0a.
(define-library (sicp extras)
  (export inc dec identity amb amb-reset!)
  (import (scheme base))
  (begin
    (define (inc x) (+ x 1))
    (define (dec x) (- x 1))
    (define (identity x) x)

    ;; The innermost choice point's retry.  Each (amb alt ...) pushes one
    ;; that tries its next alternative, and pops itself when it has none.
    (define amb-fail #f)
    (define (amb-set-fail! f) (set! amb-fail f))

    ;; Start a new search from scratch.
    (define (amb-reset!)
      (set! amb-fail (lambda () (error "amb tree exhausted"))))
    (amb-reset!)

    (define-syntax amb
      (syntax-rules ()
        ((_ alt ...)
         (let ((prev-fail amb-fail))
           (call/cc
            (lambda (sk)
              (call/cc
               (lambda (fk)
                 (amb-set-fail!
                  (lambda ()
                    (amb-set-fail! prev-fail)
                    (fk 'fail)))
                 (sk alt)))
              ...
              (prev-fail)))))))))
