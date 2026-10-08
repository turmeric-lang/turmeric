---
title: STM Guide
category: Concurrency and Async
description: Software transactional memory -- API reference and mechanics
---

# STM Guide

Reference guide for Software Transactional Memory -- composable, deadlock-free concurrent state.

## Overview

**Software Transactional Memory (STM)** replaces manual lock management with transactions. A transaction either commits atomically or rolls back and retries, with no risk of deadlock. Turmeric's STM is modeled on Haskell's `Control.Concurrent.STM`.

For a conceptual walkthrough and worked examples, see the [STM Tutorial](stm-tutorial.md). This guide focuses on the API and mechanics.

## Core Concepts

| Concept | Description |
|---|---|
| `TVar` | A mutable cell readable/writable only inside a transaction |
| `stm` | Groups TVar operations into one transaction block |
| `atomically` | Runs an `stm` block; retries automatically on conflict |
| `retry` / `check` | Voluntarily abort and block until a watched `TVar` changes |
| `or-else` | Try one branch; if it retries, try the other |

## TVar Lifecycle

The STM forms (`tvar/*`, `stm`, `atomically`, `retry`, `check`, `or-else`)
are built into the compiler -- no load or import is required.

```turmeric
;; Create a TVar with an initial value
(def counter (tvar/new 0))

;; Free when no longer needed (only when no transactions reference it)
;; via inline C over tur_tvar_free if needed
```

```sweet-exp
;; Create a TVar with an initial value
def counter tvar/new(0)

;; Free when no longer needed (only when no transactions reference it)
;; via inline C over tur_tvar_free if needed
```

`tvar/new` accepts any value; TVars are untyped at the Turmeric level.

## Reading and Writing

All TVar access must happen inside an `(stm ...)` block run by `atomically`.
Using `tvar/read`/`tvar/write` outside an `stm` block is a compile error
(`TUR-E0009`), and `atomically` requires an `stm` block as its argument:

```turmeric
;; Read
(def val
  (atomically (stm (tvar/read counter))))

;; Write
(atomically (stm (tvar/write counter 42)))

;; Read-modify-write (common pattern)
(atomically
  (stm (let [v (tvar/read counter)]
         (tvar/write counter (+ v 1)))))

;; Or use tvar/modify with a function
(atomically (stm (tvar/modify counter add1)))   ; add1 = your fn of one arg
```

```sweet-exp
;; Read
def val
  atomically $ stm $ tvar/read counter

;; Write
atomically $ stm $ tvar/write counter 42

;; Read-modify-write (common pattern)
atomically
  stm
    let [v tvar/read(counter)]
      tvar/write(counter {v + 1})

;; Or use tvar/modify with a function
atomically $ stm $ tvar/modify counter add1   ; add1 = your fn of one arg
```

### Swap and CAS

```turmeric
;; Swap: write new value and return old value (within one transaction)
(def old-val
  (atomically (stm (tvar/swap counter 99))))

;; Compare-and-swap: write new only if current value equals expected
;; Returns true if the swap succeeded
(def swapped
  (atomically (stm (tvar/cas counter 99 100))))
```

```sweet-exp
;; Swap: write new value and return old value (within one transaction)
def old-val
  atomically $ stm $ tvar/swap counter 99

;; Compare-and-swap: write new only if current value equals expected
;; Returns true if the swap succeeded
def swapped
  atomically $ stm $ tvar/cas counter 99 100
```

## Retry and check

`retry` aborts the current transaction and blocks until one of the `TVar`s read during this transaction changes, then re-runs from the beginning. `(check cond)` is the conditional form: it retries when `cond` is false and continues when it is true.

```turmeric
;; Block until counter reaches at least 10
(atomically
  (stm (check (>= (tvar/read counter) 10))
       (tvar/read counter)))
```

```sweet-exp
;; Block until counter reaches at least 10
atomically
  stm
    check {tvar/read(counter) >= 10}
    tvar/read(counter)
```

`retry` never returns. The runtime records the read set, sleeps the thread, and wakes it when any read TVar is modified by another transaction.

## or-else

`or-else` tries the first `stm` block; if it retries (a `retry` or a failed `check`), the second is tried instead:

```turmeric
;; Drain queue-a if possible, otherwise queue-b
(atomically
  (stm (or-else
         (stm (dequeue queue-a))
         (stm (dequeue queue-b)))))
```

```sweet-exp
;; Drain queue-a if possible, otherwise queue-b
atomically
  stm
    or-else
      stm $ dequeue queue-a
      stm $ dequeue queue-b
```

Both branches see the same transactional snapshot. If both retry, the outer transaction also retries.

## Transaction Lifecycle

Each call to `atomically` runs a retry loop:

1. **Begin** -- allocate a transaction context; record thread-local pointer.
2. **Execute** -- run the `stm` block body; all `tvar/read` and `tvar/write` calls are journaled (read set / write set).
3. **Validate** -- check that every read TVar still has the version seen during step 2.
4. **Commit** -- apply the write set atomically; bump versions; notify waiters. Fire commit defers.
5. **Abort** -- if validation fails or `retry` was called, discard the write set, fire abort defers, then go to step 1.

The read set holds up to 256 entries; the write set holds up to 128. Transactions exceeding these limits will panic -- keep transactions focused.

## Defers

A defer fires at the end of a transaction, after commit or abort. Register them via inline C:

```turmeric
(defn register-commit-defer [env-ptr fn-ptr] : void
  ```c
  STM_Transaction *tx = tur_stm_current_tx();
  tur_stm_defer_on_commit(tx, (stm_defer_fn_t)fn_ptr, env_ptr);
  ```)

(defn register-abort-defer [env-ptr fn-ptr] : void
  ```c
  STM_Transaction *tx = tur_stm_current_tx();
  tur_stm_defer_on_abort(tx, (stm_defer_fn_t)fn_ptr, env_ptr);
  ```)
```

```sweet-exp
defn register-commit-defer [env-ptr fn-ptr] :void
  ```c
  STM_Transaction *tx = tur_stm_current_tx();
  tur_stm_defer_on_commit(tx, (stm_defer_fn_t)fn_ptr, env_ptr);
  ```

defn register-abort-defer [env-ptr fn-ptr] :void
  ```c
  STM_Transaction *tx = tur_stm_current_tx();
  tur_stm_defer_on_abort(tx, (stm_defer_fn_t)fn_ptr, env_ptr);
  ```
```

Commit defers run once, in registration order, after the write set is applied. Abort defers run on every failed attempt, including retries -- design them to be idempotent.

Up to 32 defers per transaction; exceeding this panics.

## Building Higher-Level Primitives

### TMVar (single-slot mailbox)

> `stdlib/stm-sync.tur` ships this, plus a TChan --
> `tmvar-new` / `tmvar-new-empty` / `tmvar-take` / `tmvar-put` / `tmvar-read` /
> `tmvar-full?` and `tchan-new` / `tchan-write` / `tchan-read` / `tchan-len` /
> `tchan-empty?`. Each also has a `-stm` variant (`tmvar-take-stm`, ...) that
> is the transaction **body** only, so several operations compose into ONE
> transaction that retries as a unit -- two standalone calls are two
> transactions with a window between them.
>
> They are macros rather than functions, and that is forced: `atomically`
> requires a **syntactic** `stm` block, so `(atomically (f x))` where `f`
> returns a transaction is a hard error. An STM action cannot be a value.
>
> The sketch below is kept because it shows the mechanism -- `check` is what
> turns a read into a blocking wait.

```turmeric
;; An empty slot is represented as null (sketch)
(defn tmvar/new [] : ptr  (tvar/new (ptr/null)))

(defn tmvar/take [mv]
  (atomically
    (stm (let [v (tvar/read mv)]
           (check (not (nil? v)))
           (tvar/write mv (ptr/null))
           v))))

(defn tmvar/put [mv val]
  (atomically
    (stm (check (nil? (tvar/read mv)))
         (tvar/write mv val))))
```

```sweet-exp
;; An empty slot is represented as null (sketch)
defn tmvar/new [] :ptr
  tvar/new(ptr/null())

defn tmvar/take [mv]
  atomically
    stm
      let [v tvar/read(mv)]
        check not(nil?(v))
        tvar/write(mv ptr/null())
        v

defn tmvar/put [mv val]
  atomically
    stm
      check nil?(tvar/read(mv))
      tvar/write(mv val)
```

### TChan (unbounded FIFO)

```turmeric
;; Backed by a TVar holding a list (sketch)
(defn tchan/new [] : ptr  (tvar/new '()))

(defn tchan/write [ch val]
  (atomically
    (stm (let [q (tvar/read ch)]
           (tvar/write ch (append q (list val)))))))

(defn tchan/read [ch]
  (atomically
    (stm (let [q (tvar/read ch)]
           (check (not (nil? q)))
           (tvar/write ch (cdr q))
           (car q)))))
```

```sweet-exp
;; Backed by a TVar holding a list (sketch)
defn tchan/new [] :ptr
  tvar/new('())

defn tchan/write [ch val]
  atomically
    stm
      let [q tvar/read(ch)]
        tvar/write(ch append(q list(val)))

defn tchan/read [ch]
  atomically
    stm
      let [q tvar/read(ch)]
        check not(nil?(q))
        tvar/write(ch cdr(q))
        car(q)
```

## Composability

Because `retry` and `or-else` work purely through the transaction's read set, any two transactions compose without deadlock:

```turmeric
;; Both operations run in a single atomic transaction
(atomically
  (stm (transfer account-a account-b 30)
       (log-transfer account-a account-b 30)))
```

```sweet-exp
;; Both operations run in a single atomic transaction
atomically
  stm
    transfer(account-a account-b 30)
    log-transfer(account-a account-b 30)
```

This is the key advantage over locks: you can call sub-transactions without worrying about lock ordering.

## Side Effects Inside Transactions

Transactions may run more than once (on retry). **Avoid observable side effects** such as I/O, printing, or sending messages inside `atomically`. Use commit defers for effects that should fire exactly once on success:

```turmeric
;; BAD -- println may run multiple times
(atomically
  (stm (tvar/write counter 42)
       (println "done")))

;; GOOD -- println fires once after commit
(atomically
  (stm (tvar/write counter 42)
       (register-commit-defer nil log-fn)))
```

```sweet-exp
;; BAD -- println may run multiple times
atomically
  stm
    tvar/write(counter 42)
    println("done")

;; GOOD -- println fires once after commit
atomically
  stm
    tvar/write(counter 42)
    register-commit-defer(nil log-fn)
```

## Limitations

- **No nested `atomically`:** Calling `atomically` inside `atomically` is an error. Compose by calling `tvar/read`/`tvar/write` directly in nested functions -- they share the caller's transaction context.
- **No in-transaction I/O:** A transaction body runs optimistically and may re-run several times before it commits (on a read conflict or a failed commit-time validation). Keep side effects -- printing, network, channel sends, consuming a `^linear` value -- *out* of `atomically`; register on-commit work with the defer API instead.
- **Read set limit:** 256 TVars per transaction.
- **Write set limit:** 128 TVars per transaction.
- **Defer limit:** 32 defers per transaction.
- **Pointee immutability for boxed payloads:** A TVar's version stamp protects the stored *pointer*, not the bytes it points at. A TVar holding a boxed multi-word value (struct, vector, HAMT node) must treat that payload as immutable after it is published -- a writer allocates a fresh payload and swaps the pointer. Mutating a published payload in place defeats the optimistic read check and can expose a torn value to a concurrent reader.

### Concurrency model

Commits use **TL2** (Transactional Locking II), not a single global lock. Reads
are optimistic and lock-free, validated against a global version clock; commit
locks only the stripe buckets covering its write set (64 stripes), revalidates
the read set, and publishes under a per-TVar lock bit. Throughput scales with
the number of distinct TVars touched, not the number of live transactions.

**Compiled vs interpreted.** The compiled runtime (`tur`) is genuinely
multi-threaded and runs the TL2 path above. The tree-walking interpreter
(`turi`) is single-threaded, so a transaction never races a concurrent writer:
its read-set validation always succeeds and it never aborts or blocks on
`retry`. The *observable* isolation is identical -- an uncontended TL2
transaction behaves exactly like the interpreter's -- so a program that is
correct under `turi` stays correct under `tur`. The one practical difference is
that a `retry`/`check`-false with no way to make progress blocks forever under
`tur` (another thread may yet commit) but is reported as an error under `turi`
(nothing else can ever run).

## Quick Reference

| Function | Description |
|---|---|
| `tvar/new val` | Create a TVar with initial value |
| `tvar/read tv` | Read TVar (must be inside an `stm` block) |
| `tvar/write tv val` | Write TVar (must be inside an `stm` block) |
| `tvar/modify tv fn` | Apply `fn` to the current value and store the result |
| `tvar/swap tv new` | Write and return old value |
| `tvar/cas tv old new` | Conditional write; returns bool |
| `stm body...` | Group TVar operations into one transaction block |
| `atomically (stm ...)` | Run the block as an atomic transaction |
| `check cond` | Retry (abort + block) until `cond` holds on a re-run |
| `retry` | Abort and block until a read TVar changes |
| `or-else (stm a) (stm b)` | Try `a`; if it retries, try `b` |

## See Also

- [STM Tutorial](stm-tutorial.md) -- Conceptual overview and worked examples
- [Threading Guide](threading-guide.md) -- Locks, mutexes, `Arc<T>` for contrast
- [HAMT Guide](hamt-guide.md) -- Persistent maps suitable for storage inside TVars
- [Effects System Guide](effects-system-guide.md) -- Exception handling within transactions
- [src/stm.h](https://github.com/turmeric-lang/turmeric/blob/main/src/runtime/stm.h) -- Full C API with implementation notes
