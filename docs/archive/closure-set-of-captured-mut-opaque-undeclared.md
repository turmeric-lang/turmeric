# `set!` of a captured `^mut` opaque inside a closure emits an undeclared identifier

**Severity: medium** (compile failure in the emitted C).

**Status: OPEN.** Found 2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/xml/tests/sax_events.tur`
(the retained String is stashed through C).

## Repro

````turmeric
(defopaque S :ptr<void>)
(defn __mk [] : ptr<void>
  ```c
  return (void *)"x";
  ```)
(defn mk [] : S (:: (__mk) S))
(defn call [f : (fn [int] int)] : int (f 1))
(defn main [] : int
  (let [^mut kept (mk)]
    (call (fn [x : int] : int (do (set! kept (mk)) x)))
    (println "ok"))
  0)
````

```
error: use of undeclared identifier 'kept_8'
```

The same shape with `^mut n 0` (an `int`) promotes `n` to a shared cell and
works -- the SAX tests count events that way. With an opaque, the promotion
does not happen and the closure body refers to the original local.
