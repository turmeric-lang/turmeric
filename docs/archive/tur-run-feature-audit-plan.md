# tur run feature audit: missing just features worth implementing

> **Archived 2026-09-09.** Phases 0-3 executed 2026-08-30. What is not done
> is what the plan itself ruled out in section 3, plus the Windows shebang
> invocation path -- Windows-port work tracked with the rest of that port
> ([windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)),
> not a phase of this audit.

> **Status:** EXECUTED 2026-08-30 (drafted 2026-08-20, inventory revised and
> plan carried out the same day). **Track:** tooling. **Type:** CLI / justfile
> compatibility.
>
> **Builds on:** the original tur-run-plan.md (RN0-RN9) and
> `src/compiler/justrun.c`.  Referenced just version: 1.54.0 (from casey/just),
> which is installed on the dev box -- every behavior below was checked
> differentially against it rather than from its docs.
>
> ## Outcome
>
> Phases 0 through 3 are implemented and landed.  What this document proposed
> as ~2-3 weeks of tiered work is done except for the items explicitly ruled
> out in section 3, plus two Windows-only remainders noted below.
>
> | Phase | Item | State |
> |---|---|---|
> | 0a | Attributes matched by name; unknown ones refused | landed |
> | 0b | Backticks in `{{ }}` no longer silently empty | landed (superseded by real evaluation) |
> | 0c | Shebang + refusal test coverage; header comment | landed |
> | 1.1 | `--set VAR VALUE` (and `VAR=VALUE`) | landed |
> | 1.2 | `[group('name')]` with `--list` sections | landed |
> | 1.3 | `[unix]`/`[windows]`/`[macos]`/`[linux]`/`[openbsd]` | landed |
> | 2.1 | Named and flag parameters via `[arg(...)]` | landed |
> | 2.2 | Modules (`mod`) and imports (`import`) | landed |
> | 3.6 | `[no-cd]` | landed, and made meaningful (see below) |
> | 3.7 | `[confirm]` / `[confirm("msg")]` prompt | landed |
> | 3.8 | `path_exists`, `replace`, `join`, `error`, `os_family` | landed |
> | 3.9 | Backtick command substitution | landed |
>
> **Three bugs were found and fixed along the way**, all of the same family --
> input accepted but silently mishandled:
>
> 1. Parameterized attributes fell through a `strcmp` chain and were skipped,
>    so `[confirm("msg")]` ran its recipe with no prompt.
> 2. Backticks inside `{{ }}` expanded to the empty string.
> 3. `interpolate()` discarded a failing builtin's NULL and spliced `""`, so
>    `env_var` on an unset variable printed a misleading "unknown built-in
>    function" note and ran the command anyway with exit 0.
>
> **One behavior change:** a recipe now runs from the Justfile's directory, as
> just does.  `tur run build` from a subdirectory used to run
> `cmake -S . -B build` in that subdirectory.  This is also what makes
> `[no-cd]` meaningful -- it was going to be a no-op otherwise, since tur run
> never chdir'd at all.  `[no-cd]`, `--chdir`, and `invocation_directory()`
> are the escape hatches.
>
> **Test coverage went from vacuous to real.**  `tools/just-vs-tur-run.sh`
> had no `tests/tur/run/fixtures/` tree at all: the `ok-*/` glob matched
> nothing and it exited 0 having compared nothing, which is indistinguishable
> from "parity holds".  It now runs 11 cases against just 1.54.0 and fails
> loudly when it finds nothing to compare.  `tests/run-tur-run-attrs.sh` adds
> 42 cases covering the refusals and attribute behavior that the parity
> harness structurally cannot check (tur run exits 2 where just exits 1).
>
> **Still open:** the Windows shebang invocation path (splitting the `#!` line
> and invoking the interpreter directly, since Windows has no kernel shebang
> handling), and everything in section 3 that was deliberately skipped.
>
> The sections below are the original analysis, kept as the rationale record.
> Where a section's inventory was wrong, an inline note says so.

## 0. Summary

`tur run` is Turmeric's built-in Justfile-compatible task runner that allows
spice and project authors to ship a `Justfile` without requiring the upstream
`just` binary to be installed.  It currently implements a subset of the full
`just` feature set.  This document audits that subset against upstream just
1.54.0, identifies the gaps, and ranks them by implementation value.

The headline numbers:

| Category | just v1.54.0 | tur run (current) | Gap |
|----------|--------------|-------------------|-----|
| Recipe attributes | ~25 types | 1 (`[private]`) | **24 missing** |
| Parameters | Full (named, flags, validation) | Basic (positional, variadic) | **Large** |
| Modules / imports | Yes | No | **All missing** |
| Built-in functions | ~50 | 10 | ~40 missing |
| Settings | ~25 | 4 | 21 missing |
| CLI flags | ~20 | ~10 | ~10 missing |
| Shebang recipes | Yes | **Yes** (since 2026-06-10) | none |

The largest and most impactful gaps are in **recipe attributes**, **parameter
features**, **modules/imports**, and **built-in functions**.

Separately from the gaps, two supported-looking paths give **wrong answers
silently** rather than erroring.  Those are bugs, and they outrank every
feature in this document — see §2.0.

## 1. What tur run supports today

Re-verified 2026-08-30 by running `./build/tur run` (v0.41.0) against scratch
Justfiles, not by reading the header comment — the header comment at
`justrun.c:7-23` is itself stale (it still says "4-space indent" and omits
shebang recipes).

- Recipe definitions with shell-command bodies (tab or 2+ space indent; the
  original 4-space-only bug was fixed)
- Recipe dependencies (chained, deps-with-arguments)
- Recipe parameters with optional defaults and variadic `+PARAM` / `*PARAM`
- Variable assignment (`name := "value"`, `export name := "value"`)
- Interpolation (`{{ var }}` in recipe bodies and dep args)
- Line prefixes (`@` silent, `-` continue on failure, `@-`, `-@`)
- Settings: `set shell`, `set dotenv-load`, `set positional-arguments`,
  `set windows-shell`
- Built-in functions: `env_var`, `env_var_or_default`, `os`, `arch`,
  `justfile_directory`, `invocation_directory`, `uppercase`, `lowercase`,
  `trim`, `quote`
- Listing: `tur run` / `tur run --list` `[--json]` `[--all]`
- Aliases: `alias NAME := TARGET` (parsed and resolved)
- The default recipe
- Dotenv loading from the Justfile's directory
- Private recipes: `[private]` attribute and `_`-prefixed names hidden from
  `--list`
- **Shebang recipes** (`justrun.c:1498-1560`): a body whose first line starts
  with `#!` is materialized to a temp file, chmod'd, and exec'd, so the kernel
  honors the interpreter.  Verified with both `#!/usr/bin/env python3` and a
  `#!/usr/bin/env bash` recipe carrying a variable across lines.

## 2. Missing features ranked by implementation value

### 2.0 Bugs found while re-verifying (do these first)

Neither of these is a missing feature.  Both are cases where `tur run` accepts
input it does not implement and produces a wrong result with no diagnostic,
which is strictly worse than the clean "install just" refusal the surrounding
code was designed to give.  Filed as
[docs/reported/tur-run-silently-ignores-parameterized-attributes.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/tur-run-silently-ignores-parameterized-attributes.md).

#### 2.0.1 Parameterized attributes are silently dropped (safety)

The attribute matcher at `justrun.c:331-358` extracts the text between `[` and
the first `]` and compares it with `strcmp` against bare names.  Any attribute
carrying an argument list therefore matches nothing, falls through the whole
block, and is skipped as an ordinary non-recipe line:

```just
[confirm("This will delete everything. Continue?")]
danger:
  rm -rf build/
```

`tur run danger` runs the recipe **with no prompt**.  Bare `[confirm]` is
correctly rejected; the parameterized form — the one that is actually useful,
and the one §2.3.2 of this document uses as its own example — is not.  The
same hole swallows `[group('build')]`, `[doc("...")]`, `[extension('.sh')]`,
and every other attribute with parentheses.

The `jr_starts_with(attr, "group:")` arm compounds it: `just` has no `group:`
syntax, so that check cannot fire on any real Justfile.

Fix direction: match on the attribute *name* (text up to `(` or `]`) rather
than the full bracket body, and reject unknown-but-parameterized attributes
instead of falling through.  Cheap, and it converts a silent unsafe execution
into the intended error.

#### 2.0.2 Backticks inside `{{ }}` expand to empty string

Backtick substitution is detected and rejected in assignment RHS
(`justrun.c:370`), but not inside interpolation:

```just
show:
  echo "inline {{ `echo xyz` }}"
```

prints `inline ` — the substitution silently yields nothing.  A user gets a
plausible-looking command with a value quietly missing, rather than the
"install just" error the assignment path would have given.

Fix direction: extend the existing backtick detection to interpolation bodies
so both paths refuse consistently.  Implementing real backtick evaluation
(§2.3.4) subsumes this, but the refusal is a fraction of the work and should
not wait on it.

### 2.1 Tier 1: high value, relatively low effort

These are the "quick wins" — features that provide significant user benefit
for modest implementation cost.

| Feature | just | tur run | User value | Implementation effort |
|---------|------|---------|------------|----------------------|
| `--set VAR VALUE` | Yes | No | High | Low |
| `[group('name')]` | Yes | No | High | Low-Medium |
| `[unix]` / `[windows]` | Yes | No | High | Medium |

#### 2.1.1 `--set VAR VALUE` CLI flag

Allows overriding justfile variables from the command line without editing the
file.

```sh
# Instead of editing the Justfile, users can do:
tur run --set CFLAGS="-O3" build
```

Implementation: add to argument parsing in `cmd_justrun`, pass through to
variable evaluation.  Low effort, high user demand.

#### 2.1.2 `[group('name')]` attribute

Enables organizing recipes into logical groups for `--list` output.  With
60+ recipes in the main turmeric Justfile, the flat list is overwhelming.

```just
[group('build')]
debug:
  ...

[group('build')]
release:
  ...

[group('test')]
test: build
  ...
```

Implementation: parse attribute, store on recipe, modify `--list` output to
print group headers.  Low-medium effort, high UX improvement.

#### 2.1.3 `[unix]` and `[windows]` platform attributes

Enable cross-platform Justfiles that adapt to the current OS.

```just
[unix]
configure:
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

[windows]
configure:
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Implementation: parse attributes, check `os()` at runtime, skip or error on
non-matching recipes.  Medium effort, high value for cross-platform projects.

### 2.2 Tier 2: high value, medium effort

| Feature | just | tur run | User value | Implementation effort |
|---------|------|---------|------------|----------------------|
| Named / flag parameters | Yes | No | High | Medium-High |
| Modules / imports | Yes | No | High | High |

#### 2.2.1 Named and flag parameters

Essential for Justfiles that need CLI-like interfaces.

```just
# Current (supported):
tag VERSION:
  git tag -a v{{VERSION}}

# Desired (missing):
[arg('version', short='v', long='version')]
[arg('force', long='force', value='true')]
tag version force?:
  git tag -a v{{version}} {{force}}
```

Support for `--name value`, `--name=value`, `-n value`, and `--flag` syntax.
Implementation requires parsing attribute syntax, storing parameter metadata,
and handling option-style argument passing.  Medium-high effort but unlocks
CLI-like Justfile interfaces.

#### 2.2.2 Modules and imports

Critical for organizing large Justfiles.  The main turmeric repo's Justfile
is 577 lines spanning build, docs, web, perf, release, and Windows concerns.

```just
# Root justfile
mod build
mod docs
mod web
```

With each module in its own file (`build.just`, `docs.just`, etc.).

Implementation: add module/import parsing, file loading, scoping rules,
dependency management between modules.  High effort but essential for
large-scale adoption.

#### 2.2.3 Shebang recipes — ALREADY IMPLEMENTED, no work needed

This item was incorrect when written.  Shebang recipes have worked since
`5ed86be1f` (2026-06-10); `justrun.c:1498-1560` detects the `#!` first line,
writes the interpolated body to a temp file, marks it executable, and execs
it.  Confirmed working for Python and for bash recipes that carry state
across lines.

Two caveats remain, neither blocking:

- **No test coverage.**  Nothing in `tools/just-vs-tur-run.sh`,
  `tests/run-tur-run-alias.sh`, or `tests/run-tur-run-rhs-eval.sh` exercises a
  shebang recipe.  That is why this feature could sit implemented for two
  months and still get audited as missing.  Adding one parity case is the
  cheapest item in this document.
- **Windows.** The implementation relies on `mkstemp` + `chmod` + kernel
  shebang handling.  The "split the shebang and invoke the interpreter
  directly" path the original entry described is still needed for Windows;
  that part of the item is genuinely open.

### 2.3 Tier 3: medium value, low-medium effort

| Feature | just | tur run | User value | Implementation effort |
|---------|------|---------|------------|----------------------|
| `[no-cd]` | Yes | No | Medium | Low |
| `[confirm]` | Yes | No | Medium | Low |
| `path_exists()` etc. | Yes | No | Medium | Low-Medium |
| Backtick substitution | Yes | No | Medium | Medium |

#### 2.3.1 `[no-cd]` attribute

Prevent `tur run` from changing directory to the Justfile's location before
executing the recipe.

```just
[no-cd]
run-here:
  echo "Running from {{invocation_directory()}}"
```

Implementation: simple flag check before chdir.  Low effort.

#### 2.3.2 `[confirm]` attribute

Require user confirmation before executing destructive operations.

```just
[confirm("This will delete all build artifacts. Continue?")]
clean:
  rm -rf build/
```

Implementation: prompt user, read input, abort if not confirmed.  Low
effort, high safety value.

**Note (2026-08-30):** the example above is exactly the form that today runs
`rm -rf build/` with no prompt and no error — see §2.0.1.  Until this is
implemented, the parser must at minimum *reject* it.  Do the rejection now;
the prompt itself can follow at this tier.

#### 2.3.3 Additional built-in functions

Most commonly requested: `path_exists()`, `replace()`, `join()`, `error()`.

Implementation: per-function additions to `eval_builtin`.  Low-medium effort,
medium user value.

#### 2.3.4 Backtick command substitution

Allow shell command evaluation in variable assignments and interpolation.

```just
GIT_REV := `git rev-parse HEAD`

build:
  echo "Building at {{ `git rev-parse --short HEAD` }}"
```

Implementation: shell evaluation at parse time adds complexity (security,
quoting, caching).  Medium effort, medium value.  Can be worked around with
explicit recipes.

**Partial state (2026-08-30):** assignment-RHS backticks are already detected
and refused (`justrun.c:370`).  Interpolation backticks are not, and silently
expand to the empty string — see §2.0.2.  Closing that inconsistency is a
prerequisite for this item and worth doing independently of it.

## 3. Features NOT worth implementing (yet)

### 3.1 Unstable upstream features

- `[cache]` attribute — still unstable in just 1.54.0
- `set lists` — still unstable in just 1.54.0

Wait for upstream stabilization before implementing.

### 3.2 Niche or situational features

- Most additional settings (`allow-duplicate-recipes`, `fallback`,
  `windows-powershell`, etc.) — too niche, not enough demand
- Most additional CLI flags (`--choose`, `--completions`, `--dump`,
  `--edit`, `--man`) — not critical for core use cases
- Style functions (`style()`) — very situational
- Hash/UUID functions (`sha256()`, `uuid()`) — rarely needed in build scripts
- Datetime functions — niche use cases
- Semver matching — situational
- User directory functions — can use `env_var("HOME")` etc.

### 3.3 Can be worked around

- Conditional expressions — can use separate recipes
- Heredoc/multi-line strings — can use multiple recipe lines
- Recipe groups with `--group` flag — depends on attribute support

## 4. Recommended implementation order

### Phase 0: Stop the silent wrong answers (hours, not days)

0a. Match attributes by name so `[confirm("...")]` and friends are rejected
    rather than skipped (§2.0.1).  Safety-relevant; do it first.
0b. Reject backticks inside `{{ }}` as the assignment path already does
    (§2.0.2).
0c. Add a shebang case to `tools/just-vs-tur-run.sh`, and refresh the stale
    `justrun.c:7-23` header comment.

None of these add a feature; all three convert a wrong answer into an honest
error, and 0c is what stops this audit from drifting again.

### Phase 1: Quick wins (1-2 days each)

1. `--set VAR VALUE` CLI flag
2. `[group('name')]` attribute
3. `[unix]` / `[windows]` attributes

These provide immediate user-visible improvements with minimal risk.  Note
that (2) lands naturally on top of 0a, which has to parse the attribute
argument list anyway.

### Phase 2: Core gaps (3-7 days each)

4. Named and flag parameters
5. Modules and imports

These address the largest functional gaps and are essential for serious
Justfile usage.  (Shebang recipes were formerly item 6 here; they are already
implemented — see §2.2.3.  Only the Windows invocation path remains.)

### Phase 3: Nice-to-have (1-3 days each)

6. `[no-cd]` attribute
7. `[confirm]` prompt behavior (the *rejection* is Phase 0a)
8. Additional built-in functions (`path_exists`, `replace`, `join`, `error`)
9. Backtick command substitution (the *rejection* is Phase 0b)

These round out the feature set for power users.

## 5. What full parity would require

To achieve full `just` v1.54.0 compatibility, `tur run` would need to add:

- ~24 additional recipe attributes
- Full parameter system (named, flags, validation, constraints)
- Complete module and import system
- ~40 additional built-in functions
- ~21 additional settings
- ~10 additional CLI flags
- Conditional expressions
- Backtick command substitution
- (Shebang recipes: done, except the Windows invocation path)

Estimated effort: **6-8 weeks of focused work** for a single developer,
or **2-3 sprints** for a small team.  The value proposition is questionable
for Turmeric's use case — most spices need only the core subset, and users who
need advanced just features can install `just` itself (the documented fallback
path).

## 6. Decision framework

When considering whether to implement a missing feature, ask:

1. **Is it on the critical path for a real spice?**  If yes, prioritize.
2. **Does it have a simple workaround?**  If yes, deprioritize.
3. **Does it enable a common pattern we see in the wild?**  If yes, prioritize.
4. **Is the implementation effort < 2 days?**  If yes, probably worth doing.
5. **Is the feature stable upstream?**  If no, wait.

Using this framework:

Note that the framework as written has no row for "this silently does the
wrong thing."  It ranks *absent* features, so a bug that presents as a
working feature scores nothing and stays invisible — which is how §2.0.1
survived the original pass.  Rule 0: a silent wrong answer outranks every
feature question below, regardless of how the five criteria score.

| Feature | Critical? | Workaround? | Common? | Effort | Stable? | Decision |
|---------|-----------|-------------|---------|--------|---------|----------|
| Attr-name matching (§2.0.1) | **Yes** | None | Yes | Low | Yes | **Bug — do first** |
| Backtick refusal (§2.0.2) | **Yes** | None | Yes | Low | Yes | **Bug — do first** |
| `--set` | No | No | Yes | Low | Yes | **Do it** |
| `[group]` | No | No | Yes | Low | Yes | **Do it** |
| `[unix]/[windows]` | Yes | Bash conditionals | Yes | Medium | Yes | **Do it** |
| Named params | No | Separate recipes | Yes | Medium | Yes | **Do it** |
| Modules | Yes | Manual splitting | Yes | High | Yes | **Do it** |
| Shebang | -- | -- | -- | -- | -- | **Already done** |
| `[cache]` | No | Manual caching | No | Medium | **No** | Wait |
| Style functions | No | Shell echo | No | Low | Yes | Skip |

## 7. Related work

- Original tur-run-plan.md: the v0.1.0 scope and phase breakdown (RN0-RN9),
  archived at
  [docs/archive/history/tur-run-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/tur-run-plan.md)
- `src/compiler/justrun.c`: the current implementation (2050 lines)
- `tools/just-vs-tur-run.sh`: CI parity test between `just` and `tur run`
- `tests/run-tur-run-alias.sh`: alias resolution regression test
- `tests/run-tur-run-rhs-eval.sh`: RHS evaluation tests

**Coverage gap.**  None of the three harnesses above exercises a shebang
recipe, a parameterized attribute, or an interpolated backtick — which is
precisely the set of things this revision found wrong.  The unsupported-feature
*refusals* are the load-bearing safety property of `tur run`, and nothing
currently asserts that they fire.  A table-driven "these constructs must be
refused" case in `tools/just-vs-tur-run.sh` would have caught both §2.0 bugs.

## 8. Open questions

1. Should we add a `--strict` or `--pedantic` mode that errors on any
   unsupported feature, for CI use?  Currently errors are only raised when
   trying to execute an unsupported feature.

2. Should we track "partial support" vs "no support" separately?  For example,
   we support basic aliases but not module aliases (`alias f := frontend`).

3. Should we maintain a formal compatibility table in the docs, showing
   which just features are supported in which `tur run` version?

4. Is there value in adding a `--compatibility` flag that warns on any
   constructs not in our supported subset, even if they happen to work?

5. (2026-08-30) Should unknown attributes be refused by default rather than
   skipped?  Today the parser knows a fixed list of names and ignores
   everything else, so every attribute `just` adds in future becomes a silent
   no-op in `tur run`.  Refuse-by-default would have made §2.0.1 impossible
   and is roughly the same amount of code.

---

*Generated by Mistral Vibe.*
*Co-Authored-By: Mistral Vibe <vibe@mistral.ai>*

*Inventory revised 2026-08-30 against `./build/tur` v0.41.0 (§0 header, §1,
§2.0, §2.2.3, §2.3.2, §2.3.4, §4, §5, §6, §7, §8.5).*