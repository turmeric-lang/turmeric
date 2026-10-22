# A combinator's closure env leaks when its caller is CPS-emitted

**Severity: low (leak; one env, plus the closures it holds, per call).** Found
2026-10-22 resolving
[struct-param-closure-env-never-freed](../archive/struct-param-closure-env-never-freed.md):
with that fixed, `examples/datalog/minimal.tur` still leaks 104 B under
LeakSanitizer (`use_stacks=0:use_registers=0`), all from its one combined
query, `(db-q db (q-and (q-attr ...) (q-not (q-entity 2))))`, in a `demo` the
CPS backend emits.

## Repro

```turmeric
(defstruct D :copy [e : int x : int])
(defeffect Tick [] : nil)
(defn count-if [n : int pred : (fn [D] bool)] : int
  (let [^mut i 0 ^mut c 0]
    (while (< i n) (when (pred (D i 0)) (set! c (+ c 1))) (set! i (+ i 1)))
    c))
(defn q-e [e : int] : (fn [D] bool) (fn [d : D] : bool (= (.e d) e)))
(defn q-not [p : (fn [D] bool)] : (fn [D] bool) (fn [d : D] : bool (not (p d))))
(defn demo [] : int
  (perform (Tick))                           ; makes demo CPS-colored
  (println (count-if 10 (q-e 3)))            ; 1 -- freed
  (println (count-if 10 (q-not (q-e 3))))    ; 9 -- leaks
  0)
(defn main [] : int
  (handle (demo) (Tick [] k) (resume k nil))
  0)
```

`Direct leak of 24 byte(s)` from `q_hynot` and `Indirect leak of 24 byte(s)`
from `q_hye` (the closure `q-not`'s env holds). The plain factory call one line
up is freed, and the SAME two lines in a direct-emitted function (no `perform`)
leak nothing -- so the direct path drops the combinator's env and its drop glue
releases the capture, and the CPS path does neither for this shape.

## Fix directions

1. Compare the two paths' handling of the hoisted `__borrowc` let for an
   argument that is a call to a closure-returning function whose argument is
   itself a fresh closure (`(q-not (q-e 3))`): which test admits it on the
   direct path (`let_binding_env_freeable` / `arg_is_freeable_closure_source`)
   and what the CPS translation of the let does with it.
2. A `requires.leak-check` fixture: the repro above, plus `q-and` of two
   factories.
