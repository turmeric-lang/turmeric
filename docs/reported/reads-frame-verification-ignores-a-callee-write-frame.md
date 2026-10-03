# A `#reads` frame is not checked against the write frames of what its body calls

**Severity: low-medium (trust: a false `#reads` claim grants congruence, and no
diagnostic fires -- but the claim has to be written by hand and be wrong).**
Filed 2026-10-03, found while making the loop-invariant frozen grant sound
(item 2 of
[reads-measure-rejected-in-invariant-and-pre](reads-measure-rejected-in-invariant-and-pre.md)).

`#reads p` is a trusted claim that a body only READS p's state, and
`TUR-W0383` is advertised as reporting "the violations the compiler can see".
A body that calls a MUTATOR of p is a violation the compiler can see, and it is
not reported.

## Repro

```turmeric
(defn liar [v : (Vec int)] #reads v : int
  (do (vec-push! v 7) 0))

(defn f [^mut v : (Vec int)] : int
  (let [__frozen (& v)]
    (let [^mut i 0]
      (while (< i 2) :invariant (<= (vlen v) 3)
        (liar v)
        (set! i (+ i 1)))
      i)))
```

`liar` is declared `#reads v` and its body grows `v`. No `TUR-W0383`, no
`TUR-E0375`, nothing. The loop's frozen grant then treats `(vlen v)` as
congruent across the body on the strength of that claim, and the bound is
proved preserved when it is false from the first iteration on.

Adding an explicit write frame to the mutator does not help either -- this is
accepted with no diagnostic:

```turmeric
(defn push2 [v : (Vec int) val : int] #writes [v] : nil
  (vec-push! v val))

(defn liar2 [v : (Vec int)] #reads v : int
  (do (push2 v 7) 0))
```

## Mechanism

`rf_resolve_read_frames` (`src/compiler/elab_fns.c`) verifies a `#reads` frame
with `rf_scan` over the elaborated body, and its fixed point "consults a
callee's `reads_checked`". That is a READS question throughout: does the body
read mutable state the frame omits. Whether a callee WRITES a framed parameter
is the separate `#writes` machinery (`wf_resolve_write_frames`), and the two
passes do not talk to each other, so a call to a writer is invisible to the
reads verification.

Nothing bridges the gap from the other side either: `stdlib` declares no
`#writes` frames at all, so even a pass that did consult them would learn
nothing about `vec-push!`, whose signature is `[v : (Vec A) val : A]`, `#fx{}`.

## Why it matters more than it used to

Until 2026-10-03 the only consumer of the `#reads` congruence grant was a
call-site crossing, where the guide's soundness argument leans on a backstop:
the callee's own entry check is never elided, so a false claim costs a missed
*elision*, not a wrong answer. The loop-invariant frozen grant is a second
consumer with no such backstop -- a proved invariant's runtime check is removed
-- so a false claim there is a silent wrong answer.

That grant is still only given where a promise exists (see
`li_name_reads_only`), which is why this is a trust gap rather than an
unsoundness in the grant itself: every annotation in `stdlib/vec.tur` is
truthful, and a user has to write a false one to reach it.

## Fix directions

1. **Teach the reads verification about write frames.** In `rf_scan`'s call
   case, a callee whose `#writes` frame names a parameter the caller passes a
   framed root to is an EXCEEDED verdict. Needs (2) to be useful.
2. **Give the stdlib container mutators `#writes` frames.** `vec-push!`,
   `vec-pop!`, `vec-set-o!`, `vec-drop-last-o!`, `vec-free-o`. Measured: the
   annotation parses on a by-value parameter today and is inert.
3. **Or make the verdict loud rather than silent**: a `#reads` body that calls
   anything whose write behaviour is unknown could be reported as unverified
   (`reads_checked` stays false) and the congruence grant withheld for it,
   which turns the trust into evidence without needing (1) and (2) first. This
   is the conservative option and would withhold the grant for every `#reads`
   measure whose body is inline C -- i.e. most of them -- so it needs a
   measurement of what it costs before it is chosen.

## Not to be confused with

The `vec-len` `#fx{}`-versus-purity-walk divergence, which is about a reader
the walk cannot see into, and waits on
[trusted-refinement-claims-plan](../archive/trusted-refinement-claims-plan.md)
R4. This report is about a WRITER the verification does not look for.
