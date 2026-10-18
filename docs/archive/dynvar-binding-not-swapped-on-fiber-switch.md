# A `binding` does not move with its fiber: interleaved fibers corrupt the dynvar binding chain

**Resolved 2026-10-18** (fix direction 1). `tur_fiber_block_resume` now swaps
each dynvar's chain head on both sides of the switch, as it already swapped
the escapes and the r7rs dynamic environment. Each TU that has `defdynamic`s
registers a `TurDynSwap` from its static initializer (`_tur_dyn_swap_tu`,
emitted beside the keys in `emit_module.c`). The fiber keeps its heads in a
`TurDynSave` per TU (`FiberBlock.dynsave`), which is made only once a head
differs, so a fiber that never binds allocates nothing. A program with no
`defdynamic` pays one NULL test per switch. It is a registry rather than one
hook pointer because every TU of a multi-TU build has its own keys
(any-type-ids-are-per-tu is the same lesson). A new fiber starts from the root.

The repro below is `tests/fixtures/dynvar-binding-moves-with-fiber`. It is
leak-checked, and it also checks that a fresh fiber resumed inside a `binding`
reads the root. Every line now prints the "Expected" column, under cc and
`tur jit`. The dynamic-vars guide's "Thread Isolation" section says fibers
are isolated too.

**Severity: medium-high (silent wrong value, and a read through a frame whose
binding has exited).** A `defdynamic` binding is per thread, not per fiber. Two
fibers that bind the same var and yield in between read each other's values,
and after both finish, the thread is left reading a frame from a binding that has
already exited. No stdlib module or spice uses `binding` today, so nothing in
the tree hits this yet. Any code that binds a dynvar and then yields will: a
per-request fiber under `httpd-new-async`, a `reactor` fiber, anything that
`scheduler-park!`s. Filed 2026-10-17 while revisiting
[dynvar-adoption-spices-plan](../upcoming/spices/dynvar-adoption-spices-plan.md),
whose tourist target would bind a request context per request.

## Repro

Measured on the v0.65.5 release binary (`tur build`, then run):

```turmeric
(defdynamic *ctx* :cstr "root")

(defn show [tag : cstr v : cstr] : nil
  ```c
  printf("%s%s\n", tag, v); fflush(stdout);
  ```)

(defn yield! [] : nil
  ```c
  tur_fiber_block_yield(0);
  ```)

(defn fiber-new-fn [fn : ptr<void> stack-size : int] : ptr<void>
  ```c
  return (void *)tur_fiber_block_new((void(*)(void))fn, (size_t)stack_size);
  ```)

(defn fiber-resume-fn [f : ptr<void> arg : int] : int
  ```c
  return tur_fiber_block_resume((FiberBlock *)f, (int64_t)arg);
  ```)

(defn body-a [] : nil
  (binding [*ctx* "A"]
    (show "A before yield: " *ctx*)
    (yield!)
    (show "A after yield:  " *ctx*)))

(defn body-b [] : nil
  (binding [*ctx* "B"]
    (show "B before yield: " *ctx*)
    (yield!)
    (show "B after yield:  " *ctx*)))

(defn main [] : int
  (let [a (fiber-new-fn body-a 0)
        b (fiber-new-fn body-b 0)]
    (fiber-resume-fn a 0)
    (show "main between:   " *ctx*)
    (fiber-resume-fn b 0)
    (fiber-resume-fn a 0)
    (show "main after A:   " *ctx*)
    (fiber-resume-fn b 0)
    (show "main at end:    " *ctx*)
    0))
```

| Line | Expected | Got |
| --- | --- | --- |
| `A before yield` | `A` | `A` |
| `main between` | `root` | **`A`** -- A's binding is still visible after A yields |
| `B before yield` | `B` | `B` |
| `A after yield` | `A` | **`B`** |
| `main after A` | `root` | `root` |
| `B after yield` | `B` | **`root`** -- A's pop discarded B's frame |
| `main at end` | `root` | **`A`** -- read through A's frame, whose binding has exited |

## Root cause

Each `defdynamic` gets one `pthread_key_t` (`src/compiler/emit_module.c:20289`).
The key holds the head of a chain of `TurDynFrame`s. `binding` puts its frame on
the C stack of the code that runs it, links it in front of the current head,
and pops it with a cleanup that restores the `prev` it captured at push time
(`src/compiler/emit_expr.c:18027-18035`).

A fiber runs on its own stack but shares the thread's key, and
`tur_fiber_block_resume` (`emit_module.c:16395`) neither saves nor restores the
chain. So A's frame, which lives on A's stack, stays the thread's head after A
yields. B links its frame on top of A's, and A resumes to read B's value. A's
pop then resets the head to the root, which drops B's frame. Finally B's pop
resets the head to A's frame, whose binding has already exited.

Two other pieces of per-thread state already move with their fiber, and
`tur_fiber_block_resume` swaps both:
- the region escape list (`src/compiler/emit_dk_runtime.c:59`);
- `#lang r7rs`'s dynamic environment, including `parameterize`'s bindings
  (`emit_dk_runtime.c:70-78`, after
  [r7rs-dynamic-environment-shared-across-threads](../archive/r7rs-dynamic-environment-shared-across-threads.md)).

Turmeric's own dynvars are the one piece of dynamic state left out.

## Fix directions

1. **Swap the chain heads per fiber,** the same way the r7rs dynamic
   environment is swapped. On yield, record the fiber's head for every dynvar
   key and put back the resumer's heads; on resume, do the reverse. A new fiber
   starts from the root, as a new thread does. The DV3 snapshot helpers that
   `spawn-conveying` uses (`_tur_binding_snapshot_capture` / `_install`,
   emitted beside the keys) already walk every key and could do the walking.
   A program with no `defdynamic` should keep paying nothing.
2. Until then, document in the dynamic-vars guide that a `binding` must not
   be held across a yield or park, and say so in the reactor and httpd async
   docs.

The repro above is the fixture either fix should land with, under
`tests/fixtures/`.
