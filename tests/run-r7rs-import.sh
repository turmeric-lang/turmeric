#!/usr/bin/env bash
# tests/run-r7rs-import.sh -- the Turmeric <-> R7RS seam, both directions,
# both back ends.
#
# r7rs-lang-plan R3 / D9.  D9 says library names are module paths and the
# Turmeric namespace is one head symbol: `(import (turmeric geom))` is
# `(import geom)`, `(only ...)` is `:refer`, `(prefix ...)` and `(rename ...)`
# are elaboration-level renames over the same import, and a
# `(define-library (mylib) ...)` is a `(defmodule mylib ...)` a Turmeric file
# imports like any other.  The plan asked for this runner explicitly, "on the
# model of run-saffron-import.sh rather than being assumed from the Saffron
# one": the R7RS prelude is loaded on the import path too (elab_module.c),
# which is a different pre-pass from the entry program's, and that is where
# the forward-declared `(Vec any)` seam bug this stage found lived.
#
# Needs its own runner rather than a fixture: a multi-module fixture requires
# a dedicated runner anyway (cf. tests/fixtures/any-type-id-multi-module).
#
# Usage: bash tests/run-r7rs-import.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "run-r7rs-import: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

run_case() {
    # run_case <name> <entry file> <expected stdout>
    local name="$1" entry="$2" expect="$3" mode out
    for mode in compiled interpret; do
        if [ "$mode" = compiled ]; then
            out="$(cd "$TMP" && "$TUR" run "$entry" 2>/dev/null)"
        else
            out="$(cd "$TMP" && ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$entry" 2>/dev/null)"
        fi
        if [ "$out" != "$expect" ]; then
            echo "FAIL r7rs-import $name ($mode): expected '$expect', got '$out'"
            FAILED=1
        else
            echo "PASS r7rs-import $name ($mode)"
        fi
    done
}

# ---- Direction 1: a Turmeric module imports a Scheme define-library. ------
# The library's exports are `any`-typed (every Scheme definition is), so the
# Turmeric caller narrows with `cast`, exactly as it does for a Saffron module.
cat > "$TMP/mylib.tur" <<'EOF'
#lang r7rs
(define-library (mylib)
  (export twice greet)
  (import (scheme base))
  (begin
    (define (twice x) (* x 2))
    (define (greet name) (string-append "hi " name))))
EOF

cat > "$TMP/tmain.tur" <<'EOF'
(defmodule tmain
  (import mylib :refer [twice greet])
  (defn main [] : int
    (println (cast (twice (:: 21 any)) int))
    (println (cast (greet (:: "bob" any)) cstr))
    0))
EOF

run_case "turmeric-imports-scheme" tmain.tur "42
hi bob"

# ---- Direction 2: a Scheme program imports a Turmeric module. --------------
# `(only ...)` and `(prefix ...)` over the same module, plus a stdlib module
# through the `(turmeric stdlib/...)` head.  Each argument crosses the seam
# through Saffron's checked cast against the Turmeric signature.
cat > "$TMP/geom.tur" <<'EOF'
(defmodule geom
  (export area scale)
  (defn area [w : float h : float] : float (* w h))
  (defn scale [n : int k : int] : int (* n k)))
EOF

cat > "$TMP/prog.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (turmeric stdlib/vec)
        (only (turmeric geom) area)
        (prefix (turmeric geom) g:))
(define v (vec-new))
(vec-push! v 1)
(vec-push! v 2)
(display (vec-len v)) (newline)
(display (area 2.5 4.5)) (newline)
(display (g:scale 3 10)) (newline)
EOF

run_case "scheme-imports-turmeric" prog.tur "2
11.25
30"

# ---- Direction 2b: `(rename ...)` over a Turmeric module. -------------------
cat > "$TMP/prog2.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (rename (turmeric geom) (area rect-area)))
(display (rect-area 1.5 2.0)) (newline)
EOF

# 1.5 * 2.0 is the inexact 3.0, and R7RS writes an inexact integer with its
# `.0` (R5), where Turmeric's own println would print 3.
run_case "scheme-renames-turmeric" prog2.tur "3.0"

# ---- Direction 2c: strings both ways across the seam (r7rs-lang-plan T3). --
# A Scheme string is a cstr (a literal) or an R7rsString (a mutable one,
# `string-copy` here).  Into a Turmeric `cstr` parameter it crosses as a fresh
# UTF-8 copy; a cstr coming back is an immutable Scheme string.  `echo`
# returns the cstr it was given, so mutating the Scheme string after the call
# shows the copy: the returned string still reads "h" + lambda, and it counts
# characters (2), not the bytes Turmeric sees.
cat > "$TMP/echo.tur" <<'EOF'
(defmodule echo
  (export echo)
  (defn echo [s : cstr] : cstr s))
EOF

cat > "$TMP/prog3.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (only (turmeric echo) echo))
(define s (string-copy "h\x3BB;"))
(define r (echo s))
(string-set! s 0 #\j)
(write (list s r (string-length r))) (newline)
EOF

run_case "strings-cross-the-seam" prog3.tur '("jλ" "hλ" 2)'

# ---- Direction 3: a library exports a global spelled like a Turmeric form. --
# `gen` and `handle` are Turmeric special forms and R7RS names nothing by
# them, so the lowering renames every occurrence -- the library's definition
# and export, the importer's `only` list and uses -- in step
# (r7rs-toplevel-define-named-like-a-turmeric-form).  A Turmeric importer
# could not call a `gen` by that name anyway (TUR-W0042).
cat > "$TMP/genlib.tur" <<'EOF'
#lang r7rs
(define-library (genlib)
  (export gen handle)
  (import (scheme base))
  (begin
    (define gen (lambda () 42))
    (define (handle x) (+ x 1))))
EOF

cat > "$TMP/prog4.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (only (genlib) gen handle))
(write (list (gen) (handle 1))) (newline)
EOF

run_case "form-named-exports" prog4.tur '(42 2)'

# A library global named like a Turmeric builtin type (`any`, `int`) is
# spelled `<name>--user` wherever user code names it -- in the library and in
# every importer alike, whatever the import set -- so the stdlib's type
# annotations never read it (r7rs-srfi-plan S0: chibi's SRFI 1 defines `any`).
cat > "$TMP/tylib.tur" <<'EOF'
#lang r7rs
(define-library (tylib)
  (export any int)
  (import (scheme base))
  (begin
    (define (any pred ls) (cond ((null? ls) #f) ((pred (car ls)) #t) (else (any pred (cdr ls)))))
    (define int 5)))
EOF

cat > "$TMP/prog4b.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (tylib) (prefix (tylib) t:) (rename (only (tylib) int) (int five)))
(write (list (any odd? '(2 3)) int (t:any even? '(1)) t:int five)) (newline)
EOF

run_case "type-named-exports" prog4b.tur '(#t 5 #f 5 5)'

# ---- Direction 4: nested import sets over a user library (R7RS 5.2). -------
# `only`, `except`, `prefix` and `rename` compose in any order; the fold
# unwinds each name to the library's spelling, and a prefixed user module is
# `:as`, a kept list `:refer`, an excluded name the program's own.
cat > "$TMP/prog5.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (prefix (only (mylib) twice) m:)
        (rename (except (mylib) twice) (greet hello))
        (rename (prefix (genlib) g:) (g:gen forty-two)))
(write (list (m:twice 4) (hello "ann") (forty-two))) (newline)
EOF

run_case "nested-import-sets" prog5.tur '(8 "hi ann" 42)'

# ---- Direction 5: `.scm` files (r7rs-lang-plan open question 4). ----------
# No `#lang` line anywhere: the extension is the directive, for the entry
# file and for the library `(import (scmlib))` resolves to `scmlib.scm` when
# no `scmlib.tur` exists.  Scheme truthiness proves the LANGUAGE followed the
# reader (under Turmeric rules `(if 0 ...)` takes the else branch).
cat > "$TMP/scmlib.scm" <<'EOF'
(define-library (scmlib)
  (export thrice)
  (import (scheme base))
  (begin (define (thrice x) (* x 3))))
EOF

cat > "$TMP/prog6.scm" <<'EOF'
(import (scheme base) (scheme write) (scheme read) (scmlib))
(write (list (thrice 5) (if 0 'truthy 'falsy) (read (open-input-string "(a . b)")))) (newline)
EOF

run_case "scm-extension" prog6.scm "(15 truthy (a . b))"

# ---- A library procedure's internal define-record-type. -------------------
# r7rs-define-record-type-not-an-internal-definition: the record's struct and
# procedures are lifted into the library's module (not exported), under fresh
# names the procedure body's scope maps its own names to.
cat > "$TMP/reclib.tur" <<'EOF'
#lang r7rs
(define-library (reclib)
  (export boxed-sum)
  (import (scheme base))
  (begin
    (define (boxed-sum a b)
      (define-record-type <box> (mk v) box? (v box-v))
      (+ (box-v (mk a)) (box-v (mk b))))))
EOF

cat > "$TMP/prog7.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (reclib))
(write (boxed-sum 3 4)) (newline)
EOF

run_case "library-internal-record-type" prog7.tur "7"

# ---- (export (rename internal public)). -----------------------------------
# r7rs-library-file-shape-and-export-rename.  A definition exported once under
# a rename is itself spelled `public` in the module (so it keeps its signature
# and its set!s); one exported twice gets a forwarding defn; an imported name
# gets an `any` alias.  A library's own global named like a public name is
# moved out of the way, and a public name spelled like a Turmeric form goes
# through the clash rename on both sides.
cat > "$TMP/renlib.tur" <<'EOF'
#lang r7rs
(define-library (renlib)
  (export (rename internal-add add)
          twice (rename twice double)
          (rename a b) (rename b a)
          (rename counter-bump bump!) counter-value
          (rename car head)
          (rename my-sub sub)
          (rename my-gen gen)
          uses-own-sub)
  (import (scheme base))
  (begin
    (define (internal-add a b) (+ a b))
    (define (twice x) (* 2 x))
    (define (a) 'was-a)
    (define (b) 'was-b)
    (define counter 0)
    (define (counter-bump) (set! counter (+ counter 1)) counter)
    (define (counter-value) counter)
    (define (sub a b) 'not-exported)
    (define (my-sub a b) (- a b))
    (define (my-gen) 'generated)
    (define (uses-own-sub) (sub 1 2))))
EOF

cat > "$TMP/prog8.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (except (renlib) gen)
        (only (renlib) gen))
(bump!) (bump!)
(write (list (add 2 3) (twice 4) (double 5) (a) (b) (counter-value)
             (head '(p q)) (sub 10 3) (gen) (uses-own-sub)))
(newline)
EOF

run_case "export-rename" prog8.tur "(5 8 10 was-b was-a 2 p 7 generated not-exported)"

cat > "$TMP/tmain2.tur" <<'EOF'
(defmodule tmain2
  (import renlib :refer [add double])
  (defn main [] : int
    (println (cast (add (:: 20 any) (:: 22 any)) int))
    (println (cast (double (:: 21 any)) int))
    0))
EOF

run_case "turmeric-imports-export-rename" tmain2.tur "42
42"

# ---- A library procedure calling one defined further down. ---------------
# untyped-forward-callee-result-retagged-as-pointer: an imported library goes
# through the module path's forward declarations, which typed an unannotated
# (every Scheme) defn's result as the int placeholder, so `caller`'s call of
# the later `callee` was widened to `any` as an int and cc refused it -- from a
# Scheme program and from a Turmeric module alike.
cat > "$TMP/fwdlib.tur" <<'EOF'
#lang r7rs
(define-library (fwdlib)
  (export caller first-of)
  (import (scheme base))
  (begin
    (define (caller x) (list (callee x) (callee (- x))))
    (define (first-of x) (car (caller x)))
    (define (callee x)
      (if (> x 0)
          x
          (let ((v (vector x 'neg)))
            v)))))
EOF

cat > "$TMP/prog9.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (fwdlib))
(write (caller 5)) (newline)
EOF

run_case "library-forward-callee" prog9.tur "(5 #(-5 neg))"

cat > "$TMP/tmain3.tur" <<'EOF'
(defmodule tmain3
  (import fwdlib :refer [first-of])
  (defn main [] : int
    (println (cast (first-of (:: 5 any)) int))
    0))
EOF

run_case "turmeric-imports-forward-callee" tmain3.tur "5"

# ---- A library exporting syntax-rules macros. ------------------------------
# r7rs-define-library-cannot-export-syntax.  The exports `my-rec`, `use-helper`
# and `swap!` are macros; the export check used to refuse them ("exported
# symbol 'my-rec' is not defined in this module").  `use-helper`'s template
# calls a private macro and a helper the library does not export, and quotes
# the helper's name.  The program defines its own `helper`, which must not
# capture the template's (R7RS 4.3.2), and reaches the macros under `only`,
# `prefix` and `rename`.  A second library, in a `.scm` file, uses the first
# one's macro in its own body and exports a macro of its own under
# `(export (rename ...))`.
cat > "$TMP/maclib.tur" <<'EOF'
#lang r7rs
(define-library (maclib)
  (export my-rec twice use-helper swap!)
  (import (scheme base))
  (begin
    (define (twice x) (* 2 x))
    (define (helper x) (* x 100))
    (define-syntax my-rec
      (syntax-rules ()
        ((_ (name . args) body ...) (letrec ((name (lambda args body ...))) name))))
    (define-syntax private-twice (syntax-rules () ((_ e) (twice e))))
    (define-syntax use-helper
      (syntax-rules () ((_ x) (list (helper x) (private-twice x) 'helper))))
    (define-syntax swap!
      (syntax-rules () ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))))
EOF

cat > "$TMP/macouter.scm" <<'EOF'
(define-library (macouter)
  (export fact5 (rename my-when when2))
  (import (scheme base) (maclib))
  (begin
    (define (fact5) ((my-rec (f n) (if (= n 0) 1 (* n (f (- n 1))))) 5))
    (define-syntax my-when (syntax-rules () ((_ c e ...) (if c (begin e ...) #f))))))
EOF

cat > "$TMP/prog10.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (maclib) (macouter))
(define (helper x) 'captured)
(write (list (twice 21)
             ((my-rec (f n) (if (= n 0) 1 (* n (f (- n 1))))) 5)
             (use-helper 7)
             (let ((p 1) (q 2)) (swap! p q) (list p q))
             (fact5)
             (when2 #t 'yes)
             (when2 #f 'yes)))
(newline)
EOF

run_case "library-exports-macros" prog10.tur "(42 120 (700 14 helper) (2 1) 120 yes #f)"

cat > "$TMP/prog11.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (only (maclib) use-helper) (prefix (maclib) m:) (rename (maclib) (swap! exchange!)))
(write (list (use-helper 1)
             ((m:my-rec (g n) (if (= n 0) 0 (+ n (g (- n 1))))) 4)
             (m:twice 5)
             (let ((p 'a) (q 'b)) (exchange! p q) (list p q))))
(newline)
EOF

run_case "library-macros-under-import-sets" prog11.tur "((100 2 helper) 10 10 (b a))"

# A Turmeric module importing that library still gets its procedures; its
# macros are Scheme syntax and are not in the module's exports.
cat > "$TMP/tmain4.tur" <<'EOF'
(defmodule tmain4
  (import maclib :refer [twice])
  (defn main [] : int
    (println (cast (twice (:: 21 any)) int))
    0))
EOF

run_case "turmeric-imports-macro-library" tmain4.tur "42"

# ---- (srfi N) across modules. ----------------------------------------------
# r7rs-srfi-plan S1 (D3).  An SRFI's definitions are spliced in once per
# compile and emitted where every module sees them; its macros are
# registered by every lowering pass that imports it.  So a user library and
# the program can both import (srfi 45) and both use its `lazy` macro, and a
# library can import an SRFI the program never names -- the splice then lands
# in the library's pass, and the program still links.
cat > "$TMP/lazylib.scm" <<'EOF'
(define-library (lazylib)
  (export later-sum ready)
  (import (scheme base) (srfi 45) (srfi 38))
  (begin
    (define (later-sum a b) (lazy (eager (+ a b))))
    (define (ready x) (eager x))))
EOF

cat > "$TMP/prog12.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (srfi 45) (lazylib))
(write (list (force (later-sum 1 2)) (force (lazy (ready 5))) (force (lazy (eager 7)))))
(newline)
EOF

run_case "srfi-in-program-and-library" prog12.tur "(3 5 7)"

cat > "$TMP/prog13.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (lazylib))
(write (list (force (later-sum 10 20)) (force (ready 4))))
(newline)
EOF

run_case "srfi-in-library-only" prog13.tur "(30 4)"

# r7rs-srfi-plan S3: SRFI 1's definitions are pruned to what the whole
# program reaches, after every module is elaborated.  A library that uses
# `fold` and `delete` and a program that uses `filter` and `first` as values
# (fat boxes) and `lset-union` through apply must each keep their own; the
# library's `unused-here` is never called.
cat > "$TMP/listlib.scm" <<'EOF'
(define-library (listlib)
  (export total without unused-here)
  (import (scheme base) (srfi 1))
  (begin
    (define (total xs) (fold + 0 xs))
    (define (without x xs) (delete x xs))
    (define (unused-here xs) (lset-xor eq? xs xs))))
EOF

cat > "$TMP/prog14.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (srfi 1) (listlib))
(define pick first)
(write (list (total (iota 5)) (without 2 '(1 2 3 2)) (map pick '((a b) (c d)))
             ((lambda (f) (f odd? '(1 2 3))) filter) (apply lset-union eq? '((a b) (b c)))))
(newline)
EOF

run_case "srfi-1-program-and-library" prog14.tur "(10 (1 3) (a c) (1 3) (c a b))"

cat > "$TMP/prog15.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (listlib))
(write (list (total '(1 2 3)) (without 'a '(a b a))))
(newline)
EOF

run_case "srfi-1-in-library-only" prog15.tur "(6 (b))"

# r7rs-prelude-procedures-lose-identity: a standard procedure a library names
# is the one the program names -- one adaptor per compile, not per module --
# so a table the library keys on `car` answers the program's `car` (SRFI 17's
# `setter` is exactly this).
cat > "$TMP/idlib.tur" <<'EOF'
#lang r7rs
(define-library (idlib)
  (export get-car lookup)
  (import (scheme base))
  (begin
    (define (get-car) car)
    (define table (list (cons car 'set-car!) (cons cdr 'set-cdr!)))
    (define (lookup p) (let ((e (assv p table))) (and e (cdr e))))))
EOF

cat > "$TMP/prog16.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (idlib))
(write (list (eqv? car (get-car)) (lookup car) (lookup cdr) (lookup vector-ref)))
(newline)
EOF

run_case "library-shares-procedure-identity" prog16.tur "(#t set-car! set-cdr! #f)"

# r7rs-library-defines-standard-or-stdlib-name: a library may define a
# standard name (`square`, `abs`) or an auto-loaded stdlib one (`None`,
# `list-length`) and export it, plainly or as a rename's public name.  It
# used to be "'r7rs-square' is already defined by an auto-loaded stdlib
# module" compiled, and unreachable -- or the stdlib's `list-length` replaced
# for everyone -- interpreted.  The module spells each `<name>--user`, and an
# importer learns that from the library's source, under any import set.  The
# library's own uses mean its definition; the program's other uses of a
# standard name it did not import from the library still mean R7RS's.
cat > "$TMP/stdnlib.tur" <<'EOF'
#lang r7rs
(define-library (stdnlib)
  (export square None list-length (rename my-cube cube) (rename my-abs abs) square-of-three)
  (import (scheme base))
  (begin
    (define (square x) (list 'mine x))
    (define (None) 'none)
    (define (list-length l) 77)
    (define (my-cube x) (* x x x))
    (define (my-abs x) (list 'abs x))
    (define (square-of-three) (square 3))))
EOF

cat > "$TMP/prog17.tur" <<'EOF'
#lang r7rs
(import (except (scheme base) square abs) (scheme write) (stdnlib))
(write (list (square 1) (None) (list-length '(1 2)) (cube 2) (abs -1) (square-of-three) (length '(1 2))))
(newline)
EOF

run_case "library-defines-standard-and-stdlib-names" prog17.tur "((mine 1) none 77 8 (abs -1) (mine 3) 2)"

cat > "$TMP/prog18.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (prefix (stdnlib) s:)
        (rename (only (stdnlib) None list-length) (None nothing)))
(write (list (s:square 2) (s:None) (nothing) (list-length '()) (square 3) (s:abs 5) (abs -5)))
(newline)
EOF

run_case "library-standard-names-under-import-sets" prog18.tur "((mine 2) none none 77 9 (abs 5) 5)"

# ---- A Turmeric module calls a Scheme procedure it holds as `any`
# (no-public-dynamic-call-for-any-values).  `(f a b)` on an `any` is a dynamic
# call in every dialect (868fea84); what this pins beyond
# tests/fixtures/r7rs-turmeric-calls-scheme-procedure is the failure side: a
# wrong argument count and a non-procedure raise Scheme error objects that the
# Scheme caller can `guard`, on both back ends.
cat > "$TMP/callit.tur" <<'EOF'
(defmodule callit
  (export call0 call2)
  (defn call0 [f : any] : any (f))
  (defn call2 [f : any a : any b : any] : any (f a b)))
EOF

cat > "$TMP/prog19.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (turmeric callit))
(define (msg thunk)
  (guard (e ((error-object? e) (error-object-message e))) (thunk)))
(write (list (call0 (lambda () 42))
             (call2 (lambda (x y) (list y x)) 1 "two")
             (call2 + 3 4)
             (msg (lambda () (call2 (lambda (x) x) 1 2)))
             (msg (lambda () (call0 5)))))
(newline)
EOF

run_case "turmeric-calls-scheme-procedure-errors" prog19.tur '(42 ("two" 1) 7 "wrong number of arguments (2 given)" "not a procedure")'

if [ $FAILED -ne 0 ]; then
    echo "run-r7rs-import: FAILED"
    exit 1
fi
