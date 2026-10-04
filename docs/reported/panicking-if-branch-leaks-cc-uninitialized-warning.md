# An `if` branch that panics puts a C compiler warning on the user's stderr

**Severity:** low (noise, but on every build of a very common shape). A
value-returning function whose `if` has a `panic` in one branch makes `cc`
print a `-Wsometimes-uninitialized` warning, with emitted-C line numbers,
during `tur run` / `tur build`. Nothing is wrong at run time, but a new user
sees fifteen lines of C diagnostics before their program's output and has no
way to know they are harmless. Found 2026-10-04 while writing
`docs/guides/debugging-guide.md`, `./build/tur` v0.62.0, Apple clang 21.

## Repro

```turmeric
(defn checked-div [a : int b : int] : int
  (if (= b 0)
    (panic "division by zero")
    (/ a b)))

(defn main [] : int
  (println (checked-div 10 2))
  0)
```

```
$ tur run boom.tur
.../tur-build/boom_tur.c:8374:17: warning: variable '__t172' is used
uninitialized whenever 'if' condition is false [-Wsometimes-uninitialized]
...
1 warning generated.
5
```

## Root cause

The emitted function:

```c
static int64_t checked_hydiv(int64_t a, int64_t b) {
        int64_t __t172;
        if ((b) == (INT64_C(0))) {
            tur_panic("division by zero");
            if (tur_panicking) return ((int64_t)0);
        } else {
            __t172 = ...;
        }
        return __t172;
}
```

On the compiled path `panic` is not `noreturn`: it sets `tur_panicking` and
returns, and the per-call-site check unwinds. So the C compiler sees a path
through the panicking branch, with `tur_panicking` false, that reaches
`return __t172` with `__t172` unset. At run time `tur_panic` either aborts or
sets the flag, so the path is dead -- but cc cannot know that.

`emit_fns.c:6564` already handles the *whole-body* version of this (a body
that diverges on every path gets a synthesized dead `return ((T)0)`, for the
same reason, after the identical warning broke `tests/run-offtree-load.sh` on
macOS). The branch-level case was not covered.

## Fix directions

1. After the propagation check in a diverging branch, assign the branch's
   result temporary its synthesized zero (`__t172 = ((int64_t)0);`) -- the same
   scalar/aggregate default `emit_fns.c` uses for the whole-body case.
2. Or emit `__builtin_unreachable()` after the check when the branch is known
   to diverge.

Option 1 matches the existing precedent and works on every compiler.

## Guide upkeep

None. No guide mentions this warning.
