;;; srfi/216 -- SRFI 216, SICP Prerequisites (Portable).
;;;
;;; Written for Turmeric rather than ported: the reference implementation's
;;; `runtime` multiplies by jiffies-per-second where it must divide, and its
;;; `parallel-execute` buffers every thread's output until all have finished.
;;; docs/archive/r7rs-srfi-18-216-sicp-plan.md, stage T0.
;;;
;;; `parallel-execute` and `test-and-set!` are over SRFI 18 (stage T3): each
;;; thunk runs on a thread of its own, so SICP 3.4's interleavings really
;;; happen, and `test-and-set!` is atomic under SRFI 18's monitor (the C half
;;; in stdlib/r7rs/thread.tur, which the (srfi 18) import splices in).
(define-library (srfi 216)
  (export true false nil the-empty-stream stream-null? cons-stream
          runtime random parallel-execute test-and-set!)
  (import (scheme base) (scheme time) (srfi 18) (srfi 27))
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

    ;; Every thunk on a thread of its own, all joined before it returns.  A
    ;; thunk that raises makes the join raise SRFI 18's uncaught-exception.
    (define (parallel-execute . thunks)
      (let ((threads (map (lambda (thunk) (thread-start! (make-thread thunk))) thunks)))
        (for-each thread-join! threads)))

    ;; Atomic under SRFI 18's monitor.  When the cell was already set, the
    ;; caller is about to spin on it (SICP's make-mutex retries at once), so
    ;; let the holder run first -- under the interpreter's green threads
    ;; nothing else would ever run.
    (define (test-and-set! cell)
      (r7rs-thread-lock__)
      (let ((was (car cell)))
        (if (not was) (set-car! cell #t))
        (r7rs-thread-unlock__)
        (if was (r7rs-thread-yield__))
        was))))
