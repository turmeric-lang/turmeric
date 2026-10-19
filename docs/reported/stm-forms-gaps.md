# STM: a bare `stm` crashes the compiled program, `retry` does not end the attempt, and a nested `atomically` is silently separate

**Severity: medium.** Found 2026-10-19 checking the examples for the STM forms'
API reference (`stdlib/forms/stm.tur`).

## 1. `stm` outside `atomically` segfaults when compiled

```turmeric
(defn main [] : int
  (let [tv (tvar/new 5)]
    (println (:: (stm (tvar/read tv)) :int)))      ; compiled: Segmentation fault
  0)
```

Interpreted: `tvar/read used outside of an atomically block` -- a usable error.
`(stm 1 2 3)` alone is the same (crash compiled, `3` interpreted). The compiler
accepts the form because the `tvar/*` check is only that a `stm` block is
lexically around them (TUR-E0009), not that an `atomically` runs it.

## 2. `retry` does not end the attempt

```turmeric
(println (atomically (stm (retry) (println "after") 1)))
```

Compiled: `after` is printed about a thousand times a second, forever. `retry`
only sets a flag; the rest of the body still runs, the attempt is then
re-executed, and because no TVar was read the read set is empty, so the thread
never parks and spins. With a TVar read first (`(when (= (:: (tvar/read tv) :int)
0) (retry))`) it parks until another thread commits (`stm-retry-wakeup`), which
is the intended use; the surprise is the code after `retry` and the spin. In
Haskell `retry` aborts the attempt at once. Interpreted, the same program stops
with `transaction requested retry with no way to make progress`.

## 3. A nested `atomically` is silently a separate transaction

```turmeric
(let [tv (tvar/new 0)]
  (println (:: (atomically (stm (tvar/write tv 3)
                                (atomically (stm (tvar/read tv))))) :int))   ; 0, not 3
  )
```

The inner `atomically` does not see the outer's uncommitted write, and an inner
write commits first and is then overwritten by the outer's commit:
`(atomically (stm (tvar/write tv 3) (atomically (stm (tvar/write tv 5)
(tvar/read tv)))))` returns 5 and leaves `3` in the TVar. Both engines agree.
GHC rejects nested `atomically`; here it is accepted with no diagnostic.

## Fix directions

1. Reject a `stm` that is not under an `atomically` at compile time (or make the
   compiled runtime report the interpreter's error).
2. Make `retry` leave the attempt immediately (a non-local exit like `panic`'s
   unwind), and park even with an empty read set only if that is meant.
3. Diagnose a lexically nested `atomically`, or make it join the outer
   transaction.
4. Fixtures for all three on both engines.
