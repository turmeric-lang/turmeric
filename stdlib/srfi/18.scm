;;; srfi/18 -- SRFI 18, Multithreading support.
;;;
;;; Written for Turmeric (docs/upcoming/r7rs-srfi-18-216-sicp-plan.md, D1).
;;; Threads are OS threads; the C half is stdlib/r7rs/thread.tur, which the
;;; import splices in beside this file and which this file calls by its
;;; r7rs-thread-...__ names.
;;;
;;; All of SRFI 18's shared state -- a thread's end, a mutex's owner, a
;;; condition variable's waiters -- is Scheme data in these records, changed
;;; only under one monitor (thread.tur's), and every wait is a wait on that
;;; monitor, re-checked when it wakes.  One lock for every mutex is not fast
;;; under contention; it is simple to get right, which is what SICP 3.4 and a
;;; first cut want.
;;;
;;; Differences from the SRFI, each one a refusal rather than a quiet change:
;;;
;;;   - thread-terminate! raises an error: an OS thread cannot be stopped
;;;     safely from outside.
;;;
;;; A mutex is "abandoned" when the thread that owns it has ended; that is
;;; decided when someone looks (mutex-state, mutex-lock!), not when the
;;; thread ends.
(define-library (srfi 18)
  (export current-thread thread? make-thread thread-name thread-specific
          thread-specific-set! thread-start! thread-yield! thread-sleep!
          thread-terminate! thread-join!
          mutex? make-mutex mutex-name mutex-specific mutex-specific-set!
          mutex-state mutex-lock! mutex-unlock!
          condition-variable? make-condition-variable condition-variable-name
          condition-variable-specific condition-variable-specific-set!
          condition-variable-signal! condition-variable-broadcast!
          current-time time? time->seconds seconds->time
          current-exception-handler with-exception-handler raise
          join-timeout-exception? abandoned-mutex-exception?
          terminated-thread-exception? uncaught-exception?
          uncaught-exception-reason)
  (import (scheme base))
  (begin

    ;; ---- time -------------------------------------------------------------

    (define-record-type srfi18-time
      (make-time seconds)
      time?
      (seconds time->seconds))

    (define (current-time) (make-time (r7rs-thread-now__)))

    (define (seconds->time x) (make-time (inexact x)))

    ;; A timeout as a deadline in wall-clock seconds, or -1.0 for none: #f or
    ;; absent is none, a time is absolute, a real is relative to now.
    (define (timeout->deadline timeout)
      (cond ((not timeout) -1.0)
            ((time? timeout) (time->seconds timeout))
            ((real? timeout) (+ (r7rs-thread-now__) (inexact timeout)))
            (else (error "a timeout is #f, a time object or a real number of seconds" timeout))))

    ;; ---- the exceptions ---------------------------------------------------

    (define-record-type srfi18-join-timeout
      (make-join-timeout-exception)
      join-timeout-exception?)

    (define-record-type srfi18-abandoned-mutex
      (make-abandoned-mutex-exception)
      abandoned-mutex-exception?)

    (define-record-type srfi18-terminated-thread
      (make-terminated-thread-exception)
      terminated-thread-exception?)

    (define-record-type srfi18-uncaught
      (make-uncaught-exception reason)
      uncaught-exception?
      (reason uncaught-exception-reason))

    (define (current-exception-handler)
      (let ((hs (r7rs-handlers__)))
        (if (null? hs) raise (car hs))))

    ;; ---- threads ----------------------------------------------------------

    ;; state: new, runnable or terminated.  outcome: what the thunk returned,
    ;; or the uncaught-exception record a join raises.
    (define-record-type srfi18-thread
      (make-thread-record thunk name specific state outcome failed)
      thread?
      (thunk thread-thunk set-thread-thunk!)
      (name thread-name)
      (specific thread-specific thread-specific-set!)
      (state thread-state set-thread-state!)
      (outcome thread-outcome set-thread-outcome!)
      (failed thread-failed? set-thread-failed!))

    (define (make-thread thunk . name)
      (if (not (procedure? thunk)) (error "make-thread: not a procedure" thunk))
      (make-thread-record thunk (if (pair? name) (car name) #f) #f 'new #f #f))

    ;; The thread the program started on.
    (define primordial-thread
      (make-thread-record #f 'primordial #f 'runnable #f #f))

    ;; Per thread: the parameter bindings are part of the dynamic environment,
    ;; which a new thread starts with empty, so every thread but the primordial
    ;; one binds it first thing.
    (define current-thread-parameter (make-parameter primordial-thread))

    (define (current-thread) (current-thread-parameter))

    (define (thread-finish! t failed outcome)
      (r7rs-thread-lock__)
      (set-thread-outcome! t outcome)
      (set-thread-failed! t failed)
      (set-thread-state! t 'terminated)
      (r7rs-thread-notify__)
      (r7rs-thread-unlock__))

    (define (thread-run t)
      (parameterize ((current-thread-parameter t))
        (let ((thunk (thread-thunk t)))
          (set-thread-thunk! t #f)
          (guard (e (#t (thread-finish! t #t (make-uncaught-exception e))))
            (thread-finish! t #f (thunk))))))

    (define (thread-start! t)
      (r7rs-thread-lock__)
      (let ((fresh (eq? (thread-state t) 'new)))
        (if fresh (set-thread-state! t 'runnable))
        (r7rs-thread-unlock__)
        (if (not fresh) (error "thread-start!: the thread was already started" t)))
      (if (not (r7rs-thread-spawn__ (lambda () (thread-run t))))
          (begin
            (thread-finish! t #t (make-uncaught-exception "the system refused a new thread"))
            (error "thread-start!: the system refused a new thread" t)))
      t)

    (define (thread-yield!) (r7rs-thread-yield__))

    (define (thread-sleep! timeout)
      (let ((deadline (timeout->deadline timeout)))
        (if (>= deadline 0.0)
            (r7rs-thread-sleep__ (- deadline (r7rs-thread-now__))))))

    (define (thread-terminate! t)
      (error "thread-terminate! is not supported: an OS thread cannot be stopped safely from outside; have the thread return instead" t))

    (define (thread-join! t . opts)
      (let ((deadline (timeout->deadline (if (pair? opts) (car opts) #f)))
            (has-default (and (pair? opts) (pair? (cdr opts)))))
        (r7rs-thread-lock__)
        (let wait ()
          (cond ((eq? (thread-state t) 'terminated)
                 (let ((failed (thread-failed? t)) (outcome (thread-outcome t)))
                   (r7rs-thread-unlock__)
                   (if failed (raise outcome) outcome)))
                ((r7rs-thread-wait__ deadline)
                 (if (eq? (thread-state t) 'terminated)
                     (wait)
                     (begin
                       (r7rs-thread-unlock__)
                       (if has-default
                           (cadr opts)
                           (raise (make-join-timeout-exception))))))
                (else (wait))))))

    ;; ---- mutexes ----------------------------------------------------------

    ;; owner: the owning thread, 'not-owned, or #f when unlocked.
    (define-record-type srfi18-mutex
      (make-mutex-record name specific owner abandoned)
      mutex?
      (name mutex-name)
      (specific mutex-specific mutex-specific-set!)
      (owner mutex-owner set-mutex-owner!)
      (abandoned mutex-abandoned? set-mutex-abandoned!))

    (define (make-mutex . name)
      (make-mutex-record (if (pair? name) (car name) #f) #f #f #f))

    ;; Monitor held: a locked mutex whose owner has ended is unlocked and
    ;; abandoned.
    (define (mutex-settle! m)
      (let ((o (mutex-owner m)))
        (if (and (thread? o) (eq? (thread-state o) 'terminated))
            (begin
              (set-mutex-owner! m #f)
              (set-mutex-abandoned! m #t)))))

    (define (mutex-state m)
      (r7rs-thread-lock__)
      (mutex-settle! m)
      (let* ((o (mutex-owner m))
             (s (cond ((thread? o) o)
                      ((eq? o 'not-owned) 'not-owned)
                      ((mutex-abandoned? m) 'abandoned)
                      (else 'not-abandoned))))
        (r7rs-thread-unlock__)
        s))

    ;; (mutex-lock! m [timeout [thread]]): #t once locked, #f on timeout.  A
    ;; thread of #f locks it not-owned.  Locking an abandoned mutex locks it
    ;; and then raises abandoned-mutex-exception.
    (define (mutex-lock! m . opts)
      (let ((deadline (timeout->deadline (if (pair? opts) (car opts) #f)))
            (owner (if (and (pair? opts) (pair? (cdr opts)))
                       (or (cadr opts) 'not-owned)
                       (current-thread))))
        (r7rs-thread-lock__)
        (let try ()
          (mutex-settle! m)
          (cond ((not (mutex-owner m))
                 (let ((was-abandoned (mutex-abandoned? m)))
                   (set-mutex-owner! m owner)
                   (set-mutex-abandoned! m #f)
                   (r7rs-thread-unlock__)
                   (if was-abandoned (raise (make-abandoned-mutex-exception)))
                   #t))
                ((r7rs-thread-wait__ deadline)
                 (mutex-settle! m)
                 (if (not (mutex-owner m))
                     (try)
                     (begin (r7rs-thread-unlock__) #f)))
                (else (try))))))

    ;; (mutex-unlock! m [cv [timeout]]): unlock; with a condition variable,
    ;; also wait on it -- atomically with the unlock -- until it is signalled
    ;; (#t) or the timeout passes (#f).  The mutex is NOT locked again.
    (define (mutex-unlock! m . opts)
      (let ((cv (and (pair? opts) (car opts)))
            (deadline (timeout->deadline (if (and (pair? opts) (pair? (cdr opts))) (cadr opts) #f))))
        (r7rs-thread-lock__)
        (set-mutex-owner! m #f)
        (set-mutex-abandoned! m #f)
        (r7rs-thread-notify__)
        (if (not cv)
            (begin (r7rs-thread-unlock__) #t)
            (let ((token (list #f)))
              (cv-add-waiter! cv token)
              (let wait ()
                (cond ((car token) (r7rs-thread-unlock__) #t)
                      ((r7rs-thread-wait__ deadline)
                       (if (car token)
                           (wait)
                           (begin
                             (cv-remove-waiter! cv token)
                             (r7rs-thread-unlock__)
                             #f)))
                      (else (wait))))))))

    ;; ---- condition variables ----------------------------------------------

    ;; waiters: the tokens of the threads waiting, oldest first; a token is a
    ;; one-element list whose car becomes #t when it is woken.
    (define-record-type srfi18-condition-variable
      (make-cv-record name specific waiters)
      condition-variable?
      (name condition-variable-name)
      (specific condition-variable-specific condition-variable-specific-set!)
      (waiters cv-waiters set-cv-waiters!))

    (define (make-condition-variable . name)
      (make-cv-record (if (pair? name) (car name) #f) #f '()))

    (define (cv-add-waiter! cv token)
      (set-cv-waiters! cv (append (cv-waiters cv) (list token))))

    (define (cv-remove-waiter! cv token)
      (let loop ((ws (cv-waiters cv)) (acc '()))
        (cond ((null? ws) (set-cv-waiters! cv (reverse acc)))
              ((eq? (car ws) token) (set-cv-waiters! cv (append (reverse acc) (cdr ws))))
              (else (loop (cdr ws) (cons (car ws) acc))))))

    (define (condition-variable-signal! cv)
      (r7rs-thread-lock__)
      (let ((ws (cv-waiters cv)))
        (if (pair? ws)
            (begin
              (set-car! (car ws) #t)
              (set-cv-waiters! cv (cdr ws))
              (r7rs-thread-notify__))))
      (r7rs-thread-unlock__))

    (define (condition-variable-broadcast! cv)
      (r7rs-thread-lock__)
      (for-each (lambda (token) (set-car! token #t)) (cv-waiters cv))
      (set-cv-waiters! cv '())
      (r7rs-thread-notify__)
      (r7rs-thread-unlock__))))
