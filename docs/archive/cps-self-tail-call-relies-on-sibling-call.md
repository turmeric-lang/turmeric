# A CPS function's self tail call is a C call, not a loop

**RESOLVED 2026-09-28** -- see [Resolution](#resolution-2026-09-28). Self
tail calls were fixed 2026-09-26, and mutual recursion is fixed as of
2026-09-28. Pinned by `tests/fixtures/r7rs-cps-loops-unoptimized` (self) and
`tests/fixtures/r7rs-cps-mutual-tail-unoptimized` (mutual). Both are built at
`-O0` by their `hook.sh`. One shape was left open, filed on its own as
[mutual-tail-call-through-guard-grows-the-stack](mutual-tail-call-through-guard-grows-the-stack.md)
and resolved 2026-09-29.

**Self tail calls, 2026-09-26.** A CPS function's self tail call in its own
body that hands on its own `__kont` is now a backedge. The parameters are
rebound (all arguments are evaluated first) and it jumps to `__tur_cps_self`,
a label after the binder declarations (`emit_cps_ir.c`, the cps->cps arm of
CT_TAILCALL; `emit_cps_ir_try_fn` places the label only when it is used).
Every self-recursive repro below -- `for-each`, `map`, `member` with a
comparison, a `delay-force` stream a million deep, and the loop through a
variable -- now runs at `-O0` and `-O1`. So do named-let and `do` loops, and
`vector-for-each` and `string-for-each`, over a million elements.
`r7rs-cps-loops-unoptimized` is built at `-O0` on purpose: `--debug` is
`-Og`, where gcc still makes the sibling call, so it would not have caught
the regression.

What was left then was a tail call to ANOTHER CPS function. Two procedures
that recurse into each other, each calling a procedure variable on the way,
still overflowed below `-O2`:

```scheme
(define (ping f n) (if (= n 0) 'done (begin (f n) (pong f (- n 1)))))
(define (pong f n) (if (= n 0) 'done (begin (f n) (ping f (- n 1)))))
(write (ping (lambda (x) x) 1000000))   ; was: done at -O2, segfault at -O1 and -O0
```

**Severity:** medium. Every dialect that has effectful (CPS-compiled)
functions; most visible in `#lang r7rs`, where any procedure that calls a
Scheme procedure is CPS. A CPS function that loops by calling itself grows
the C stack by one frame per iteration unless the C compiler turns the call
into a jump: gcc does at `-O2`, not at `-O1`, and gcc 13 has no `musttail`.
Found by r7rs-lang-plan T8's sanitizer audit (`r7rs-control` overflowed in
`r7rs-force` under an `-O1` ASan build).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(let ((n 0))
  (for-each (lambda (x) (set! n (+ n 1))) (make-list 1000000 1))
  (write n))
```

Measured 2026-09-25 with gcc 13:

| build | result |
|---|---|
| `TUR_CC_FLAGS=-O2` (the default) | `1000000` |
| `TUR_CC_FLAGS=-O1` | segfault |
| `tur --interpret` | `1000000` |

The same holds for one-list `map`, `member` and `assoc` (both go through
`r7rs-member-by__`/`r7rs-assoc-by__`, which call the comparison), and a
`delay-force` stream a million deep (`force`): each is a prelude loop that
calls a Scheme procedure, so it is CPS.

## Root cause

The CPS emitter writes a self tail call as a C tail call and never as a
backedge: `src/compiler/emit_cps_ir.c:6920`

```c
ce_line(ce, "return %s__cps(%s, %s); /* cps->cps */", fn, argv_t, thread);
```

(and the heap-join variant at line 7940). The direct emitter's self-TCO
(`tco_mark`/`emit_tail`, emit_fns.c) does not run on `__cps` bodies, and
`TUR_MUSTTAIL` is not applied here and is empty on gcc anyway. So the
constant-stack guarantee is the C optimizer's, at `-O2`.

A related shape, already fixed in the prelude: a loop whose step calls a
SEPARATE CPS procedure resumes the loop from inside that procedure's frame
(`mapn_go_j0` nesting under `apply_list__cps`), which overflows even at
`-O2`. T8 moved the call into the loop body; see the note at
`r7rs-mapn-go__` in stdlib/r7rs/prelude.tur.

## Fix directions

- Lower a CPS self tail call to a backedge the way the direct emitter does:
  reassign the parameters and jump to a label at the top of the `__cps`
  body. The continuation argument is unchanged across a self tail call, so
  it needs no reassignment.
- Or put `TUR_MUSTTAIL` on these returns. That covers clang only (gcc gained
  `musttail` in 15), so it is a partial answer.

## Also measured, Apple clang 21 (macOS arm64, 2026-09-25)

The `-O1` row above is gcc's, not the shape's. clang keeps the sibling call
lower down, so the threshold moves:

| build | gcc 13 | Apple clang 21 |
|---|---|---|
| `-O2` (the default) | constant stack | constant stack |
| `-O1` | segfault | constant stack |
| `-O0` | segfault | segfault |

Two probes that pin the shape rather than the library. A loop whose non-tail
call is to a NAMED procedure is not CPS and is lowered to a backedge, so it
holds at `-O0`:

```scheme
(define (noop x) x)
(define (go i acc) (if (= i 0) acc (go (- i 1) (+ acc (noop 1)))))
(write (go 1000000 0))                      ; 1000000 at -O0
```

The same loop calling through a VARIABLE is CPS (`run__cps` in the emitted C)
and overflows at `-O0`:

```scheme
(define (run f i acc) (if (= i 0) acc (run f (- i 1) (+ acc (f 1)))))
(write (run (lambda (x) x) 1000000 0))      ; segfault at -O0, 1000000 at -O2
```

Tail calls themselves are unaffected at any level: `tests/fixtures/r7rs-tail-calls`
runs self, mutual and through-a-variable tail calls 10,000,000 deep with its
`--debug` flags file (`-O0`).

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries a bullet
beginning "**Mutual recursion through a procedure variable needs the C
compiler's tail call.**" (narrowed from the loop bullet on 2026-09-26, when
the self backedge landed). When cross-function CPS tail calls are constant
stack at every level, delete that bullet whole -- the guide's "Lists,
vectors, strings" section already states that every Scheme call is a proper
tail call, which becomes the complete story.

## Resolution (2026-09-28)

The mutual-recursion half needed two changes, because a procedure in such a
pair gets one of two lowerings depending on its body.

**1. The CPS backend fuses mutual tail-call groups** (`emit_cps_ir.c`, "CPS
mutual tail-call groups"; this is proper-tail-calls T5 for `__cps` bodies).
The emitter builds the cps->cps tail-call graph over the CPS functions it
emits and finds its strongly-connected components. Each component of two to
eight members becomes one C function,
`static int64_t __cps_tcg_N(int __tcg_st, <every member's params>, DK *__kont)`.
It dispatches on `__tcg_st` in a `switch` at the top of a loop. Each
member's rendered main body is one `case`, with its parameters as block
locals read from their slots and its join labels prefixed (`__tcgmK_L...`)
so they cannot collide.

A tail call from a main body to any member, itself included, that hands on
`__kont` evaluates all its arguments, writes the target's slots, sets
`__tcg_st` and jumps to the top. `__kont` is shared by every member and a
tail call leaves it unchanged, so nothing else moves.

Each member keeps `<name>__cps` as a one-line wrapper into the fused
function. Calls from outside the group, from a member's own lifted helpers,
and from a member's non-tail positions all go through it.

The fused function is written once its last member has been rendered. If a
member never is, `emit_cps_ir_flush_groups` writes it before the static
initializer, at both `static_init_emit` sites in `emit_module.c`.

The members are the functions the self backedge could already take. That
excludes a closure (its env is param 0), a monomorph, the entry, and a
function with a parameter kept in a cell or carried by a loop. A body in the
split prelude library is excluded too, because its `__cps` has external
linkage.

**2. T5's direct groups take colored functions** (`emit_fns.c`,
`tcg_member_ok`). The report's own `ping`/`pong` return a symbol. That is a
widen the CPS backend does not lower, so both fall back to ordinary direct
C. They are CPS-*colored* all the same, and T5 refused any colored function,
on the reasoning that a colored body was not a C tail position. That stopped
being true when the fiber path went away: a colored function the CPS backend
declines is emitted as plain direct C. T5 now refuses a function only when
the CPS backend actually emits it (`emit_cps_ir_emits_binding`), so
`ping`/`pong` fuse into a `__tcg_group_N` like any uncolored pair.

With both changes, the `ping`/`pong` repro prints `done` at `-O0` and `-O1`
(gcc 13).

The new fixture `r7rs-cps-mutual-tail-unoptimized` (built at `-O0`) runs a
million steps each through:

- `ping`/`pong` (direct T5);
- a two-member and a three-member CPS group;
- a group reached from a non-tail position (`count-even`, the wrapper path);
- a group whose member body holds a `call/cc` escape.

Every one of these overflowed below `-O2` before. The full suite (3327
passed), the r7rs sanitizer run (`run-r7rs-sanitize.sh`, 100/100) and the JIT
suite pass. On the JIT suite, `gc-heap-struct-rc` read a constant 736-byte
engine allocation until its probe got a second warm-up window; that is not a
leak and not related to this change.
`run-r7rs-sanitize.sh` keeps `-foptimize-sibling-calls`, but no CPS loop
depends on it now.

### What is still a C call

- ~~**A group member reached through a closure, an internal `define`, or a
  lifted continuation.** Members are top-level procedures only. A `__cps`
  closure takes its env as param 0, and a tail call that happens inside a
  lifted join helper is not in a main body. Those calls stay
  `return g__cps(...)`, constant stack at `-O2` as before.~~ **Covered
  2026-10-12** by
  [cps-self-call-through-join-grows-the-stack](cps-self-call-through-join-grows-the-stack.md):
  every such call is still `return g__cps(args, __kont)` on a flat stack,
  but is guarded by a depth check against the trampoline driver's landing
  and bounces to the driver once the stack has grown, so it is constant
  stack at every `-O`.
- ~~**A component larger than eight members or thirty-two parameter slots**
  (`CTG_MAXMEM`, `CTG_MAXSLOTS`) is not fused. Raise the caps if a real
  program hits them.~~ Same: an unfused member's tail calls take the guarded
  C call, so the caps bound only how much of a cycle is a `goto`.
- ~~**A tail call after a `guard`, into another procedure.**~~ Resolved
  2026-09-29: the partner was evicted from CPS by its base case, not by the
  tail call. `guard` makes
  its procedure CPS. Its partner, which only tail-calls it, is direct, so the
  cycle alternates between the two lowerings. The CPS side calls the direct
  side as `cps->direct` and hands the result to `dk_run`, and the direct side
  enters the CPS one through its prompt-and-setjmp wrapper, so neither call
  is a tail call at any `-O`. Filed as
  [mutual-tail-call-through-guard-grows-the-stack](mutual-tail-call-through-guard-grows-the-stack.md);
  the guide's "Where it differs" bullet is gone.

### Guide upkeep, done

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") no longer has the
"Mutual recursion through a procedure variable needs the C compiler's tail
call" bullet. Its replacement names only the `guard` shape above.
