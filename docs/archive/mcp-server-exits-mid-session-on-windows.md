# The MCP server exits mid-session on Windows, between two tool calls

**Severity: medium.** Intermittent CI failure on `Windows build + suite 1/3
(MSYS2/UCRT64)`. The `tur` MCP server answers one `tools/call` and then closes
its stdout before answering the next, ending the session. Not reproduced on
Linux or macOS.

**Status: RESOLVED 2026-09-26** -- see "Resolution" at the end. It was not
the transport and not intermittent in kind: the CPS emitter kept a
classification cache keyed on the program's and the EmitCtx's *addresses*,
and a process that compiles repeatedly eventually gets the same addresses
back for a different program. The server then emitted stale CTerms whose
Bindings had been freed, and died with an access violation, which Windows
reports silently. Observed 2026-09-15 on PR #875, which does not touch the
LSP, the MCP server, or the stdio transport.

## What actually happens

The harness reported it as:

```
File "tests/lsp/mcp_lsp_test.py", line 317, in test_mcp
    def_text = r["result"]["content"][0]["text"]
TypeError: 'NoneType' object is not subscriptable
```

which names the wrong thing, and is worth unpicking because it is what made
this look like "a tool returned null".

`srv.call` -> `await_id` -> `mcp_read_one`, and `mcp_read_one` returns `None`
in exactly one case:

```python
line = stream.readline()
if not line:
    return None          # EOF
```

So `r` is `None` only when the server's **stdout reached EOF**. The server did
not answer unhelpfully; it **exited**. The preceding `hover` call, against the
same file and the same position, had just been answered normally -- so the
process died between answering `hover` and answering `definition`.

The harness half of this is fixed in the same change: `tool_text()` now reports
"server closed stdout (exited mid-session)" with the exit status and the
server's stderr, instead of raising a `TypeError` several frames from anything
meaningful. That does not fix the exit; it makes the next occurrence
diagnosable, which this one was not.

## Why it is not the PR it was seen on

Worth recording, because "unrelated-looking" is not evidence:

- the immediately preceding run (`35014899506`, head `91c31b74`) contains every
  code change of that PR and this same job **passed**;
- the only delta to the failing head `1674153f` is **one table row added to
  `docs/reported/README.md`**.

Identical compiler code, one docs line apart, passing and then failing. That
makes it intermittent and environmental rather than caused by a diff.

## Where to look

`mcp_tool_definition` (`src/lsp/mcp.c`) is itself defensive -- every early exit
frees and returns a message, and a missing symbol is a normal
`"No definition found for 'x'"` response, not an exit. So the exit is most
likely below it, in `mcp_load_and_analyze` (which runs the elaborator) or
`mcp_find_symbol`. Note `hover` calls `mcp_load_and_analyze` on the same file
and survived, so a first analysis of that file works; whatever fails is either
the second analysis in one process or something `definition` reaches that
`hover` does not.

Adjacent, and probably the same neighbourhood: the test's own recent history is
Windows transport work (`fix(win): put the LSP/DAP stdio transport in binary
mode`, `fix(win): make LSP cross-module resolution work`), and
[windows-subprocess-and-shared-lib-gaps](windows-subprocess-and-shared-lib-gaps.md)
was open against Windows subprocess behaviour (resolved 2026-09-26).

## Fix directions

1. **Get the evidence first.** With the harness fix above, the next occurrence
   reports the exit status and stderr. A non-zero status or an abort message
   distinguishes a crash from a clean-but-premature exit, and those have
   different causes.
2. If it is a crash, run the MCP server under a debugger or with
   `-fsanitize=address` on MSYS2/UCRT64 and drive the same two calls
   (`hover` then `definition`, same path and position).
3. If it is a clean exit, suspect the stdio read loop terminating early -- the
   binary-mode fix above is precedent for that class of bug on this platform.

## Repro

Intermittent; no reliable local repro. Seen once in
[run 35015960062](https://github.com/turmeric-lang/turmeric/actions/runs/35015960062),
job `Windows build + suite 1/3 (MSYS2/UCRT64)`, on `1674153f`. The same job
passed on `91c31b74` minutes earlier and passed again on re-run.

## Resolution (2026-09-26)

### It had happened three times, not once

Scanning the failed `Windows build + suite 1/3` jobs among the last 150 failed
`ci.yml` runs found three occurrences, on 2026-09-10, 09-14 and 09-18, every
time on `definition`. The 09-18 one ran with the improved harness and reported
`exit status None; stderr: ''`. The harness polled for the status at the
instant of EOF, while the process was still tearing down, and Windows prints
nothing for an access violation. The whole MCP step had run for 0.7 s.

### Reproduced, and the cause

A driver that replays the harness's call sequence (`check_file` good,
`check_file` bad, `symbols`, `hover`, `definition`) in ONE server process
killed **18 of 20 servers**. Each died with `0xC0000005` on a `definition` 25
to 40 calls in. A canned 40-round stdin file crashed deterministically. With
a crash reporter in place (below), gdb and `addr2line` gave:

```
raw_name_for_binding (b=0x1114c1c0)       emit_core.c:2578  b->name is garbage
callee_name                               emit_cps_ir.c
emit_term / emit_letraw ...               emit_cps_ir.c     CTerms at 0xc5xxxx
emit_cps_ir_try_fn -> emit_fn_def -> emit_program
compile_to_c <- tur_collect_symbols <- mcp_load_and_analyze <- mcp_tool_definition
```

`tur_collect_symbols` runs a full `compile_to_c`, so every MCP tool call
compiles the file again inside the same process. `ensure_S`
(`src/compiler/emit_cps_ir.c`) caches the CPS classification, and the CTerms
in `g_arena`, keyed on `g_prog == program && g_ents_ctx == g_emit_ctx`: two
pointers. Between compiles both the program and the EmitCtx are freed. Once
the allocator hands a later compile the same two addresses, the cache "hits",
and CTerms that point at the old program's freed Bindings get emitted. It is
the same ABA hazard for `fd_for_binding`'s table (`fdc_prog`). Nothing about
it is Windows-specific. On Linux the stale memory is arena memory, which ASan
does not poison, and glibc's reuse pattern evidently rarely lines the two
addresses up. `hover` only surviving in the original report was luck of
ordering, and so was "the second analysis": the crash comes whenever the
addresses coincide.

### The fix

- `emit_cps_ir_forget()` drops the program-keyed caches: the CTerm arena, the
  classified entries, and `fdc_prog`. The four emission entry points in
  `emit_module.c` (`emit_program`, `emit_exports_manifest`, `emit_header`,
  `emit_implementation`) call it at the start and end of every top-level
  emission, with a depth count so a nested emission cannot free its outer
  one's CTerms. This also returns the arena between compiles, where it used to
  stay resident in a long-lived server. Every caller of `ensure_S` sits inside
  an emission, so no lookup can land between compiles.
- **Crash reporting on Windows.** `main()` installs an unhandled-exception
  filter that prints `tur: fatal exception 0x%08lx at %p (tur.exe+0x...)` to
  stderr and then continues the default handling, so the exit status is
  unchanged. `addr2line -e tur.exe <ImageBase + offset>` resolves the offset
  against the same build. That is how this was found in minutes once it
  reproduced.
- **Harness.** `tool_text` now *waits* up to 10 s for the exit status instead
  of polling for it, and prints a Windows NTSTATUS in hex. A new
  `test_mcp_repeated_analysis` makes 200 calls of the CI sequence in one
  server and fails with the exit code and stderr if the server dies.

### Verified (Windows 11, MSYS2/UCRT64, gcc 16.1, Debug)

| check | before | after |
| --- | --- | --- |
| replay driver, 20 servers x 40 rounds x 5 calls | 18 of 20 died (`0xC0000005`, call 25-40) | 0 of 20 |
| canned 40-round stdin file | died after 40 responses | 201 of 201 responses |
| `run-mcp-lsp.sh` | 70 passed | 71 passed, 0 failed (with the new section) |
| `run-mcp-lsp.sh` with `emit_cps_ir_forget` mutated to a no-op | -- | the new section FAILS: "server died at definition in round 8 ... 0xc0000005" |
