---
title: Time-Travel Tracing Guide (`tur trace`)
category: Editor and IDE
description: Recording an interpreted run and scrubbing it -- what the .turtrace format holds, what the step cap protects, and how `tur dap` replays a recording so a debugger can step backwards
tags:
  - tracing
  - time-travel
  - dap
  - debugging
  - tur-trace
---

# Time-Travel Tracing Guide (`tur trace`)

`tur trace <file.tur>` runs a program under the interpreter with a recorder
attached, writing every node it evaluates into a byte buffer. The recording can
then be read back, summarised, or replayed as a debugging session that steps
**backwards**.

```sh
tur trace hello.tur                        # record; print a summary
tur trace hello.tur -o run.turtrace        # record; write the bytes
tur trace hello.tur --max-steps=50000      # cap the recording
tur trace hello.tur --lines                # coarser: one step per source line
tur trace --dump run.turtrace              # read one back, record by record
```

A program that defines a top-level `main` records the run of `(main)`: its
definitions load first, un-recorded, and the recording starts at `main`'s
first form. A program with no `main` is a top-level program -- its forms are
the run -- so the recording (and, under `tur dap`, the entry stop and any
breakpoints) starts at the file's first form instead. The same rule decides
where `tur run` starts, so the two never disagree.

## Why record, when the debugger already pauses

`tur debug` and `tur dap` both pause a live program today, with frames, locals
and in-frame `evaluate`. So the case for a recording is not that a pause is
impossible. It is three other things:

1. **Backwards.** The question a debugger cannot answer is *how did this value
   come to be 7*, and stepping back is the answer. A pause cannot go back.
2. **It is nearly free to build.** The pause handler is already called at every
   node and already has an API for frames and locals, so
   `turi_debug_set_pause_handler` plus resume-step-node in a loop *is* a tracer.
   There is no source instrumentation anywhere in this: the interpreter is
   ours, and `turi_debug_frame_at` gives real frames rather than sentinel
   addresses that have to be ordered by guesswork.
3. **The web has no debugger.** `tur dap` is stdio, and a browser tab has no
   way to host a blocking pause loop. A trace is a byte buffer, and a byte
   buffer crosses the wasm boundary with no protocol at all -- which is what
   Try Turmeric's timeline is built on. See
   [Recording in the browser](#recording-in-the-browser).

The recorder is an **interpreter** feature. Compiled programs are not traced; a
native tracer is a different job and would start from the `#line` emission
behind `--debug`.

## What a recording holds

```
header   "TURTRACE\0"        9 bytes
         u16 version         (2)
         u8  flags           bit 0: truncated
                             bit 1: node granularity (clear = line)
         u32 name_count      name[name_count]
         u32 site_count      site[site_count]
         u32 record_bytes    record[] filling exactly that many bytes
name     u16 len, u8 bytes[len]
site     u32 file_name, u32 fn_name, u32 line, u32 col, u32 col_end
record   u8 tag
  1 ENTER   u32 site, u16 depth
  2 STEP    u32 site, u16 depth, u16 n, change[n]
  3 POP     u16 depth
  4 OUTPUT  u32 len, u8 text[len]
change   u32 name, u16 len, u8 repr[len]
```

All integers are little-endian. Everything that would otherwise repeat -- file
paths, function names, binding names -- is interned in the name table, so
nothing crosses as a string at run time. It crosses as *bytes* rather than as a
C string because a rendered value may contain a NUL, and this is the one thing
here that reaches megabytes.

Three properties are worth stating outright:

- **Deltas, not states.** A `STEP` carries only the bindings whose rendered
  value changed since that frame's last step. A step carrying every live
  variable repeats the whole frame on every pass of a loop; in practice one or
  two values move per node. Measured on a 20,000-iteration `while` loop:
  140,008 steps in 1.47 MB, about 10 bytes a step.
- **No keyframes.** A decoder builds its own snapshots if it wants them, which
  is the same work in a language that can afford it, and keeps the recorder to
  one rule: write what changed.
- **Output is interleaved.** The program's stdout is captured through a pipe
  and drained at every node, so `OUTPUT` records sit in step order and a scrub
  backwards rewinds the transcript with the cursor. It is forwarded to the real
  stdout on the way past -- the recorder is a recorder, not a muzzle.
  (Unavailable on Windows, where the non-blocking pipe read this relies on has
  no counterpart; the recording simply carries no `OUTPUT` records there.)

Only the *innermost* frame's locals are recorded per step. An outer frame
cannot change while a callee is executing, so recording it would repeat the
whole stack on every node of every call.

`col_end` and the granularity flag arrived in **v2**. The reader still accepts
a v1 recording -- its sites are 16 bytes rather than 20 and read back with
`col_end` 0, which is how a client knows it has a point rather than a range.

## A step is one expression

The unit of a recording is an **evaluation**, not a source line. `--dump` shows
it: a site is a column range, and the sub-expressions of a line are steps in
their own right.

```
$ tur trace gran.tur -o g.turtrace && tur trace --dump g.turtrace
turtrace v2  names=6 sites=13 records=164 bytes  steps=per expression  truncated=no
ENTER  depth=1 gran.tur:4:3-56 main
STEP   depth=1 gran.tur:4:3-56 main         ; (let [a (f (g 3))] ...)
STEP   depth=1 gran.tur:4:11-20 main        ;         (f (g 3))
STEP   depth=1 gran.tur:4:14-19 main        ;            (g 3)
STEP   depth=1 gran.tur:4:17-18 main        ;               3
ENTER  depth=2 gran.tur:2:25-32 g
STEP   depth=2 gran.tur:2:25-32 g  x=3
```

This matters more here than it would in a C-shaped language. A line is a unit
of *layout*; in a Lisp one line routinely holds a whole expression tree, and
Turmeric leans further that way than most -- neoteric `f(g(x))` and sweet-exp
`$` chains exist to put **more** on a line, not less.

Recording per line, as the recorder originally did, has two consequences that
are hard to defend in a debugging record:

- **A loop whose body fits on one line collapses into one step.** The
  induction variable jumps from its first value to its last in a single delta
  and every iteration's output arrives in one drain. "How did this come to be
  5" -- the question the whole recording exists to answer -- has no answer in
  a recording like that.
- **Fidelity becomes a function of formatting.** The same loop written on one
  line recorded 3 steps; broken across four lines it recorded 23. Where the
  newlines went is not something a recording is allowed to have an opinion
  about. Per expression, both spellings record 58.

`--lines` selects the old granularity. It is an escape hatch for a program too
large to record per expression under the cap, not a default: it is coarser by
construction, and both the summary and the `--dump` header say which
granularity a recording was taken at.

The two clients present this differently, and deliberately:

- **The browser timeline** scrubs per step, and highlights the expression's
  column range inside the current line. It owns the editor, so sub-expression
  movement is something it can actually show.
- **`tur dap` maps steps back onto lines.** DAP is a line protocol -- an editor
  draws a line marker -- so `stepIn` and `next` advance to the next step whose
  line *or frame depth* differs, rather than to the next raw step. Four
  keypresses that leave the marker where it was would read as a debugger that
  has stopped responding. The recording stays fine; the presentation is coarse.

## The step cap, which is not optional

A recording of a runaway loop is a tab that dies. `--max-steps=N` (default
1,000,000) ends the run through the same unwind a fuel exhaustion takes, rather
than letting an untraced tail run on -- a recording that describes a prefix of
a program whose answer came from somewhere it cannot show is worse than a short
one.

The default was 200,000 when a step was a line. It moved with the granularity:
a cap bounds the recording, but what it *means* is how much of a program fits
under it, and holding the number fixed across that change would have quietly
cut the reach of every recording by the multiplier (about 3.5x on the `fib 6`
fixture -- 65 steps per line against 226 per expression).

**Truncation is reported, never silent.** The header carries a `truncated`
flag, the summary line says `truncated yes`, and `--dump` prints it.

One trap worth knowing: the tree-walking interpreter retains roughly 4 KiB per
step of a trampolined loop, so a 1e6-step program already peaks at ~3.5 GiB RSS
*without* a tracer. Trace small programs; the cap is what keeps them small.

## Cost

Measured on a Debug build with ASan, on a 20,000-iteration `while` loop:
untraced 0.12s, traced 0.27s -- roughly **2x**, and 0.18s with `--lines`. The
constant is dominated by the locals enumeration, which happens per *node*. If
that matters for a particular program, `--lines` is the cheaper scrub, and the
shape of a cheaper one still would be to capture only on frame entry/exit and
on `let` / `set!` nodes.

Values that do not render get their type tag and `?`, which is the same honest
answer `turi_try_show_by_tag` already gives. Prelude and stdlib loading are not
recorded: recording starts where the debugger arms.

## Reverse execution over DAP

`tur dap` launched with `"replay": true` records the whole run and then serves
the debug session **from the recording**:

```json
{ "command": "launch",
  "arguments": { "program": "hello.tur", "stopOnEntry": true, "replay": true } }
```

`stackTrace`, `scopes` and `variables` answer from a trace cursor rather than
from a live frame, which is what makes `stepBack`, `reverseContinue` and
`reverseNext` answerable at all. The server advertises `supportsStepBack` and
`supportsReverseContinue`, and VS Code and nvim-dap draw the entire
reverse-execution UI off those -- the scrubber, the backwards breakpoints, the
rewinding variables pane -- so nothing here is a widget.

Two deliberate differences from a live session:

- **`evaluate` refuses.** There is no live frame to evaluate in, and saying so
  beats returning a stale value that looks like an answer. Relaunch without
  `"replay"` for a session that can.
- **Conditional breakpoints become unconditional.** A condition is an
  expression in a frame, for the same reason. That stops more often than asked,
  never less.

`stepIn` / `next` / `stepBack` / `reverseNext` move by **line**, not by trace
step -- see [A step is one expression](#a-step-is-one-expression). The
recording is finer than that; DAP is where it gets presented at the
granularity the protocol speaks.

Replay is opt-in. A plain `launch` is still a live session, still the one that
can `evaluate`, and unchanged.

### The timeline extension

DAP describes execution as a sequence of steps, never as an **axis**. That is
the right model for a live debuggee — there is nowhere to scrub to — but a
recording *is* an axis, and the three things a scrubber needs of one have no
standard request. Three custom ones add them, advertised as
`supportsTurmericReplayTimeline` in the `initialize` response:

| Request | Arguments | Body |
| --- | --- | --- |
| `replayInfo` | — | `{"steps": N, "index": i, "depth": d, "outputLength": n}` |
| `replaySeek` | `{"index": N}` | `{"index": actual}`, then a `stopped` event |
| `replaySites` | `{"indices": [...]}` **or** `{"buckets": N}` | `{"steps": N, "sites": [{"index", "file", "line", "depth"}, …]}` |

Each answers something that is expensive or impossible to approximate:

- **`replayInfo`** gives a slider its range. A range that is a guess is worse
  than no slider.
- **`replaySeek`** jumps to an arbitrary step. Approximating it with repeated
  `stepBack` is the trap [Reading a recording in C](#reading-a-recording-in-c)
  describes: every seek rebuilds state from the start of the stream, so one per
  candidate turns a scan of an 80k recording from milliseconds into a hang. The
  index is clamped into range and the reply reports where the cursor actually
  landed — believe that over your own arithmetic. A `stopped` event with reason
  `step` follows, because from the client's side that is what happened.
- **`replaySites`** says where steps are and how deep they are. Ask by
  `indices` for specific steps — a cursor readout, a tooltip — or by `buckets`
  for the whole recording downsampled (default 256, max 4096, never more than
  there are steps).

  Position and depth come back **together**, which is the shape Try Turmeric's
  `trace-site-at` already uses: a timeline's cursor readout wants `file:line`
  and a depth ribbon wants `depth`, and serving them separately doubles the
  traffic over the same steps for nothing.

  A bucket reports the **maximum** depth in its range and the site of the step
  where that maximum occurred — not the bucket's first step. A ribbon is read
  for recursion shape, so a deep call falling between two samples is exactly
  what the reader is looking for; and pointing at the deepest step means
  clicking a spike goes where the spike is.

  Neither form seeks. `depth_at` and `site_at` are index reads by construction,
  which is what keeps a full-width ribbon over a 1M-step recording a scan
  rather than a hang.

All three refuse in a live session, naming the reason rather than falling
through to a generic error — a client that asked has a scrubber in mind.

### The console rewinds

Forward motion appends to the transcript through ordinary `output` events, as
before. Backward motion cannot: the transcript at the new cursor is a *prefix*
of what the client has already been sent, and a delta has no way to express a
truncation.

So a backwards seek emits **`replayOutput`** carrying the whole transcript,
to be used in place of what the client holds:

```json
{ "type": "event", "event": "replayOutput",
  "body": { "category": "stdout", "length": 0, "output": "" } }
```

Whole-transcript rather than a cut offset, because a client that missed an
earlier event would otherwise cut in the wrong place and never know. A client
that does not recognise the event ignores it and behaves exactly as it did
before — the console simply does not rewind.

**At the last step the transcript is the whole recording's, not the cursor's.**
A replay transcript otherwise holds the output produced *strictly before* the
cursor's step — and a program whose final act is a `println` drains it after
the final STEP, so a cursor-relative answer at the last index reports nothing.
Measured: the replay fixture reports `outputLength: 0` at step 24020 of 24021
without this. An empty console at the end of a run that printed reads as a
broken timeline rather than a precise one, so the final step is special-cased
to concatenate every OUTPUT record. Try Turmeric hit this first and answers it
the same way — `turi_wasm_trace_output_full`, asked for only at the last step.

## Recording in the browser

Try Turmeric has a **Trace** button next to Run, and a `:trace` command at the
prompt. Both record the tab's program and open a timeline under the console:

- A slider over the run, with first / step-back / step-forward / last. Stepping
  **back** is the point -- it is the question a pause cannot answer.
- The editor gutter follows the cursor, and clicking a line number jumps to that
  line's next execution (Alt+click for the previous one). A line with no hit
  ahead of it says so rather than silently running to the end.
- The frame stack at the cursor, innermost first, and the bindings of whichever
  frame you select -- each carrying its most recent rendering.
- The console shows what the program had printed by that step, so scrubbing
  backwards rewinds the transcript. Closing the timeline puts your own
  transcript back.
- **Download** writes the recording as a `.turtrace` that `tur trace --dump`
  reads, so a run recorded in a browser can be inspected on the command line.

Trace is a second button rather than something Run always does, for the reason
in [Cost](#cost) -- and the browser's cap is 250,000 steps, a quarter of the
native default, because a tab pays for the interpreter's per-step retention as
well as the recording's. A run that hits the cap says so in a banner.

**A recording starts from a fresh session.** `tur trace <file>` is a new
process with an empty environment, and Trace is the same: it resets the
interpreter before recording, so two recordings of one program are comparable
and a second Trace does not re-evaluate the program on top of the first one's
definitions. Run is the opposite by design -- it is how a tab's definitions
become callable at the prompt -- so anything you defined at the prompt or in an
earlier Run is gone after a Trace. The banner says which it was.

The page does not decode the format. Every question the timeline asks goes
through the same `turi_trace_replay_*` calls `tur dap` uses, exported from
`src/web/wasm_glue.c`, so there is one decoder for `.turtrace` and it is the C
one.

One thing to know when reading line numbers: the browser session accumulates
every evaluation into one source blob, so an interpreter line is absolute in
that blob rather than relative to your tab. The timeline subtracts the offset
before it highlights anything -- but a raw `.turtrace` downloaded from the tab
carries the absolute lines.

## Reading a recording in C

`src/turi/trace.h` carries a reader (`turi_trace_open` / `turi_trace_next` /
`turi_trace_name` / `turi_trace_site` / `turi_trace_change`) and a replay
(`turi_trace_replay_*`) that reconstructs the frame stack and each frame's
locals at any step.

The replay rebuilds from the start of the stream on every seek rather than
undoing deltas backwards. That is O(records) per seek -- a few milliseconds at
the default cap -- and it is the difference between a decoder that is obviously
correct and one that has to get an undo log right in both directions.

**Per seek** is the operative phrase. Asking *where was step N* is
`turi_trace_replay_site_at`, which reads an index and does not seek; asking it
by seeking, once per candidate, is how a scan to the end of an 80k-step
recording goes from 0.02 seconds to not finishing. Anything that walks the step
axis looking for something -- breakpoint matching, a search -- reads the index.

## See also

- [lsp-guide.md](lsp-guide.md) -- the language server, including scope-aware
  highlight, rename and references.
- `docs/artifacts/debugger-dap-phase3.md` -- the live DAP session this replays
  alongside. Referenced as a path rather than linked: the offline docs pack
  carries guides and API modules only, and `genpack.py --strict-links` rejects
  a link that would leave the pack.
