# The spice READMEs fail the strict guide-pairs check, and nothing runs it

**Severity: low (docs; no gate)** -- 57 Turmeric blocks in the spice READMEs
have no sweet-exp companion and 3 pairs do not parse-check. The checker
that would catch them (`--spices`, which implies `--strict-unpaired`) runs
in no CI job in either repo, so both numbers can only grow. turmeric-spices
still carries the old Python checker as well.

**Status: OPEN.** Filed 2026-10-23, measured against turmeric `main` @
`43bee293` (Release `tur` built from source) and turmeric-spices `main` @
`ac688be`.

## Repro

From a turmeric checkout with `../turmeric-spices` beside it:

```sh
./build/tur run tools/check-guide-pairs.tur -- --tur build/tur --spices
```

```
Files checked    : 51
Files with pairs : 35
Pairs found      : 53
Pairs ok         : 50
Pairs failed     : 3
Unpaired blocks  : 57
Manifests found  : 1
Manifests failed : 0
Early-closing fences: 0
```

Exit status 1. `tur run check-spices` (Justfile) runs the same command.

## The 3 failing pairs

| README | Line | Why |
| --- | --- | --- |
| `spices/glsl/README.md` | 29 | The sweet-exp side writes `println $` with nothing after the `$` and the argument on the indented lines below. It reads as `(println () (compile-glsl ...))`: an extra `()` argument. Drop the `$` and keep the indented child. |
| `spices/nng/README.md` | 45 | The two halves are different programs: the s-expression side is a req/rep `ping`/`pong`, the sweet-exp side a push/pull `job:42`. One was rewritten and the other was not. |
| `spices/sdf-raylib/README.md` | 44 | The sweet-exp side writes `println $ "SDF at origin:" sdf-eval(...)`. `$` wraps the rest of the line in one list, so it reads as `(println ("SDF at origin:" (sdf-eval ...)))`: a call to a string. Write `println("SDF at origin:" sdf-eval(scene 0.0 0.0 0.0))` (README line 83). |

## The 57 unpaired blocks, by spice

ansi 2, c-dsl 1, crdt 5, ecs 2, ecs-raylib 1, glsl 1, http 1, httpd 2,
json 2, linalg 2, msgpack 3, nng 5, notebook 3, secret 4, template 1,
thread-pool 5, tls 2, tourist-session 6, tourist-session-valkey 2, valkey 1,
watch 2, ws-client 2, ws-server 2.

Each one needs either a ```` ```sweet-exp ```` companion or a
```` ```turmeric no-check ```` marker (install, config and API-signature
blocks, where a second spelling adds nothing).

## Why nothing catches it

- **turmeric CI** runs the checker in the check-guides job on
  `docs/guides/` only (`.github/workflows/ci.yml`, "Check guide pairs"). No
  step passes `--spices`, though that job already checks out the spices
  sibling for other steps.
- **turmeric-spices CI** (`.github/workflows/ci.yml`) has no docs step at
  all.
- **turmeric-spices** still has `tools/check-guide-pairs.py`, the Python
  checker that `tools/check-guide-pairs.tur` replaced
  ([markdown-library-plan](../upcoming/markdown-library-plan.md) M5c). Only
  its Justfile's `check-docs` recipe calls it. When last compared it reported
  the same lines on the spice READMEs, but it is no longer maintained:
  checks added to the `.tur` port (list-item and block-quote fences, the
  absolute `--tur` path the manifest check needs) do not reach it.

## Fix directions

1. Fix the 3 pairs. They are wrong as published, so they matter more than
   the unpaired count.
2. Add companions or `no-check` markers for the 57 blocks, by spice.
3. Once the check is green, gate it: one step in turmeric's check-guides job
   (`./build/tur run tools/check-guide-pairs.tur -- --tur build/tur
   --spices`), or one in turmeric-spices CI against the `tur` it already
   builds. Gating before step 2 would turn the job red.
4. Delete `tools/check-guide-pairs.py` from turmeric-spices and point its
   `check-docs` recipe at the `.tur` tool, so there is one checker.
