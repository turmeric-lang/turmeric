# LSP/DAP on Windows: what still fails after the stdio fix

> **RESOLVED.** The stdio transport, the spice walk-up, the file-URI spelling
> and the debuggee-output capture are all fixed. On Windows:
> `tests/lsp/run-mcp-lsp.sh` **70 passed / 0 failed**, `tests/run-dap.sh`
> **all 68 assertions passed**.

**Severity: high for any editor on Windows.** `tur lsp` and `tur dap` produced
*zero output* on Windows until the stdio transport was put in binary mode (fixed
alongside this report). With that fixed, most of both servers works. What
remains is recorded here.

Found 2026-09-06 while bringing [Trowel](https://github.com/turmeric-lang/trowel)
up on Windows. Trowel drives `tur lsp`, `tur dap`, `tur format` and `tur repl`,
which makes it a good exercise of these surfaces -- none of which the Windows CI
job touches.

## How this went unnoticed

The coverage exists and does not run. `tests/lsp/run-mcp-lsp.sh`
(`tur_mcp_lsp_tests`) and `tests/run-dap.sh` (`tur_dap`) drive exactly these
servers, but:

- the Windows CI job runs `tests/run.sh` only -- it never invokes `ctest`, so
  neither target has ever run on Windows;
- `run-mcp-lsp.sh` hardcoded `TUR="./build/tur"`, with no `$TUR` override and no
  `.exe`, so it would have reported "not built" and skipped even if invoked.

Both are fixed with this report. The CI half -- actually running these on
Windows -- is a separate decision, because two of the checks below are still red
there.

## 1. Cross-module resolution returns nothing (LSP) -- FIXED

`tests/lsp/run-mcp-lsp.sh` on Windows went **65 passed / 5 failed -> 70 passed
/ 0 failed.** Two independent defects, and a wrong diagnosis of my own on the
way.

### The correction first

An earlier revision of this report said:

> Not spice discovery generally. `tur check src/user.tur` on the same tree
> resolves the sibling module and exits 0.

**That was wrong**, and it pointed away from the actual cause. `tur check` does
exit 0 -- but not because discovery worked. The module search path it printed
for a deliberately-bad import gives it away:

```
  searched:
    .../src/no-such-module.tur    (importing file's directory)
    .../stdlib/no-such-module.tur    (stdlib)
```

Two entries. The spice's own `src/` is absent, so
`auto_append_spice_includes` contributed **nothing**. `tur check` resolved the
sibling anyway because `mathy.tur` sits in the importing file's own directory,
which is search path #1 -- adjacency, not discovery. The LSP cannot lean on
that: it analyses a scratch copy in the temp directory, whose neighbours are
other scratch files.

The lesson is the one this codebase keeps teaching: exit 0 is not evidence that
the mechanism under test ran. Asking for a module that does not exist, and
reading the search path it printed, took a minute and settled it.

### Defect A: the spice walk-up could not step (src/main.c)

`find_spice_root` canonicalises with `realpath()` and then walks up with
`strrchr(dir, '/')`. On Windows `realpath` is `_fullpath`, which returns
`C:\dir\sub` **even when the caller passed forward slashes** -- so the step
found no separator, the loop broke at depth 0, and no spice root was ever
found. Every `(import sibling)` inside a spice went unresolved, so the symbol
index had no cross-module names and definition, completion and rename all
answered "nothing here".

Two more loops in the same file had the identical bug and are fixed with the
same helper: `find_project_root`, and the `tur docs` root probe. That makes
four instances of this class in total, after `find_stdlib_beside_exe` and
`rewrite_autolink_relative_paths` (#824). They all fail by returning "not
found" rather than by erroring, which is why they survive so long.

### Defect B: the URIs were not file URIs (src/lsp/lsp_util.c, lsp_docs.c)

With A fixed, definition resolved and returned:

```
file://C%3A%5CUsers%5Croger%5C...%5Csrc/mathy.tur
```

`lsp_path_to_uri` percent-encoded the drive colon and every backslash. That
round-trips through this codebase's own decoder, so every internal comparison
agreed and the server looked self-consistent -- but no editor spells a path
that way, so a definition answer naming another file was a URI the client could
not match to any document it had. Go-to-definition would land nowhere.

The reverse direction was broken too: `lsp_uri_to_path` stripped exactly
`file://` and stopped, turning the standard `file:///C:/dir/x.tur` that every
client sends into `/C:/dir/x.tur`, which no Windows API will open.

Both now speak `file:///C:/dir/x.tur`. POSIX is untouched: there is no drive
letter, so the same code produces the same bytes it always did, and the
backslash-to-slash rewrite is `#ifdef _WIN32` because a backslash is a legal
character in a POSIX filename.

`tests/lsp/mcp_lsp_test.py` built its URIs as `"file://" + path`, which is
correct on POSIX and not a file URI at all on Windows. It now goes through a
`to_uri()` helper that spells them the way a client does.

## 2. `spice_root_of` cannot walk up a backslash path (LSP) -- fixed

`src/lsp/lsp.c`'s own walk-up had the same defect as A, in a second copy.
Fixed with the same shape.

An earlier revision noted this fix "changes no observable behaviour, because
(1) masks it". With (1) fixed that is no longer true -- it is on the live path
for rename refusals now.

## 3. Time-travel replay (DAP) -- FIXED, and it was not the replay

`tests/run-dap.sh` on Windows: **driver exited non-zero -> all 68 assertions
passed.**

The symptom was `FAIL: timeout waiting for event output`, which read like a
replay defect. It was not. Dumping the raw bytes `tur dap` writes showed the
protocol stream corrupted mid-session:

```
MESSAGE 5   configurationDone
offset 787: header has no Content-Length: b'done\nContent-Length: 106'
```

That `done` is the DEBUGGEE's output. The replay fixture ends with
`(println "done")`, and it went straight to the real stdout, landing between
the `configurationDone` response and the `stopped` event. Every client's
framing desynchronised there; the driver's reader thread died with a
JSONDecodeError and the main thread then timed out waiting for an event that
could never arrive.

### Both halves were already documented as unsupported

Neither site was a mystery -- `dap.c` said so in a comment:

> Not captured on Windows. `_pipe`/`_dup2` exist, but the non-blocking read
> this relies on does not: fcntl/O_NONBLOCK have no counterpart for a Win32
> anonymous pipe [...] So the DAP debugger is effectively unsupported on
> Windows until this is done properly with overlapped I/O (WIN3).

`trace.c`'s `output_begin`/`output_drain`/`output_end` were compiled out the
same way, so the recording carried no `TUR_TRACE_OUTPUT` records at all and a
replay had nothing to replay.

### Overlapped I/O was not needed

`PeekNamedPipe` answers exactly the question `O_NONBLOCK` was wanted for -- how
many bytes are ready -- so the read is capped at what is already buffered and
can never block. About twenty lines per site, and the POSIX paths are untouched
except that `want` replaces `sizeof buf`, which is the same value there.

One thing worth recording: in `trace.c` the Windows includes I added landed
INSIDE the existing `#ifndef _WIN32`, so they compiled to nothing and the build
failed on five implicit declarations. The pre-existing block had the same shape
for the same reason -- the code it guarded was compiled out too -- so nothing
had ever noticed the headers were missing.
## What remains

Nothing in this report. Both harnesses are green on Windows.

`tur_dap` can now be wired into the Windows CI job alongside the LSP harness
and the stdio smoke test -- it was held out only because of section 3.
