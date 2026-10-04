# `:run` / `:reload` on a sweet file switch the session's reader

**Severity:** low-medium (`:run`); medium (`:reload`, which discarded the session).
**Status:** RESOLVED (2026-10-03). Both commands now evaluate a file whose
reader differs from the session's as a `(load "...")` form, so the session's
reader never changes; a Scheme file in a non-Scheme session is declined by
name. See [Resolution](#resolution).
**Found:** 2026-10-03, against v0.60.1.

*Filed as "`:run` on a sweet file invokes `main` but leaves no definitions
behind". That diagnosis was wrong -- the definitions were always there -- and
this is the rewrite. The slug is kept so links to it still resolve.*

## What was reported

```sh
$ printf ':run both.tur.sweet\nboth-x\n' | tur repl
main ran
             <-- nothing for `both-x`, then `(cancelled)`
```

and from that, that `:run` dropped a sweet file's top-level definitions.

## What actually happens

The definitions are in the session. A second line shows it:

```sh
$ printf ':run both.tur.sweet\n(+ both-x 1)\n' | tur repl
main ran
=> 0
;; ready
=> 8

$ printf ':run both.tur.sweet\nboth-x\n\n' | tur repl     # note the blank line
...
=> 7
```

What changed is the **session's reader**. `cmd_run` evaluates the file with
`turi_eval_file`, which applies the file's extension to the session
(`src/turi/eval.c:15190-15193`):

```c
ReaderType ext_type = reader_type_from_extension(path);
if (ext_type != READER_TURMERIC && ext_type != env->reader_type) {
    turi_env_reset_to_prelude(env);
    env->reader_type = ext_type;
}
```

So after `:run both.tur.sweet` a plain Turmeric prompt reads every later line
as sweet-exp. A bare `both-x` is a complete sweet-exp datum only once the
reader has seen the line after it -- a blank line, or one that is not indented
-- so at the end of piped input it was still pending, and the REPL printed
`(cancelled)`. With no error message, that looks like an unbound name.

The assignment is not a bug in isolation. The REPL rebuilds each turn from its
accumulated source, re-read under `env->reader_type`, so once sweet source has
been accumulated the session *must* keep reading sweet, and a reader change
*must* discard what came before (which is what `turi_env_reset_to_prelude`
does). The defect was in sending a whole file's text through that path when
the session's reader differs from the file's.

`load` never had the problem, and that is why the original table showed it
working: `(load "x.tur.sweet")` reads the file under its own reader, and what
the session accumulates is the `load` form, which reads the same under any
reader.

## The worse half: `:reload`

`:reload` used the same call on the *live* session, so the reset discarded
everything typed before it:

```sh
$ printf '(def keep 9)\n:reload both.tur.sweet\nkeep\n' | tur repl
=> 9
error [TUR-E0003]: unbound symbol 'keep'
```

## And a Scheme file

`:run x.scm` in a Turmeric session set the session to R7RS halfway: the reader
and language switched, but the Scheme prelude a Scheme session loads was never
loaded. The file's own `display` failed (`unknown function or operator
'r7rs-display'`), and so did a later `(+ 1 2)`. There is no way to run a
Scheme file inside a Turmeric session, so the right answer is to say so.

## Resolution

`repl_eval_file_keeping_reader` (`src/turi/repl.c`), called by `cmd_run` and
`cmd_reload`, works out the file's own (reader, language) with the precedence
every CLI entry point uses -- extension, then `#lang` line -- and then:

- **same reader as the session**: `turi_eval_file`, as before;
- **different reader, compatible language**: evaluates `(load "<path>")`, so
  the file is read under its own reader and the session's reader is untouched;
- **another language** (a `.scm` file outside an R7RS session): declines with
  `:run: x.scm is r7rs, and this session is not -- start one with
  `tur repl --lang r7rs` to :run it`.

```sh
$ printf ':run both.tur.sweet\nboth-x\n' | tur repl
main ran
=> 0
;; ready
=> 7
$ printf '(def keep 9)\n:reload both.tur.sweet\nkeep\n' | tur repl
=> 9
reloaded both.tur.sweet
=> 9
```

Pinned by `tests/fixtures/repl-run-sweet-keeps-session-reader` (all three
cases). The REPL guide's `:reload` section now states the rule.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo -- the editor this was found from.)*

Trowel's smoke test `test_run_sweet_buffer_does_not_leave_its_definitions_behind`
pins the *symptom*, not the mechanism: if its probe ends on a bare symbol with
no blank line after it, that is the `(cancelled)` case above. Against a fixed
`tur` it should now see the definition. Rename it for what it checks, or
delete it.
