# `(Ok nil)` cannot be built in Turmeric code -- `(Result nil E)` only comes from inline-C

**Severity:** low (expressiveness hole; a workaround exists).

## Summary

A `(Result nil E)` return type is legal and inline-C bodies build one with
`tur_ok_int(0)`, and callers `match` it fine. But a Turmeric-bodied function
cannot construct the ok side: `nil` in expression position is `:void`, so
`(Ok nil)` fails in the C compiler, and binding it first is TUR-E0023.

## Repro

```turmeric
(defopaque IoError :int)
(defn a [n : int] : (Result nil IoError)
  (Ok nil))                      ; cc: error: invalid use of void expression
(defn main [] : int
  (match (a 3) (Ok _) (println "ok") (Err e) (println "e"))
  0)
```

`(let [u nil] (Ok u))` is TUR-E0023 ("cannot bind 'u' to an expression of
type :void"); `(:: (Ok nil) (Result nil IoError))` hits the same cc error.
A related hole: calling a function returning `(Result nil E)` BEFORE its
definition (mutual recursion) is a TUR-E0012 kind mismatch reported at 0:0 --
"cannot apply a type of kind '*' as a type constructor" -- even with no
`(Ok nil)` anywhere. The same shape with `(Result int E)` compiles:

```turmeric
(load "stdlib/io-error.tur")
(defn wa [i : int] : (Result nil IoError)
  (if (> i 3) (io-error/ok-unit) (wb (+ i 1))))   ; forward ref -> TUR-E0012
(defn wb [i : int] : (Result nil IoError) (wa i))
(defn main [] : int 0)
```

`fs/walk-names` (stdlib/fs.tur) is written self-recursive to avoid it.

## Workaround in tree

`stdlib/io-error.tur` carries an inline-C `io-error/ok-unit` returning
`tur_ok_int(0)`; Turmeric-bodied fs/process functions (e.g. `fs/walk-fn`)
return through it.

## Fix directions

Treat `nil` as a first-class unit value when it is a constructor argument (an
`int64_t 0` payload, which is what `tur_ok_int(0)` produces), or add a `unit`
value/type and steer `(Result nil E)` users to it.
