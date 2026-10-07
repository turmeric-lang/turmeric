---
title: Security Guide
category: Security
description: What Turmeric promises to handle safely -- the trust boundary for each input, the promise made at it, and where the implementation does not yet keep that promise
---

# Security Guide

Turmeric is a compiler, an interpreter, a runtime library, a package fetcher, a
web playground, an installer, and a release pipeline. Each has a different
notion of "untrusted input". This guide says which inputs the project promises
to handle safely, so that a bug report has something to be graded against.

Read this before filing a security report -- see
[`SECURITY.md`](https://github.com/turmeric-lang/turmeric/blob/main/SECURITY.md)
for how. The difference between a bug and a non-bug here is usually the
boundary, not the crash.

Each promise below carries its **status today**. Where a promise is not yet
kept, the gap is named. A promise with no status note is one the implementation
keeps.

---

## The five boundaries

| # | Boundary | Untrusted input | Promise |
| --- | --- | --- | --- |
| T1 | Compiling a project | a `.tur` tree, its `build.tur`, its `spices/`, its Justfile | Split -- see below. `tur build` makes **no promise**. `tur check`, `tur run --list` and the language server promise not to execute repo-supplied code or shell unless you asked them to. |
| T2 | A compiled program's own inputs | bytes handed to stdlib readers: `bytes->serial-cont`, image files, JSON, HTTP requests to `httpd`, `read-async` lengths | A malformed input is a `result` error or a panic -- never a wild read or write. Well-formed is not authentic: authenticating bytes is the program's job. |
| T3 | The sandboxed interpreter | the program text evaluated inside `Env/new-sandboxed`, the macro environment, or the playground | A capability-denied environment does no I/O, no filesystem, no process, no environment, no FFI, no inline C; it cannot corrupt or end the host process; and it terminates under fuel. |
| T4 | The supply chain | the installer, release assets, `tur fetch` of a `:url` spice, Actions inputs | Installing a release gets you the bytes CI built, verifiably. A spice pinned in `tur.lock` cannot change under a rebuild without a diagnostic. |
| T5 | Editor protocols | LSP, DAP and MCP messages over stdio | The peer is your editor, so it is semi-trusted -- but framing must be robust. A bad `Content-Length` must not overflow. |

---

## T1 -- Compiling a project

This boundary splits, and the split is the most important thing in this guide.

### `tur build` makes no promise

**Building a Turmeric project runs that project's code.** A tree you build can
execute arbitrary code through at least:

- an inline C block in any module, including a fetched spice;
- a compile-time macro (see T3);
- its Justfile, when you invoke a recipe;
- `:link-flags` in `build.tur`, which is documented as the verbatim sibling of
  `:link-libs` -- no prefix is added, because that is the only way to spell
  `-framework Cocoa` -- and which therefore lands in the compiler command line
  as written;
- `:c-sources`, which names C files to compile into your binary;
- `:cmake-deps`, which runs upstream CMake.

What a manifest and an inline-C `__tur_autolink__` marker may contribute to
that command line is now a fixed vocabulary -- `-l<name>`, `-L<dir>`,
`-I<dir>`, `-D<key>[=<val>]`, `-framework <name>`, `-Wl,<...>`, a source or
object path, or one of a short list of bare toolchain flags -- and anything
else is a build error naming the token. That closes the *shell*: a manifest
cannot smuggle `; touch x` into the command any more. It does not change the
promise, because the vocabulary is itself enough to run code: `-l` names a
library whose static initializers run, a `.c` path is compiled into your
binary, and `-Wl,` speaks directly to the linker. It is a narrower channel,
not a closed one, which is why `tur build` still promises nothing.

This is not a defect list. It is the same position every compiler takes.
`build.tur` is Turmeric's `.cargo/config.toml`: Cargo honours a repo's
`rustflags` and `[target.*] runner` and documents that building a crate runs
`build.rs` and its proc macros; `make` runs a Makefile. Turmeric is in the same
place, and naming the analogy is more useful than a severity number would be.

**So: do not `tur build` a tree you would not `make`.** Read a new dependency's
`build.tur` the way you would read its `build.rs`.

### `tur check`, `tur run --list`, and the language server do promise something

These are different, because an editor runs them on a tree you have merely
*opened*. The promise is the one clangd makes, and it is narrower than "safe":

> **They do not execute repo-supplied code or shell unless you asked them to.**

Two things bear on that promise today.

#### `tur check` expands macros in a capability-denied, handle-checked environment

`tur check` expands compile-time macros, which is the same exposure Rust has
with proc macros. The macro environment is capability-denied, and that is
enforced for every native function as well as the builtins (T3): a
`defmacro*` body that calls `process/spawn`, deletes a file or reads the
environment gets a diagnostic, and nothing runs.

It is also memory-checked the way T3 describes: the macro env turns on the
handle-provenance registry when its capabilities drop, so a `defmacro*` body
can no longer turn an integer into a pointer -- through a native or through an
erasing ascription in the interpreter's value model -- and read or write the
compiler's memory (S-5, resolved 2026-10-07). That makes a capability-denied
macro environment a *better* story than Rust's, where a proc macro is native
code with the compiler's full authority. It rests on a per-native table that a
test sweeps rather than on a type system, so the opt-out below stays for
anyone who wants no macro-time code at all.

There is an opt-out. The global flag `--no-proc-macros` refuses
every `defmacro*` with a diagnostic, so no macro-time code runs -- the
equivalent of turning rust-analyzer's `procMacro.enable` *off*. (rust-analyzer
ships it **on**, and has since 2021; it also runs `build.rs` on a tree you
merely open, under `cargo.buildScripts.enable`, also on by default.) The flag is
global, so it refuses `defmacro*` in `tur build` too, where rust-analyzer's
setting is scoped to the editor and `cargo build` always runs proc macros:

```
tur --no-proc-macros check src/
```

Template `defmacro` still expands, because substitution runs nothing.

#### `tur repl` auto-discovery compiles and dlopens (open, medium)

`tur repl` walks up from the working directory for a `build.tur`, builds the
tree into a shared library under `.tur-repl-cache/`, and `dlopen`s it. That is
Gradle-tier behaviour and this guide makes **no promise** about it.

What it does guarantee is that the object loaded is one *this* `tur` built: the
cache carries a `.built-by` sidecar recording the compiler's version, path,
size and mtime, and a mismatch forces a rebuild. So a repository that commits a
`.tur-repl-cache/lib-N.so` does not get it loaded.

The opt-out, `TUR_NO_AUTO_SPICE=1`, is still default-allow, which points the
wrong way: pnpm, Bun, Deno and Neovim have all moved to default-deny plus an
allowlist. The intended replacement is direnv's model -- hash the tree's
`build.tur`, ask once, remember the answer -- which would turn auto-discovery
from something this guide declines to promise into a documented design.
Tracked as D-3 in the
[security audit plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/security-audit-plan.md).

### Editor trust support

Whatever the CLI promises, an editor integration should declare its posture so
the editor can enforce it. Both VS Code extensions in this repository declare
`capabilities.untrustedWorkspaces`:

- `vscode-syntax-ext` (highlighting, formatting, LSP client) is **limited**,
  with `turmeric.serverPath` listed as a restricted configuration. In VS Code's
  Restricted Mode the workspace cannot redirect the language server to another
  executable; highlighting still works.
- `editors/vscode-turmeric` (debugging over DAP) is **limited**. Its adapter
  executable comes from the workspace's launch configuration, so debugging a
  repository means trusting it.

That gives Restricted Mode users the same protection Go's extension gives them,
and it costs two blocks of JSON.

---

## T2 -- A compiled program's own inputs

A program you wrote, reading bytes it did not choose: a saved continuation, an
image, a JSON body, an HTTP request. **The stdlib reader must not corrupt memory
on any input.** A malformed input is a `result` error or a panic, never a wild
read or write. This is the ordinary promise a runtime library makes.

What the stdlib readers do:

- **Serialized continuations.** Every route that rebuilds one --
  `bytes->serial-cont`, `resume-cont!`, `image/blob-resume!` -- runs the same
  check inside the runtime: each record must fit the buffer, carry a known tag,
  and, for a call frame, name a frame this program registered, with the
  environment kind that frame was registered with. `bytes->serial-cont` turns a
  bad buffer into an `Err`; the others panic.
- **Images.** The header's payload length is held to the file's real size, a
  CRC covers the payload as well as the header, and the continuation is checked
  before the image counts as loadable -- a damaged image is a cold start.
- **JSON.** Both decoders (compiled and interpreter) cap nesting at 256, decode
  `\uXXXX` (a lone surrogate or `\u0000` is an error), and free what they built
  when they fail.
- **`httpd`.** A malformed, conflicting or oversized `Content-Length`, and any
  `Transfer-Encoding`, is refused before a byte of the body is read; the body
  cap defaults to 8 MiB (`httpd-set-max-body!`). Servers bind loopback unless
  the program asks for more. See
  [httpd-guide](httpd-guide.md#binding-and-request-limits).

These readers, along with the LSP framing and the compiler's own front door
(the reader, the manifest reader, the Justfile parser), run nightly under
libFuzzer with ASan and UBSan -- see `tests/fuzz/README.md`.

**Checked is not authenticated.** A continuation buffer that passes the check
still rebuilds a continuation of *this program's* frames with whatever
environment values the buffer carries, and an image's CRCs catch corruption,
not tampering -- anyone who can write the file can recompute them. Do not
resume bytes that crossed a trust boundary without authenticating them first;
the guestbook example keeps continuations server-side and hands the client only
an HMAC-signed name. A `Serializable` instance's own `deserialize`, which
receives an environment's bytes, is the program's code and the program's
responsibility.

---

## T3 -- The sandboxed interpreter

`turi_env_new_sandboxed()` and the compile-time macro environment both promise:
no I/O, no filesystem, no process spawning, no environment variables, no FFI,
no inline C, no unsafe memory, no way to corrupt or end the host process, and
termination under a step-fuel bound. See the
[Sandboxing Guide](sandboxing-guide.md) for the embedding API.

**Capabilities are enforced.** Every native function the interpreter ships
has a row in one classification table that names the capability it requires,
and the single native dispatch refuses a call whose environment lacks it -- by
name from Turmeric, via `turi_call` from C, and through a higher-order native
alike. A new native without a row fails the sandbox test. The
[Sandboxing Guide](sandboxing-guide.md#capability-classification) has the
classes and the rows that are not pure. `load` and `import` are refused
outright, and the `extern-c` overrides for `printf`, `getenv` and `exit` need
FFI like every other `extern-c`.

**Status today: handle forgery is closed (S-5, resolved 2026-10-07).** Most
natives take a collection, string or continuation handle as a bare integer and
cast it to a pointer, and the interpreter itself re-types words in places --
an erasing ascription to `cstr` or a struct, a field read through a bare
integer, a call through a function-typed word, `gen-unwrap`, a `TVar`, a caught
panic's payload. In a restricted env one per-env **handle-provenance registry**
stands in front of all of them. A handle a native mints is recorded under its
kind; a value that loses its type tag -- handed to a native, stored in a rest
list or a `TVar` -- is recorded where it loses it; and every cast checks that
its word is a live handle of the kind it is about to be read as:

```
(vec-get 4096 0)                                    ; => error: not a live handle of the expected kind (S-5)
(let [s : cstr (:: 4096 cstr)] (str-concat s "a"))  ; => error: string value is not a live handle ... (S-5)
```

Kind confusion (a Vec where a HAMT is expected, a count replayed as a handle, a
string literal where a Vec is expected) and use-after-free are refused the same
way, while a genuinely built vector, map, string, struct or continuation still
round-trips. Each native's row in `src/turi/native_caps.c` names the handle
kind of each argument position; natives that follow words no row sees (a
stored element, a child link) check them themselves.

The coverage is tested, not only asserted: the sandbox test calls **every**
capability-free native with forged arguments in a forked child and fails on any
that crashes, and the registry has been run over the whole interpreter fixture
suite to find legitimate programs it would refuse. The details, and the few
things it deliberately does not cover -- `String`-keyed maps are refused in a
restricted env, re-entrant `call/cc` is escape-only outside a top-level form, an
unrestricted env has no registry at all -- are in
[the S-5 report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-sandbox-handles-are-forgeable-integers.md).

A panic, by contrast, no longer ends the host. In an environment without
`TURI_CAP_PROC`, a panic that nothing catches, and the error exits of natives
like an out-of-bounds `vec-get`, come back to the embedder as a `TURI_ERROR`
reading `panic: <msg>`, and the environment stays usable. A panicking
`defmacro*` is an ordinary expansion diagnostic.

`Env/new-sandboxed` is a boundary against code written to escape it, in the
sense T3 states. It is an in-process boundary, built from a hand-maintained
per-native table that a test sweeps: an embedder running code from someone it
does not trust should still run it in a separate, unprivileged process as
defence in depth, as with any in-process sandbox.

### Try Turmeric

The web playground runs with all capabilities granted, deliberately. Its
boundary is the browser's, and the WebAssembly build has no filesystem and no
process spawning to reach. That is an acceptable posture for a playground and
this guide records it as intentional rather than a gap.

What the site does promise is the ordinary web one: **text you did not write --
a pasted file, an opened project zip, a restored tab, a docs page -- is shown
as text and never runs as script in the page.** Three things keep it:

- **A Content-Security-Policy on every response from turmeric-lang.com**,
  defined once in `web/csp.js`. Its `script-src` has no `'unsafe-inline'`, so
  markup that gets past escaping does not execute. It allows WebAssembly
  compilation (`'wasm-unsafe-eval'`), mermaid from one jsDelivr path, the doc
  pages' web fonts, and inline *styles* -- Monaco needs them -- and it refuses
  framing (`frame-ancestors 'none'`). The generated doc pages load their
  scripts from files for the same reason.
- **Escaping that holds in attributes as well as in element content**, so a
  value interpolated into `value="..."` cannot add attributes of its own.
- **A console transcript stored as data.** What the playground keeps in
  `localStorage` is rebuilt with DOM calls on load, so nothing read back from
  storage is ever parsed as markup.

A program that never returns does not take the playground with it. After a
second a **Stop** button ends it; after 30 seconds the playground stops it
itself. Either way the interpreter restarts in a fresh session, and the
definitions from earlier runs are gone.

`'wasm-unsafe-eval'` is understood from Chrome 97, Firefox 102 and Safari 16.
An older browser that also applies CSP to WebAssembly compilation will not load
the interpreter.

**The documentation is trusted content.** The in-app docs pane and the pages
under `/docs/html/` are HTML generated from this repository's guides and
docstrings and from the READMEs and docstrings in `turmeric-spices`
(`tools/genguides.py`, `gendocs.py`, `genspices.py`). Markdown passes raw HTML
through, so a spice whose README carries HTML puts that HTML on
turmeric-lang.com, in the playground's origin. The policy stops it running
script; it does not stop it restyling or rewording the page. The defence is
review: read a documentation change to `turmeric-spices` as a change to the
site.

---

## T4 -- The supply chain

**Installing a release should get you the bytes CI built, verifiably. A spice
pinned in `tur.lock` should not change under a rebuild without a diagnostic.**

**Status today: the installer half is kept. The lockfile half detects a change
but cannot yet pin against one.**

### Installing

`curl -sSf https://turmeric-lang.com/install | sh` installs the version manager
(`tvm`) and then the latest **release**. The release tarball is checked against
that release's `sha256sums.txt` before it is unpacked, and the install **stops**
if that check cannot be made -- a missing sums file, a missing row for your
platform's asset, or no `sha256` tool on the system are all refusals, not
skips. `--insecure` is the single opt-out, and it does not apply to a checksum
*mismatch*: a check that ran and said no is not a check that could not run.

`tvm` fetches itself at the release's tag rather than from `main`, so the
bootstrap does not reintroduce what it removes. That is the same trust root as
the release, not a stronger one -- whoever can move a tag can move the assets.

Release assets carry [build provenance](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations),
signed through Sigstore with a short-lived certificate minted from the release
job's OIDC token, so there is no long-lived key to lose:

```sh
# Releases built before the 2026-10-02 move to the turmeric-lang org
# (v0.59.0 and earlier) -- note `--owner`, not `--repo`:
gh attestation verify turmeric-<tag>-<target>.tar.gz --owner rjungemann
# Releases built after it:
gh attestation verify turmeric-<tag>-<target>.tar.gz --repo turmeric-lang/turmeric
```

The owner is bound into the signature, so the right flag follows the release's
**vintage**, not where the repo lives now. A pre-move asset stays recorded
under the account that owned the repo when it was built, and a transfer does
not move that record -- so `--repo` fails for those assets under *either* owner
name, and `--owner rjungemann` is the form that works. A failure here means the
wrong flag, not a compromised download.

That is the check worth running, because `sha256sums.txt` is served from the
same origin as the assets: on its own it proves the bytes did not change in
transit, not who produced them. Tags are annotated rather than signed, which is
a recorded decision -- the attestation is what protects a downloader, and it
needs no key anyone has to hold.

**`brew install --HEAD turmeric-lang/turmeric/turmeric` builds whatever `main` is
at that moment and verifies no checksum.** That is the supported way to track
development and the wrong way to install the compiler. The Homebrew formula is
`--HEAD`-only by design; it is not a pinned channel.

### `tur.lock` detects drift; it does not yet pin against it

`tur fetch` compares a freshly fetched tree against the hash `tur.lock`
recorded and **fails** when they differ, naming both hashes and pointing at
`tur fetch --update` as the deliberate way to accept the change. A refused
fetch leaves the recorded hash alone, so the failure does not evaporate on the
next run. `tur run`, `tur build` and `tur audit` all re-hash the trees they are
about to use, so an edit made to `spices/` after a fetch is caught by whichever
you reach for.

A fetch also **checks out the commit it recorded**, not wherever `:ref` points
now, so a branch-shaped `:ref` that has moved upstream still yields the locked
commit. If that commit can no longer be fetched -- history rewritten -- the
fetch fails and keeps no clone rather than falling back to the branch, which
would quietly turn the pin back into branch-tracking. `tur fetch --frozen`
holds a whole fetch to the lock and never writes it: run that in CI.
([lock-tracks-ref-not-resolved-commit](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/lock-tracks-ref-not-resolved-commit.md),
resolved.)

So: **prefer a tag over a branch for `:ref`, and read a new spice before you add
it.** A `:cmake-deps` entry is a trust decision equivalent to running build
scripts from that repository.

### Workflows

Every workflow action is pinned to a full commit SHA with its version as a
trailing comment, and Dependabot keeps those pins current -- a pinned action
otherwise never moves, including past the fix for its own vulnerability.
`ci.yml` and `release.yml` declare `permissions: contents: read` at the top and
raise it per job; the repository's default workflow token is read-only as well,
so the declaration is defense in depth rather than the only lock. Toolchain
installs are pinned: an exact Emscripten SDK version, and `pip` requirements
with hashes under `--require-hashes`.

The `turmeric-spices` checkout in CI is deliberately **not** pinned. It is the
same owner under the same account, inside the trust boundary `main` already
draws, and a hand-maintained SHA in this repo is a pin that goes stale and then
gets bumped blind.

### Where a fuzz or TSan finding goes

The nightly fuzz search, the libFuzzer parser targets and the TSan run all
report findings to **Sentry**, and to nothing else: no public artifact, and the
workflow log says only that something failed. This is deliberate. These jobs
run on a public repository, where a run page is readable signed out and an
artifact is downloadable by any signed-in user, so the previous arrangement --
an auto-filed GitHub issue, plus target names in the step summary, plus the
reproducers uploaded as an artifact -- published un-triaged memory-safety
findings the moment a scheduled run finished.

Two consequences worth stating plainly:

- **Reproducers for un-triaged crashes are sent to a third party.** Sentry is
  the custodian of that data. If that is not an acceptable dependency for your
  fork, unset the `SENTRY_DSN` secret -- but read the next point first.
- **A finding is never dropped to protect privacy.** If Sentry is
  unconfigured or unreachable, the workflows fall back to uploading the
  findings as a public artifact and say so loudly in the job summary. Losing a
  memory-safety finding is worse than publishing one; the fallback is meant to
  be fixed, not lived with.

Findings group on target plus crash type plus the first non-sanitizer frame, so
the same defect found on consecutive nights is one Sentry issue with a count
rather than one report per night. The seed and the run URL travel as context,
never as part of the grouping key.

---

## T5 -- Editor protocols

The peer is your own editor, so it is semi-trusted -- but **message framing
must be robust**.

`tur lsp` and `tur dap` accept a `Content-Length` of plain decimal digits, at
most 64 MiB, after a header block of at most 8 KiB; anything else ends the
session as a framing error.

---

## Out of scope

- Denial of service by a developer's own program against their own machine.
- The R7RS embedding's continuation memory growth, which is tracked as an
  ordinary bug.
- The Godot bindings, which live in a separate repository.

---

## What the effect system does and does not promise

Nothing, today, for security. `--strict-effects` defaults off and warns
(`-Werror=strict-effects` makes those warnings fail the build).
Inline C outside an `Unsafe` effect row is a lint behind
`--lint-inline-c-unsafe`, also default off. The deserializers discussed under
T2 infer plain effect rows.

`#fx{Unsafe}` itself is **enforced**, the way Rust's `unsafe fn` is:

- Calling an `#fx{Unsafe}` function is a hard error unless the call sits inside
  `(unsafe ...)` -- `unsafe function 'poke' requires an enclosing (unsafe ...)`
  -- or the caller declares `#fx{Unsafe}` itself, in which case the row
  propagates to *its* callers.
- `(unsafe ...)` discharges the obligation **and erases the row**: a function
  that wraps its unsafe calls in a block infers `#fx{}`. That is the feature --
  it is how a safe abstraction is built over an unsafe primitive, exactly as in
  Rust.

What the marker means is decided (the audit plan's open question 3, answered as
Option A): it describes a **body** -- pointer arithmetic, inline C, a raw
dereference -- not an input contract. It does not mean "may corrupt memory on
bad input". So it is a *discipline*, not a queryable boundary: you cannot ask
"which functions here can corrupt memory on bad input?", because every
competently written wrapper has deliberately erased the answer with
`(unsafe ...)`. Do not read the absence of `#fx{Unsafe}` from a function's row
as a safety claim about its inputs. (A propagating marker that *would* answer
that question is a possible future feature under its own name; it is not
`Unsafe`.)

---

## Reporting

Private advisory form:
<https://github.com/turmeric-lang/turmeric/security/advisories/new>. See
[`SECURITY.md`](https://github.com/turmeric-lang/turmeric/blob/main/SECURITY.md).

The open items above are the audit's own backlog, tracked in the
[security audit plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/security-audit-plan.md).
Reporting one of them again is welcome but will not be news.
