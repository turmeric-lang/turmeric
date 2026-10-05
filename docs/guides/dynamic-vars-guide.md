---
title: Dynamic Vars Guide
category: Concurrency and Async
description: Thread-local, dynamically-scoped mutable cells with defdynamic and binding
---

# Dynamic Vars Guide

Dynamic vars are typed, thread-local, dynamically-scoped mutable cells.
They let you pass implicit context (log level, locale, request ID) through
deep call chains without threading an explicit parameter everywhere.

## Quick Start

```turmeric
;; Declare a dynamic var at module toplevel
(defdynamic *log-level* :int 1)

;; Override for a dynamic extent
(binding [*log-level* 0]
  (do-work))

;; Read anywhere -- no parameter required
*log-level*   ; => current binding, or root value if none active
```

```sweet-exp
;; Declare a dynamic var at module toplevel
defdynamic *log-level* :int 1

;; Override for a dynamic extent
binding [*log-level* 0]
  do-work()

;; Read anywhere -- no parameter required
*log-level*   ; => current binding, or root value if none active
```

## Core Forms

### `defdynamic`

```turmeric
(defdynamic *name* :type root-expr)
```

```sweet-exp
defdynamic *name* :type root-expr
```

Declares a dynamic var with a fixed declared type and a root value. The root
value is the initial value visible on any thread that has never called
`binding` for this var.

- Must appear at module toplevel (not inside a function or `let`).
- The type is fixed; all overrides must match exactly.
- Substructural types (`^linear`, `^affine`, `^unique`) are rejected --
  dynamic vars may be read by any number of callers.

By convention, names use `*earmuffs*` to signal dynamic scope. The compiler
warns (`TUR-W0600`) if earmuffs are omitted.

### `binding`

```turmeric
(binding [*var1* expr1
          *var2* expr2]
  body)
```

```sweet-exp
binding [*var1* expr1
         *var2* expr2]
  body
```

Pushes an override frame for each var on the current thread, evaluates
`body`, then pops all frames before returning. The override is visible to
all code called transitively from `body` -- not just lexically enclosed code.

Multiple vars in one `binding` form are all pushed before `body` runs.

Binding forms compose: inner forms take precedence over outer ones:

```turmeric
(binding [*log-level* 1]
  (binding [*log-level* 3]
    *log-level*)   ; => 3
  *log-level*)     ; => 1
```

```sweet-exp
binding [*log-level* 1]
  binding [*log-level* 3]
    *log-level*   ; => 3
  *log-level*     ; => 1
```

### `set!`

```turmeric
(binding [*log-level* 1]
  (set! *log-level* 4)
  *log-level*)   ; => 4
```

```sweet-exp
binding [*log-level* 1]
  set!(*log-level* 4)
  *log-level*   ; => 4
```

Mutates the current thread's top binding frame for the var. Requires an
active binding frame on the current thread (`TUR-E0601` otherwise). The
root value is never mutated by `set!`.

`set!` is rejected inside `atomically` blocks (`TUR-E0605`): STM
transactions may be retried, but binding stack mutations cannot be rolled
back.

## Thread Isolation

Each thread has its own binding stack. A `binding` in one thread does not
affect any other thread's view of the var.

```turmeric
(defdynamic *locale* :cstr "en-US")

(defn show-locale [] : int
  (println *locale*))

(defn use-fr [] : int
  (binding [*locale* "fr-FR"]
    (show-locale))   ; show-locale sees "fr-FR" from its callsite
  0)

(use-fr)             ; prints fr-FR
(show-locale)        ; prints en-US -- binding was restored when use-fr returned
```

```sweet-exp
defdynamic *locale* :cstr "en-US"

defn show-locale [] :int
  println(*locale*)

defn use-fr [] :int
  binding [*locale* "fr-FR"]
    show-locale()   ; show-locale sees "fr-FR" from its callsite
  0

use-fr()             ; prints fr-FR
show-locale()        ; prints en-US -- binding was restored when use-fr returned
```

A spawned thread (via `spawn`) starts with no binding frames; all var reads
return root values.

## Conveying Bindings to Child Threads

`spawn-conveying` (from `stdlib/dynvar.tur`) starts a new thread with a
snapshot of the parent's current binding frame:

```turmeric
(load "stdlib/dynvar.tur")

(defdynamic *request-id* :cstr "none")

(defn thread-join [t : ptr<void>] : nil ...)  ; see stdlib/thread.tur

(defn main [] : int
  (binding [*request-id* "req-1"]
    (let [t (spawn-conveying (fn [] (println *request-id*)))]
      (set! *request-id* "req-2")   ; parent changes its own binding
      (thread-join t)                ; child printed "req-1" (snapshot)
      (println *request-id*)))       ; prints "req-2"
  0)
```

```sweet-exp
load("stdlib/dynvar.tur")

defdynamic *request-id* :cstr "none"

defn thread-join [t :ptr<void>] :nil ...  ; see stdlib/thread.tur

defn main [] :int
  binding [*request-id* "req-1"]
    let [t (spawn-conveying (fn [] println(*request-id*)))]
      set!(*request-id* "req-2")   ; parent changes its own binding
      thread-join(t)                ; child printed "req-1" (snapshot)
      println(*request-id*)         ; prints "req-2"
  0
```

The snapshot is a **copy** of each var's current top-of-stack value taken at
spawn time. Parent and child binding stacks are independent afterwards.

## Common Stdlib Vars

`stdlib/dynvar.tur` declares four standard dynamic vars:

| Var | Type | Default | Purpose |
|---|---|---|---|
| `*log-level*` | `:int` | `1` | Logging verbosity (0 = verbose, 1 = info, 2 = warn, 3 = error) |
| `*locale*` | `:cstr` | `"en-US"` | BCP-47 locale for formatting and i18n |
| `*random-seed*` | `:int` | `0` | RNG seed; 0 = use system entropy |
| `*current-module*` | `:cstr` | `""` | Module name for structured log output |

Load the file to get both the vars and `spawn-conveying`:

```turmeric
(load "stdlib/dynvar.tur")
```

```sweet-exp
load("stdlib/dynvar.tur")
```

## Effects vs. Dynamic Vars

Both algebraic effects and dynamic vars solve the "implicit context" problem.
The right tool depends on whether the context is configuration (rarely
overridden, never intercepted) or an interceptable operation.

### Use dynamic vars when

- The value is almost always the root: log level, locale, feature flags.
- You want zero call-site annotation: any function can read the var without
  declaring anything in its signature.
- You want snapshot isolation across threads (`spawn-conveying`).
- You don't need multi-shot resumption or handler composition.

```turmeric
;; Dynamic var: no call-site annotation and no per-boundary handler
(defdynamic *log-level* :int 1)

(defn log-info [msg : cstr] : int
  (if (>= *log-level* 1) (println msg) 0))

(defn process [] : int
  (log-info "processing")
  42)

(binding [*log-level* 0]   ; one override, deep stack covered automatically
  (process))
```

```sweet-exp
;; Dynamic var: zero call-site annotation; no boundary handler required
defdynamic *log-level* :int 1

defn log-info [msg :cstr] :int
  if {*log-level* >= 1}
    println(msg)
    0

defn process [] :int
  log-info("processing")
  42

binding [*log-level* 0]   ; one override, deep stack covered automatically
  process()
```

### Use effects when

- The operation has multiple implementations (prod vs. mock, local vs. remote).
- You want the compiler to enforce that a handler is installed at each
  architectural boundary (effect rows in types).
- You need multi-shot resumption (e.g. backtracking, generators).
- The handler needs to observe every invocation.

```turmeric
(defeffect DbEffect (query [sql :str] :str))

(defn run-tests [] : unit
  (handle
    assert!({query("SELECT 1") = "1"})
    [(DbEffect.query sql k)
     (resume k (mock-db-exec sql))]))
```

```sweet-exp
defeffect DbEffect query([sql :str] :str)

defn run-tests [] :unit
  (handle
    (assert! {(query "SELECT 1") = "1"})
    [(DbEffect.query sql k) (resume k (mock-db-exec sql))])
```

### Comparison

| Property | `binding`/`defdynamic` | `perform`/`handle` |
|---|---|---|
| Call-site annotation | None | `perform` required |
| Compile-time enforcement | Type of override must match | Effect row must be handled |
| No-override cost | Single global load | Effect row overhead |
| Multi-shot resumption | No | Yes |
| Thread isolation | Automatic (per-thread stack) | Manual (handler in thread body) |
| Cross-thread conveyance | `spawn-conveying` | Not built-in |
| Interceptable per-call | No | Yes |

The two mechanisms are orthogonal. `binding` may appear inside `handle`
bodies and `perform` may appear inside `binding` bodies without interaction.

## Common Patterns

### Scoped log level

```turmeric
(defdynamic *log-level* :int 1)

(defn log-debug [msg : cstr] : int
  (if (= *log-level* 0) (println msg) 0))

(defn run-verbose [thunk] : int
  (binding [*log-level* 0]
    (thunk)))
```

```sweet-exp
defdynamic *log-level* :int 1

defn log-debug [msg :cstr] :int
  if {*log-level* = 0}
    println(msg)
    0

defn run-verbose [thunk] :int
  binding [*log-level* 0]
    thunk()
```

### Test fixture injection

```turmeric
(defdynamic *db* :int 0)   ; :int as a stand-in for a connection handle

(defn query [sql : cstr] : int
  *db*)

(defn run-tests [] : int
  (binding [*db* 42]   ; inject test connection
    (println (query "SELECT 1")))
  0)
```

```sweet-exp
defdynamic *db* :int 0   ; :int as a stand-in for a connection handle

defn query [sql :cstr] :int
  *db*

defn run-tests [] :int
  binding [*db* 42]   ; inject test connection
    println(query("SELECT 1"))
  0
```

### Module-tagged structured logging

```turmeric
(load "stdlib/dynvar.tur")   ; provides *current-module*

(defn log [msg : cstr] : int
  (println *current-module*)
  (println msg))

(defn start-auth-module [] : int
  (binding [*current-module* "auth"]
    (log "starting"))
  0)
```

```sweet-exp
load("stdlib/dynvar.tur")   ; provides *current-module*

defn log [msg :cstr] :int
  println(*current-module*)
  println(msg)

defn start-auth-module [] :int
  binding [*current-module* "auth"]
    log("starting")
  0
```

## Error Reference

| Code | Message |
|---|---|
| `TUR-E0600` | `set!` or `binding` target is not a dynamic var |
| `TUR-E0601` | `set!` with no active `binding` frame on the current thread |
| `TUR-E0602` | Override type does not match the `defdynamic` declared type |
| `TUR-E0603` | Dynamic var declared with a substructural (linear/affine/unique) type |
| `TUR-E0604` | `defdynamic` at non-toplevel position |
| `TUR-E0605` | `set!` on a dynamic var inside an `atomically` block |
| `TUR-W0600` | `defdynamic` name does not use `*earmuffs*` convention |

Run `tur explain TUR-E0600` (etc.) for full explanations with examples.

## See also

- [mutable-globals-guide.md](mutable-globals-guide.md) -- `def ^mut`, `^atomic`, `^thread-local`, and what the concurrency annotations do not cover
