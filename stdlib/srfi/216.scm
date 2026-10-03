;;; srfi/216 -- SRFI 216, SICP Prerequisites (Portable).
;;;
;;; Written for Turmeric rather than ported: the reference implementation's
;;; `runtime` multiplies by jiffies-per-second where it must divide, and its
;;; `parallel-execute` buffers every thread's output until all have finished.
;;; docs/upcoming/r7rs-srfi-18-216-sicp-plan.md, stage T0.
;;;
;;; `parallel-execute` and `test-and-set!` are here before SRFI 18 is: until
;;; T3, `parallel-execute` runs its thunks one after another, in order.
;;; That is one schedule a concurrent run may produce, so every result it
;;; gives is a result SICP 3.4 allows -- but never an interleaved one. With
;;; one thread running, `test-and-set!` is atomic as written.
(define-library (srfi 216)
  (export true false nil the-empty-stream stream-null? cons-stream
          runtime random parallel-execute test-and-set!)
  (import (scheme base) (scheme time) (srfi 27))
  (begin
    (define true #t)
    (define false #f)
    (define nil '())

    ;; Streams (SICP 3.5): the book's own stream-car and stream-cdr are left
    ;; to the reader, as the SRFI does.
    (define the-empty-stream '())
    (define (stream-null? x) (null? x))
    (define-syntax cons-stream
      (syntax-rules ()
        ((_ a b) (cons a (delay b)))))

    ;; Microseconds, as an exact integer.
    (define (runtime)
      (round (/ (* (current-jiffy) 1000000) (jiffies-per-second))))

    ;; Exact in, exact out; inexact in, inexact out.
    (define (random x)
      (if (exact-integer? x)
          (random-integer x)
          (* x (random-real))))

    (define (parallel-execute . thunks)
      (for-each (lambda (thunk) (thunk)) thunks))

    (define (test-and-set! cell)
      (if (car cell)
          #t
          (begin (set-car! cell #t) #f)))))
