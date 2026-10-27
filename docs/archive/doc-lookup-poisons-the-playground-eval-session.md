# A doc-panel lookup poisons the Try Turmeric session, and re-running any program then fails

**Severity:** high for the playground. One doc lookup makes the Run button
stop working for the rest of the session, and the error it shows blames the
user's own code for colliding with a stdlib module that does not contain it.
The advice in the message ("rename the local definition") cannot fix anything.

**Status:** RESOLVED 2026-09-16 by
[playground-session-hygiene-plan](playground-session-hygiene-plan.md) (PS1-PS5),
whose execution record also corrects two of this report's claims: a doc lookup
never spliced `(doc-lookup ...)` into the session -- the failed turn was never
committed, and the damage was the discarded elaboration session it left
behind -- and `web/examples.js` is not the page's examples list. Originally
reproduced by hand on the live site (turmeric-lang.com/try, deployed build
`tur-try-v1-0.45.0-57976dc3f`) on 2026-09-09.

**Fix sequence:**
[playground-session-hygiene-plan.md](playground-session-hygiene-plan.md)
(PS1-PS5), whose five open questions were researched and decided 2026-09-09.
That plan carries two mechanisms this report does not:

- The wrong "auto-loaded stdlib module" diagnostic is the whole-program
  fallback passing `stdlib_prefix = prior`, which marks the user's own previous
  turns as stdlib (`src/turi/eval.c:12837`). **Any** failed eval discards the
  elaboration session and drops the session onto that path -- which is why one
  doc lookup is enough, and why the fix is to stop the discard rather than to
  re-scope `stdlib_prefix` (which turns out to carry four roles, including the
  stdlib/user partition of both elaboration passes).
- Preloading `docstrings.tur` cannot fix defect 2: its only function is an
  inline-C body and `doc-lookup` is not registered as an interpreter native, so
  the tree-walker could not execute it. The table is already a C array inside
  that inline-C body; PS3 emits it as real C instead.

Reported from the playground as: a repeating
`warning [TUR-W0040]: unknown name 'doc-lookup'; will runtime-dispatch -- typo?`
after running a `#lang saffron` script, followed by
`defeffect: 'Ask' is already defined` and
`defn: 'use-ask' is already defined by an auto-loaded stdlib module`.

**Saffron is incidental.** Every symptom reproduces under `#lang turmeric`.
The dialect is not on the causal path; it was just what the reporter had
selected.

## Three defects, one visible failure

### 1. `turi_doc_lookup` mutates the eval session (the root cause)

`src/web/wasm_glue.c:642` implements a *read-only documentation query* by
running it through the *accumulating* evaluator:

```c
/* Build a Turmeric expression: (doc-lookup "name") */
...
TuriValue result = turi_eval(g_env, expr);
```

`turi_eval` appends its source to the session accumulator. So every doc-panel
lookup permanently splices `(doc-lookup "<name>")` into the session's program
text, where it is re-elaborated on every subsequent Run. That is the repeating
TUR-W0040: the warning is not about the user's code at all, and it never goes
away, because the offending form is now part of the session.

It also shifts the `<eval>:NN` line numbers the user sees for their own code,
which is why the reported line numbers do not match anything in their file.

### 2. `doc-lookup` is not defined in the playground at all

`wasm_preload_stdlib` (`src/web/wasm_glue.c:95-107`) preloads macros, native
stubs, collections and typeclasses. It does **not** load
`stdlib/docstrings.tur`, which is the only definition site of `doc-lookup`
(`stdlib/docstrings.tur:5`).

So the query in defect 1 can never resolve, for any name. Measured:
`wasmDocLookup('vec-map')` returns `null` even though `vec-map` has a
docstring row in `stdlib/docstrings.tur`. **The doc panel's wasm path is
non-functional for stdlib symbols**, independently of the session damage.

`stdlib/macros.tur:107` has the same latent hole -- the `doc` macro expands to
a `doc-lookup` call, so `(doc 'foo)` warns and misbehaves in the playground for
the same reason.

### 3. `defeffect` redefinition across turns is not covered, and its fallback lies

`src/turi/env.c:198-215` documents that the incremental-elaboration path (on by
default) intentionally fixed exactly this class of error:

> redefining a top-level `defn` across turns now works instead of failing with
> "already defined by an auto-loaded stdlib module" (an artifact of the old
> path re-elaborating prior turns under `stdlib_prefix`)

That fix holds for `defn` and does **not** hold for `defeffect`. Re-running a
program containing `(defeffect Ask [] :int)` fails on the second run with
`defeffect: 'Ask' is already defined`.

Worse, once a run fails this way the session appears to fall back to the
whole-program path, and the *next* run reports the user's own `defn` as
`already defined by an auto-loaded stdlib module; rename the local definition`.
`use-ask` appears in no stdlib module -- `grep -rn "use-ask" stdlib/` is empty.
The diagnostic (`src/compiler/elab_fns.c:5645`) names a cause that does not
exist and prescribes a fix that cannot work.

## Minimal repro

Live site or `just web-dev`, at `/try`. No `#lang` line needed.

**A -- one doc lookup breaks Run (defects 1 + 2):**

```js
// editor contains:  (defn main [] : int (println "hi") 0)
runCode()                       // => "hi", 0        -- fine
runCode()                       // => "hi", 0        -- fine, redefinition works
await turmericApp.wasmDocLookup('vec-map')   // => null (defect 2)
runCode()                       // => error: defn: 'main' is already defined by
                                //    an auto-loaded stdlib module; rename the
                                //    local definition
runCode()                       // => same error, forever
```

Measured exactly this on the live site. Before the lookup, re-running is clean;
after it, the identical program never runs again in that session.

**B -- `defeffect` is not re-runnable (defect 3), no doc lookup involved:**

```turmeric
(defeffect Ask [] : int)

(defn use-ask [] : int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))
```

Run once => `42`. Run again => `<eval>:74:12: error: defeffect: 'Ask' is
already defined`. Run a third time => that plus
`defn: 'use-ask' is already defined by an auto-loaded stdlib module`.

This is the "effects" entry in `web/examples.js:99`, so it is reachable
straight from the examples dropdown -- run the shipped example twice and the
playground breaks.

**Control matrix** (live site, `resetWasm()` between trials, each run twice):

| Program | Run 1 | Run 2 |
| --- | --- | --- |
| `defn` only, `#lang turmeric` | ok | **ok** |
| `defn` only, `#lang saffron` | ok | **ok** |
| `defeffect` + `defn`, `#lang turmeric` | ok | **`'Ask' is already defined`** |
| `defeffect` + `defn`, `#lang saffron` | ok | **`'Ask' is already defined`** |

The dialect column is inert; the `defeffect` row is the whole story.

## Fix directions

**Defect 1 -- do not evaluate a doc query into the session.** The lookup wants
a value out of a table, not a turn of the REPL. Either give
`turi_doc_lookup` a non-accumulating evaluation entry point (evaluate the form
without appending to `src_acc`), or -- better -- skip the evaluator entirely
and read the docstrings table directly from C, the way
`turi_doc_lookup_builtin` already does a few lines above at
`src/web/wasm_glue.c:616`. The current shape means a hover can change program
semantics, which no documentation feature should be able to do.

**Defect 2 -- decide whether the playground has docstrings at all.** Right now
it ships a doc panel whose wasm path resolves nothing. Either add
`docstrings.tur` to `wasm_preload_stdlib`, or (given its size, and that
`web/public/doc-names.json` already ships the index separately) drop the
`doc-lookup` eval path and serve stdlib docs from the JSON pack the panel
already loads. Whichever way, `(doc 'foo)` at the playground prompt should stop
warning about a name the environment was never given.

**Defect 3 -- extend the redefinition fix past `defn`, and fix the message.**
`defeffect` is the case in hand; `defstruct`, `defadt`, `defclass` and
`definstance` are worth checking in the same pass, since nothing in
`env.c:198-215` suggests `defn` was meant to be special. Separately, the
`elab_fns.c:5645` diagnostic should only say "auto-loaded stdlib module" when
the prior definition actually came from one -- otherwise it should say the name
was defined earlier in this session and point at the reset, which is the action
that works.

## Ergonomics -- the reporter's actual question

Underneath all three defects is one design question: **the Run button
re-evaluates the editor buffer into a session that still holds the previous
run's definitions.** That is right for a REPL prompt and wrong for a "run this
program" button -- running the same program twice should be idempotent, and a
user pressing Run twice is not asking to redefine anything.

Three options, cheapest first:

1. **Reset the session on Run.** `runCode()` resets to the pinned prelude
   before evaluating the buffer. The REPL prompt keeps accumulating; the Run
   button stops. This makes every symptom above unreachable from the Run path
   and costs nothing but the prelude re-pin, which is already a supported
   operation (`turi_env_reset_to_prelude`). It is also what a user already
   believes the button does.
2. **Keep accumulating but make redefinition total** -- every `def*` form
   shadows its predecessor across turns, not just `defn`. Strictly more work
   than (1) and still leaves the buffer and the session able to disagree.
3. **Leave it and fix the message.** The weakest option, but even this is worth
   doing: the current text sends the user to rename a function over a collision
   that does not exist.

(1) is the recommendation. It also subsumes defect 3 for the reported path,
though defect 3 is still real at the REPL prompt and worth fixing on its own.

## Notes

- Defects 1 and 2 are playground-only (`src/web/wasm_glue.c`). Defect 3 is in
  shared elaboration and reaches `tur repl` too -- worth checking there.
- Nothing here is a regression from the v0.45.0 Saffron work; the `defeffect`
  and doc-lookup paths predate it. Saffron only appears in the report because
  that is what the reporter had selected.
