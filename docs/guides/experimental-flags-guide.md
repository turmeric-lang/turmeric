---
title: Experimental Feature Flags
category: CLI Tools
description: The `--enable=<name>` surface for genuinely in-flight compiler features, the two lifecycles, the TUR-W006x warnings, and the `tur experiments` command.
---

# Experimental Feature Flags

Turmeric gates genuinely **in-flight** compiler features behind a single,
uniformly-named opt-in surface: `--enable=<name>`. This is the successor to
the retired `-X<name>` regime (see
[compiler-flags-guide.md](compiler-flags-guide.md#removed-feature-flags)).
Unlike `-X`, which conflated "in development", "stable but opt-in", and
"stable and on" into one namespace until it accreted 16 entries, the
`--enable=` mechanism is *only* for features that are not yet stable, and
every flag carries a hard expiry by which it must graduate or be removed.

> **The registry is small by design.** Run `tur experiments` for the
> authoritative current set (at the time of writing it holds two rows:
> `reflected-measures` -- `^reflect` on a defn admits a total measure's
> defining equation to the refinement solver, see
> [reflected-measures-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/reflected-measures-plan.md)
> and the refinement guide's "Reflected measures" section; and
> `repl-jit-inline-c` -- the interpreter compiles an inline-C defn it cannot
> run, see [aot-compiled-repl-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/aot-compiled-repl-plan.md)
> and the REPL guide's "Inline-C at the prompt" section); this guide
> describes how the mechanism behaves for whatever rows are present.

## Opting in

On the command line, pass a comma-separated list:

```sh
tur build --enable=fancy-rows,deep-refinements src/
```

Or, per-spice, in `build.tur` with a top-level `:experiments` key (names are
keywords):

```turmeric
(defpackage my-spice
  :name "my-spice"
  :version "0.1.0"
  :experiments [:fancy-rows :deep-refinements])
```

Or, per-user, in `$XDG_CONFIG_HOME/turmeric/experiments.tur` (falling back to
`$HOME/.config/turmeric/experiments.tur`; on Windows, `%APPDATA%\turmeric\experiments.tur`):

```turmeric
;; ~/.config/turmeric/experiments.tur
;;
;; Experiments to enable by default in every turmeric invocation that
;; is NOT running inside a project whose build.tur declares its own
;; :experiments list.

:enable [fancy-rows
         deep-refinements]
```

All three shapes resolve to the same internal set. Precedence, lowest to
highest:

1. **User file** (`~/.config/turmeric/experiments.tur`) -- your per-machine
   baseline.
2. **Project manifest** (`build.tur` `:experiments`) -- the project's stated
   set. **Any `:experiments` key -- even the empty list `:experiments []` --
   fully suppresses the user file.** The project owner has stated their
   intent; user preferences do not silently union in.
3. **CLI** (`--enable=<name>`) -- the per-invocation override; wins over both.

An unknown name -- on the CLI, in the manifest, or in the user file -- is a
**hard error**, not a warning, so typos surface immediately:

```
error [TUR-E0310]: unknown experiment 'fancy-roows'; run 'tur experiments' for the list
```

A **graduated** name (a feature that was gated and is now unconditionally
on) is accepted as a no-op with a `TUR-W0063` warning rather than the hard
error, so a downstream `build.tur` or `experiments.tur` that opted in keeps
compiling across the graduation boundary.

That shim is a **migration window, not a permanent alias**. One minor line
after graduation the name ages out of `GRADUATED[]` and goes back to being a
hard `TUR-E0310` -- so treat `TUR-W0063` as a deprecation notice with a
deadline and delete the flag when you see it. Twelve names aged out this way at
0.37.0 and 0.38.0; `jit-ffi`, which graduated in 0.38.0, is the only shim
currently live and becomes eligible at 0.39.0. The same rule and the same
one-line window apply to a graduated `#lang` layer token (`TUR-W0064`, then
`TUR-E0330`).

Unknown *keys* in the user file (anything other than `:enable`) are a
`TUR-W0062` warning and otherwise ignored -- forward-compatible with future
additions.

Run `tur experiments` to see the exact set of recognized names.

## The two lifecycles

Every experiment is in one of two lifecycle states, which determines the
warning you see at the first use site:

| Lifecycle | Meaning | Warning (once per compile) |
|---|---|---|
| `prototype` | Algorithm or surface still changes between releases. | `TUR-W0060: experimental feature '<name>' (prototype) -- breaking changes likely; see <plan>` |
| `beta` | Surface frozen, soaking for one release cycle before graduation. | `TUR-W0061: experimental feature '<name>' (beta) -- graduates in <expires_at>; see <plan>` |

The warning fires **once per compile**, on first use, regardless of how many
times the feature is used in the file. Each warning links to the feature's
plan document.

The warnings always fire when the flag is enabled -- that is the design
point: the cost of the opt-in scales with its riskiness. There is no gate to
silence them; enabling an experiment (via `--enable=<name>`, `build.tur`, or
`~/.config/turmeric/experiments.tur`) is itself the acknowledgment. The
`--allow-experimental` suppression flag was retired -- passing it is now a
hard error that points you here.

## `tur experiments`

The registry is the single source of truth; this guide does not restate the
list -- the command does:

```sh
tur experiments          # human-readable table
tur experiments --json   # machine-readable (the docs site consumes this)
```

The table shows each entry's name, lifecycle, the version it was introduced
in, its expiry, whether it is enabled in the current invocation, and the
plan link. When an experiment is enabled, the table also shows where the
enable came from (`cli`, `manifest`, or `user-config`) so a surprise is easy
to debug.

## Expiry policy

No flag should sit at "experimental" indefinitely. Every entry carries an
`expires_at` version. At a cut where some entry's `expires_at` is at or before
the version being cut, the release author reviews that entry and either:

- **graduates** the feature -- deletes the row from the registry; the feature
  becomes always-on (its enable bit stays `true`, mirroring how the retired
  `-X<name>` flags became accept-and-warn no-ops), or
- **shelves** it -- removes the row and the feature, or
- **bumps `expires_at`** with a one-line rationale in the row's plan doc. This
  is a normal, precedented move (see the `cps-async` contingency), not a
  failure.

`expires_at` is a **deadline, not an earliest date** -- graduating early is
routine (`closure-drop-glue` graduated at 0.30.2 carrying `expires_at 0.34.0`).

### Graduating flips a default -- so the old default's harness INVERTS, it does not retire

When a feature graduates, the path it *stopped* being becomes the path nothing
compiles any more. If a bisection hatch survives graduation (`--enable` gone,
`TUR_<NAME>=0` kept as the A/B switch), the harness that covered the old default
does not retire with the flag: **it inverts, and keeps covering what is now the
off path.**

Four graduations have hit this and three got it right.
`run-sr4-seam.sh`, `run-option-niche-seam.sh` and `run-regions-seam.sh` all
flipped their harness at graduation; SR2's was deleted, on the reasoning that "every `bash tests/run.sh`
compiles all eleven of its fixtures that way now" -- true, and it covers the ON
path, while leaving the OFF path with no cover at all. That is precisely the
state the harness had been built to prevent, with the two paths swapped.

What accumulated there in the meantime: a compiler abort, a hard C compile
error, and a silent wrong answer, none of which touched the default path and all
of which broke the instrument you reach for when something *else* is already
wrong. See [docs/archive/sr2-carrier-seam-rotted.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/sr2-carrier-seam-rotted.md).

A hatch nobody turns on decays into a hatch nobody notices -- the same lesson as
`sanitizer-gate-not-armed-in-ci`. If you keep the switch, keep a harness on it.

### `expires_at` is ADVISORY -- it never blocks a release

**An expiring entry is surfaced at the cut. It does not refuse the cut.** The
release-cut skills report expiring rows and proceed; the author decides what to
do about them, in that release or a later one.

Earlier revisions of this guide called `expires_at` a "hard contract" that the
release-cut process "refuses to proceed" past. **That tooling never existed** --
`cut-minor-release.md` and friends contain no registry scan of any kind -- and
the prose alone was enough to strand two releases, because a reader would
honour a gate that was not there. Releasing is never blocked on an experiment's
expiry.

The forcing function is the review prompt, not a refusal. It still answers the
`-X` regime's drift: an expiry repeatedly bumped with no rationale is a signal
to shelve, and that is a judgement the author makes -- not a gate the tooling
imposes.

## If you see `TUR-W006x` in your build -- runbook

You are seeing a `TUR-W0060`/`TUR-W0061` because something enabled an
experiment. Walk it back:

1. **Did you pass `--enable=`?** Check your invocation and any wrapper
   scripts. Drop the flag if you did not mean to opt in.
2. **Does a `build.tur` in scope list `:experiments`?** A spice you are
   building (or one of its enclosing workspace manifests) may have opted in.
   Run `tur experiments` from that directory -- the `ENABLED` column shows
   the source.
3. **Is your user file enabling it?** Check
   `$XDG_CONFIG_HOME/turmeric/experiments.tur` (or
   `~/.config/turmeric/experiments.tur`). `tur experiments` will show
   `user-config` in the source column for anything that came from there.
   Remember: this file is suppressed when the current project's `build.tur`
   carries any `:experiments` key, including the empty list.
4. **Is the warning expected?** If you genuinely want the feature, the
   warning is informational: the feature works, it is just not stable. Do
   **not** depend on its surface from a published spice yet -- a `prototype`
   feature can change shape in the next release, and a `beta` feature
   graduates (and its flag becomes a no-op) on the version named in the
   `TUR-W0061` message.
5. **The warning is informational, not suppressible.** There is no flag to
   silence it -- `--allow-experimental` was retired. If an experiment is
   enabled, the `TUR-W006x` line fires once per compile; that is intended.

## What is *not* an experimental flag

The `--enable=` surface is for in-flight **features** only. These stay where
they are:

- **Diagnostic strictness** (`--strict-effects`, `--lint-panic`, ...) -- tunes
  noise on already-shipped behavior; see
  [compiler-flags-guide.md](compiler-flags-guide.md#diagnostic-flags).
- **Codegen / operator knobs** (`--emit-abi-trace`, `--dump-*`).
- **Build-system options** (`--build-dir`, `-I`, `--no-auto-spice`).
- **Partial-but-shipping features** that already went always-on at their
  current level (e.g. sized types, uniqueness types). Those are not re-gated
  retroactively; new partial features land behind `--enable=` from day one.

## For compiler contributors

A new in-flight feature ships behind `--enable=<name>`, never gateless until
graduation. Add one row to `EXPERIMENTS[]` in
`src/runtime/experiments.c` with **all seven fields** populated -- `name`,
`summary`, `plan_path`, `introduced`, `expires_at`, `lifecycle`, and an
`opt_global` pointing at a `g_opt_<name>` bool the feature's elaboration
reads -- and a plan in `docs/upcoming/`. Call `experiment_warn_if_used(name)`
from the feature's elaboration entry point (the helper handles the
once-per-compile dedup). The CLI/manifest parsing, the `tur experiments`
listing, and the release-cut expiry review all pick the entry up
automatically from the table.

## See also

- [compiler-flags-guide.md](compiler-flags-guide.md) -- diagnostic/debug
  flags and the retired `-X<name>` set.
- [docs/archive/history/experimental-flag-mechanism-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/experimental-flag-mechanism-plan.md)
  -- the design plan (XF0--XF6).
