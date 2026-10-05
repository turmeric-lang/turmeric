---
title: Software Transactional Memory (STM) Tutorial
category: Concurrency and Async
description: STM tutorial: concepts, patterns, and worked examples
---

# Software Transactional Memory (STM) Tutorial

Composable, deadlock-free concurrent programming using transactional memory.

## Overview

**Software Transactional Memory (STM)** provides an alternative to locks. Transactions execute atomically with automatic conflict detection and retry, eliminating deadlocks and priority inversion while enabling composability.

Turmeric's STM is modeled on **Haskell's `Control.Concurrent.STM`**, offering a proven API and semantics.

## Core Concepts

### Transactions as Atomic Units

Like database transactions, an STM transaction either completes entirely or rolls back:

```turmeric
;; Transfer funds between accounts atomically
(atomically
  (stm (let [from-balance (tvar/read from-account)
             to-balance   (tvar/read to-account)]
         (when (>= from-balance amount)
           (tvar/write from-account (- from-balance amount))
           (tvar/write to-account (+ to-balance amount))))))
```
```sweet-exp
;; Transfer funds between accounts atomically
atomically
  stm
    let [from-balance tvar/read(from-account)
         to-balance   tvar/read(to-account)]
      when {from-balance >= amount}
        tvar/write(from-account {from-balance - amount})
        tvar/write(to-account {to-balance + amount})
```

### Transactional Variables: TVar

**`TVar`** is a mutable reference that can only be accessed within a transaction (an `(stm ...)` block run by `atomically`):

```turmeric
;; Create a transactional variable
(def counter (tvar/new 0))

;; Read within a transaction
(atomically
  (stm (let [value (tvar/read counter)]
         (tvar/write counter (+ value 1)))))
```
```sweet-exp
;; Create a transactional variable
def counter tvar/new(0)

;; Read within a transaction
atomically
  stm
    let [value tvar/read(counter)]
      tvar/write(counter {value + 1})
```

### Properties

- **Atomicity:** A transaction completes entirely or rolls back.
- **Isolation:** Concurrent transactions appear to execute serially.
- **Composability:** Transactions can call other transactions freely without deadlock risk.
- **Automatic retry:** On conflict, transactions restart from the beginning.

## Advantages Over Locks

| Aspect | Locks | STM |
|--------|-------|-----|
| **Deadlock** | Possible; require careful ordering | Impossible; automatic retry |
| **Priority inversion** | Possible | Impossible |
| **Composability** | Difficult; lock ordering required | Easy; transactions compose freely |
| **Error handling** | Lock held during exception; manual cleanup | Automatic cleanup on transaction abort |
| **Debugging** | Hard; deadlock traces are complex | Easier; transactional semantics |

## Quick Start

### Simple Counter

```turmeric
(def counter (tvar/new 0))

;; Increment atomically
(atomically
  (stm (let [v (tvar/read counter)]
         (tvar/write counter (+ v 1)))))

(println (atomically (stm (tvar/read counter))))  ; => 1
```
```sweet-exp
def counter tvar/new(0)

;; Increment atomically
atomically
  stm
    let [v tvar/read(counter)]
      tvar/write(counter {v + 1})

println $ atomically $ stm $ tvar/read counter  ; => 1
```

### Bank Transfer (No Deadlock!)

```turmeric
;; Accounts as transactional variables
(def account-a (tvar/new 100))
(def account-b (tvar/new 50))

;; Transfer with automatic conflict resolution
(defn transfer [from to amount]
  (atomically
    (stm (let [from-balance (tvar/read from)]
           (when (>= from-balance amount)
             (tvar/write from (- from-balance amount))
             (tvar/write to (+ (tvar/read to) amount)))))))

(transfer account-a account-b 30)

;; Concurrent transfers never deadlock!
(async (fn [] (transfer account-a account-b 10)))
(async (fn [] (transfer account-b account-a 5)))
```
```sweet-exp
;; Accounts as transactional variables
def account-a tvar/new(100)
def account-b tvar/new(50)

;; Transfer with automatic conflict resolution
defn transfer [from to amount]
  atomically
    stm
      let [from-balance tvar/read(from)]
        when {from-balance >= amount}
          tvar/write(from {from-balance - amount})
          tvar/write(to {tvar/read(to) + amount})

transfer(account-a account-b 30)

;; Concurrent transfers never deadlock!
async $ (fn [] transfer(account-a account-b 10))
async $ (fn [] transfer(account-b account-a 5))
```

## Core API

### Creating and Reading/Writing

```turmeric
;; Create a transactional variable
(def tv (tvar/new 42))

;; Read (only inside an stm block)
(atomically (stm (tvar/read tv)))  ; => 42

;; Write (only inside an stm block)
(atomically (stm (tvar/write tv 100)))

;; Value is now 100
(atomically (stm (tvar/read tv)))  ; => 100
```
```sweet-exp
;; Create a transactional variable
def tv tvar/new(42)

;; Read (only inside an stm block)
atomically $ stm $ tvar/read tv  ; => 42

;; Write (only inside an stm block)
atomically $ stm $ tvar/write tv 100

;; Value is now 100
atomically $ stm $ tvar/read tv  ; => 100
```

### Running a Transaction

```turmeric
;; Atomically: run the stm block, retry on conflict
(atomically (stm ...))  ; => result of the block's last expression

;; Returns the result of the transaction
(def result
  (atomically
    (stm (let [x (tvar/read counter)]
           (tvar/write counter (+ x 1))
           (+ x 1)))))
```
```sweet-exp
;; Atomically: run the stm block, retry on conflict
atomically $ stm ...  ; => result of the block's last expression

;; Returns the result of the transaction
def result
  atomically
    stm
      let [x tvar/read(counter)]
        tvar/write(counter {x + 1})
        {x + 1}
```

### Retry on Conflict

```turmeric
;; Wait until balance > 10
(atomically
  (stm (check (> (tvar/read account) 10))
       (tvar/read account)))  ; Block and re-run when any watched TVar changes
```
```sweet-exp
;; Wait until balance > 10
atomically
  stm
    check {tvar/read(account) > 10}
    tvar/read(account)  ; Block and re-run when any watched TVar changes
```

When a `retry` (or a failed `check`) fires:
1. The transaction aborts (without side effects).
2. Turmeric records which `TVar`s were read.
3. The transaction sleeps until one of those `TVar`s changes.
4. Execution resumes from the beginning.

### Choice: Try One Branch Then Another

```turmeric
;; Try to withdraw from account-a, else account-b
(atomically
  (stm (or-else
         (stm (withdraw account-a 50))
         (stm (withdraw account-b 50)))))
```
```sweet-exp
;; Try to withdraw from account-a, else account-b
atomically
  stm
    or-else
      stm $ withdraw account-a 50
      stm $ withdraw account-b 50
```

If the first branch retries, the second branch is tried. Both branches see the same transactional state at the moment of choice.

## Transactional Synchronization Primitives

TMVar and TChan are not compiler built-ins -- both are small patterns over a
`TVar` plus `check` -- but you do not have to write them: `stdlib/stm-sync.tur`
ships both, with a `-stm` variant of every operation so several can compose
into one transaction. See the
[STM Guide](stm-guide.md#building-higher-level-primitives) for the API.

The sketches below are worth reading anyway, because they show the mechanism:
`check` is what turns an ordinary read into a blocking wait, by making the
whole transaction retry until the condition holds. The shapes are:

### TMVar: Single-Slot Mailbox

A `TVar` holding either a value or an empty sentinel.

```turmeric
;; Take (blocks while empty, via check)
(defn tmvar/take [mv]
  (atomically
    (stm (let [v (tvar/read mv)]
           (check (not (nil? v)))
           (tvar/write mv (ptr/null))
           v))))

;; Put (blocks while full, via check)
(defn tmvar/put [mv val]
  (atomically
    (stm (check (nil? (tvar/read mv)))
         (tvar/write mv val))))
```
```sweet-exp
;; Take (blocks while empty, via check)
defn tmvar/take [mv]
  atomically
    stm
      let [v tvar/read(mv)]
        check not(nil?(v))
        tvar/write(mv ptr/null())
        v

;; Put (blocks while full, via check)
defn tmvar/put [mv val]
  atomically
    stm
      check nil?(tvar/read(mv))
      tvar/write(mv val)
```

### TChan: FIFO Channel

A `TVar` holding a list; writers append, readers `check` for non-empty and
pop the head. See the STM Guide's `tchan/new` / `tchan/write` / `tchan/read`
sketch. (For cross-thread queues outside a transaction, the mutex-backed
channels in the [Threading Guide](threading-guide.md#channels) are usually
the better tool.)

## Common Patterns

### Producer-Consumer

```turmeric
;; Shared queue in a TVar
(def queue (tvar/new '()))

(async
  (fn []
    ;; Producer: generate items
    (for-each (range 10)
      (fn [i]
        (atomically
          (stm (let [q (tvar/read queue)]
                 (tvar/write queue (conj q i)))))
        (sleep 100)))))

(defn pop-item []
  (atomically
    (stm (let [q (tvar/read queue)]
           (check (not (empty? q)))
           (tvar/write queue (cdr q))
           (car q)))))

(async
  (fn []
    ;; Consumer: process one item per transaction
    (while true
      (println (pop-item)))))
```
```sweet-exp
;; Shared queue in a TVar
def queue tvar/new('())

async
  (fn []
    ;; Producer: generate items
    (for-each range(10)
      (fn [i]
        (atomically
          (stm
            (let [q tvar/read(queue)]
              tvar/write(queue conj(q i)))))
        sleep(100))))

defn pop-item []
  atomically
    stm
      let [q tvar/read(queue)]
        check not(empty?(q))
        tvar/write(queue cdr(q))
        car(q)

async
  (fn []
    ;; Consumer: process one item per transaction
    (while true
      println(pop-item())))
```

### Gate (write token)

```turmeric
;; A TVar holding 1 (free) / 0 (held) as a simple gate
(def write-lock (tvar/new 1))

(defn acquire-write [] : nil
  (atomically
    (stm (check (= (tvar/read write-lock) 1))
         (tvar/write write-lock 0))))

(defn release-write [] : nil
  (atomically (stm (tvar/write write-lock 1))))
```
```sweet-exp
;; A TVar holding 1 (free) / 0 (held) as a simple gate
def write-lock tvar/new(1)

defn acquire-write [] :nil
  atomically
    stm
      check {tvar/read(write-lock) = 1}
      tvar/write(write-lock 0)

defn release-write [] :nil
  atomically $ stm $ tvar/write write-lock 1
```

### Barrier

> If you just want a barrier, `stdlib/barrier.tur` ships one -- `barrier-new`
> / `barrier-wait` / `barrier-free` over mutex + condvar, reusable across
> rounds, with `barrier-wait` returning `true` for the one thread that tripped
> it. See the [Threading Guide](threading-guide.md#barrier). The sketch below
> is for when the rendezvous has to compose with other transactional state, and
> is deliberately minimal: it counts arrivals but never resets, so it is a
> one-shot latch rather than the reusable barrier the stdlib one is.

```turmeric
;; Synchronize N threads: count arrivals, then block until all arrive
(defn barrier-new [n]
  (tvar/new 0))          ; arrivals so far; n is captured by the waiters

(defn barrier-wait [barrier n]
  (atomically
    (stm (tvar/write barrier (+ (tvar/read barrier) 1))))
  (atomically
    (stm (check (>= (tvar/read barrier) n)))))
```
```sweet-exp
;; Synchronize N threads: count arrivals, then block until all arrive
defn barrier-new [n]
  tvar/new(0)          ; arrivals so far; n is captured by the waiters

defn barrier-wait [barrier n]
  atomically
    stm
      tvar/write(barrier {tvar/read(barrier) + 1})
  atomically
    stm
      check {tvar/read(barrier) >= n}
```

## Limitations

- **No nested transactions:** Calling `atomically` inside `atomically` raises an error. (Haskell allows this; Turmeric does not.)
- **No interactive transactions:** Transactions must be pure (no I/O); side effects inside `atomically` may occur multiple times on retry. Defer commit-time work with the on-commit defer API instead.
- **Pointee immutability:** A TVar's optimistic read check protects the stored pointer, not the bytes it points at. Boxed multi-word payloads (structs, vectors, HAMT nodes) must be treated as immutable after publication -- writers allocate a fresh payload and swap the pointer rather than mutating in place.
- **Fixed-size sets:** 256 reads, 128 writes, 32 defers per transaction.
- **Concurrency model:** The compiled runtime commits with TL2 (Transactional Locking II) -- optimistic lock-free reads validated against a global version clock, with striped per-bucket commit locks -- not a single global lock; lock-free/wait-free variants remain out of scope. The interpreter is single-threaded, so it never contends: it observes the same isolation as an uncontended compiled transaction.

## Example: Concurrent Merge Sort

```turmeric
(defn merge-sort-stm [vec]
  (if (<= (len vec) 1)
    vec
    (let [mid (/ (len vec) 2)
          left-result  (tvar/new (ptr/null))
          right-result (tvar/new (ptr/null))]

      ;; Sort left half in parallel
      (async
        (fn []
          (atomically
            (stm (tvar/write left-result
                             (merge-sort-stm (slice vec 0 mid)))))))

      ;; Sort right half in parallel
      (async
        (fn []
          (atomically
            (stm (tvar/write right-result
                             (merge-sort-stm (slice vec mid (len vec))))))))

      ;; Merge results (check blocks until both halves are written)
      (atomically
        (stm (let [left  (tvar/read left-result)
                   right (tvar/read right-result)]
               (check (and (not (nil? left)) (not (nil? right))))
               (merge left right)))))))
```
```sweet-exp
defn merge-sort-stm [vec]
  if {len(vec) <= 1}
    vec
    let [mid {len(vec) / 2}
         left-result  tvar/new(ptr/null())
         right-result tvar/new(ptr/null())]

      ;; Sort left half in parallel
      async
        (fn []
          (atomically
            (stm
              tvar/write(left-result
                merge-sort-stm(slice(vec 0 mid))))))

      ;; Sort right half in parallel
      async
        (fn []
          (atomically
            (stm
              tvar/write(right-result
                merge-sort-stm(slice(vec mid len(vec)))))))

      ;; Merge results (check blocks until both halves are written)
      atomically
        stm
          let [left  tvar/read(left-result)
               right tvar/read(right-result)]
            check {not(nil?(left)) and not(nil?(right))}
            merge(left right)
```

## Performance Tips

1. **Keep transactions short:** Minimize the time window to reduce conflict probability.
2. **Read-only transactions:** Batch reads into an `atomically` block.
3. **Predicate isolation:** Use `retry` and watches efficiently; don't poll.
4. **Watch out for `or-else`:** Can cause cascading retries; use judiciously.

## See Also

- [Threading Guide](threading-guide.md) -- Locks, mutexes, atomic types
- [Async/Await Guide](async-await-guide.md) -- Lightweight concurrency with fibers
- [Effects System Guide](effects-system-guide.md) -- Exception handling in transactions
