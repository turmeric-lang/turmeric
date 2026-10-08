---
title: Typing Handles, Callbacks, and Results -- Avoiding `:int` Stand-Ins
category: Language Basics
description: How to type opaque handles, callbacks, and option/result values properly in Turmeric. Concrete patterns and fix recipes drawn from the 2026-06-14 spice ecosystem audit.
---

# Typing Handles, Callbacks, and Results

`CLAUDE.md` carries the strict rule -- **"No Lazy `:int` Stand-Ins"** --
that bans typing handles, callbacks, options, and results as `:int`
"because it's a pointer under the hood." This guide is the long-form
counterpart: *why* the rule exists, *what* the right shapes are for
each case, and *how* to migrate an API that already leans on `:int`.

The companion audit
([`docs/archive/spices-int-stand-in-audit-2026-06-14.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/spices-int-stand-in-audit-2026-06-14.md))
caught the pattern across 19 of 35 spices. The audit is the source of
truth for the historical numbers; this guide is the playbook.

## Why `:int` stand-ins are the wrong default

A spice exports `db-close [db : int]`, `db-exec [db : int sql : cstr]`,
`db-prepare [db : int sql : cstr]`. To the elaborator these signatures
say nothing about what `db` is. Three problems follow:

1. **Argument swaps compile silently.** Pass a `Stmt` where a `Db` is
   expected and it typechecks. The bug surfaces as a segfault or a
   confusing SQLite error at runtime.
2. **Downstream looseness propagates.** Every spice that wraps the DB
   inherits `:int` because that's what upstream returns. Each
   downstream is a fresh place a swapped argument compiles silently.
3. **Doc-only contracts rot.** "This callback should have shape
   `(fn [Request] Response)`" lives in prose. The compiler can't
   enforce it, so wrong-arity or wrong-type handlers ship.

The audit's S1 row -- five callbacks typed `:int` -- is the sharpest
case. A web framework's middleware registration accepting `fn : int`
silently swallows handlers of any shape; a wrong handler shape becomes
a runtime "no response was generated" debug session instead of a
compile error.

## The four cases and their fixes

The audit organised findings into a severity rubric. The same rubric
gives you the fix template by case.

### S1 -- Callbacks (`fn : int`)

**Wrong:**

```turmeric
(defn use! [ctx : int fn : int] : nil ...)
```

`fn` is a function pointer that the docs say should be
`(c-fn [Ctx] (Option Response))`. Anything compiles.

**Right -- write out the callback type:**

```turmeric
(defn use! [ctx : Ctx fn : (c-fn [Ctx] (Option Response))] : nil ...)
```

Or, when the same callback shape appears in many places, give it a
name:

```turmeric
(defopaque Handler (c-fn [Ctx] Response))

(defn get!    [path : cstr h : Handler] : Item ...)
(defn post!   [path : cstr h : Handler] : Item ...)
(defn put!    [path : cstr h : Handler] : Item ...)
(defn delete! [path : cstr h : Handler] : Item ...)
```

The named-newtype form is what every shipping spice settled on
(`tour-tourist`, `tur-httpd`, `tur-rtmidi`, `tur-osc`). It scans better
at call sites and gives you one place to evolve the shape.

For raw C-ABI callbacks (`qsort` comparators, signal handlers, audio
streams) the type is `c-fn` with the literal C signature; the body
must be `#fx{Unsafe}` and the defn `#[used]` so the symbol survives
DCE. See [c-integration-guide.md](c-integration-guide.md) for the
inline-C side.

### S2 -- Opaque handles (`db : int`, `ctx : int`, `tex : int`)

**Wrong:**

```turmeric
(defn db-close   [db : int] : nil ...)
(defn db-exec    [db : int sql : cstr] : int ...)
(defn db-prepare [db : int sql : cstr] : int ...)
```

**Right -- declare a `defopaque` newtype and thread it everywhere:**

```turmeric
;; In db.tur (the module that owns the handle):
(defopaque Db   :ptr<void>)
(defopaque Stmt :ptr<void>)

(export Db Stmt)

(defn db-open    [path : cstr]                  : (Result Db cstr) ...)
(defn db-close   [db   : Db]                    : nil              ...)
(defn db-exec    [db   : Db sql  : cstr]        : (Result int cstr) ...)
(defn db-prepare [db   : Db sql  : cstr]        : (Result Stmt cstr) ...)
(defn stmt-step  [stmt : Stmt]                  : (Result int cstr) ...)
```

Three rules of thumb when picking the carrier for the `defopaque`:

| The handle is... | Carrier to use |
|---|---|
| An externally-allocated C pointer (SQLite, raylib, etc.) | `:ptr<void>` -- always. |
| Owned-by-us layout with a `defstruct` | `:ptr<MyStruct>` -- you get field projection for free. |
| Genuinely an integer index, not a pointer (a slot id, a file descriptor) | `:int` is the LAST resort -- still wrap in a `defopaque` so the type checker can distinguish "fd" from "uint32 you just happened to have." |

Two things to watch for when migrating:

- **Failure sentinels.** Many C handles use `NULL`/`0` as "failed to
  allocate." Once the handle is a `defopaque`, callers can no longer
  spell `(= ctx 0)` -- it doesn't typecheck. Export a predicate
  (`tls-ctx-null?`, `db-null?`) so callers don't have to break the
  abstraction:

  ```turmeric
  (defn db-null? [db : Db] : bool
    ```c
    return (void*)db == NULL;
    ```)
  ```

  Better still, return a `Result` from the constructor and skip the
  sentinel entirely -- see the S3 section.

- **Coercion at the C boundary.** Inline-C bodies inside the spice
  receive the `defopaque` as its carrier type. A `defopaque Db
  :ptr<void>` lands as `void *db` in the C body, ready for
  `sqlite3_close((sqlite3*)db)`. No `intptr_t` round-trip needed.

### S3 -- Optionals and results (`result : int`)

**Wrong:**

```turmeric
(defn req-header  [req : Request name : cstr] : int ...)
(defn param       [ctx : Ctx     name : cstr] : int ...)
(defn mw-chain-run [mws : list   ctx  : Ctx]  : int ...)
```

The internal C body is already producing a `(Result cstr cstr)` or
`(Option Response)`; the signature just lies about it. Every caller is
forced into inline-C to unwrap.

**Right -- declare the real shape:**

```turmeric
(defn req-header   [req : Request name : cstr] : (Result cstr cstr) ...)
(defn param        [ctx : Ctx     name : cstr] : (Result cstr cstr) ...)
(defn mw-chain-run [mws : (list Middleware) ctx : Ctx] : (Option Response) ...)
```

Caller side becomes ordinary pattern matching:

```turmeric
(match (req-header r "X-Token")
  ((ok token)  (use-token token))
  ((err what)  (fail what)))
```

And exhaustiveness checking comes back: forgetting the `(err ...)` arm
is a compile error now, not a runtime crash on the missing header
case.

If the underlying C function genuinely cannot fail, return `T`
directly; don't reach for `Result` "for symmetry." The right type is
the most precise one.

### S4 -- Cons lists (`xs : int`)

**Wrong:**

```turmeric
(defn __mw-cons-head [xs : int] : int ...)
(defn __mw-cons-tail [xs : int] : int ...)
(defn __mw-chain-run [mws : int ctx : Ctx] : (Option Response) ...)
```

These are internal helpers that walk a linked list of Middleware
values. `:int` is a pointer to a cons cell of `int` heads -- which
also lie about what's stored.

**Right -- type the list and its element:**

```turmeric
(defn __mw-cons-head [xs : (list Middleware)] : Middleware ...)
(defn __mw-cons-tail [xs : (list Middleware)] : (list Middleware) ...)
(defn __mw-chain-run [mws : (list Middleware) ctx : Ctx] : (Option Response) ...)
```

For internal helpers in `#fx{Unsafe}` blocks the cons cell layout
(`struct { int64_t head; int64_t tail; }`) is documented in
`CLAUDE.md` under "Cons-list manipulation in `#fx{Unsafe}` code." The
declared element type is still required -- variadic rest with a real
element type is type-checked, so `& xs : Middleware` rejects mixed
calls at the call site rather than reading them back as opaque `:int`.

## A migration recipe for an existing spice

When you take an existing `:int`-leaking spice to its v0.2.0 cut, the
order that worked across the audit was:

1. **Pick the handle types first.** Add `defopaque`s for every
   distinct kind of handle the spice exposes (Db, Stmt, Ctx, Texture,
   ...). Export them from a single `types` module so downstream spices
   can re-use them without re-deriving.
2. **Update constructors to return `Result`.** Each "constructor" that
   used `NULL` as a failure sentinel becomes
   `[...] : (Result Handle cstr)` -- the cstr carries the underlying
   library's error message. Callers stop spelling `(= h 0)` and start
   spelling `(match (open path) ((ok h) ...) ((err msg) ...))`.
3. **Sweep accessor signatures next.** Every `[h : int]` that takes
   the handle becomes `[h : Handle]`. Mechanical; the bodies don't
   change unless they were doing manual `intptr_t` casts that the
   `defopaque` carrier now removes.
4. **Sweep callbacks last.** S1 is the largest blast radius -- it
   forces every downstream registration site to update. Land it once
   the handle types are in place so the callback signature
   (`(c-fn [Ctx] Response)`) can use them.
5. **Add swap-reject probes.** A test fixture under `tests/errors/`
   per spice that *intentionally* swaps the argument types and
   confirms the compiler now rejects it. These are the regression nets
   that keep the looseness from creeping back.

Concrete examples to copy from:

- `tur-postgres` v0.2.0 -- `defopaque Conn` + `Rows` + `Notification`,
  all `db-*` / `stmt-*` / `notify-*` retyped, internal `__pq-*-raw`
  helpers also typed.
- `tur-httpd` v0.2.0 -- `defopaque Request` + `Response` + `Wbuf`,
  `req-header` returning `(Result cstr cstr)`, handler typed
  `(c-fn [Request] Response)`.
- `tur-tourist` v0.2.0 / v0.2.1 / v0.2.6 -- `Ctx` / `Response` /
  `Item` opaques, middleware retype, internal route/router accessors
  cleaned in v0.2.6.

## Anti-patterns to walk past

The audit caught these specific shortcuts. Don't take them.

- **"It's an int handle, so I'll just type it `:int` with a comment."**
  The comment doesn't help the type checker. Wrap in a `defopaque`
  even if the carrier truly is `:int`.
- **"I'll define `(defopaque Db :int)` privately and not export it, so
  the public API stays `:int`."** This is the failure mode the audit
  was measuring. The public API is exactly where the type matters --
  it's where downstream spices read it.
- **"I'll accept `:int` and cast inside the body."** Every downstream
  spice will inherit `:int` and do the same. The cast moves around;
  the bug stays.
- **"Variadic rest of opaque handles -- declare it `:int` and cast in
  the body."** Not needed: variadic rest with a real opaque /
  struct / ADT element type is type-checked at the call site. Write
  `& routes : Route` and a wrong-handle caller fails to elaborate.
- **"Use a status-code `:int` return because the caller might want a
  bool."** A `Result` / `Option` carries strictly more information,
  and a bool projection (`(ok? r)`) is one line. Status-code ints
  bring back the "what does 0 mean" problem the type system already
  solved.

## When `:int` is actually right

The rule has one legitimate exception, called out in CLAUDE.md: when
the value is *literally a machine integer with no further structure*.
Examples:

- A byte count (`[len : int]`), a port number (`[port : int]`).
- A file descriptor on Unix -- it really is an int the kernel hands
  you. Optional refinement: `(defopaque Fd :int)` to make swaps with
  another int harder, but `:int` is acceptable.
- A "raw byte" parameter in a low-level helper.

Everything else -- handles, callbacks, options, results, sums, tagged
unions, cons cells, struct handles -- gets a real type.

## Where to look next

- `CLAUDE.md` -- "No Lazy `:int` Stand-Ins -- STRICT RULE" (the
  one-line rule).
- [`opaques-guide.md`](opaques-guide.md) -- the `defopaque` mechanism
  in detail.
- [`fat-closure-annotation-guide.md`](fat-closure-annotation-guide.md)
  -- function pointer typing, fat closures, callback ABI.
- [`error-handling-guide.md`](error-handling-guide.md) -- `Result` /
  `Option` patterns at the API boundary.
- [`c-integration-guide.md`](c-integration-guide.md) -- inline-C,
  `#[used]`, and FFI bridging.
- [`docs/archive/spices-int-stand-in-audit-2026-06-14.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/spices-int-stand-in-audit-2026-06-14.md)
  -- the original ecosystem-wide audit with per-spice findings.
