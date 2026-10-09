#!/usr/bin/env bash
# r7rs-srfi-plan S1's exit criterion: a built-in SRFI's import is a no-op.
# R7RS adopted these SRFIs, so their names are (scheme ...)'s own, and
# importing them must cost nothing -- the same program with and without the
# imports emits the same C, byte for byte.  The program imports what the
# SRFIs re-export from (scheme case-lambda) and (scheme process-context), so
# the one difference would be the SRFI imports themselves.  Both copies are
# named p.tur, in two directories, so the file name cannot differ either, and
# both keep the whole import on line 2, so no form moves to another line: a
# panic site the emitter interns carries the form's line
# (panic-location-names-the-runtime-not-the-call-site), which is not what
# this comparison is about.
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$1"
# The emits run from inside $TMP, so the compiler's path must not be relative.
case "$TUR" in /*) ;; *) TUR="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;; esac
mkdir -p "$TMP/without" "$TMP/with"
BODY='(define (show x) (write x) (newline))
(show (let ((p (open-output-string))) (write (quote hi) p) (get-output-string p)))
(define-record-type point (make-point x y) point? (x point-x))
(show (point-x (make-point 1 2)))
(show (let-values (((a . r) (values 1 2 3))) (list a r)))
(show ((case-lambda ((a) 1) ((a b) 2)) 1 2))
(show (guard (e (#t (error-object-message e))) (error "boom")))
(define p (make-parameter 1))
(show (parameterize ((p 2)) (p)))
(show (case 3 ((3) => (lambda (x) (* x 2))) (else 0)))
(show (string? (get-environment-variable "HOME")))'
printf '#lang r7rs\n(import (scheme base) (scheme write) (scheme case-lambda) (scheme process-context))\n%s\n' \
    "$BODY" > "$TMP/without/p.tur"
printf '#lang r7rs\n(import (scheme base) (scheme write) (scheme case-lambda) (scheme process-context) (srfi 6) (srfi 9) (srfi 11) (srfi 16) (srfi 23) (srfi 30) (srfi 34) (srfi 39) (srfi 87) (srfi 98))\n%s\n' \
    "$BODY" > "$TMP/with/p.tur"
(cd "$TMP/without" && "$TUR" emit-c p.tur > p.c 2>/dev/null)
(cd "$TMP/with" && "$TUR" emit-c p.tur > p.c 2>/dev/null)
if cmp -s "$TMP/without/p.c" "$TMP/with/p.c"; then
    echo "emit-c: identical with and without the ten built-in SRFI imports"
else
    echo "emit-c: DIFFERS"
    diff "$TMP/without/p.c" "$TMP/with/p.c" | head -20
fi
(cd "$TMP/with" && "$TUR" run p.tur 2>/dev/null)
