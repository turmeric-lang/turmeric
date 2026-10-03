;;; tests/r7rs/srfi/216/tests.scm -- SRFI 216's tests: srfi-216-tests.scm
;;; from the SRFI's repository, rewritten from (srfi 78)'s `check` into
;;; (chibi test)'s `test` for tests/r7rs/run-conformance.py (r7rs-srfi-plan
;;; D7).  Copyright (C) 2020 Vladimir Nikishkin; MIT licence, see
;;; stdlib/srfi/COPYING.  Changes: `runtime` is checked by waiting until it
;;; moves rather than by a one-second busy loop; the parallel-execute check
;;; prints nothing; the stream checks import (scheme lazy) for `promise?`.
(test-begin "srfi-216: SICP prerequisites")

;;; runtime
(test #t (exact-integer? (runtime)))
(test #t (let ((t0 (runtime)))
           (let loop ()
             (let ((t1 (runtime)))
               (if (= t1 t0) (loop) (> t1 t0))))))

;;; random
(test #t (> (random 100) -1))
(test #t (< (random 100) 100))
(test #t (exact-integer? (random 100)))
(test #f (exact-integer? (random 100.0)))
(test #t (inexact? (random 1.5)))
(test #t (let ((r (random 1.5))) (and (>= r 0) (< r 1.5))))

;;; parallel-execute: the result is one a concurrent run allows (2 or 3).
(define testval 1)
(define (my-wait n) (if (= n 0) #t (my-wait (- n 1))))
(test #t (begin
           (parallel-execute
            (lambda () (my-wait (random 100)) (set! testval 2))
            (lambda () (my-wait (random 100)) (set! testval 3)))
           (or (= testval 2) (= testval 3))))

;;; test-and-set!
(define cell (list #f))
(test #f (test-and-set! cell))
(test #t (car cell))
(test #t (test-and-set! cell))

;;; booleans and the empty list
(test #f (if false #t #f))
(test #t (if true #t #f))
(test '() nil)

;;; streams
(test #t (stream-null? the-empty-stream))
(test #f (stream-null? (cons-stream 1 2)))
(test 'a (car (cons-stream 'a 'b)))
(test #t (promise? (cdr (cons-stream 'a 'b))))
(test 'b (force (cdr (cons-stream 'a 'b))))
(test 1 (car (cons-stream 1 (error "cons-stream evaluated its second part"))))

(test-end)
