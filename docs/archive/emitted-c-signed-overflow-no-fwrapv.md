# Emitted C does signed `int64_t` arithmetic without `-fwrapv`: wrapping hashes are undefined behavior, and the optimizer acts on it

- **Status:** RESOLVED 2026-10-11 (fix direction 1, see
  [Resolution](#resolution)). Reproduced on v0.55.1 and v0.63.9 (a Release
  build from `main`).
- **Severity:** high (silently wrong results). No diagnostic, no crash, and the
  affected spice's own test suite stays green.
- **Found:** 2026-10-10 writing the `tur-prob` guide. The Count-Min Sketch
  returned `0` for every key, including one added 20,000 times.

## What happens

Turmeric's `int` is a signed 64-bit integer, and the emitter lowers `+`, `-` and
`*` on it to the C operators on `int64_t`. In C, signed overflow is undefined
behavior. Hashing code wants the opposite: `prob/hash.tur`'s splitmix64
finalizer multiplies by large odd constants and relies on the product wrapping
modulo 2^64.

The driver's default flags carry no `-fwrapv`:

| Where | Flags |
|---|---|
| `src/main.c:3843` (native, `tur build` / `tur run`) | `-O2 -std=c99 -Wall -fno-strict-aliasing` |
| `src/main.c:3841` (`--debug`) | `-g -Og -std=c99 -Wall -fno-strict-aliasing` |
| `src/main.c:3831` (wasm) | `-O2 -std=c99 -Wall -fno-strict-aliasing -s WASM=1` |
| `src/main.c:7344`, `:8170-8171` | the same strings again |

So `cc` is entitled to assume the multiplications never overflow, and what it
does with that depends on the surrounding code. The comment above those flags
already says why `-fno-strict-aliasing` is there ("emitted code ... routinely
pun pointers"); `-fwrapv` is the same kind of fact about the emitted code and is
missing.

## Repro

Needs a `turmeric-spices` checkout (`spices/prob`). From `spices/prob`:

```turmeric
(defmodule zzp
  (import prob/cms :refer [CountMin cms-new cms-free cms-add! cms-estimate])

(defn reps [^borrow c : CountMin k : int i : int n : int] : nil
  (if (>= i n) nil (do (cms-add! c k) (reps c k (+ i 1) n))))

(defn main [] : int
  (let [sk (cms-new 5 1024)]
    (reps sk 1 0 100)
    (reps sk 2 0 7)
    (println (cms-estimate sk 1))
    (println (cms-estimate sk 2))
    (println (cms-estimate sk 3))
    (cms-free sk))
  0)
) ;; end
```

`tur run zzp.tur` prints `0 0 0`. Expected `100 7 0`.

Delete the third `println` and it prints `100 7`: the lines *before* the one you
changed change their answers. That is the shape of an optimizer acting on UB, not
of a logic error in `cms-estimate`.

## Evidence that it is the overflow

Taking the emitted C out of the picture (`tur emit-c zzp.tur > zzp.c`):

| `cc` flags on `zzp.c` | output |
|---|---|
| `-O0` | `100 7 0` (correct) |
| `-O1`, `-O2`, `-O3` | `0 0 0` |
| `-O1`, `-O2`, `-O3` **`-fwrapv`** | `100 7 0` (correct) |
| `-O0 -fsanitize=undefined` | `signed integer overflow` at the multiplications in `prob__hash__*` (e.g. `4658895280902244378 * -7723592293110705685`) |

The emitted `main` is straight-line: three calls to `prob__cms__cms_hyestimate`
and three `printf`s, with nothing about the program that could make the first two
prints depend on the third. Reproduced the same way with the installed v0.55.1.

`tur test spices/prob/tests/prob` passes with and without `-fwrapv`, so the
suite does not catch it. The comments in `prob/*.tur` blaming "let-binding names
collide with the caller" and the `cms-`/`sk-` prefixes in the tests describe the
same symptom (a wrong value that depends on surrounding code); that is probably
this defect misdiagnosed. Not verified, not touched.

## Impact

Any Turmeric program that hashes or does modular arithmetic with `int`
multiplication that is expected to wrap: every structure in `tur-prob`
(`prob/hash.tur`), and any user hash, PRNG or checksum. `stdlib` has not been
audited for the same pattern.

The Debug build of `tur` itself runs UBSan (see
`docs/archive/sanitizer-gate-not-armed-in-ci.md`), but that instruments the
*compiler*, not the C it emits, so it never sees these.

## Workaround

```sh
TUR_CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -fwrapv" tur run my-app.tur
```

`TUR_CC_FLAGS` replaces the defaults rather than adding to them.

## Fix directions

1. **Add `-fwrapv` to the default flags** (the five strings above). One-line
   change per site, makes `+ - *` on `int` wrap, which is what the hash code
   assumes (and what the emitted C does in practice at `-O0`). Needs a perf check on `benchmarks/` (`-fwrapv` blocks some loop
   strength-reduction), and the same flag where fixtures are built by
   `tests/run.sh` (`TUR_CC_FLAGS` set by a harness replaces the default, so each
   harness must carry it too -- check `grep -rn TUR_CC_FLAGS tests/*.sh`).
2. **Emit wrapping arithmetic explicitly** (`(int64_t)((uint64_t)a * (uint64_t)b)`
   behind a `TUR_WRAP_MUL` macro). No flag dependence, works for a user's own
   `cc`, but touches the emitter's arithmetic and every snapshot.
3. **Define the language semantics first.** If `int` overflow is meant to be
   checked or trapped, say so; today the AOT path is undefined behavior. Whether
   the MIR JIT and `--interpret` wrap was not checked here, and if they do, the
   back ends disagree on a program that overflows.

Either way the fix should come with a fixture that fails without it. The program
above is one but depends on the optimizer's mood; a more direct one (an idea, not
tried) is a loop that overflows and then tests `x + 1 > x`.

## Guide upkeep

`tur-prob`'s guide (`docs/guides/prob-guide.md` in `turmeric-spices`) has a
"Compiling -- read this first" section and a "Needs `-fwrapv`" bullet under
"Limits" that document this. Delete both when this is fixed and the spices CI
pins a compiler that has it, and drop the `TUR_CC_FLAGS` line from
`spices/prob/examples/01_tour.tur`'s header.

## Resolution

Fix direction 1, with the flag placed where no override can drop it:

- **`-fwrapv` is in `TUR_EMITTED_C_CC_FLAGS`** (`src/main.c`), the suffix every
  driver `cc` over emitted C appends *after* the user's flags -- the native and
  wasm `tur build`/`tur run` link, the prelude-split library unit, `tur
  compile`, and `tur link`. So `TUR_CC_FLAGS` (which replaces the defaults, and
  which every test harness sets) cannot lose it, and no harness needed editing.
  The comment there records it as a fact about the language, like
  `-fno-strict-aliasing`, not a tuning knob.
- **The CMake archives built from emitted-C-shaped code carry it too:**
  `turt_preamble` (the split runtime preamble) and `turt_runtime`, whose flags
  are documented as mirroring the default `tur build` ones.
- **The interpreter folds `+ - *` on `int` in `uint64_t`**
  (`eval_builtin`'s `BS_VARIADIC_FOLD`, `src/turi/eval.c`), so `--interpret`
  wraps by definition rather than by the host compiler's mood, and the Debug
  `tur`'s UBSan has nothing to report there.
- **The MIR JIT** already agreed: `tur jit` prints the fixture's expected
  output unchanged.

So the semantics (fix direction 3) are: `int` `+ - *` wrap, two's complement,
on every back end -- what `docs/guides/numeric-tower-guide.md` already claimed
("matching what `int` already does").

**Pinned by** `tests/fixtures/int-arith-wraps`: `(> (+ x 1) x)` at
`INT64_MAX`, `(< (- x 1) x)` at `INT64_MIN`, `(+ big 1)`, `(* big 2)` and a
multiplicative mix of the `prob/hash.tur` shape. On the old driver at `-O2`
the two comparisons print `true`; with the fix, `false`, matching `--interpret`
and `tur jit`. Fixture suite: 3724 passed, 0 failed.

**Cost** (the perf check the report asked for; each program built by `tur
build`, then rebuilt from the same `cc` line minus `-fwrapv`, best of 5 runs,
4-core Linux box, gcc 13): `triangle/loop-sum`, `triangle/fib`,
`bench-parsec-json`, `bench-logic-subst`, `bench-poly-specialize` and
`bench-clone-overhead` are within run-to-run noise either way.
`bench-dfs-queens` is the one that moved: 375 ms vs 357 ms for 10 runs, best
of 12 -- about 5% slower with `-fwrapv`. That is the price of defined
arithmetic; a hot loop that wants the old code generation can say so in its
own inline C.

**Spices follow-up (not in this repo):** once turmeric-spices pins a `tur` with
this fix, delete `tur-prob`'s "Compiling -- read this first" section and its
"Needs `-fwrapv`" bullet, and the `TUR_CC_FLAGS` line in
`spices/prob/examples/01_tour.tur` (see "Guide upkeep" above).
