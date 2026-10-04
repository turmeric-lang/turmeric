# Changelog

All notable changes to Turmeric are documented here.

## [Unreleased]

### Added

- **stdlib OS surface, P0 (stdlib-os-surface-plan).** `io/read-line` /
  `io/read-stdin` (`(Option cstr)`; `None` at end of input), `file-write` /
  `file-write-str` / `file-seek` / `file-tell` on `FileHandle` with a
  `SeekFrom` ADT, `fs/append-text`, `time/now-ms` and `time/monotonic-ns`,
  `process/kill` over a `Signal` ADT plus `process/child-pid`, and the
  `eprintln` / `eprint` builtins -- `println`'s overload set, on stderr.

### Changed

- **BREAKING -- fs / io / process report failure as `(Result T IoError)`
  (stdlib-os-surface P1).** `IoError` (new `stdlib/io-error.tur`) carries the
  errno, with `io-error/message`, `/code`, `/not-found?`, `/exists?`,
  `/permission?`. Retyped in place, with no deprecation window: `fs/mkdir`,
  `mkdirp`, `rmdir`, `rm`, `rename`, `copy`, `write-text`, `append-text`
  (`(Result nil IoError)`); `fs/stat`, `fs/read-text`, `fs/glob` (now a
  `(Vec cstr)`); `file-open` (`FileHandle` is now a linear opaque), `file-read`,
  `file-write`, `file-seek`, `file-tell`, `file-close`; `process/spawn`,
  `process/run` and `process/wait` (an `ExitStatus`: `Exited` / `Signaled`),
  `process/capture`, `process/cwd`, `process/chdir`, `process/kill`;
  `env/all` (a `(Vec cstr)`). `process/*` argv is now variadic after the
  program, which is also argv[0]: `(process/run "/bin/ls" "-l")`.
  `fs/read-text` reads pipes instead of failing on them, and a missing
  program is an `Err` from `process/spawn` rather than a child exiting 127.
- **`get-time-ms` has millisecond resolution.** It was `time(NULL) * 1000`.

### Deprecated

- **Deprecation step of the io.tur cleanup (stdlib-os-surface P1.5).**
  `read-file`, `write-file` and `file-exists?` (use `fs/read-text`,
  `fs/write-text`, `fs/exists?`), `file-handle-ok?` (a `FileHandle` is always
  open now) and `fs/glob-free` (use `fs/paths-free`) warn at every use. The
  next minor release removes them.

## [0.62.0] -- 2026-10-03

### Added

- **Loop invariants graduated: `(while c :invariant p ...)` is always on.**
  The invariant is checked on entry and after every iteration, the checks are
  elided where the solver proves them, and a proved invariant is a fact after
  the loop. `--enable=loop-invariants` is now a `TUR-W0063` no-op (eligible to
  age out at 0.62.0).
- **Try Turmeric examples for all three languages.** Eleven new Examples-menu
  entries covering Turmeric, Saffron and R7RS Scheme: pattern matching over a
  `defdata`, Option/Result, data literals, `syntax-rules` + named let +
  `call/cc`, `define-record-type` + `guard`, and the sweet readers.
- **`examples/reflected-measures`** -- a judged-round scoreboard whose API
  contracts are `^reflect` measures (Int, Bool and Float); every obligation
  proves under `--strict-refine`.

### Changed

- **`#reads` measures are checked for writes.** A measure written in Turmeric
  whose body writes a framed parameter -- a store through it, or a call to a
  callee whose `#writes` frame names it -- draws `TUR-W0383` and backs no
  proof. The stdlib's Vec mutators (`vec-push!`, `vec-pop!`, `vec-set-o!`,
  `vec-drop-last-o!`, `vec-free-o`) now declare `#writes [v]`. Inline-C
  measures remain trusted.
- **`reflected-measures`'s advisory `expires_at` moves to 0.64.0** -- built and
  fuzzed, waiting on a consumer.
- **LSP diagnostics land where you can see them.** An error in a transitively
  loaded file anchors on the document's own load/import of it (`in
  deep.tur:1:29 (via mid.tur): ...`); an error raised inside a macro expansion
  anchors on the outermost call (`(expanding map-get)`); an error inside the
  stdlib itself marks the first line and says to check the stdlib dir against
  `tur --version`. `TUR-W0039` is quiet for explicit stdlib loads.

### Fixed

- **A loop invariant over a container could be "proved" through an alias.**
  Inside a `frozen` region, `(vec-push! (id v) 7)` -- `id` a pure function that
  returns its argument -- let `(<= (vlen v) 3)` be proved preserved and its
  check elided while `v` grew. The frozen set is now also checked against the
  elaborated loop, so the check stays and fires.
- **Bool `^reflect` measures scale.** The DNF expansion now unit-propagates
  before splitting, so a four-element ground list no longer overflows
  `REFINE_MAX_CUBES` and falls to `TUR-W0372`.
- **`tur run <file>` honours the enclosing manifest's `:experiments`**, as
  `tur check` already did.

## [0.61.0] -- 2026-10-03

### Added

- **`(srfi 216)` -- SICP Prerequisites for `#lang r7rs`.** `true`, `false`,
  `nil`, `runtime`, `random` (over SRFI 27), `cons-stream` and the stream
  primitives, plus a sequential `parallel-execute` / `test-and-set!` until
  SRFI 18 lands. `(features)` reports `srfi-216`; a clash with SRFI 41's
  `stream-null?` is a named error.
- **`httpd-set-bind-addr!` binds a named interface, IPv4 or IPv6.** It takes a
  numeric address (`"192.168.1.5"`, `"::1"`, `"::"`), wins over
  `httpd-set-bind-any!`, clears with `""`, and refuses an unparsable address.
  `httpd-new-pool-with-limit` joins `httpd-new-async-with-limit`.

### Changed

- **`tur emit-c` is ~15x faster on large programs.** Five lookups that were
  linear in program size are now indexed; the r7rs conformance program went
  from 130 s to 8.5 s (Debug) with byte-identical output.
- **Both HTTP servers cap pending connections at 512 by default**, answering
  503 past the cap instead of growing without limit. `0` still means
  unlimited when asked for by name. `mw-rate-limit` now keys on the IP itself
  (8-way set-associative, evicting the stalest window) and never fails open
  once many distinct IPs have been seen.
- **Loop invariants prove inside a `frozen` region.** A bounded-index walk
  over a container discharges both runtime checks there, and a frozen marker
  no longer declines the loop (`--enable=loop-invariants`).
- **Editor integration.** LSP diagnostics from a loaded or imported file are
  anchored on the form that names it, sibling imports and
  `#use-reader-macros` resolve from the document's own directory, and a
  headerless `.scm` / `.tur.sweet` is analyzed under its own reader.
  `tur fmt` / `tur format` keep `turmeric/sweet` and `saffron/sweet` as
  written and accept `--lang`, and the REPL's `:run` / `:reload` load a
  file in a different reader without switching the session's.

### Fixed

- **The JIT engine works on linux-aarch64.** Vendored MIR now parses glibc's
  `<sys/user.h>` with `ucontext_t`'s real layout, so the 0.60.1 `TUR-W0070`
  fallback on that platform is gone. Separately, a JIT-run program that
  started threads no longer has its code freed under them when `main`
  returns.
- **Try Turmeric loads again.** Pointer-keyed hash mixing used an over-wide
  shift on wasm32, hanging the worker in `turi_wasm_init`.
- **Codegen and CPS fixes.** A constrained generic over a pass-by-pointer
  aggregate compiles; an associated-type projection at a type variable stays
  unreduced until the call's types are known; a tail-position `let` binding
  a carrier value (Vec, list, heap ADT) stays on the tail path; several
  serial-shift / capturing-lambda shapes lower instead of evicting, with two
  use-after-frees and two chain leaks fixed along the way; a bare extra
  parameter on a parametric-head Saffron instance is `any`.
- **The sandboxed interpreter refuses a forged call target**, so an integer
  ascribed to a function type is no longer re-tagged as a closure.

## [0.60.1] -- 2026-10-03

`v0.60.0` was tagged but never published: on linux-aarch64 the release
workflow's "run a program through the JIT from the extracted archive" step
failed, and the release job requires every leg, so `Create Release` was
skipped and no assets went out. This entry therefore carries everything from
that tag as well as the fixes since, and the `v0.60.0` tag has been retired.

### Changed

- **The JIT engine is on by default, and MIR is vendored.** `TUR_JIT` defaults
  ON on 64-bit x86-64 and arm64, with MIR's sources (the three TUs `tur_mir`
  compiles, 2.7 MB) vendored under `external/mir/` -- no configure step reaches
  the network. Release archives ship `libtur_mir.a` and run a program through
  `tur jit` after unpacking, so a host embedding `libturi` has the engine
  beside it. `tools/update-mir.sh` re-syncs the copy.
- **A default `tur build` links the prebuilt runtime preamble instead of
  recompiling it**, on Linux and Windows. `--runtime=auto` swaps the fixed
  preamble for its decls region and links `libturt_preamble.a` rather than
  recompiling ~4400 lines in every program: 81-86s -> 73s on a 4-core Linux
  box over 1/10 of the suite, and 41.1 -> 36.8 min across the Windows CI
  shards. `auto` quietly keeps the whole preamble when any precondition fails
  (a sanitizer in `TUR_CC_FLAGS`, `--debug`, wasm, an r7rs prelude split, or
  either archive missing). `TUR_PREAMBLE_SPLIT=0` opts out; `=1` opts in on
  macOS, where the split stays opt-in but now links with `-dead_strip`.
- **A written `: nil` / `: void` makes a defn void.** The body's value used to
  win, so `(defn noop [x : int] : void (let [_ x] 0))` was emitted as
  `static int64_t noop(int64_t)`; a body of any other type now runs for effect.
  Relatedly, a word-result function is refused where a `nil` result is
  expected -- that mismatch called the function through the wrong C type.
- **Effect checking reaches code it used to skip.** A `defn` inside a
  `(defmodule ...)` body, or inside a macro's top-level `(do ...)`, was never
  resolved or checked, so `#fx{}` on it was a promise nothing read. The
  `FnDef` index was also a fixed 1024-slot array that silently dropped entries
  once full, and `#lang r7rs` programs index well past that, so a late
  callee's inferred effects never reached its callers. An annotation that was
  silently unchecked may now report a real error.
- **`stdlib/either.tur` is generic in `(Either L R)`.** `left?`, `right?`,
  `from-left`, `from-right`, `either`, `either-map` and `either-map-left` take
  and return `(Either L R)` instead of erasing to `:int`, and
  `str->int-checked` declares `(Either int int)`. Every caller in the tree
  passed unchanged, but an `(Either cstr float)` now carries its payloads (the
  `:int` signatures rejected a float outright), and an int default against an
  `(Either cstr cstr)` is now `TUR-E0001`.
- **The repos live in the `turmeric-lang` GitHub org.** Clone URLs, the
  Homebrew tap and the installer name the new owner. Build provenance is bound
  to the owner that built the asset, so verification follows a release's
  vintage: v0.59.0 and earlier need `gh attestation verify <asset> --owner
  rjungemann`, this release and later `--repo turmeric-lang/turmeric`.

### Fixed

- **The release workflow publishes on linux-aarch64 again.** On glibc aarch64
  the emitted unit reaches `<ucontext.h>` -> `sys/user.h`, whose
  `struct user_fpsimd_struct` c2mir cannot parse, so the engine declines every
  program on that platform until the vendored fork learns `__uint128_t`.
  Keeping the archive's JIT check strict there published nothing on any
  platform. The exception is conditional, not an opt-out: on linux-aarch64 a
  `TUR-W0070` fallback passes only when `libtur_mir.a` is in the extracted
  archive (asserted first and unconditionally), the program prints the right
  answer, and the diagnostics name `sys/user.h`. A missing engine, a fallback
  for any other reason, or the same fallback on any other target still fails.
- **`Arrow`'s `>>>` / `<<<` are specialized at the call's element types.** The
  `(->)` instance built its closure once at erased words, calling through
  `int64_t (*)(void *, int64_t)` while the caller read the result at
  `(fn [float] float)`. The class now spells its arrows and the instance
  bodies name the element types; the emitter also binds the instance's element
  variables per spec inside a `(defn pipe [^Arrow A] ...)` generic, where
  nothing had bound them and every spec called the erased base. Three further
  defects behind the same report are fixed: `>>>` returned `ptr<void>`, so the
  module's own docstring example `((>>> f g) 7.1)` printed
  `-9223372036854775808`; a direct invoke of any generic's returned closure
  value-converted a float argument through the generic's carrier; and the
  type-variable-parameter escape shim boxed an already-fat binding a second
  time, so `(pipe f g)` for `[^Arrow A]` segfaulted at every element type.
- **A generator's lifted closure head calls its thunk at the thunk's own
  return type.** The call site re-derived a pointer result after the head
  block had already read the recorded `int64_t`, and at `A := bool`/`int8`/
  `uint8` the slot-0 widen wrapper called the base thunk at the spec-resolved
  narrow type. Return-type-only and ABI-benign on LP64, so only
  `-fsanitize=function` saw it.
- **A dict wrapper spells a pass-by-pointer parameter the way the impl does.**
  A typeclass method whose result is a by-value ADT forces a per-instance
  `__dictwrap_*`, whose parameters were spelled by value while the impl and
  the slot typedef spell `const T *` -- a hard C error at the call and an
  incompatible-pointer assignment into the slot.
- **An imported generic now waits for the importer's instances.** A class and
  a constrained generic over it in one module with the instances in the
  importer -- the layout a spice takes -- failed `TUR-E0015` "this program
  declares no 'Box' instance at all" for a program declaring several. Such a
  defn is parked and retried at the importer's next statement boundary after a
  new instance registers; whatever is still parked at the end is elaborated
  for real, so an instance-less program still reports `TUR-E0015`.
- **`httpd` multipart parsing is strict and NUL-safe.**
  `httpd-req-multipart-parse` found `boundary=` anywhere in the Content-Type
  case-sensitively (so a quoted `charset="boundary=YY"` decoyed it and
  `BOUNDARY=` missed), never checked the media type was multipart, read
  `name="` out of `filename="`, and matched part headers by prefix. Every
  search was `strstr`, so a NUL byte in an uploaded file ended the scan and
  every part was lost. The media type is checked, the boundary bounded at 70
  per RFC 2046, headers matched by whole field name, and the delimiter line's
  CRLF required.
- **A `turi` sandbox cannot forge a continuation handle.** `(resume-cont! 4096
  0)` and the lowered clone/serialize forms cast a caller integer straight to
  a `TuriCont *` and walked it as a frame array. A handle has exactly two
  producers, both of which now register it, and every consumer checks the kind
  first.
- **`tur run` no longer hangs on a malformed Justfile shell array.**
  `parse_shell_array` advanced only by what `parse_value` consumed and never
  checked it consumed anything, so a `#`, `\n` or `\r` where a value was
  expected stalled the cursor, appending an empty entry per turn until the
  allocation reached 2 GB (~268 million turns) -- a denial of service on
  untrusted Justfile input, found by the `fuzz_justfile` fuzzer.
- **A class variable mentioned only inside a function-typed parameter counts
  as reaching a parameter.** `type_mentions_named_tyvar` fell through on
  `TY_FN`, so a nullary method over such a generic hard-errored.
- **Refinements and `loop-invariants`:** an early `return` no longer bypasses a
  return refinement or a `:post` clause, a float field's selector takes its
  field's sort, `Bool` equality is treated as iff, an unknown note is reported
  as unknown, and a `#reads` measure passes the CT1 purity gate in every
  contract position. `loop-invariants` havocs a nested loop's names, prunes an
  early return's paths, analyses `definstance` methods, and declines
  top-level lambdas out loud instead of silently.
- **`with-cancel-guard`** uses the preamble's portable `setjmp`, calls typed
  closures at their type, and checks nullary results.

### Docs

- **The Homebrew one-liners spell the tap URL out.** `brew` resolves a tap
  name `user/repo` to `github.com/user/homebrew-repo` with no fallback, and
  there is no `homebrew-turmeric` repo -- `Formula/turmeric.rb` lives in this
  one. A bare `brew tap turmeric-lang/turmeric` clones a URL that 404s, so the
  explicit-URL form is load-bearing and now says so.
- The guides gained a comparison of Turmeric typeclasses with OCaml's modular
  implicits, and `genguides` renders fenced code inside blockquotes and list
  items as code rather than prose.

## [0.59.0] -- 2026-10-02

### Changed

- **`println` declares `#fx{IO}`, so `#fx{}` means "does not even print".**
  Printing was invisible to the effect system: `println` was a builtin with an
  empty row. Now a function annotated `#fx{}` that prints is `TUR-E0009`, and
  an annotated function that prints must name `IO` (which also covers `Write`,
  `FS`, `Net`, `Proc` and `Rand`, its children). Unannotated code is
  unaffected. `IO` is a `^capability` -- tracked, never handled -- so for
  output a handler should be able to intercept, `(perform (Write s))` is still
  the path; the effects guide now says which to reach for. Under
  `--strict-effects`, an unannotated function that prints now gets
  `TUR-W0030`. Saffron's dynamic `println` carries the row too, and hover
  shows it: `(println : (fn [int] #fx{IO} : nil))`.
- **`IO`, `FS`, `Net`, `Proc` and `Rand` are compiler-known**, like `Unsafe`:
  `#fx{IO}` resolves with nothing loaded. `stdlib/effects.tur` keeps its
  declarations; a `defeffect` of one of these names must match the built-in
  exactly (`(defeffect FS [] :nil ^extends IO ^capability)`), and anything else
  is an error.
- **Compiler attributes moved out of the effect row.** `#fx{...}` now holds
  effects and row variables only. `(defn ^construct some ...)` replaces
  `#fx{Construct}`, `(defn ^byval name ...)` replaces `#fx{ByVal}`, and
  `(match ^non-exhaustive x ...)` replaces `#fx{NonExhaustive}`, which still
  works with a `TUR-D0004` deprecation warning. An unknown attribute before a
  defn's name (`(defn ^contruct f ...)`) is an error instead of becoming the
  function's name.
- **An undeclared effect name in `#fx{...}` is an error (`TUR-E0026`).** It
  used to be dropped silently, so `#fx{IO}` in a file that had not loaded
  `stdlib/effects.tur` checked as `#fx{}`, and a caller's `#fx{}` then passed
  the `TUR-E0009` check the tag existed to buy. The error names the tag,
  offers a did-you-mean for a near miss, and says which module to load for
  the `stdlib/effects.tur` names. It covers every position a row is written:
  a `defn`, a `fn` literal, a fn-typed parameter, a record field, a class
  method. The check found four rows in the tree that had never resolved: a
  stdlib `#fx{FS}` on a stdout writer (now `#fx{IO}` -- stdout is not FS), and
  three fixtures' undeclared `Write` and `#fx{|e}`.
- **`--lint-effects` is a deprecated alias for `--strict-effects`
  (`TUR-W0050`).** It was a byte-identical second copy of the `TUR-W0030`
  check. `-Werror=strict-effects` is new: it makes the `--strict-effects`
  warnings errors, and implies the flag.
- **Effect diagnostics no longer print compiler-made names.** A `fn` literal
  is *anonymous function in 'dfs-or'* rather than `__fn_38`, an instance
  method is *method 'eq?' of instance Eq [int]*, and a class default body is
  *default body of method 'greeting' in class Greet*. A rank-2 wrapper the
  compiler generates (`__poly_N`) no longer gets its own `TUR-W0030`; the
  function it wraps already does. `TUR-W0030` spells the row to
  add (`add #fx{Bt} after its parameter vector`) in the current `#fx{}`
  syntax rather than the retired `#{}`. `TUR-W0031` is no longer reported on
  an instance method, whose row is its class method's and cannot be changed
  from the instance.
- **A fat closure's slot 0 takes an untyped `ptr<void>` parameter as the
  word (`int64_t`).**  A function-typed parameter already crossed that way, and
  a program that erases a closure to `ptr<void>` and calls it back as
  `(fn [ptr<void>] ...)` needs the two to share one spelling, or the call goes
  through a function pointer of the wrong type (a WASM `call_indirect` trap).
  Turmeric code is unaffected; **inline C that calls a closure's slot 0 by
  hand** must spell such a parameter `int64_t` --
  `TUR_APPLY1_T(void *, int64_t, f, p)`, not `TUR_APPLY1_T(void *, void *, f,
  p)`.  The closure's own body still sees a `void *`.  See
  [docs/guides/value-representations-guide.md](docs/guides/value-representations-guide.md#slot-0s-signature-which-parameters-are-the-word).
- **Stdlib comparators take a real `(fn [A A] bool)`.** `vec-eq?`,
  `map-eq-raw?`, `set-eq-cmp?`, `result-eq?`, `pair-eq-carrier?`,
  `mutmap-eq-storage?` and `map-eq-dynamic` call their comparator's slot 0
  with each element as a word but declared it an untyped `^fat`, so the
  comparator was boxed at its own signature: for `(fn [a : float b : float]
  ...)` the elements went in integer registers and the thunk read xmm
  registers nobody set. Compiled, `(vec-eq? [7.1] [3.25] cmp)` answered true
  where `tur --interpret` said false. The Turmeric wrappers `map-eq?`,
  `map-eq-k?` and `mutmap-eq?` keep an independent type variable, so an
  erased comparator over a typed map is still accepted.

### Fixed

- **A caller written above a generic callee now sees the callee's real
  signature.** An arity-only forward declaration produced `TUR-E0709` for a
  by-value result, "expected int, got float" for a float argument, and a
  SIGSEGV when a lambda was handed to a later generic higher-order function.
  Pass 2 now orders a defn after any not-yet-elaborated "lossy" defn it calls
  (generic, or with a fn-typed parameter), breaking a cycle at its first lossy
  member. Inside a `defmodule` the same pre-pass had kept every non-scalar
  declared return -- `(Result Handle cstr)`, `(Option Box)`, a bare `: Box` --
  as the int-carrier placeholder, which is what broke the `secret`, `valkey`
  and `tourist-session-valkey` spices. Mutually recursive generics that saw
  each other's placeholder result are primed before elaboration.
- **An open argument ahead of the one that fixes a type variable no longer
  types the result as `int`.** `(get-or (none) 1.5)` against
  `[o : (Option A) d : A]` printed `1`, and `(err "e")`, `(vec-new)` and
  `(map-new)` in first position did the same; a `cstr` default printed an
  address. Such a binding is provisional now and grounded after the loop, so
  emit monomorphizes it. Also fixed in the same sweep: a catch-all variable
  arm over `(Option A)` in a generic emitted invalid C at every
  instantiation, and a `uint8` payload (`(cx nn (:: 200 uint8))`) aborted the
  compiler.
- `^fat x : (fn ...)` in a `let` bound to a captureless lambda or a named
  defn stored a bare code pointer, and the first call took SIGSEGV; bound to a
  closure-returning call such as `(>>> f g)`, a float call printed garbage.
- A CPS-emitted tail call into an inline join leaked the fresh sum box it
  produced itself -- `(ok? (result-map (ok 1) f))` inside a colored function
  dropped its 16-byte `Ok`.
- The fixture corpus is clean under clang's `-fsanitize=function`, and a new
  `fnsan` CI job keeps it that way. A `known.fnsan` marker names a fixture's
  open report, and such a fixture is run alone and required to still trap, so
  the list cannot go stale.

### Documentation

- **The security guide is accurate about `#fx{Unsafe}` and proc macros.**
  `#fx{Unsafe}` is enforced at every call site (only `(unsafe ...)` or an
  `Unsafe` caller discharges it), and it means "this body does pointer
  arithmetic", not "this function may corrupt memory on bad input" -- the
  audit plan's open question 3, answered. `--no-proc-macros` is
  rust-analyzer's `procMacro.enable` turned off; rust-analyzer ships it on.

## [0.58.0] -- 2026-10-01

### Security

- **The security audit's remaining work packages landed (WP2, WP5, WP6, WP7).**
  WP1 and WP3 shipped in the 0.57 line; this release closes the rest of
  [docs/upcoming/security-audit-plan.md](docs/upcoming/security-audit-plan.md).
  - **WP2 -- compiler driver and filesystem.** Every command the driver
    constructs and every path it touches was reviewed against the plan's nine
    D rows; three of them were wrong as filed and are corrected in the plan's
    own verification pass. Covered by 27 assertions in ctest
    `tur_security_driver`.
  - **WP5 -- runtime memory safety.** Caller-supplied sizes are range-checked
    before they reach an allocation (M-5, and 26 further sites the sweep
    found); a wire length is checked against the input before it sizes a
    `malloc`; two region store-hook gaps are closed and a third class the
    survey did not have (M-6); format strings are policed under `-Werror`; and
    the concurrency fixtures run nightly under ThreadSanitizer.
  - **WP6 -- the web app and the published site.** One Content-Security-Policy
    (`web/csp.js`) is applied by the dev and preview servers, stamped into the
    built `_headers`, and set by the worker on its own responses. `escapeHtml`
    escapes both quote characters, the Try Turmeric console transcript is
    stored and rehydrated as data rather than HTML, the wasm eval worker has a
    watchdog and a Stop, and the doc-page generators write their scripts as
    files instead of inline `<script>` blocks, so every page under
    `/docs/html/` runs under the policy.
  - **WP7 -- supply chain.** Release assets carry
    [build provenance](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations):
    each one, `sha256sums.txt` included, is signed through Sigstore from the
    release job's OIDC token, verifiable with
    `gh attestation verify <asset> --repo rjungemann/turmeric` (C-4). Every
    GitHub Action is pinned to a SHA, emsdk and pip dependencies are pinned,
    and each workflow declares its permissions (C-5). Dependabot and CodeQL
    are on (C-8). `tvm` fails closed when it cannot verify a checksum (C-2),
    and `/install` bootstraps `tvm` and installs a checksum-verified release
    rather than building from a moving branch (C-1).

- **A sandboxed interpreter refuses forged handles (S-5, direction 1).**
  Interpreter handles were bare integers that natives cast back to pointers
  unchecked, so `(vec-get 4096 0)` inside a sandbox or a `defmacro*` was a wild
  read and the setters a wild write. A per-restricted-env handle-provenance
  registry plus a 242-row per-native handle-kind table make the native dispatch
  reject any handle argument no matching constructor minted, closing the
  arbitrary read/write, kind confusion and use-after-free while real handles
  round-trip. A `panic` or native error inside a restricted env now returns
  `TURI_ERROR` instead of ending the host process. The value-model channel
  (an erasing ascription, continuation resume) stays open as direction 2.

- **Untrusted-input parsers hardened (WP4).**
  - Serialized continuations are checked inside the runtime on every rebuild
    route: `bytes->serial-cont` returns an `Err`, while `resume-cont!` and
    `image/blob-resume!` panic. A forged buffer could previously hand a
    cstr-env frame an integer to dereference.
  - Images carry a payload CRC and are held to their file size.
  - Both JSON decoders fix an over-read past a trailing backslash, cap nesting
    at 256, decode `\uXXXX`, and free partial trees on error.
  - `tur lsp` / `tur dap` reject a malformed or oversized `Content-Length`. A
    `-1` was a heap overflow.
  - `httpd` changes:
    - It refuses malformed, conflicting or oversized `Content-Length`, and
      any `Transfer-Encoding`, before reading a body.
    - It caps bodies at 8 MiB (`httpd-set-max-body!`).
    - It drops response headers that would split the response.
    - It keeps `mw-static` inside its root, symlinks included, and sizes its
      `realpath` buffers without trusting `PATH_MAX`.
    - It fixes a stack overrun in `httpd-set-cookie!`.
    - Six residual request-path items from WP4's own follow-up report.
  - The reader no longer `free()`s arena memory on `f[x]` in a neoteric or
    sweet-exp file. That was a crash in `tur check` and the language server.
  - `tur run --list` no longer hangs, allocating without bound, on a Justfile
    dependency argument list cut off by a comment (`(dep #`). It also no longer
    overflows the stack on deeply nested parentheses: nesting past 256 is a
    parse error.
  - The parsers now run nightly under libFuzzer (`tests/fuzz`,
    `-DTUR_FUZZ=ON`), reporting findings to Sentry.

### Added

- **`tur fetch --frozen`.** The `npm ci` / `cargo --locked` shape: fetch
  exactly what `tur.lock` pins and fail if anything would move, if a dep is
  missing from the lock, or if there is no lock at all. `--frozen` and
  `--update` contradict each other and are refused together. Plain `tur fetch`
  also **checks out the commit the lock pins** rather than re-resolving the
  ref it was written from -- a lock that named a branch used to float.

### Changed

- **`#lang r7rs` graduated.** R7RS-small Scheme is an ordinary base dialect
  now, on the same footing as `#lang turmeric` and `#lang saffron`: no
  `EXPERIMENTS[]` row, nothing to enable, no `TUR-W0061` lifecycle warning on
  every compile, and no way for a project manifest to refuse a directive the
  file itself carries. `tur dialects` reports all ten bases as `stable`,
  `tur experiments` no longer lists `r7rs`, and Try Turmeric drops the
  `experimental` chip from the two Scheme rows. `--enable=r7rs` is accepted as
  a `TUR-W0063` no-op for a minor line, so a `build.tur` or
  `experiments.tur` that named it keeps working.

  Emitted code is unchanged: the one side effect the gate carried
  incidentally -- `g_opt_r7rs`, which the emitter reads to pick which
  collector opt-out governs the program (`--no-r7rs-gc` versus Saffron's
  `--no-saffron-gc`) -- now comes from a `LangTraits.scheme_runtime` bit set
  at the same moment, when the `#lang` line is read. The emitted C is
  byte-identical across all six cells of {r7rs, saffron} x {default,
  `--no-r7rs-gc`, `--no-saffron-gc`}.

  The dialect was a prototype from 0.52.0, beta from 0.56.0, and graduated on
  its advisory `expires_at` of 0.57.0 rather than past it. The plan is
  archived at [docs/archive/r7rs-lang-plan.md](docs/archive/r7rs-lang-plan.md).

- **`httpd` servers bind 127.0.0.1 by default.** Call
  `(httpd-set-bind-any! true)` or set `TUR_HTTPD_BIND_ANY=1` to listen on
  every interface. A body shorter than its `Content-Length` now drops the
  connection instead of reaching the handler truncated. See the httpd guide's
  "Binding and request limits".

### Fixed

- **P0 representation confusion: the generic-spec matrix is at zero and the
  emitted-C indirect-call corpus is down from 328 trapping fixtures to 9.**
  The long-running family where a value crosses between a typed
  representation and the int64 carrier -- and is read back at the wrong one --
  is the bulk of this release. Two detectors now police the mechanism rather
  than the shape: `tests/generic-spec-matrix.py` (ctest
  `tur_generic_spec_matrix`) walks every PRODUCER x SINK x TYPE cell compiled,
  interpreted and linted, and its open-cell baseline is now **empty**; and
  clang's `-fsanitize=function` is armed in all four source fuzzers, where the
  count of fixtures making an indirect call through a wrongly-typed function
  pointer fell 328 -> 165 -> 106 -> 66 -> 16 -> 9 across seven sweeps
  (report-only until it reaches zero; the remaining nine are tracked in
  [docs/reported/emitted-c-indirect-calls-are-not-type-exact.md](docs/reported/emitted-c-indirect-calls-are-not-type-exact.md)).
  Several of the crossings were **silently wrong answers**, not just C
  undefined behavior, whenever the differing type was a `double` or a 16-byte
  tagged `any`: a dictionary slot converting a class-variable `float`
  parameter by value, a `tvar` float payload, a rank-2 class method result,
  a boxed `(Option float32)` read at the wrong offset, and a sub-word payload
  box. Sixteen reports in this family are archived under `docs/archive/`;
  the fixes span carrier adapters, fat-box spelling keyed on the callee the
  call selects, variadic rest shims, runtime callbacks, CPS joins and
  typed-pointer binders, class-var applied results inside constrained
  generics, and applied type annotations (which were going unchecked
  against return, argument and `let` positions).

- **`#lang r7rs`: a re-entrant `call/cc` no longer gives a wrong value when
  the unit also calls `eval`.** Fixed by `7c90e00b8`: the re-entry path's
  thread-local stores were made through a stale address after `setjmp`'s
  second return, so a variable `set!` between the capture and the re-entry
  read back as a non-number (`error: +: not a number`) or, on another base,
  faulted inside the capture. `(scheme eval)` mattered only because linking
  the embedded interpreter changed where the stale store landed.
  `tests/fixtures/docs-r7rs-guide-examples` covers it. The fix shipped inside
  0.57.0 undocumented; the note lands here. Report archived at
  [docs/archive/r7rs-reentrant-callcc-wrong-with-eval.md](docs/archive/r7rs-reentrant-callcc-wrong-with-eval.md).

- **Try Turmeric share links encode and decode.** Share has never worked:
  `pako` was never loaded, so the compressor the encoder called was
  `undefined`. The codec moved into `web/share-codec.js` with a Playwright
  spec over a real round trip.

- **Eight more compiler and runtime defects, each with its report archived.**
  A phantom-parametric `:heap` `let` binding ICEd during representation
  selection; a transparent `:int` newtype bound in a `let` was freed as a
  pointer; a refined ADT return type miscompiled; `turi`'s inline-C `bool`
  return was tagged as an int; `vec-push!` of a by-value struct parameter
  emitted an unbridged pointer; a Saffron dynamic witness defaulted every
  function's arity to unary; a `tvar` captured by a closure inside `stm` was
  reported undeclared; and `musttail` across a by-value aggregate argument
  dangled on aarch64 (now refused). Closure captures are tracked under every
  binding form, and `with-handler` over a literal is treated as a `handle`.

- **`Buf` keeps `data[len]` NUL on every append.** `buf_puts` did not, while
  `main.c` reads the `aux_includes` / `aux_sources` buffers as C strings --
  a heap overread the fixture suite could not see, because it only compares
  printed output.

- **The documentation pack.** Deploying a pack built without the spices
  checkout is refused rather than silently shipping a pack missing every
  spice page, and a cross-spice README link resolves instead of failing
  `--strict-links`.

## [0.57.0] -- 2026-09-30

### Added

- **Reflected measures, behind `--enable=reflected-measures`.** `^reflect` on
  a `defn` lets the refinement solver *use* the function's definition: at a
  call whose argument is a constructor term or a literal, the body is
  unfolded and `f(t) = <body at t>` is asserted, so
  `(head-of (Cons 1 (Nil)))` against `(> (len v) 0)` proves instead of
  keeping its runtime check. The equation is admitted only for a function
  shown **total** -- pure, structurally recursive in one fixed argument
  position, exhaustively matching -- and a `^reflect` that fails that gate is
  a hard `TUR-E0384` naming the gate (an unfolded non-total function is an
  inconsistent hypothesis, which would discharge every obligation in the
  unit). Unfolding is fuel-bounded per obligation (default 8,
  `TUR_REFLECT_FUEL` overrides); running out is `TUR-W0385` beside the
  ordinary `TUR-W0372`, promoted by `--strict-refine`. `--dump-reflect`
  prints each site's verdict. Inside a `match` arm of the function being
  proved, a reflected measure applied to the scrutinee unfolds through the
  arm's own tag fact, with the arm's binders standing for the field
  selectors (plan RF4). A false obligation over reflected measures at
  ground arguments is refuted (`TUR-E0371`, "false for the value given
  here") rather than left unknown: the bounded model search runs when every
  uninterpreted symbol in the VC is a constructor or an unfolded reflected
  measure, and evaluates each application by its own equation (plan RF6).
  Nothing changes for a program that does not
  write `^reflect`; without the flag the attribute warns and is inert. See
  `docs/upcoming/reflected-measures-plan.md` and the refinement guide's
  "Reflected measures" section.

- **Loop invariants, behind `--enable=loop-invariants`.** A `while` may carry a
  user-written `:invariant p` directly after its condition. The annotation
  always parses and is validated as a pure bool (`TUR-E0375`, where a stray
  keyword used to be a silently ignored value statement); the gate withholds
  the acting. Enabled, `p` is checked on entry and at the end of every
  iteration in compiled and interpreted code, and per-conjunct initiation and
  preservation obligations are discharged against the solver -- a proof elides
  the check, a refutation is `TUR-E0371` naming the broken conjunct and path,
  and undecided or declined is `TUR-W0372` (promoted by `--strict-refine`).
  The post-loop fact `p AND (not c)` is then usable by return obligations and
  by call-site crossings. Conservative declines keep both checks for place
  writes, early exits, borrowed names, names assigned in lambdas or handler
  clauses, field/deref/mutable-global reads in the invariant, and shadowing
  body lets. See `docs/upcoming/loop-invariants-plan.md`.

- **`#lang r7rs/sweet` -- sweet-expressions over Scheme's lexemes.** Scheme was
  the one language with no sweet-exp base, though SRFI-110 is a Scheme SRFI.
  The preprocessor's scanners now ask one helper which bytes are not structure
  -- a character literal (`#\(`, `#\;`, `#\"`) in every dialect, and under
  Scheme a `|delimited symbol|` and the `#;` prefix -- so a Scheme reader runs
  under the sweet-exp layer with neoteric on, following SRFI-105: `f{n - 1}`
  is `(f (- n 1))`, `f{}` is `(f)`, and `f[x]` is `f(x)`. A library found by
  `import` may be written in it, `tur fmt` keeps its layout (idempotent, with
  `--lang r7rs/sweet` for a bare buffer), and the playground picker lists it
  under Scheme. `r7rs/neoteric` and `r7rs/curly-infix` stay unknown. Fixed on
  the way: in `turmeric/sweet` a `#\(` opened a group that never closed and a
  `;` inside a string after `$` cut the line short, and in every dialect a
  datum comment last in a list was "unexpected `)`" -- `#;` is intertoken
  space now, as R7RS 2.2 says.

- **A security guide, `SECURITY.md`, and a private disclosure channel.**
  `docs/guides/security-guide.md` states the five trust boundaries, each with
  its promise and a "status today" block naming the open defect where the
  promise is not kept. `SECURITY.md` carries the private advisory form, a
  7-day acknowledgement / 14-day assessment, and an explicit scope and
  non-scope; GitHub private vulnerability reporting is enabled on the repo.
  `CODEOWNERS` is grouped by the boundary each path sits on rather than by
  directory.

- **Function values cross an instance body and a typed `fn` parameter.** Four
  open reports, all about a function value losing its shape at a boundary.
  Non-HKT method dispatch binds the method's own type variables from the
  arguments, so an instance spec resolves them instead of spelling the call
  through the int64 carrier. A new `any` bridge marshals a function whose
  signature differs from the slot's only in where `any` appears -- at a method
  dispatch, and at a typed call's ground `fn` parameter -- and an HKT `: any`
  result attaches the M7 element bindings. An inline-C body may now declare
  `: any`.

### Changed

- **The CPS/effects and `#lang r7rs` runtimes keep their state per thread and
  per fiber.** Both kept all of their mutable state in process globals, so any
  compiled program running CPS code or a Scheme `guard` on two threads at once
  corrupted itself: a worker's entry replaced the landing another thread's tail
  resume longjmps to (15 of 15 runs), five threads in `call/cc` escapes
  segfaulted in `__dk_reap_push` (20 of 20), and a `guard` on one thread caught
  or missed a `raise` on another (every run). The DK reap registry, entry
  depth, trampoline landing, resume chain and meta-stack -- eleven variables --
  and the r7rs handler stack, wind stack and re-entry value are thread-local
  now, with host accessors for `tur jit` and as collector roots. A `FiberBlock`
  carries its own copies, swapped by `tur_fiber_block_resume`, so a fiber that
  yields inside a CPS entry, a `guard` or a `parameterize` finds them again on
  whatever thread resumes it. `parameterize` puts `(cell . value)` bindings in
  front of the running code's list for its extent instead of writing the shared
  cell.

- **A restricted interpreter env can no longer act on the host, or end it.**
  A `turi_env_new_sandboxed()` env created and deleted files, forked, read the
  environment, and ended or aborted the host process -- and from `tur check`,
  with no embedder and no flags, a `defmacro*` body calling a native by its
  bare name deleted a file and spawned a process at expansion time. Every
  builtin native is classified (656 names, 63 not pure) and
  `turi_env_register_native` stamps the row's `TURI_CAP_FS` / `TURI_CAP_PROC` /
  `TURI_CAP_ENV` bits on the closure, which `eval_apply_driven` refuses before
  the native runs. Separately, `turi_eval` and `turi_call` install a landing
  pad on an env without `TURI_CAP_PROC`, so an uncaught panic, a typed panic, a
  double panic or a native's own error exit returns `TURI_ERROR` instead of
  ending the process. Unrestricted envs print and exit exactly as before.

- **The `#lang r7rs` prelude compiles in parallel pieces on a cold cache.** The
  first build compiled the whole ~1,200-function library unit at `-O2` in one
  `cc` (9.1 s); it now compiles in one piece per CPU, up to eight
  (`TUR_PRELUDE_JOBS` overrides, `1` is whole), and `cc -r` joins the pieces
  into the object the cache already keeps: 4.6 s on four cores, 6.3 s on two.
  Warm builds are unchanged. Each piece keeps every declaration, type and
  static helper and gets a contiguous run of the external functions; a small
  function also gets a static twin in every other piece so `-O2` inlines it as
  before while its address still names the one definition. Any failure
  compiles the unit whole.

- **The JIT prunes the prelude a program never reaches before c2mir.** Every
  program paid a fixed ~0.46 s and c2mir was 60% of it, compiling the whole
  auto-loaded prelude -- ~377 static functions for `(println 42)` -- plus the
  heavy system headers it needs, where `cc` drops an unreferenced static
  function for free. The prune drops the static functions, objects, fat boxes
  and extern declarations nothing live names, and the unused
  regex/inet/socket/select/hamt includes. `TUR_JIT_NO_PRUNE=1` opts out, the
  full-TU retry (`TUR-W0071`) covers a pruned TU, and `run-jit.sh` fails a
  fixture that passes only on that retry. Measured over the corpus: per-fixture
  sum 1843 s -> 1355 s, median 508 -> 350 ms.

- **The last operand of `and`/`or` is a tail position.** One predicate now
  answers `tco_mark`, the `^tailcall` verifier and `emit_tail`, so the three
  agree: the last operand is in the enclosing tail position and the others are
  tests (the verifier names that, `TC_SC_TEST`). `emit_tail` lowers
  `(and a .. z)` to `if (!(a)) return false; .. <tail z>`, symmetrically for
  `or`, so a self call there is a backedge at `-O0`.

- **The REPL reads multi-line forms the way the reader does.** It decided a
  form was complete with a per-line bracket count that knew only `"` strings
  and `;` comments, so a ```` ```c ```` body's `for (...;...;...)` kept the
  `..` prompt open forever (piped input swallowed, exit 0), a `')'` character
  literal evaluated the form halfway through the fence, and multi-line
  strings, `#| |#` comments and `#\(` literals broke the same way. A blank
  line inside a string, fence or block comment is kept as content, and end of
  input mid-form prints `(cancelled)`. A duplicate `defn` is rejected rather
  than silently shadowed.

### Fixed

- **Two source-reachable refinement soundness holes around int/real
  literals.** Both elided a return check on a program whose refinement is
  false for the input given -- the reference build panics, the normal build
  printed the value and exited 0. S1's literal-conflict check treated the Int
  literal `3` and the Real literal `3.0` as a contradiction, so any cube
  equating a real-sorted term with an int-sorted one was refuted and the goal
  under it "proved"; literals now conflict only when their values differ. And
  `(as T e)` is a cast, not a measure. Alongside: the bounded counterexample
  search covers Real and Bool variables, so a plainly false float refinement
  or one with a bool parameter gets `TUR-E0371` with a witness instead of the
  vague `TUR-W0372`, and a hypothesis the encoder cannot express is a decline
  rather than a dropped fact.

- **A `match` arm's binders carry their own field's sort.** They were declared
  to the refinement solver at the sort of the function's *result* refinement
  rather than of their own field, so in a `: #refine{ r : bool | ... }` body
  every binder was a proposition and its field equation (`(= t (.tl xs))`) was
  silently dropped as a sort mismatch.
  Binders now carry their field's sort; a bool-returning `match` body knows
  what its arms destructure.

- **The refinement purity walk no longer memoizes a verdict taken across a
  forward reference.** It memoized a caller's UNKNOWN verdict when its
  callee had no body yet (a forward reference), so a function asked about
  early stayed non-congruent for the whole unit even once the callee was
  defined. Every frame open at such a miss is now provisional, as it already
  was across a recursion edge.

- **A threaded `call/cc` capture no longer swallows the thread's own
  thread-locals.** `r7k_stack_base` took the stack top from
  `pthread_getattr_np`, which on glibc is the top of the mapping, and for
  every thread but the main one glibc keeps the TCB and each module's static
  TLS block up there. A capture that no top-level form bounds copied the
  thread's thread-locals into the image, and every re-entry wrote them back as
  they were at the capture -- including the value the re-entry delivers. The
  image now stops below the TCB and static TLS. A parked thread's registers
  are aligned and spilled in the caller's frame, the frame pointer is read
  plainly at every spill, and a fiber that finishes on a different thread gets
  fresh thread-locals and keeps its own live-escape set.

- **macOS r7rs-gc: the stop handler polls instead of `sigsuspend`, and a
  nested entry does not wait.** Also the prelude split's GC seam, and the
  allocator table is built on the stack rather than as a `static const`.

- **A constructor that leaves a type parameter open keeps the ones it fixes.**
  `(Ok 7.1)` fixes `A` and nothing names `B`, and it was typed as the bare
  `Result` -- which dropped `A` as well. Beyond the reported `fmap` refusal,
  that was a silent wrong answer: `(ok-val (Ok 7.1))` and `(err-val (Err
  3.25))` printed the float's bits. The constructor is typed as the
  application now, each open parameter a type variable carrying an
  `open_slot` bit, and an argument's open slot matches a variable another
  argument already bound. Separately, an enclosing definition's type variable
  stays rigid at a call through a local `fn` value, where the expected-return
  binding used to bind it to the enclosing `: any` and never widen.

- **`catch-error` boxes an ascription-grounded handler result, and a
  quoted-symbol base case keeps its tail-call partner.** An inline-C instance
  body reads its handler's result as the int64 carrier word, so method-call
  poly-fn packing asks for the carrier-spill shim there too; and the let-init
  carrier bridge no longer dereferences an init whose emitted text already is
  one. In CPS, a quoted-symbol base case no longer evicts a guard's
  tail-call partner.

## [0.56.3] -- 2026-09-28

### Added

- **SRFI 17, generalized `set!`, under `#lang r7rs`.** `(import (srfi 17))`
  makes `(set! (f arg ...) v)` mean `((setter f) arg ... v)`. `car`, `cdr`,
  the whole `c[ad]r` family, `vector-ref`, `string-ref` and
  `bytevector-u8-ref` are settable out of the box; `getter-with-setter`
  attaches a setter to a procedure of your own, and `(set! (setter f) s)`
  adds one to any procedure. The target is an arm of the lowering's `set!`,
  turned on by importing this SRFI's `set!` under any name, so the export is
  R7RS's own and the import sits beside `(scheme base)` as one binding.
  Without the import the shape is an error naming the SRFI, in place of
  Turmeric's "set! target must be a symbol". The table is built on first use,
  so an `(import (srfi 17))` a program does not use emits byte-identical C.
  It was the last SRFI held by r7rs-srfi-plan S2: `setter` is keyed on
  procedure identity, which standard procedures did not keep until
  2026-09-27.

- **Multi-party sessions take a timed receive.** A `defprotocol` step
  `(timeout (-> A B T) [ok ...] [expired ...])` lets the receiving role give
  up after a deadline supplied at the op, `(recv-timeout-from ch A ms)`,
  matched like `recv-timeout`. Only the receiver observes the outcome, so
  projection requires every other role -- the sender included -- to continue
  the same way in both branches (TUR-E0220, checked at declaration for timed
  protocols). At run time an expiry leaves the router slot a skip: the
  message the receiver gave up on is dropped when it is sent, instead of
  blocking the sender forever or arriving at the receiver's next receive.

### Changed

- **A compiled Saffron program allocates from the r7rs-gc collector.** A
  Saffron `any` value aliases freely -- a widen boxes a by-value payload, the
  box is copied into arguments, fields and results, and a dynamic call may
  keep any of them -- so most of those boxes have no static owner and a
  rebuild loop leaked linearly. The conservative collector every compiled
  `#lang r7rs` program already runs now backs single-unit compiled
  `#lang saffron` as well (`TUR_SAFFRON_GC=0` / `--no-saffron-gc` opts out);
  the static drops still run. Measured: a rebuild-map-fold loop peaks at 6 MB
  instead of 84 MB and growing, no slower.

- **A GADT constructor application knows its index.** `(LNil)` is
  `(LVec Zero)` and `(LCons 7 (LNil))` is `(LVec (Succ Zero))`: the index
  rides beside the value's type, which stays the bare ADT, on the channel the
  size indices already use -- so a `let`, an ascription and a declared return
  carry it, and no unification rule changes. Call arguments, ascriptions,
  annotated lets and a declared return against the body's tail are checked
  against it, and only a provable clash of two type constants is rejected. A
  bare argument whose index is fully known is refined to the parameter's
  application, so `(lhead (ltail v))` now needs no annotation.

- **CPS mutual tail calls are jumps.** A strongly-connected component of the
  CPS tail-call graph (2..8 members, 32 parameter slots) is fused into one
  dispatching function, so two procedures that tail-call each other no longer
  overflow below `-O2`; each member keeps a one-line wrapper for outside,
  helper and non-tail calls. T5's direct tail-call groups also stop refusing
  a CPS-colored function the CPS backend declines -- it is emitted as plain
  direct C, whose tail calls are C tail positions.

- **stdlib spells its last `^fat` callback types.** `free-bind` / `free-fmap`
  / `free-run`, parsec's `mbind`, `compose-middleware-of`'s base and
  `future-map` / `future-then` take real function types in place of `:int`
  and `ptr<void>`, so a wrong-arity or wrong-register-class lambda is a
  TUR-E0001 instead of a silent miscall; the C bodies are unchanged. Typing
  them exposed a checker hole, now closed: a variadic callee's fixed
  `fn`-typed parameters were never shape-checked.

- **The refine solver's EUF core is indexed.** Terms intern through an
  open-addressed hash index, congruence closes by a per-round signature table
  instead of comparing all pairs, and the shared set is recorded at
  registration. Corpus output is byte-identical -- verdicts and cap telemetry
  alike -- and the 512-term stress unit drops from ~510 ms to ~70 ms.

### Fixed

- **Four more classes of value are freed in emitted code.** A dynamic closure
  env is released for returned lambdas and for lambdas passed to
  non-retaining parameters; a by-value recursive ADT's spine is freed where a
  consuming callee does not pass it on, and a fresh spine lent to a
  non-retaining callee is freed after the call; a shared view is cloned where
  it becomes an owner rather than double-released (four ASan
  heap-use-after-frees, with the new TUR-E0108 for a `ref` field that would
  escape as a result); and a lambda-captured `^mut` cell is freed at the
  `let`'s scope end when every closure capturing it is provably dead.

- **A panic no longer strands an open region generation.** On both back ends
  a region scope's call ran the panic-propagation check before its pop, so a
  panic out of a `with-region` left the generation open and every later
  allocation landed in it. The bracket now retires its generation on the
  panic arm, and every emitted catch boundary records the region depth on
  entry and retires down to it, covering a generation stranded any other way.

- **`letrec` members that capture and call each other compile.** Two
  capturing members that call each other -- and so a `#lang r7rs` body's
  internal defines of that shape -- failed in `cc` with `'od_N' undeclared`.
  Elaboration now predicts which members capture before elaborating any init,
  and emission zeroes a not-yet-bound slot and fills it the moment the target
  is bound.

- **A re-entrant continuation invoked on another thread is refused.** Its
  image is a copy of the capturing thread's stack at that thread's addresses;
  invoked elsewhere it copied the main thread's frames over the worker's, so
  the worker ran the rest of the main program itself and exited the process
  while the real main thread was still in `pthread_join`. Both back ends now
  record the capturing thread and raise a guardable error, with no
  `dynamic-wind` thunk run.

- **A colored serial-shift receiver is admitted, and its effects reach the
  enclosing handler.** The refusal keyed on coloring, which is conservative;
  it now keys on whether an effect actually escapes. A receiver typed
  `k : serial-cont` takes the DK chain as its declared spelling instead of
  always `void *`, and when an effect does escape a named receiver the reset
  is lowered as an ordinary colored call on its own continuation, so the
  effect walks out to the handlers around it.

- **The JIT emits no struct-valued statement expressions, and the c2mir bug
  behind them is fixed upstream.** The union widen, `dyn_widen_to_any`'s
  by-value box and `emit_agg_box`'s box now build their values with
  statements and leave a plain expression. The defect itself -- c2mir
  reserved a struct statement expression's result slot at the frame size so
  far, while stack variables are laid out afterwards from offset 0, so the
  slot overlapped the first of them and the `({ ... })` copy-out overwrote
  it -- is root-caused and patched in the fork: the MIR pin moves to
  96c34860, which assigns those slots after the stack layout, picking up
  c2mir's C11 6.3.1.8 arithmetic conversion on the way (on win64
  `long long OP unsigned int` is now `long long`). Generated code no longer
  depends on either fix; user inline C still can, so
  `jit-inline-c-struct-stmtexpr-slot` pins it.

- **The `linux-aarch64` release binary ships again.** That leg had failed
  since v0.56.1, so both v0.56.1 and v0.56.2 published without
  `turmeric-<tag>-linux-aarch64.tar.gz`: a GCC `-Wmaybe-uninitialized` false
  positive on `eval.c`'s `EX_DYN_OP` operand read was fatal under `-Werror`
  on that backend alone. The `release` job's gate had also tolerated a failed
  build leg, which is why it shipped twice unnoticed; it no longer does.

## [0.56.2] -- 2026-09-28

### Changed

- **`call/cc` keeps far less memory.** Under `--interpret`, a capture now
  stores only the words that differ from the latest whole image of the same
  stack range, copies a drive's work stack at its length rather than its
  capacity, and frees the work stacks a re-entry abandons: a 16,000-step
  generator went from 1131 MB and 1.15 s to 284 MB and 0.68 s. An escape-only
  `(call/cc (lambda (k) ...))` is lowered to the one-shot escape `guard` uses
  and copies nothing. Compiled, a nested CPS entry forgets its registrations
  on exit instead of leaving them for the outermost one to drain: 2,000,000
  guards went from 285 MB and 2.8 s to 10 MB and 1.0 s. The drop is
  single-threaded only -- the reap list is a process global, so a threaded
  program keeps exactly the old behavior.

- **The Try Turmeric language picker is grouped by language.** A `#lang` base
  names a (language, reader) pair, so the list has a heading per language --
  Turmeric, Saffron, Scheme -- with the readers under it, and each row is
  labelled by the `#lang` line it writes. Curly-infix and neoteric are no
  longer offered as separate rows: `{a + b}` is enabled in every dialect and
  neoteric is one of sweet-exp's three tools. Both stay spellable, and a
  buffer that names one still gets its row.

### Fixed

- **Output that stops mid-line reaches the Try Turmeric page.** `#lang r7rs`
  plus `(display "Hello, world!")` printed nothing at all: a last line with no
  newline behind it sat in libc's FILE buffer and Emscripten's TTY device
  until some later run emitted a newline, and then arrived glued to the front
  of that run's output. Every entry point that runs user code now flushes on
  the way out, and the eval worker takes the bytes itself.

- **A `define-library` may define a standard or stdlib name.** A library
  defining `square` was refused as "already defined by an auto-loaded stdlib
  module", and one defining `None` or `list-length` was unreachable from its
  importers (or, interpreted, replaced the stdlib's `list-length` for
  everyone). Such a definition is now respelled, and each importer binds its
  names to it under any import set -- plain, `only`, `except`, `rename` or
  `prefix`.

- **A statically mistyped call raises at run time.** In a Scheme file a
  mistyped argument is widened to `any` and takes the checked cast, so
  `(let ((x #f)) (if x (car x) 0))` compiles and a call that runs raises
  "car: not a pair". A variadic procedure's fixed parameters were not checked
  at all (`(vector-fill! 5 0)` crashed) and take the same path; too many
  arguments to a known procedure raises "f: too many arguments"; the
  interpreter's cast to a symbol checks, so `(symbol->string "s")` raises
  instead of crashing.

- **The R7RS REPL keeps state across turns.** Each prompt/eval turn was
  lowered alone, so an earlier turn's macros, imports and `set!`-ability were
  gone in the next, and a turn importing a user library ran none of its
  expressions. The prompt also echoes each of multiple values on its own line,
  and nothing for `(values)` or a definition.

- **A split `#lang r7rs` build prints the right symbols on Windows.** A quoted
  symbol printed as other bytes (`|uired field|` for `caught`): MinGW-w64's
  GNU ld resolves a reference at an offset into a weak DATA definition to the
  wrong bytes. A split build now emits no weak records, and separately
  compiled modules spell their records' linkage as `selectany` on Windows. The
  split stays off on Windows until one Windows run confirms it.

## [0.56.1] -- 2026-09-27

### Changed

- **A `#lang r7rs` program builds in about a second.** `tur build` and `tur
  run` compile the runtime and the R7RS prelude once, cache the object under
  `<tmpdir>/tur-build/prelude/`, and link it, so later builds compile only
  the program's own C. A one-line program took 3.0 s to build and now
  takes 0.95 s with a Release `tur`; the first build, which compiles the
  library, takes about 8.5 s. Linux only for now; macOS and Windows build
  as before. A program the compiler cannot
  split this way builds as one unit, as before. `TUR_PRELUDE_SPLIT=0` forces
  a one-unit build (docs/archive/r7rs-programs-compile-slowly.md).

- **Unreachable code is dropped from the emitted C.** A procedure used as a
  value gets its fat box as a static initializer instead of a startup store,
  and a function making a `musttail` call takes its own address ahead of the
  call instead of appearing in one program-wide `used` table. Both roots used
  to keep every function they named alive, so `cc -O2` compiled prelude
  functions nothing could reach. Codegen snapshots moved with it.

- **A Scheme file sees only Scheme and what it imports.** Under `#lang
  r7rs`, Turmeric's stdlib (`vec-new`, `map-assoc`, `some`, ...) is visible
  only through `(import (turmeric stdlib/<file>))`, with `only`, `prefix`,
  `rename` and `except` working as for any library. A Turmeric built-in such
  as `println` is not visible at all. Turmeric's `#map{...}`-family literals,
  inline C and `@` are read errors that name the Scheme spelling or the
  import to use. `true`, `false`, `nil` and `^tailcall` are ordinary
  identifiers. A name nothing binds is an error under `--interpret` too,
  where the interpreter used to run a native of that name. All of these used
  to work with no import.

### Fixed

- **A standard procedure is `eqv?` to itself.** Under `#lang r7rs`, `(eqv? car
  car)` was `#f` on both back ends, so a table keyed by `car` never found
  it. Each reference to a typed prelude procedure made a new adaptor. There
  is now one per procedure, shared by a library and the program that imports
  it.

- **A Scheme procedure called with the wrong number of arguments raises.**
  Under `#lang r7rs`, `(f)` for `(define (f a . rest) a)` returned a
  procedure, because Turmeric curried the call, and the program went on with
  a wrong value. A call through a variable with the wrong count ended the
  program. Both now raise an error object `guard` catches, as does calling a
  value that is not a procedure. SRFI 41's test suite now passes in full.

- **A Scheme type error is an error object, not a panic.** Under `#lang
  r7rs`, `(car 5)`, `(vector-ref '() 0)`, `(+ 'a 1)`, `(< 'a 1)`, `(negative?
  "four")` and the prelude's other "it is an error" checks used to end the
  program with a Turmeric panic that `guard` could not catch. Each now raises
  an R7RS error object, such as "car: not a pair" with 5 as its irritant, on
  both back ends. SRFI 64's `test-error` catches them. An unhandled one is
  reported like any other error, with exit status 70.

- **`sqrt`, `pow`, `log` and the other `stdlib/math.tur` functions no longer
  call themselves.** Each wrapper was a C function named after the libm
  function it wraps. Where the compiler turns the math builtin into a libm
  call (to set `errno`), that call reached the wrapper again. It overflowed
  the stack at `-O0`. Under clang on Linux it returned garbage (`(sqrt 2.25)`
  was 0.0) or hung. The wrappers now get their own C names.

- **A Scheme program that raises builds with clang on x86-64.** Every `#lang
  r7rs` program that reached `raise` (so `error`, `guard`, every SRFI test
  suite) failed with "failed to perform tail call elimination on a call site
  marked musttail". LLVM rewrote the return type of a function under its own
  guaranteed tail call. Functions that make one are now pinned so their
  signature stays as written. Only clang builds change.

## [0.56.0] -- 2026-09-27

### Changed

- **`#lang r7rs` moves from prototype to beta.** The dialect's plan is
  complete -- r7rs-lang-plan's R0-R10 and Section 9's T0-T8, and
  r7rs-srfi-plan's S0-S7 -- and R10's exit criterion is met:
  `tur_r7rs_conformance` runs chibi-scheme's R7RS suite and reports 1223
  passing test invocations on both back ends, 2 settled as differences kept
  on purpose and none failing, of the 1216 tests the suite writes. The
  surface is frozen; beta is the soak, not more design. Every `#lang r7rs`
  compile now prints **TUR-W0061** ("graduates in 0.57.0") in place of
  TUR-W0060 ("breaking changes likely"), and `expires_at` comes in from
  0.70.0 to 0.57.0 so the date reads as the one-cycle soak the lifecycle
  describes rather than fourteen minor lines out. It stays advisory and
  still never blocks a release cut.

  Four open reports are the graduation checklist, and the registry row names
  them: every program that reaches `raise` fails to build under clang on
  x86-64, an under-saturated call returns a partial application instead of
  erroring, type errors are uncatchable panics, and Turmeric's syntax and
  auto-loaded names leak into Scheme source.

### Fixed

- **Two r7rs harnesses filtered the lifecycle warning by its code.**
  `tests/run-init-r7rs.sh` and `tests/check-r7rs-srfi-prune.sh` dropped it
  with `grep -v W0060`, which a beta row's TUR-W0061 slips past; both now
  match `W006[01]`.

### Docs

- **`syntax-guide.md` still said a Scheme program's data is never freed.**
  `r7rs-gc` graduated on 2026-09-25, so a compiled single-unit `#lang r7rs`
  program allocates through the conservative collector on Linux and macOS
  (`TUR_R7RS_GC=0` / `--no-r7rs-gc` opts out, which a program that starts
  threads must do). The r7rs and syntax guides also carried the prototype
  framing and TUR-W0060.

## [0.55.1] -- 2026-09-27

### Added

- **`(import (srfi N))` under `#lang r7rs`.** Thirty-two SRFIs ship as
  libraries a Scheme program can import -- 1 (lists), 2/8/26/31 (binding and
  lambda shorthands), 4 and 66 (homogeneous and octet vectors), 13 and 14
  (strings and char sets), 17 (generalized `set!`), 27 (random bits), 28 and
  48 (format), 34 and 35 (conditions), 41 (streams), 42 (eager
  comprehensions), 60 (integers as bits), 61 (`cond`'s receiver clause), 64
  and 78 (test suites), and 69 (hash tables), among the rest. `(features)`
  and `cond-expand` list the `srfi-N` identifiers. A pruning pass
  (`src/passes/srfi_prune.c`) drops every SRFI definition a program never
  reaches, so an unused `(import (srfi 1))` emits the same C as no import at
  all (3.50 s to build against 4.91 s unpruned). r7rs-srfi-plan S1-S7.

- **More of `define-library`.** `(export (rename internal public))`,
  exported `syntax-rules` macros, and `define-record-type` as an internal
  definition.

- **The interpreter takes `-I` and finds its enclosing spice.** `tur
  interpret` / `tur debug` collect `-I dir` before the file, `tur eval
  --file` takes it anywhere, and the enclosing `build.tur`'s `src/` and
  `:spices` deps are appended, so a multi-module program outside one
  directory can be interpreted.

### Changed

- **Scheme source is Scheme.** Under `#lang r7rs`, brackets read as
  parentheses, a leading `:` is an ordinary identifier rather than a keyword,
  and Turmeric forms are refused. A program's own definition is its own,
  whatever it shadows. R7RS's `append` is variadic, per R7RS 6.4.

- **Diagnostics print parametric types in their source spelling** rather than
  the elaborated carrier.

### Fixed

- **Windows.** The JIT's whole-preamble path links and runs (and `#lang r7rs`
  with it), `tur repl --engine jit` loads in-process, `call/cc` is
  re-entrant, an over-capacity async httpd server sends its 503 intact, and
  two saffron fixture crashes are gone.

- **`tur mcp` / `tur lsp` crashed after a few dozen analyses** on a stale CPS
  cache.

- **Types carried through generics and instances.** A `let` bound to a
  generic call's `A` result is typed `A`, not the carrier int; a dispatched
  method's applied class-variable result is typed per instance, and an
  instance impl's carrier result is settled at one chokepoint; an imported
  dynamic file's unannotated `defn` forward-declares as `any`; a float tyvar
  reaching an `fn`-typed callback in a generic closure, and list helpers over
  a `(Cons any)` head, both take the typed path.

- **Ownership and effects.** A lambda-captured `^mut` is shared rather than
  copied on the compiled path, a `let`-bound `any` from an aliasing call is
  no longer dropped at scope end, a panic inside a CPS-lowered Saffron
  function reaches `catch-unwind`, an async body driving a session endpoint
  runs on its own thread, and a partial application checks a captured
  session/role endpoint's protocol.

- **Regions and the collector.** An erased node handed to an inline-C callee
  is noted for regions, `extern-c` refuses a region node, and the r7rs
  collector no longer parks around its own locks or deadlocks on macOS around
  `pthread_create`.

- **`stdlib/capability.tur` compiles again** -- file-scope vtables and typed
  handles. A self tail call is lowered as a backedge rather than a C sibling
  call, and a closure is inlined as a pap only when its body forwards exactly.

## [0.55.0] -- 2026-09-25

### Changed

- **The lattice classes declare their superclasses.** In
  `stdlib/typeclass-lattice.tur`, `Monoid` is now declared over `Semigroup`,
  `BoundedJoin` over `JoinSemilattice`, and `BoundedMeet` over
  `MeetSemilattice`. A `[^Monoid A]` function may call `combine` without also
  writing `^Semigroup A`, and `mconcat`, `mconcat-from`, `law-identity?`,
  `law-bottom-identity?` and `law-top-identity?` now carry the single
  constraint. **Breaking for downstream instances:** a `Monoid`,
  `BoundedJoin` or `BoundedMeet` instance now needs the superclass instance
  for the same type somewhere in the program, or the build stops with
  TUR-E0393. Every instance the stdlib ships already has one. Existing
  two-constraint signatures such as `[^Semigroup A ^Monoid A]` still compile.
  `JoinSemilattice` is deliberately not declared over `Semigroup`: the two
  share a shape but not a meaning, and a join is not spelled `combine`. The
  auto-loaded classes (`Eq`, `Ord`, `Functor`, `Monad` and the rest) stay
  flat for now. SC8a of typeclass-superclasses-plan.

- **`Ord` is declared over `Eq`.** The auto-loaded `Ord` carries the
  preamble `[(Eq a)]`, so a `[^Ord A]` function may call `eq?` without also
  writing `^Eq A`. **Breaking for downstream instances:** an `Ord` instance now
  needs an `Eq` instance for the same type somewhere in the program, or the
  build stops with TUR-E0393. Every stdlib `Ord` instance already has one, and
  no spice declares an `Ord` instance. A program that re-declares `Ord` itself
  must now spell the same preamble, or it is "typeclass 'Ord' is already
  defined". SC8b step 1 of typeclass-superclasses-plan.

- **`Alternative`, `MonadError` and `Traversable` declare their
  superclasses.** The auto-loaded `Alternative` is declared over
  `Applicative` and `MonadError` over `Monad`; `Traversable` in
  `stdlib/typeclass.tur` over `Functor` and `Foldable`. So
  `[^Alternative F]` licenses `pure`, `[^MonadError M]` licenses `bind`, and
  `[^Traversable T]` licenses `fmap` and `foldl`. The same instance obligation
  applies (TUR-E0393); every stdlib instance already satisfies it and no spice
  declares an instance of these classes. SC8b step 2 of
  typeclass-superclasses-plan.

- **`Applicative` is declared over `Functor`.** An `[^Applicative F]` function
  may call `fmap`. **Breaking for downstream instances:** an `Applicative`
  instance now needs a `Functor` instance for the same type (TUR-E0393).
  Every stdlib instance has one and no spice declares an `Applicative`
  instance; six test fixtures that declared `Applicative` for a toy type
  gained a one-line `Functor`. A constrained rank-2 `forall` implies its
  constraints' superclasses the same way a `defn` does, so a function passed
  to it still lines up dictionary for dictionary. SC8b step 3 of
  typeclass-superclasses-plan.

- **The arrow classes declare their superclasses.** In `stdlib/arrow.tur`,
  `Arrow` is declared over `Category`; `ArrowChoice`, `ArrowLoop` and
  `ArrowApply` over `Arrow`; `ArrowZero` over `Category`; and `ArrowPlus` over
  `ArrowZero`. `ArrowZero` departs from Haskell's `Arrow` superclass because
  `Kleisli` is a `Category` with an honest zero arrow and no `Arrow` instance.
  Every stdlib instance satisfies the new obligations. SC8b step 4 of
  typeclass-superclasses-plan.

- **`Monad` is declared over `Applicative`.** With `Applicative` over
  `Functor`, a `[^Monad M]` function may call `pure` and `fmap` -- the
  Haskell `Applicative m => Monad m` shape, and what makes a `do-m` block
  ending in `pure` generic over any monad. **Breaking for downstream
  instances:** a `Monad` instance now needs `Applicative` (and so `Functor`)
  instances for the same type (TUR-E0393). Every stdlib `Monad` has them, now
  that `Result` is an `Applicative`; no spice declares a `Monad` instance; one
  test fixture's toy monad gained both. SC8b step 5, the last step of
  typeclass-superclasses-plan's stdlib adoption.

- **Saffron: every container grounds to all-`any`.** A `#map{}` literal
  widens its keys as well as its values, and `[]` consults the top-level
  form's dialect, so every map a Saffron file builds is `(Map any any)`, as
  vectors and sets already were. An immutable borrow whose parameter's type
  variable is bound to exactly `any` borrows a widened copy, so `map-assoc`
  and `map-get` pass their `(& K)` key check; and only a container sibling
  pins a seam's instantiation, so `(map-assoc (mk) "k" 42)`,
  `(vec-push! v "x")` and `(unwrap-or o 0)` on an `any` no longer panic
  compiled. **Behavior change:** an annotated `(Map Sym any)` parameter no
  longer accepts a `#map{}` literal, the same way `(Vec int)` never accepted
  `[1 2 3]` -- build one with `(:: (map-new) (Map Sym any))`. A borrowed
  generic parameter's mismatch now prints what its variable is bound to
  (`expected &:Sym, got &cstr`, not `&?`). Resolves
  saffron-open-generic-result-not-grounded; M10 of saffron-lang-plan.

### Added

- **`tur` reports its own crashes on Windows.** An access violation or other
  fatal exception in `tur.exe` used to end the process with nothing on
  stderr. It now prints `tur: fatal exception 0x... at ... (tur.exe+0x...)`,
  and `addr2line -e tur.exe` resolves it against the same build once the
  image base (`objdump -p tur.exe`, usually `0x140000000`) is added to the
  offset. The exit status is unchanged.

- **`Result` is an `Applicative`.** `stdlib/result.tur` ships
  `Applicative [(Result _ B)]`: `pure` is `ok`, and `ap` applies an `ok`
  function to an `ok` argument and returns the first `err` it meets, the
  function's before the argument's. `(ap ff fa)` on a
  `(Result (fn [int] int) int)` has the type `(Result int int)`, so its result
  can go straight to a typed parameter.

- **`#lang r7rs`: threads run under the collector, in parallel** (stages A
  and B of docs/archive/r7rs-gc-threads-plan.md). A compiled Scheme
  program that starts a thread -- through `stdlib/thread`, a session, a
  task group, the multi-threaded scheduler, or a Turmeric module's own
  `pthread_create` -- no longer stops with exit 70. The r7rs-gc collector
  registers every thread; allocation is a per-thread cache of slots
  refilled from the shared free lists under a heap lock; a collection stops
  every other thread by signal wherever it is (Boehm's design, `SIGPWR` /
  `SIGXCPU` on Linux, `SIGXCPU` / `SIGXFSZ` on macOS) or leaves it where it
  parked itself in a blocking call, and scans every thread's stack,
  registers, thread-local runtime state (thread-local again under the
  collector; each thread registers its instances through
  `tur_rt_tls_roots`), allocation cache and region generations. A
  collection on a fiber's stack scans the right memory. The blocking calls
  the unit spells are release points that also retry an EINTR the stop
  signal caused. Gate: `tests/run-r7rs-gc.sh` section 3
  (threads-run/share/roots/tls/parallel/pause/syscall/lint) and
  `tests/fixtures/r7rs-threads-*`.

- **Saffron: a constrained typeclass instance dispatches at `any`** (S9 of
  saffron-lang-plan). `(definstance Eq [Vec] [(Eq A)] ...)` discharges its
  constraint at the element type, and every container a Saffron file builds
  holds `any`, so there was nothing to discharge it with -- and each route
  failed differently: compiled, the registry shim called the carrier base
  impl, so every vector answered as the elaborator's `int` representative (a
  silent wrong answer); interpreted, a boxed collection was named by its int
  handle and a dynamically entered constrained instance bound no
  dictionaries; statically, the instance was dropped as unsatisfied and
  `(.eq? [7.25 1] [7.25 1])` reported TUR-E0020 "receiver type is erased"
  over 21 candidates. A dynamic file now mints an `[any]` instance whose
  methods dispatch on the box tag, a constrained parametric head gets a
  witness at `(Head any..)`, and a carrier-word element bridges to the
  minted method's box parameter. Fixtures `saffron-eq-vec-any`,
  `saffron-dyn-constrained-instance`.

### Fixed

- **`tur mcp` and `tur lsp` no longer crash after a few dozen requests.** Both
  compile the file again on every request, in one process. The CPS emitter
  cached its classification keyed on the addresses of the program and the
  emitter context, both of which are freed between compiles. Once a later
  compile got the same two addresses back, the server emitted the old
  program's leftovers and died with an access violation. On Windows that
  happened 25 to 40 requests in, and on CI as early as the fifth. The cache
  is now cleared around every compilation.

- **The JIT's whole-preamble path works on Windows.** When `tur jit` cannot use
  its split runtime, it compiles the whole preamble instead. On Windows that
  path could not link, so every such program, including every `#lang r7rs`
  program, silently fell back to `cc`. It now runs in the engine. Variadic
  functions defined in a program's own inline C also run in the engine there;
  they used to fall back too. Scheme programs that now run in the engine keep
  what `cc` gave them: re-entrant `call/cc` works there, and bignum arithmetic
  gives the same answers.

- **An over-capacity async `httpd` server sends its 503 intact on Windows.**
  `httpd-new-async-with-limit` answered a connection past its cap with a 503
  and closed the socket without reading the request. Closing over unread data
  resets the connection, and a Windows client that had not yet read the 503
  lost it to the reset: `recv` failed with `WSAECONNRESET` and no bytes
  arrived. The server now closes such a connection gracefully: it sends the
  503, signals end-of-stream, and discards what the client sends until the
  client closes (at most 2 s). The graceful close lives in the reactor as
  `tur_reactor_linger_close`, which never blocks the event loop. This was
  also why `httpd-async-limit` hung on two-core Windows CI runners; that
  fixture runs on Windows again.

- **`#lang r7rs`: `call/cc` is re-entrant on Windows.** A continuation
  can now be invoked after its `call/cc` has returned there too, so
  generators and coroutines written with `call/cc` work; before, Windows
  stopped at the first re-entry with "continuation invoked after its
  call/cc prompt returned". The runtime reads the stack base from the
  thread's TEB, and jumps into a copied stack with GCC's
  `__builtin_setjmp`/`__builtin_longjmp`, which unwind nothing, where
  Windows' `longjmp` would unwind through frames it has just overwritten.
  Both the compiled program and `tur --interpret` are covered.

- **`tur repl --engine jit` loads a spice in-process on Windows.** The
  in-process build maps module names to source files through a shadow
  directory, and made each entry with `symlink()`, which is an `ENOSYS` stub
  on Windows. Every load there printed `symlink ... Function not
  implemented` and quietly used the `tur build --shared` subprocess path
  instead. On Windows the entries are now hard links, or copies where a hard
  link cannot reach; POSIX still uses symlinks.

- **A `none` returned by a constrained generic no longer crashes at a typed
  `Option` parameter.** A higher-kinded generic returns the carrier, and the
  call site converts it back to the by-value `(Option int)`. That conversion
  dereferenced the carrier unconditionally, and `none` rides it as 0, so
  `(show (add-one (:: (none) (Option int))))` segfaulted compiled while an
  inline `match` and `--interpret` were fine. For a sum whose tag-0
  constructor is nullary, the conversion now answers the tag-0 value for a 0
  carrier -- the reading `match` already gives.

- **A generic that forwards a continuation to `bind` no longer crashes.**
  `(defn chain [^Monad M] [m : (M int) k : (fn [int] (M int))] : (M int)
  (bind m k))` segfaulted compiled when `k` returned a by-value `Option` or
  `Result`: the monad's `bind` reads the continuation's result as a boxed
  carrier, and got the aggregate in registers. The call site now boxes such a
  result in the continuation's calling shim, for a plain function and for a
  capturing closure.

- **A `do-m` with two or more bindings compiles inside a constrained
  generic.** The second `bind` runs inside the first continuation, on a
  captured `(M int)`. It was not recognized as a dispatch on the constrained
  variable, so it kept a fixed instance, and the captured value was stored as
  an aggregate into a carrier slot, a C type error. The continuation now
  captures the Monad dictionary, and the capture is boxed.

- **A constrained generic can call another constrained generic.**
  `(defn add-two [^Monad M ^Applicative M] [m : (M int)] : (M int)
  (add-one (add-one m)))` failed to compile or link. The inner call now goes
  through the callee's dictionary-passing version with the caller's
  dictionaries, and its result converts back to the caller's by-value type in
  an argument, a `let`, an `if` arm, the caller's own result and direct
  recursion.

- **A `bool` closure called through a generic instance no longer reads as
  true for false.** `(fmap (:: (ok 3) (Result int cstr)) (fn [x : int] :
  bool (> x 10)))` answered `ok true` compiled. The instance calls the
  function through the 64-bit carrier and read the whole return register, of
  which a `bool` defines one byte. Every closure entry point such a caller can
  reach now returns a narrow integer result (`bool`, `int8` to `int32` and
  the unsigned widths) widened to 64 bits: in `fmap` and `ap`, for a lambda,
  a capturing closure, a top-level function or a function stored in a
  `Result`. The emitted C for a `bool` closure changes shape, and 155
  snapshots were regenerated for the stdlib comparator every program carries.

- **Two pointer-to-integer mismatches in emitted C are gone.** A capturing
  closure stored in a user `defdata` whose field is a type variable
  (`(Right (fn ...))` in an `(Either (fn [int] int) int)`) was handed to the
  constructor's 64-bit slot uncast, and a generic over `Category` passed its
  64-bit argument to the function arrow's instance, which takes closure
  handles. Both programs ran correctly, but the C carried a
  `-Wint-conversion` warning, which GCC 14 and macOS clang treat as an error.
  A `[^Arrow A]` generic calling `comp` is now covered by a test.

- **A dictionary-passing generic's result reaches a typed parameter of a
  user type.** `(show-t (or-default (Tally 7 2) 5))` with
  `show-t [t : (Tally int)]` was a C type error, because the conversion back
  from the carrier covered `Option` and `Result` but not a single-constructor
  user type. Inside such a generic, one dispatched method's result fed to
  another (a map into a fold) is no longer re-spilled as an aggregate.

- **A dictionary-passing generic no longer returns a dangling stack
  address.** A by-value argument to a method dispatched through a runtime
  dictionary was spilled to the generic's stack, and a method that returns its
  argument (an `Alternative`-style `myalt` keeping `x`, a `bind` passing
  `none` through) handed that address back out of the generic. It happened to
  read correctly on x86-64 and printed garbage under the arm64 JIT. The
  argument is now heap-allocated.

- **A subclass constraint now carries its superclasses' dictionaries.** A
  higher-kinded generic constrained only by a subclass could not call a
  return-directed superclass method such as `pure` under `[^Alternative F]`.
  Compiled, the generic received only the subclass's dictionary and called
  the method through the wrong one (a C type error, or with matching slot
  types a wrong method); the interpreter found no dictionary at all. A
  declared constraint now implies its superclass closure, as if written out.

- **Compiled `ap` over a partially applied instance head no longer
  segfaults.** An instance over a head such as `(Result _ B)` or a user
  `(Either _ E)` typed its own body with the arms swapped, so in `ap` the
  function was typed as the fixed arm. The natural body did not type-check
  ("'f' is not a function"), and the ascription that worked around it called
  a fat closure as a plain C function pointer, crashing the compiled program
  while `--interpret` printed the right answer. The instance body now puts the
  applied type in the hole slot, and the call site grounds `ap`'s result from
  the function inside the receiver. A related binding that ran inside
  constrained generics, and paired a partial head's fixed variable with the
  function, is now limited to statically resolved calls.

- **`tur jit`: threaded programs no longer hang or crash under load.**
  Generating a function lazily ends in MIR rewriting its call thunk in place,
  and a thread executing that thunk at the same moment could jump anywhere.
  A program's first `pthread_create` now generates every function still
  pending while the program is single-threaded; a program that never starts
  a thread stays fully lazy. Five thread-locals that were shared between
  threads under the JIT now have per-thread host slots: the
  escape-continuation registry, and the r7rs prelude's two `call/cc` stack
  bases.

- **`#lang r7rs` threads: the collected heap under contention** (stages C
  and D of docs/archive/r7rs-gc-threads-plan.md, which is now complete and
  archived). In a compiled Scheme program under the r7rs-gc collector:
  - A detached thread (a future's timeout, a task group's, `thread-detach`)
    leaves the collector's registry once it is gone. Before, its record,
    result and key values stayed for the life of the process.
  - A child forked while another thread allocates no longer deadlocks at
    its first allocation. The collector's locks are taken around `fork`.
  - A value kept with `pthread_setspecific` is a root. The `^thread-local`
    block and a spawned thread's conveyed dynamic bindings live there. The
    block went at the first collection, even in a one-thread program.
  - A large object can no longer be freed in the instant between its
    allocation and its return.
  - Threads that cross the collection threshold together run one
    collection, not one each.
  - The region walker cannot miss a slab a stopped thread was adding
    (src/runtime/arena.c).

  Gate: `tests/run-r7rs-gc.sh` gains `threads-stress` (eight threads assoc
  and dissoc Scheme values in one shared persistent map while a ninth
  churns, every value checked) and `threads-lifecycle`. Both also run under
  ASan in `tests/run-r7rs-sanitize.sh`, at a collection every 31
  allocations.

- **The keyword `:seed` compiles.** The symbol table's seeder shared the C
  name `__tur_sym_seed` with the keyword's interned record, so every unit
  with the runtime symbol registry (every `#lang r7rs` program) that spelled
  `:seed` failed at the C compile. The seeder is now `__tur_symtab_seed`.
  Fixture `r7rs-keyword-seed`.

- **`(__TUR_RET__)` names the type an inline-C function returns.** A
  non-generic inline-C function with a `:heap` result, such as
  `(Map int int)` or `(Vec int)`, is declared `int64_t`, but `__TUR_RET__`
  expanded to the typed pointer. The documented
  `return (__TUR_RET__)(intptr_t)v;` drew a -Wint-conversion. Fixture
  `inline-c-tur-ret-heap-result`.

- **`tur jit`: a child forked while another thread generates code no longer
  hangs.** The engine's lazy-generation lock is now taken around `fork`
  (src/jit_engine.c).

- **A class-method call on an `A` receiver is typed as `A`, not the
  representative instance's `int`.** Inside a constrained generic such a call
  binds to a carrier representative (the first `int` instance) and is
  re-targeted per spec at emit, but the call's *type* was still read from the
  representative, so a `let` took the `int`:
  `(defn two [^N A] [x : A] : A (let [y (n x x)] (n y x)))` printed `-4` for
  `(two -4.25)`, the double having been stored into an `int64_t`. When the
  class declares both the receiver and the result as its own variable, the
  call is now typed with the receiver's `A`, as the return-directed path
  already did. **Behavior change:** `(println (n x x))` in such a generic
  used to compile and print `0` for `0.5`; it is now the TUR-E0006 that
  `(println x)` on an `A` value has always been. The generic-*function* twin
  is split out, not fixed, as
  let-bound-generic-call-result-in-generic-truncates.

- **A constrained generic's float result no longer reads as `-nan` through
  another generic.** A generic whose tail is a class-method call, called at
  float, with its result passed by value into another generic: the spec's
  tail elaborated against the class's first-declared (`int`) instance, and
  the emitter re-targets the call but the return ladder read the elaborated
  `int`, so `-4.25` printed `-nan`. The return spelling now reads the
  re-resolved instance's declared result. Found by the nightly
  type-confusion fuzzer at four seeds -- which had filed nothing, because
  its reporting step requested a `fuzz` label that never existed and all
  four findings died with "could not add label"; the step now creates the
  label and falls back to an unlabelled issue.

- **`list-dir` no longer overflows its array when a directory grows
  mid-listing.** `stdlib/io`'s `list-dir` counted a directory's entries,
  rewound, and filled an array sized by that count with no bound on the
  fill, so an entry created between the two passes wrote past the allocation
  -- which the fixture suite hit intermittently as "corrupted size vs.
  prev_size", parallel fixtures churning `/tmp` while `io-stdlib-roundtrip`
  listed it. It reads the directory once into a growable array now: under
  ASan, 3000 listings of a directory another process fills and empties
  report a heap-buffer-overflow WRITE with the old code and nothing with the
  new.

- **The boxing shim's local no longer collides with MinGW's `__in`.**
  `__tur_fatshim_boxres_fat_*` named a local `void *__in`, and MinGW's
  headers define `__in` as an empty SAL annotation, so the line preprocessed
  to `void * = ...` and `hkt-generic-calls-generic` and
  `hkt-generic-forwarded-continuation` failed to compile in the Windows
  suite and the split-runtime jobs. Renamed to `__tur_inner`.

- **`tur jit`: an `any` field is read without a cast to its own struct
  type.** The `Eq [Cons]` / `[Tuple2]` / `[Pair]` specs at the all-`any`
  instantiation read their `any` elements as `(tur_tagged_t)(x).e1`, and a
  cast to the slot's own struct type is a gcc/clang extension ISO C forbids,
  so c2mir rejected the emitted C ("conversion to non-scalar type
  requested") and `saffron-eq-vec-any` passed only via the `cc` fallback.
  Two field-read spellings drop the no-op cast for a `tur_tagged_t` slot,
  and the C `[any]` argument bridge consults a note of the exact text so it
  still recognizes an already-boxed read rather than dereferencing it as a
  carrier word.

## [0.54.0] -- 2026-09-25

### Changed

- **`defclass` superclass preambles graduate.** The constraint preamble
  `(defclass Monoid [a] [(Semigroup a)] ...)`, the entailment it licenses (a
  `[^Monoid A]` body may call `combine`) and the instance obligation that
  makes it sound are unconditional -- `--enable=class-superclasses` is no
  longer needed and is accepted as the TUR-W0063 graduated no-op. Elaboration
  only: no codegen change and no snapshot moved. The fifteen
  `class-superclass-*` fixtures lose their `flags` file and run ungated on
  both back ends; `errors/class-superclass-gate-off` is deleted with the gate
  and `errors/class-superclass-empty-preamble` takes its place, pinning the
  TUR-E0390 branch that rejects `[]` rather than reading it as "declares no
  superclasses". No bisection hatch: the preamble did not parse at all before
  graduation, so there is no old path to cover and `g_opt_class_superclasses`
  retires with the row. The stdlib's own classes stay flat -- retrofitting a
  preamble obliges every existing instance, in-tree and downstream, which is a
  separate audit (typeclass-superclasses-plan SC8). SC7 of
  typeclass-superclasses-plan.

## [0.53.0] -- 2026-09-25

### Added

- **`#lang r7rs`: `.scm` files.** A `.scm` file is Scheme without the
  `#lang r7rs` line (r7rs-lang-plan open question 4, decided): the
  extension selects the Scheme reader and the Scheme language at every site
  that pairs an extension with the directive -- the entry file, `(import
  ...)`, `(load ...)`, `tur --interpret` -- and a module name resolves to
  `<name>.tur`, then `<name>.scm`, so a `define-library` in a `.scm` file is
  importable by its name. `tur run`, `tur build` (default output name),
  `tur check` and `tur --interpret` take `.scm` entries; a `#lang` line in
  one is a redundant hint. `tests/run-r7rs-import.sh` covers a `.scm`
  program importing a `.scm` library on both back ends.

### Changed

- **`#lang r7rs`: the collector is on by default (r7rs-gc graduated).** A
  compiled single-unit `#lang r7rs` program on Linux or macOS now allocates
  everything through the conservative mark-sweep collector that was behind
  `--enable=r7rs-gc` (a no-op now, on the GRADUATED list): a Scheme
  program's data is reclaimed, as are its `call/cc` images, the records a
  caught `raise` abandons and the prelude's scratch -- a loop building a
  dead four-element list a million times peaks at 10 MB (from 429 MB),
  100,000 escaping `call/cc`s at 10 MB (from 527 MB), 200,000 caught raises
  at 30 MB (from 266 MB). The TUs of `libturt_runtime.a` (the HAMT behind
  `stdlib/map`, rc<T> and its cycle collector, owned strings, symbols)
  allocate through a new hook, `src/runtime/rt_alloc.h` -- libc unless
  something installs another -- so a Scheme value kept only in a Turmeric
  map is scanned through the node that holds it instead of freed under it
  (it segfaulted under `TUR_GC_TORTURE=1`; fixture `r7rs-gc-seam`); the same
  files compiled beside a program by a stdlib autolink marker carry
  `rt_alloc.c` on the marker, and the link driver keeps a repeated bare `.c`
  source once. On macOS the roots are the main image's writable segments and
  `pthread_get_stackaddr_np`'s stack base. `TUR_R7RS_GC=0`, or
  `--no-r7rs-gc` on `tur build`, builds a program without it; a program that
  starts a thread under the collector (any `pthread_create` in the unit)
  stops at the start site with the reason and that opt-out (exit 70).
  `--shared`, a project build, `tur jit`, the interpreter and every other
  platform are unchanged; the trail (`stdlib/trail`) stays on libc, its
  arrays being rooted in `__thread` storage. Every `#lang r7rs` fixture of
  the ordinary suite now runs under the collector, and
  `tests/run-r7rs-gc.sh` keeps the torture, seam, thread and reclamation
  gates -- it runs on macOS too, its address-space check staying Linux-only.
  Archived: r7rs-heap-data-never-reclaimed,
  r7rs-caught-raise-leaks-runtime-records, r7rs-remaining-scratch-leaks and
  the plan (docs/archive/r7rs-gc-plan.md); r7rs-callcc-memory-never-freed
  stays open for the interpreter.
- **Every `cc` over emitted C runs with `-Wno-misleading-indentation`.**
  GCC's check is quadratic on the long brace-less `if` chains the Scheme
  lowering emits and was 71% of a `#lang r7rs` build's C compile: a
  one-line Scheme program built in 6.4 s and builds in 3.1 s. The driver
  appends the flag after the user's `TUR_CC_FLAGS` (`TUR_EMITTED_C_CC_FLAGS`,
  src/main.c), so a harness's own `-Wall` still gets it
  (docs/archive/r7rs-programs-compile-slowly.md).
- **The R7RS prelude's `-lp__` loops are folded back** into their `: nil`
  originals (28 in stdlib/r7rs/prelude.tur and read.tur), now that a `: nil`
  self tail call lowers to a loop; the wrappers are gone and no caller
  changed. A million-element `string-fill!`, `write`, `read` and `read-line`
  pass at `-O1` (archived: r7rs-prelude-value-returning-loop-workaround).

### Fixed

- **Top-level `def` initializers run in source order, compiled.** A
  top-level `def` whose initializer has an effect used to run in
  `__tur_static_init`, before every top-level expression; the interpreter
  ran the forms in order. With no user `main` the initializer is now a
  statement of the synthesized `int main()` at its source position, and the
  synthesized-main fold steps aside for a `def` after a statement whose
  initializer is a call. A `#lang r7rs` program with imports (a module,
  whose `def` initializers all precede its body) declares a `define` after
  the first expression unset and assigns it in the body. Fixtures
  `toplevel-def-init-order` (both back ends) and `r7rs-toplevel-order`;
  archived: toplevel-def-initializers-run-before-toplevel-expressions.
- **`#lang r7rs`: a procedure body may name a top-level variable defined
  after it** (R7RS 5.3.1): `(define (f) (* y 2))` before `(define y 21)`
  was "unbound symbol 'y'" on both back ends. The lowering marks such a
  define `^mut : any`, and the elaborator's Pass 1 pre-declares every
  top-level `(def ^mut name : any init)` ahead of the bodies, the way it
  pre-declares a `defn`, with the def filling the binding in when reached;
  the initializer still runs in source order. Programs and library bodies,
  both back ends; a `(def ^mut x : any ...)` in any dialect gets the same
  pre-declaration. Fixture `r7rs-forward-reference`; archived:
  r7rs-procedure-body-forward-reference.
- **`#lang r7rs`: `map` and `for-each` take up to eight sequences**, the
  shim arity, where a fifth was a runtime error; `vector-map`,
  `vector-for-each`, `string-map` and `string-for-each` share the walker
  and the new cap. The arms are spelled inline like the first four, so the
  million-element `(map + a b)` still runs at `-O2` (a helper call per
  element overflowed the stack, r7rs-lang-plan T8). Five- and
  eight-sequence cases in `tests/fixtures/r7rs-base-library`; archived:
  r7rs-map-for-each-at-most-four-sequences.
- **`#lang r7rs`: each top-level form runs under its own prompt.** A
  continuation captured in a top-level form was the rest of the program
  (the stack image reached `main`'s frame, or the interpreter's loop over
  the forms), so re-entering it from a later form re-ran the forms after
  it. The lowering now wraps every statement of a program in
  `r7rs-toplevel__`, whose frame bounds a capture and, in the interpreter,
  the drive snapshot; a re-entry finishes the captured form and continues
  after the invoking one, as chibi and Racket do. Fixture
  `r7rs-toplevel-reentry`; `r7rs-continuation-after-return` pins the
  delimited answer. Archived: r7rs-toplevel-reentry-reruns-forms. The
  prompt's frame (`r7k_run_form`) is reached through a volatile function
  pointer, so no compiler folds it into `main` (clang and MIR did, which
  re-ran later forms), and it runs the form between a `setjmp` and a
  `longjmp` back into itself so the registers a re-entry's restored frames
  hand back are its own; a capture declines without a stack base, so
  Windows keeps its escape-only `call/cc`.

### Docs

- **The R7RS guide's "Where it differs from R7RS" is re-measured** against a
  v0.52.0 `tur` on both back ends. Six bullets held; two were imprecise --
  `apply`'s eight-argument cap is both back ends, while the same cap on a
  call through a procedure variable is the compiled back end only, and the
  optimization-level bullet named the wrong mechanism and the wrong
  threshold (it is a non-tail call through a procedure variable, which is
  CPS; `-O1` is gcc's threshold, where Apple clang 21 keeps the sibling call
  and loses it at `-O0`). Four differences were missing and were filed as
  reports, two of them fixed in this release. Generated guides also
  syntax-highlight Scheme (`tools/genguides.py`).

## [0.52.0] -- 2026-09-25

### Added

- **`*argv0*` -- the running program's own name.** A new pre-declared global
  (`:cstr`) carrying `argv[0]` of a compiled binary, and the script path under
  `--interpret`; every emitted `main` sets it. `*args*` keeps its meaning (the
  arguments after the program name). `#lang r7rs`'s `(command-line)` conses
  `*argv0*` in place of the constant `"tur"`.

- **`#lang r7rs`: `include` and `include-ci`.** Each named file is read with the
  Scheme reader, relative to the including file's directory, and its forms are
  spliced where the `include` stood: at top level (ahead of the whole-program
  scans, so an included `define` or `set!` is seen like one written in place), in
  expression position, and as a `define-library` declaration. The file is
  registered with the diagnostic registry, so an error inside it names it.
  `include-ci` reads with `#!fold-case` in force.

- **`#lang r7rs`: `except`, and import sets nested in any order.** The import
  lowering folds a set of any nesting -- `only`, `except`, `prefix`, `rename` --
  into one spec, unwinding each name to the library's spelling through the
  modifiers inside it. Over a `(scheme ...)` library an excluded name stops
  meaning the library's, so `(except (scheme base) assoc)` lets the program
  define its own `assoc`. Over a user library or Turmeric module, `only` is
  `:refer`, `prefix` is `:as`, `rename` is a refer plus a read-time rename, and
  `except` is a full import.

- **`#lang r7rs`: an experimental collector (`--enable=r7rs-gc`).** A
  conservative mark-sweep collector (`src/runtime/r7gc.c`) for compiled
  `#lang r7rs` programs on Linux/glibc. Under the flag the emitter pastes it
  into the program and routes that translation unit's `malloc` family and
  the region allocator's fallback through it; roots are the stack, the data
  segment and live region generations. A loop building a million dead lists
  drops from 429 MB to 10 MB, 100,000 escaping `call/cc`s from 527 MB to
  10 MB, and both run faster; programs with a large live set pay 1.2x-1.8x.
  Every Scheme fixture and chibi's suite pass with it, including with a
  collection on every allocation (`TUR_GC_TORTURE=1`). New
  `tests/run-r7rs-gc.sh` (ctest `tur_r7rs_gc`) and fixture `r7rs-gc-basic`.
  Limits: single-threaded, single translation unit, the interpreter is
  unchanged, and Scheme values held in Turmeric maps or `rc<T>` cells are
  not seen. Plan: docs/upcoming/r7rs-gc-plan.md. A `call/cc` image is now
  malloc'd in the capture body rather than hoisted C (both builds).

- **`#lang r7rs`: memory audit (r7rs-lang-plan T8).** Every Scheme fixture
  was run under ASan, UBSan and LeakSanitizer on both back ends, and the
  prelude was stressed with million-element inputs.
  - **Stack.** `append`, `list-copy`, `list`, `map` (one to four lists),
    `string-map`, `vector-map`, `string->list`, `vector->list`, `equal?`,
    `read-line`, `read` and `write` handle a million elements at the default
    `-O2` and interpreted; several overflowed the C stack before.
  - **`equal?` is linear** (union-find), where two equal 10^5-element lists
    took half a minute, and a cycle through vectors now terminates.
  - **Scratch leaks fixed.** The printer's number spellings (3.2 MB in one
    fixture), `quotient`'s per-call message, the bignum core's temporaries,
    the re-encoding of mutable strings, `utf8->string`'s per-character
    appends, and the rest-argument lists `display` and friends built.
  - **Regions.** A `call/cc` captured inside a Turmeric `with-region`
    bracket kept pointers into memory the bracket then rewound, a silent
    wrong answer. The capture now notes its stack image
    (`region-escape-via-callcc`).
  - **`TUR_REGIONS=0`.** A variadic dynamic call (any dialect) called an
    undeclared region allocator on that arm and segfaulted; it uses
    `malloc` there now.
  - **Gate.** New `tests/run-r7rs-sanitize.sh`, ctest `tur_r7rs_sanitize`:
    every Scheme fixture compiled with ASan and UBSan and run.
  - **Known and filed.** A Scheme program's data is never freed (no
    collector); a caught `raise` leaks about 1 KB; a `: nil` self tail call,
    and a CPS one below `-O2`, is not a loop (the prelude works around the
    first). No output of any fixture changes.

- **R7RS conformance: settled tests (r7rs-lang-plan T7).** The conformance
  runner now reports `P passed, S settled, F failed`. A settled test fails on
  a difference kept on purpose, where R7RS allows both answers and chibi's
  test accepts only its own. Today there are two: `#lang r7rs` writes
  `1.7976931348623157e308` where chibi's tests want `e+308`, and both
  spellings are R7RS and read back the same. The runner checks each settled
  test's reason at the end of every run, and a settled test that starts
  passing fails the run. Chibi's suite: 1223 passed, 2 settled, 0 failed on
  both back ends. No Scheme program's output changes.

- **`#lang r7rs`: complex numbers (r7rs-lang-plan T6).** `3+4i`, `-i`,
  `1/2+3/4i`, `1.5+2i` and polar `1@0.5` are numbers, on both back ends,
  in source, `read` and `string->number`.
  - **The value.** The parts are any reals, and a complex number is exact or
    inexact as a whole. An exact-zero imaginary part leaves the real, so
    `(* +i +i)` is -1; an inexact one stays, so `(real? 1.0+0.0i)` is #f.
  - **The tower.** `+ - * /` and `=` work part by part, exactly on exact
    parts. `<` and the other orderings on a non-real are #f. `eqv?`,
    `zero?`, `nan?`, `finite?`, `infinite?`, `exact` and `inexact` take
    complex arguments; `number?` and `complex?` include them, `real?` and
    `rational?` do not.
  - **Functions.** `sqrt`, `exp`, `log`, `expt`, `sin`, `cos`, `tan`,
    `asin`, `acos` and `atan` leave the reals when they have to, and
    `(scheme complex)` has `real-part`, `imag-part`, `magnitude`, `angle`,
    `make-rectangular` and `make-polar`.
  - **Writing.** `write` and `number->string` spell a number as chibi does:
    `+2i`, `1-i`, `0.0+1.0i`.
  - **Visible changes.** `(sqrt -4)` is `+2i`, where it was `+nan.0`. The
    log of a negative number, and `asin`/`acos` outside [-1, 1], are complex,
    where they were NaN. A non-real literal compiles, where it was refused.
    The inexact functions return any number, not only a float.

  Turmeric has no complex type, and `math.tur`'s `sqrt` of a negative stays
  NaN. Chibi's suite: 1223 passing invocations on both back ends, up from
  1152; the 2 failures left are float spellings (T7). Fixtures:
  `r7rs-complex`, and `r7rs-number-syntax` regenerated;
  `errors/r7rs-reader-complex` is gone.

- **`#lang r7rs`: re-entrant `call/cc` (r7rs-lang-plan T5).** A continuation
  can be invoked after its `call/cc` has returned, any number of times, on
  both back ends. Generators, coroutines and same-fringe written with
  `call/cc` work.
  - **Winding.** Invoking a continuation travels the `dynamic-wind` stack:
    `after` thunks out, `before` thunks back in.
  - **How it works.** A continuation is a copy of the C stack, taken at the
    `call/cc` and copied back on re-entry, plus the runtime state that tracks
    the stack. `guard` and `raise` keep the old one-shot escape, which copies
    nothing.
  - **Visible change: every `set!` variable is now a heap cell.** Before,
    only variables that a closure captures were. A re-entry must see the
    latest value, not the value a stack copy saved.
  - **Visible change: re-entry after return no longer errors.** Invoking a
    continuation after its `call/cc` has returned used to be the named error
    "continuation invoked after its call/cc prompt returned". At top level a
    continuation is the rest of the program.
  - **DK runtime.** The runtime gains a `tur_dk_pinned` flag. Only the R7RS
    `call/cc` sets it, and once set, DK frames are never freed. 154 codegen
    snapshots change by those lines.
  - **Interpreter.** The driver's heap work stacks and per-call temporaries
    survive a re-entry.
  - **ASan.** A sanitized `tur`, and a sanitized compiled Scheme program,
    default `detect_stack_use_after_return=0`. ASan's fake stack is
    invisible to a stack copy. `ASAN_OPTIONS` still overrides the default.

  Turmeric's `call/cc`, `call/cc*`, `reset`/`shift` and cloneable
  continuations are unchanged. Chibi's suite: 1152 of 1216 on both back
  ends, up from 1151. Fixtures: `r7rs-continuations`, and a rewritten
  `r7rs-continuation-after-return`.

- **`#lang r7rs`: `eval`, with the interpreter linked in on demand
  (r7rs-lang-plan T4).** `(scheme eval)`, `(scheme repl)`, `(scheme load)`
  and `(scheme r5rs)` are no longer refused. They give `eval`,
  `environment`, `interaction-environment`, `null-environment`,
  `scheme-report-environment` and `load`.
  - **Linking.** Importing one of the four links the interpreter (libturi)
    into a compiled program, through an autolink marker in
    `stdlib/r7rs/eval.tur`. A program that imports none of them links
    nothing extra (`tests/check-r7rs-eval-link.sh`, ctest
    `tur_r7rs_eval_link`).
  - **The embedded session.** Evaluated code runs in one embedded R7RS
    session per run (`src/turi/r7rs_embed.c`), and `tur --interpret` uses
    the same one through native twins, so both back ends agree.
    Definitions evaluated in `(interaction-environment)` persist for later
    `eval`s.
  - **Crossing values.** Data crosses by copy, as `write` text. Procedures
    cross as handles in both directions: an evaluated procedure is callable
    from the program, and a program procedure is callable from evaluated
    code. A raise crosses both ways, so a `guard` on either side catches it.
  - **Finding the stdlib.** A built program finds the stdlib it was built
    against without `TUR_STDLIB_DIR`. The token `@TUR_STDLIB_ROOT@` in an
    autolink marker is resolved to that root.
  - **In-tree builds.** `tur run` / `tur build` of a program that links
    `-lturi` find the archive in an in-tree build (`<build>/src/libturi.a`)
    without `TUR_CC_FLAGS`.
  - **Two envs in one process.** The builtin operator table and the
    diagnostic file registry are process-global. Every crossing between the
    program's interpreter and the embedded one swaps them, as the macro env
    already does.

  Chibi's suite: 1151 of 1216 on both back ends, up from 1147. Fixture:
  `r7rs-eval`, and `docs-r7rs-guide-examples` gains the guide's `eval`
  example.

- **`#lang r7rs`: mutable, character-indexed strings (r7rs-lang-plan T3).**
  A Scheme string is a sequence of characters. `string-length`,
  `string-ref`, `substring` and every other string procedure count
  characters, not bytes, so `(string-length "\x3BB;")` is 1.
  - A string a procedure makes is mutable, and `string-set!`, `string-fill!`
    and `string-copy!` (overlap-safe) work on it. This covers
    `make-string`, `string`, `string-copy`, `substring`, `string-append`,
    `list->string` and the like.
  - A literal is an immutable Turmeric `cstr` (R7RS allows this), and
    mutating one is a named error.
  - **Visible change:** `string-length` of non-ASCII text used to count
    bytes.
  - A Scheme string crosses into a Turmeric `cstr` as a fresh UTF-8 copy,
    through the prelude's `r7rs-str__`. The same holds for a Turmeric module
    `cast`ing a Scheme library's string result to `cstr`. Both are handled
    in `elab_any_unbox_to`, only when the Scheme prelude is in the program.
  - Turmeric's `cstr` is unchanged.

  Chibi's suite: 1147 of 1216 on both back ends, up from 1134. Fixtures:
  `r7rs-strings`, `r7rs-string-literal-immutable`, and the
  `strings-cross-the-seam` case of `tests/run-r7rs-import.sh`. The
  `errors/r7rs-string-mutation` refusal is removed.
- **`#lang r7rs`: exact rationals (r7rs-lang-plan T2).** An exact
  non-integer is a ratio in lowest terms, and its numerator and denominator
  are int64 or bignum.
  - **Visible change:** `(/ 7 2)` is now 7/2. It used to be the inexact 3.5.
    A quotient that divides is still an integer (`(/ 6 2)` is 3).
  - Ratios work through the whole tower:
    - exact arithmetic and comparison, with a double compared by its exact
      value;
    - `floor`/`ceiling`/`truncate`/`round`, which give exact integers
      (`round` ties to even);
    - `numerator`/`denominator`;
    - `exact` of any finite double, which gives its exact value
      (`(exact .5)` is 1/2);
    - a correctly rounded `inexact`;
    - `(expt 2 -10)` is 1/1024 and `(sqrt 4/9)` is 2/3;
    - an exact `rationalize`;
    - `number->string` and `write` in `n/d` form.
  - Literals (`1/2`, `#x11/2`), `read` and `string->number` read ratios
    through the shared parser. `#e` reads a decimal exactly: `#e1.2` is 6/5,
    and `#e1e30` is exactly 10^30.
  - `(features)` lists `ratios`.
  - A ratio passed to a Turmeric `int` or `float` parameter is a checked
    cast error.

  Turmeric's and Saffron's `/` are unchanged. Chibi's suite: 1134 of 1216 on
  both back ends, up from 1103. Fixture: `r7rs-rationals`. The reader error
  fixtures for `1/2` and `#e1.5` are removed.
- **`#lang r7rs`: bignums (r7rs-lang-plan T1).** Exact integers are
  unbounded. An exact integer is an int64 while it fits. Arithmetic that
  leaves int64 continues as a bignum (`R7rsBig`), and a result that fits is an
  int again, so the int64 path stays the fast one.
  - Bignums work through the whole tower:
    - literals, `read`, `string->number`, and `number->string` in any radix;
    - `quotient`/`remainder`/`modulo` and `floor/`;
    - `/`, `gcd`/`lcm`, `expt`, `exact-integer-sqrt`, `sqrt`;
    - `exact` of any integral double;
    - `eqv?`/`equal?` and `write`.
  - A comparison with a double is exact:
    `(= (- (expt 2 1000) 1) (inexact (expt 2 1000)))` is `#f`.
  - The arithmetic is one C file, `src/compiler/r7rs_bignum.inc`, shared by
    both back ends. The interpreter includes it, and
    `tools/gen-r7rs-inc.py` copies it into `stdlib/r7rs/bignum.tur`. ctest
    `tur_r7rs_inc_sync` replaces `tur_r7rs_numsyntax_sync` and checks both
    copies.
  - **Visible change:** `(* 3037000500 3037000500)`, `(expt 2 64)` and the
    like used to panic (D8). They now answer.
  - A bignum passed where an int64 is required (a Turmeric `int` parameter, a
    vector index) is the checked cast error `cast: any holds R7rsBig, not
    int`. It is never truncated.

  Turmeric's and Saffron's `int` are unchanged. Chibi's suite: 1103 of 1216
  on both back ends, up from 1096. Fixtures: `r7rs-bignums`,
  `r7rs-bignum-int-seam`. They replace `r7rs-exact-overflow`.
- **`#lang r7rs`: one number parser, and two wrong answers fixed (r7rs-lang-plan
  T0).** `src/compiler/r7rs_numsyntax.inc` parses the whole R7RS number syntax
  for the source reader, `read` and `string->number` on both back ends. The
  compiled back end's copy is `stdlib/r7rs/numsyntax.tur`, written by
  `tools/gen-r7rs-inc.py`; `tur_r7rs_inc_sync` keeps the two equal.
  - `1/2` and `3+4i` are one token each. They used to split into `1` and the
    symbol `/2`, or into `3`, `+4` and `i`, and the error named the wrong thing.
  - A ratio or complex number the tower holds reads as its value: `10/2` is 5,
    `#i3/2` is 1.5, and `3+0i` is 3.
  - One it cannot hold yet is refused with the reason and the plan task that
    brings it: complex numbers (T6), and rationals until T2 (above). A source
    literal gets a compile-time error. `read` and `string->number` raise an
    error `guard` can catch.
  - `(exact 1e30)` used to answer 9223372036854775807; it is exact now (see
    bignums above).

  Chibi's suite: 1096 of 1216 on both back ends, up from 1082.
  Fixtures: `r7rs-number-syntax`, `errors/r7rs-reader-complex`.
- **`#lang r7rs`: referential transparency.** A `syntax-rules` template's
  free identifiers now mean what they meant where the macro was defined. The
  lowering tracks lexical scope, gives local binders unique names, and resolves
  each template identifier in the macro's definition scope. So
  `(let ((list vector)) (my-list 1))` is `(1)`, and a local variable shadows a
  keyword or macro of its name. The R4 named failing test
  `r7rs-syntax-rules-referential-transparency` now passes and its
  `expected.xfail` is gone. Diagnostics on Scheme files show source names.
  Chibi's suite: 1082 of 1216 on both back ends.
  `tests/fixtures/r7rs-syntax-rules-hygiene`.
- **`#lang r7rs`: Unicode `(scheme char)`, and 1077 conformance tests.**
  Char case mapping and classification, `digit-value`, and full string case
  mapping (`"\xDF;"` upcases to "SS") now cover Unicode. The tables come from
  `tools/gen-r7rs-unicode.py`, which writes the same C into the prelude's
  `stdlib/r7rs/unicode.tur` and the interpreter's
  `src/turi/r7rs_unicode.inc`; `tur_r7rs_unicode_sync` keeps them equal.
  Also fixed:
  - `'nil`, `'true` and `'false` are symbols.
  - An exact integer and a double compare exactly.
  - `#;` before a lone `.` is a read error.
  - The interpreter ignores a top-level C block instead of failing the load.

  The chibi count is 1077 of 1216 on both back ends, up from 1036, and is the
  ctest floor.
- **`#lang r7rs` conformance (R10).** chibi-scheme's R7RS test suite
  (vendored under `tests/r7rs/` with its BSD licence) runs as the ctest target
  `tur_r7rs_conformance`, which reports a pass count with a regression floor:
  1036 of 1216 tests pass on both back ends. The first run passed 887 on the
  interpreter, and the compiled program did not build. The suite found, and
  R10 fixes:
  - `set!` of a variable a lambda captures was lost on the compiled back end
    (a `do` loop summing into an outer variable answered 0); the Scheme
    lowering now boxes such variables.
  - A binder named `return` (or any Turmeric special form) was that form.
  - A program could not define a name an auto-loaded stdlib module defines
    (`list-length`).
  - Vector literals evaluated their elements.
  - Quasiquote ignored an unquote under a quote and the long forms.
  - `syntax-rules` treated a literal `_` as the wildcard and a literal
    ellipsis as the ellipsis.
  - Internal defines were not `letrec*`.
  - Numeric fixes: inexact integer division; inexact `numerator` and
    `denominator`; case-insensitive `+nan.0`/`+inf.0` and the R5RS exponent
    markers.
  - Catchable `apply` errors, `write`'s bars for number-like symbols, and
    `make-bytevector`'s optional fill.

  Outside the dialect:
  - A typed parameter naming a record, forward-declared as `int`, was cast to
    int from `any`.
  - The interpreter's lifted lambdas could not see an enclosing `letrec`.
  - A global `def` holding a closure was read in C before its declaration.
  - `+inf.0` was emitted as the C identifier `inf`.
  - The closure-env registry overflowed a `uint8_t` and crashed `tur` at 256
    environments.
  - `emit-c` spent quadratic time resolving callees; a table now makes the
    suite's four-minute build take 42 seconds.

  The one finding left open: a compiled closure still copies a captured
  `^mut` in typed Turmeric and Saffron, so the two back ends disagree there
  ([compiled-closure-copies-a-captured-mut](docs/reported/compiled-closure-copies-a-captured-mut.md)).
  `tests/fixtures/r7rs-conformance-fixes`, `letrec-lifted-lambda-frame`,
  `global-closure-def-called-in-lambda`; `r7rs-control`'s expected output had
  recorded the `return` bug and is corrected.
- **`#lang r7rs` tooling (R9).** `tur repl --lang r7rs` (and `#lang r7rs` at
  the prompt, which switched the language but kept Turmeric's preload and
  skipped the Scheme renames) with results echoed in Scheme's spelling; `tur
  fmt` re-indents a Scheme file and never rewrites a token (the form printer
  turned `#\x`, `|two words|` and `#t` into a different program), with
  `--stdin --lang r7rs`; `tur init --r7rs` scaffolds a program or, with
  `--lib`, a `define-library`, both of which build and test; the LSP analyses
  and formats Scheme (formatting answered "no edits" for every `#lang`
  document) and the browser LSP and REPL picker learn the dialect; the vim and
  VS Code packs highlight Scheme lexemes in `#lang r7rs` files; `gendocs`
  reads Scheme definitions and library names; and
  `docs/guides/r7rs-guide.md`. Fixed on the way, in project mode (`tur build
  .`): a Scheme program built a shared library instead of a binary, a Scheme
  library was refused over the prelude's own definitions, and per-module C
  emission lacked the forward declarations for globals. `fmt-bootstrap-stdlib`
  is green again. `tests/turi/repl-lang-r7rs.sh`, `tests/run-init-r7rs.sh`,
  `tests/lsp/r7rs-diagnostics.py`, new cases in `run-fmt.sh`,
  `run-editor-syntax.sh`, `check-gendocs-parse.sh` and the two wasm unit
  tests, and `tests/fixtures/docs-r7rs-guide-examples`. Also green again:
  `tur_regions_fuzz_src` (its self-test predated R3's new region-escape case)
  and `tur_leak_check` (`tailcall-dyn-leak` had passed on stale-pointer luck;
  filed as `docs/reported/dynamic-returned-closure-env-is-never-freed.md`).
- **`#lang r7rs` ports and I/O (R8).** String, bytevector and file ports
  over one C buffer (`defopaque` handles, inline C with interpreter twins),
  the whole R7RS I/O surface of `(scheme base)` with UTF-8 characters, and
  the current ports as parameter objects. `write` and `display` label cycles
  (`#0=(1 2 . #0#)`), `write-shared` labels all sharing, `write-simple`
  labels nothing, and `write` escapes strings, names characters and bars
  symbols that would not read back. `(scheme read)` reads the R7RS external
  representation, datum labels and cycles included, and raises a
  `read-error?` object on a malformed datum; `(scheme file)` gains its file
  ports and the `call-with-`/`with-` forms. Fixed on the way: a record field
  typed as a pointer opaque (constructor argument spelling, and `set!` of a
  field of such a record emitted invalid C); a vector was not `eq?` to itself
  under the interpreter; `eqv?` on two records panicked; binding a
  `nil`-returning call in a Scheme `let` or `guard` body was TUR-E0023; a
  module program whose modules use `call/cc` was emitted without the escape
  runtime (the presence scan skipped module bodies), which had left
  `tests/run-r7rs-import.sh` red on its compiled cases since R6.
  Filed: a compiled top-level `def` initializer runs before every top-level
  expression (`docs/reported/toplevel-def-initializers-run-before-toplevel-expressions.md`).
  `tests/fixtures/r7rs-ports`, `r7rs-write-labels`, `r7rs-read`,
  `r7rs-file-ports`.
- **`#lang r7rs` libraries (R7).** Every R7RS-small procedure that is not a
  port. The rest of `(scheme base)` (variadic char/string comparisons,
  `boolean=?`/`symbol=?`, `list-set!`/`make-list`/`make-string`/`string`,
  `[start [end]]` ranges, multi-list `map`/`for-each`, `string-map`,
  `vector-map`/`-for-each`/`-append`/`-copy`/`-copy!`, bytevector copies,
  checked `utf8->string`, `member`/`assoc` with a comparison, `apply` with
  leading arguments, `rationalize`, `features`, `write-simple`), `(scheme
  char)` with ASCII case mapping, the 24 `(scheme cxr)` compositions and
  `(scheme complex)` over the reals live in the prelude. `(scheme time)`,
  `(scheme process-context)` (`exit` runs the outstanding `dynamic-wind`
  afters) and `(scheme file)`'s `file-exists?`/`delete-file` (a failure
  raises a `file-error?` object) are files under `stdlib/r7rs/` spliced in
  only when imported. `(scheme eval)`/`repl`/`load`/`read`, `include` and
  string mutation are refused with the reason; an unknown `(scheme ...)`
  library is an error. Fixed on the way, most of it outside Scheme: a
  static variadic call now narrows an `any` argument into a concrete fixed
  parameter; a typed variadic passed as a value gets an all-`any` adaptor
  that forwards its rest list (it was called through the wrong convention);
  a variadic's forward declaration carries its rest shape and an annotated
  `: any` result forward-declares as `any`, so callers may precede their
  callees; import/load file ids no longer collide with the compiled
  driver's auto-loaded files (a `(load ...)` overwrote one's source record),
  and the source-file registry holds 512 files instead of 64;
  float literals are emitted with the shortest round-trip spelling
  (`3.141592653589793` compiled as a different double); and a Scheme vector
  literal is self-evaluating. `tests/fixtures/r7rs-base-library`,
  `r7rs-system-libraries`, `float-literal-round-trip`,
  `saffron-variadic-fixed-arg-narrow`, `saffron-forward-ref-any-and-variadic`,
  and four `errors/r7rs-*` fixtures.
- **`#lang r7rs` control (R6), and the compiled back end catches up.**
  `call/cc`/`call-with-current-continuation` as a one-shot upward escape --
  the receiver's continuation is a procedure, `(k 1 2)` delivers two values,
  invoking it after its call/cc has returned is the named error on both back
  ends (the compiled runtime keeps a live-prompt set instead of reading a
  dead frame's flag) -- `dynamic-wind` with a wind stack that escapes
  unwind, `with-exception-handler`/`raise`/`raise-continuable`/`guard` and
  error objects (`error`, `error-object?`, `-message`, `-irritants`; an
  uncaught raise reports on stderr after flushing stdout and exits 70),
  `parameterize`/`make-parameter` with converters, and
  `delay`/`delay-force`/`force`/`make-promise`/`promise?` in constant space.
  The three compiled dynamic-closure gaps that kept every named `let`, `do`,
  `letrec`, `call-with-values`, `apply` and `for-each` interpreter-only
  since R2 are closed (`docs/archive/r7rs-compiled-dynamic-shapes.md`): the
  letrec placeholder in a dynamic file is `any`, every Scheme lambda returns
  `any`, and a dynamic call packs surplus arguments for a variadic callee --
  a fn type's `any` box id now spells its rest slot, boxed variadics are
  registered with their fixed count, `__tur_dyn_call_var` and the T6
  trampoline build the `(Cons any)` chain, the fat shim types the rest slot
  as the chain pointer, a capturing closure's value type carries the rest
  marker, and the interpreter packs at its dynamic call. Every Scheme
  procedure call is therefore a proper tail call compiled too
  (`tests/fixtures/r7rs-tail-calls`, 1e7 at `-O0`). Fixed on the way: a
  closure that `set!`s a mutable global read it before its declaration in
  the emitted C, a two-clause `case-lambda` lost its second clause's value
  compiled, and a top-level `(define f (lambda ...))` is a `defn`.
  `tests/fixtures/r7rs-control`, `r7rs-uncaught-error`,
  `r7rs-continuation-after-return`, `saffron-letrec-any-closure`,
  `saffron-variadic-dynamic-call`, `saffron-closure-sets-global`,
  `saffron-callcc-stale-prompt`; the four r7rs fixtures that carried
  `requires.interp-only` run on both back ends. Found and filed, not fixed:
  `list-length` on a `(Cons any)` chain
  (`docs/reported/list-length-on-cons-any-segfaults.md`).
- **`#lang r7rs` -- the Scheme base, reader only, behind the `r7rs`
  experiment.** R0 and R1 of
  [r7rs-lang-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/r7rs-lang-plan.md).
  R0 de-Saffronized the dynamic substrate: every "is this file Saffron?"
  test in the elaborator and emitter now asks a per-language trait row
  (`lang_traits`, `lang_span_is_dynamic`, `g_opt_dynamic_any`) instead,
  with no change to any emitted C. R1 adds the ninth `#lang` base: `tur
  dialects` lists `r7rs` as `experimental (r7rs)`, the directive is itself
  the enable (no `--enable=r7rs`; TUR-W0060 prints once per compile), and
  the file is read by a Scheme variant of the reader -- `#t`/`#f`, `#\c`
  with the R7RS names and `#\x<hex>`, `#(...)`, `#u8(...)`, `,`/`,@`,
  dotted pairs, `|sym|`, the `#x`/`#o`/`#b`/`#d`/`#e`/`#i` prefixes, `+5`
  and `.5`, `+inf.0`, the R7RS string escapes and `#!fold-case`. There are
  no Scheme semantics yet: a `#lang r7rs` file elaborates exactly as the
  same forms would under `#lang saffron` (pinned by
  `tests/fixtures/r7rs-elaborates-as-saffron`), and the playground picker
  badges the row rather than hiding it. `r7rs` has no reader axis, so
  `#lang r7rs/sweet` is TUR-E0331.
- **`#lang r7rs` core forms (R2).** `define`, `lambda`, `let`/`let*`/`letrec`/
  `letrec*` and named `let`, `do`, `begin`, `set!`, `if`, `cond` (with `=>`),
  `case`, `and`/`or`, `when`/`unless`, `case-lambda`, `define-values`/
  `let-values`/`let*-values` and internal defines are lowered onto Turmeric's
  own forms before elaboration (`src/compiler/scheme_lower.c`), with Scheme
  truthiness -- only `#f` is false -- as a per-file trait on both back ends.
  A new `stdlib/r7rs/prelude.tur` supplies the core procedures (`car`, `cdr`,
  `cons`, `list`, `length`, `append`, `reverse`, `map`, `for-each`, `eqv?`,
  `equal?`, `display`, `write`, `newline`, `values`, `call-with-values`,
  `apply`, the type and numeric predicates). Two dynamic-substrate gaps this
  surfaced are fixed for Saffron too: `set!` into an `any` cell now widens a
  concrete value instead of rejecting it, and Scheme's `if` accepts a
  statically typed condition. The compiled back end runs the definitions,
  conditionals, closures over mutable locals and list procedures; a
  letrec-bound closure over `any` (named `let`, `do`) and dynamic
  multi-argument apply are interpreter-only until R6
  (`docs/archive/r7rs-compiled-dynamic-shapes.md`).
- **`#lang r7rs` data and the Turmeric seam (R3).** Pairs are a mutable heap
  struct of two `any` fields (`set-car!`/`set-cdr!` are the region-noted
  field store), with the null and eof singletons, chars as an opaque over
  the scalar value, vectors over `(Vec any)`, bytevectors, symbols,
  `define-record-type`, `quote`/`quasiquote`/`unquote-splicing` as runtime
  constructors, `cond-expand`, and `eq?`/`eqv?`/`equal?` with `equal?`
  terminating on a cycle. A rest parameter is a Scheme list. Strings stay
  `cstr` and immutable: `string-set!`/`string-fill!` are declined, not
  aliased. The seam is open in both directions: `(define-library (a b) ...)`
  is `(defmodule a/b ...)`, `(import (turmeric x/y))` is `(import x/y)`
  with `only`/`prefix`/`rename` (`except` is a diagnostic naming `only`),
  `(scheme base)` and its siblings map onto the prelude, and an imported
  `#lang r7rs` module gets the prelude on the import path.
  `tests/run-r7rs-import.sh` (ctest `tur_r7rs_import`) runs both
  directions on both back ends; `tests/fixtures/r7rs-stdlib-seam` is the
  plan's D9 exit criterion (a Scheme program calling the stdlib map);
  `tests/fixtures/r7rs-data-forms` runs every R3 shape compiled and
  interpreted with identical output.
- **`#lang r7rs` `syntax-rules` (R4).** `define-syntax`, `let-syntax`,
  `letrec-syntax` and `syntax-error`, with the full pattern language (`_`,
  literals, `...` at any depth and after a subpattern, elements after an
  ellipsis, improper tails, vector patterns, datum literals, a custom
  ellipsis, the `(... ...)` escape) and renaming hygiene: an identifier a
  template introduces in a binding position is renamed fresh, so the
  standard's own `or`, `let*` and `do` expand correctly and `swap!` cannot
  capture. The expander is part of the Scheme lowering pass, so every
  expansion is lowered on the spot; a macro may expand to a `define` at
  body start. The referential-transparency gap of renaming hygiene (a free
  identifier the use site shadows) is on record as a named failing test,
  `tests/fixtures/r7rs-syntax-rules-referential-transparency`, under a new
  `expected.xfail` fixture marker that both `tests/run.sh` and
  `tests/run-turi.sh` honour: the expected mismatch passes as `(xfail)` and a
  match fails until the marker is deleted. `er-macro-transformer` is
  deferred with a diagnostic.
- **`#lang r7rs` numbers (R5).** R7RS 6.2 over int64 and double: `(+)`,
  `(*)`, `(- x)`, `(/ x)`, n-ary comparison chains, a mixed exact/inexact
  literal pair promotes, `(/ 7 2)` is the inexact 3.5 (no rationals), and
  exact `+`/`-`/`*`/`expt` SIGNAL on overflow instead of wrapping (D8; a
  panic until R6's `raise`/`guard`). The exactness predicates and
  conversions (`exact?`, `inexact?`, `exact-integer?`, `integer?` on 7.0,
  `exact`, `inexact`, `nan?`, `infinite?`, `finite?`), `floor`/`ceiling`/
  `round` (ties to even)/`truncate`, `quotient`/`remainder`/`modulo` and
  `floor/`, `truncate/`, `gcd`/`lcm`, `min`/`max` with inexact contagion,
  `sqrt` (exact for a perfect square), `exact-integer-sqrt`, `expt`, the
  transcendental set, `square`, and `number->string`/`string->number` with
  a radix. A float prints as R7RS spells it on both back ends (`7.0`,
  `1e21`, `+inf.0`). `stdlib/math.tur` gains `tan`, `asin`, `acos`, `atan`,
  `trunc` and `rint`. A bare operator in value position is a variadic
  procedure, but `(apply + xs)` hits the variadic-through-`apply` gap on both
  back ends (`docs/archive/r7rs-compiled-dynamic-shapes.md`).
- **Fixed: a forward-referenced callee with a compound parameter type in a
  Saffron (or R7RS) file unboxed its `any` argument to `int`.** The pass-1
  forward declaration recorded `[v : (Vec any)]` as the `int` placeholder,
  and the dynamic seam took that placeholder at its word: "cast: any holds
  Vec, not int" at runtime, and C passing an int64 to a `tur_adt_Vec__any
  *`, whenever the callee was defined below its caller. Defining the callee
  first avoided it, which made it look like an ordering rule. In a dynamic
  file the forward declaration now carries the full type of a closed
  compound parameter -- and, inside a `defmodule`, a closed compound return
  -- so both orders agree (`tests/fixtures/saffron-fwd-decl-app-param-seam`).
  Typed files keep their placeholders.

- **`^tailcall` -- a checked tail-call annotation.** Whether a call became a
  real tail call was invisible in the source: you either got the backedge or you
  did not, nothing said which, and the failure mode was a stack overflow at an
  unpredictable depth in production. `^tailcall (loop v)` asserts that a call
  MUST be in tail position, and a call the compiler cannot place there is now
  `TUR-E0716` naming the specific reason -- an argument position, a live
  `defer` or owned-local drop, a `match` arm, a different or indirect callee, a
  parameter a backedge cannot reassign, and a dozen more, each with its own
  message and its own line in `tur explain TUR-E0716`. The annotation is a
  prefix, so it fits in a `match` or `handle` arm where an extra list element
  would silently re-pair every clause after it; `(^tailcall (f x))` is the same
  form and is what `tur fmt` writes. It changes nothing about how the call runs
  -- removing it silences the error without making the call a tail call. The
  check runs during C emission (so `tur build`/`run`/`emit-c` perform it and
  `tur check` does not), and `--interpret` never reports it, because the
  tree-walking evaluator already trampolines every tail call. This is T1 of
  [proper-tail-calls-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/proper-tail-calls-plan.md),
  and it is the test instrument the later stages are verified with.
- **A Recently Updated section, under Recently Added, in the guides index and
  Try Turmeric's docs pane.** Recently Added answers "what is new?"; nothing
  answered "what changed under me?" -- a guide rewritten last week looked
  exactly like one untouched since May. Both surfaces now carry a second dated
  list of the ten most recently edited pages: a card beneath the first on
  turmeric-lang.com/docs/html/guides/, and a second `<details>` -- collapsed on
  load, like its neighbour -- beneath the first in the pane's nav. The dates are
  the two ends of each page's git history, read at generation time in a single
  traversal, so the pack now carries `updated` alongside `added`. A page whose
  last commit is the one that added it is left out of the second list: it is
  new, not updated, and the section above already says so.

### Changed

- **A dynamic call carries eight arguments, where it stopped at four or five.**
  One constant now bounds every path: the preamble's fat shims, the `any` widen
  of a bare function, the `^fat` auto-shim, the fat-normalization rule, the
  dynamic call's fixed slots, the tail-call trampoline's descriptor, and the
  R7RS `apply`. A `#lang r7rs` procedure of up to eight parameters is therefore
  a first-class value on the compiled back end -- through `apply`, a variable, a
  parameter, `call-with-values`, a variadic callee, and a 100,000-deep dynamic
  tail call. Past eight, a dynamic call is refused at compile time and `apply`
  panics, each saying to pass the rest as a list.

- **`#lang r7rs`: the Unicode tables come from the UCD at a pinned release.**
  `tools/fetch-ucd.sh` fetches `UnicodeData`, `SpecialCasing`, `CaseFolding` and
  `DerivedCoreProperties` from ICU's copy at one tag (release-76-1, Unicode
  16.0.0), and the generator reads those files instead of the build host's
  Python, so the Unicode version moves only when the tag does. The gaps that
  closes: simple case mapping is `UnicodeData`'s fields 12-13 plus
  `CaseFolding`'s C+S entries, so `(char-upcase #\x1F80)` is `#\x1F88` while
  `#\xDF` stays itself; full mapping is `SpecialCasing`'s unconditional entries;
  `char-alphabetic?` is the Alphabetic property, `Other_Alphabetic` included;
  and `string-downcase` applies Final_Sigma. The language-specific
  `SpecialCasing` entries are not applied.

- **A loop that owns a `ref<T>` or `rc<T>` local is a real loop again.** A
  self tail call under a `let` that owns a value with drop glue -- a `ref<T>`,
  an `rc<T>`, a move-only Drop value, or a by-value ADT with an owning field --
  used to be an ordinary recursive call, because the local's scope-exit drop
  ran after it. When the local is dead at the call (every use copies a plain
  number out of it, like `@b` or `(.count o)`), its drop now runs just before
  the backedge instead, so the loop runs in constant stack: 10,000,000
  iterations at `-O0` where it used to overflow. A local that is still live --
  passed to the call, captured by a closure, borrowed -- keeps the old
  behavior, and `^tailcall` on such a call says why (`TUR-E0716`, "an owned
  local ... is still live at the call"). A `defer` you wrote is unaffected: it
  still runs after the call, in order. This is T4 of
  [proper-tail-calls-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/proper-tail-calls-plan.md).
- **Mutual tail calls run in constant stack.** Functions that tail-call each
  other in a cycle -- `is-even?`/`is-odd?`, a state machine, an 8-way
  dispatcher -- are now fused into one C function with a dispatch loop, so the
  cycle is a `goto` rather than a chain of C calls: 10,000,000 steps at `-O0`
  where it used to overflow. Before, a small cycle survived only at `-O2`, and
  only because the C compiler happened to inline it. Each function keeps its
  own entry point, so calls from outside the cycle are unchanged. A group
  forms when every member is a plain top-level `defn` (no closure, inline-C,
  effectful or `catch-unwind` body; no variadic or `fn`-typed parameter), they
  share a return type, and there are at most 8 of them with 16 parameters in
  all; `^tailcall` on a call that misses says so. This is T5 of
  [proper-tail-calls-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/proper-tail-calls-plan.md).

- **Saffron: a call through a function value in tail position runs in constant
  stack.** `(f f (- n 1))` where `f` is an `any` used to be a nested C call per
  step, and a loop written that way overflowed at around 30,000 steps -- fewer
  at `-O0`. Such a call now bounces: it is recorded and handed back to a
  trampoline loop one frame below, which makes it. A self loop, a lambda, a
  capturing closure and two functions bouncing to each other all run
  10,000,000 deep at `-O0` in about 1.4 MB, and the same calls got 2.7x faster
  (`benchmarks/saffron-dyn-tail-results.md`). Typed code that calls a Saffron
  function as a callback is unaffected: only a trampoline ever asks a
  function to bounce. `^tailcall` can now annotate a dynamic call. This is T6,
  the last stage of
  [proper-tail-calls-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/proper-tail-calls-plan.md),
  and the tail-call prerequisite of `#lang r7rs`.

- **`musttail` where the C compiler can promise it.** A tail call to another
  function that the mutual-group fusion does not reach -- a cycle of more than
  8 functions, or plain forwarding -- is emitted as
  `TUR_MUSTTAIL return f(args);`, which asks the C compiler for a guaranteed
  tail call. Under clang on x86-64/aarch64 that makes such a cycle run in
  constant stack at `-O0`; everywhere else (gcc, the JIT, wasm) the macro is
  empty and the call is what it was. Applied only when both functions have
  identical C signatures and the caller's body takes no address, since a
  forced tail call must not leave an argument pointing into the frame it
  replaces. `-DTUR_MUSTTAIL=` turns it off. Fixtures that assert the deep
  case carry a new `requires.musttail` marker, which `tests/run.sh` probes
  once against `$CC`. This is T2b of
  [proper-tail-calls-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/proper-tail-calls-plan.md).

### Fixed

- **A `: nil` self tail call is a loop.** A void-returning function's tail spine
  was never walked, so a `when` loop in a `: nil` body was an ordinary recursive
  call. Each leaf now ends in a statement plus a bare return (firing open
  drop-glue frames and any scope drops first), and in a void body a one-armed
  `if` is a tail position. `^tailcall` is accepted in a `: nil` body.

- **`@` on an `rc<T>` returned the control block, not the payload.** `EX_DEREF`
  had no `rc` arm, so `(+ @x 1)` printed an address. It reads through to the
  value now, in whichever layout `rc/of` chose, and the `rc`'s ADT definition is
  kept so a `match` or a field read of `@s` resolves. Also: an `-O0` build could
  not link the contract handler, which `libturt_runtime.a` now defines.

- **A generic closure capturing a float truncated it.** `((capture 7.25))`
  printed 7. An unannotated lambda whose body is a value of a named type
  parameter now records that parameter as its result type, as an explicit `: A`
  does, so the call can instantiate it instead of reading the int carrier. The
  per-specialization inner-closure clone also covers a type parameter bound to a
  by-value aggregate, and a specialization body filling a shared environment's
  carrier slot bridges the bits -- a pointer through `intptr_t`, a double or
  float by union reinterpret -- rather than performing a numeric conversion.

- **A plain `fn`-typed parameter's shape is checked, as a `^fat` one's was.** A
  wrong-arity or float-slot function passed to an `(fn [int] int)` parameter was
  accepted and ran, because the parameter is routed onto the typed poly carrier
  and the structural check was gated on the slot's kind. The declared signature
  was already recorded; the call now checks a function argument against it for a
  non-function slot too, under the same carrier-class rule as `^fat`.

- **A non-parametric ADT's forward typedef is no longer emitted twice**, which is
  C11-only and which clang rejects under `-std=c99`.

- **`#lang r7rs`: `letrec*` forward references, and a widened REPL echo.** A
  body's `define` that an earlier definition's initializer mentions --
  `(define (a) (set! b 1))` before `(define b 0)`, or a closure reading a later
  variable -- is hoisted as a mutable cell bound around the whole body and
  assigned in place, so every name a body defines is in scope throughout it
  (R7RS 5.3.2). At the R7RS prompt a top-level expression is passed through an
  identity with an `any` parameter, so a `let` yielding a vector echoes as
  `#(1 2)` rather than a pointer; programs and Turmeric forms typed at the
  prompt are left alone.

- **`#lang r7rs`: `char-ready?` asks the descriptor.** It answers from the
  buffer, a string or bytevector port, a `FILE` at eof, else a zero-timeout
  `poll()` on the descriptor, so an empty pipe or an idle console answers `#f`.
  It used to be `#t` always. Windows keeps `#t` (no `poll` over a `FILE`).

- **`#lang r7rs`: a global spelled like a Turmeric special form.** A program or
  `define-library` body that defines -- or imports by name -- `gen`, `handle`,
  `return` and the like now goes through the lowering's clash table wherever the
  name occurs, so a library and its importer stay in step, and a `set!` on such a
  global is looked up through the rename. Scheme syntax sharing a spelling
  (`set!`, `do`, `let`) is exempt, and a Turmeric form written in a Scheme file
  stays reachable.

- **JIT: `math.tur` and `#lang r7rs` programs run on the engine, not the `cc`
  fallback.** c2mir knew none of `math.tur`'s new `__builtin_*` trig, nor the
  `__builtin_isinf`/`isfinite`/`nan`/`inf` the Scheme runtime uses; the engine
  declares and shims all ten. The prelude's `call/cc` helper spelled its
  thread-local `__thread`, which c2mir cannot parse, so no `#lang r7rs` program
  ever reached the engine. With the engine really running them, two wrong answers
  surfaced and are fixed: c2mir accepts `__builtin_{add,sub,mul}_overflow` and
  answers 0 for every int64 operand, so `(abs INT64_MIN)` and 2^32 * 2^32 never
  promoted to bignums (the overflow tests are spelled out now), and the
  `r7rs-gc` collector scanned the executable's data segment, where a JIT'd
  program's globals are not.

- **A call to a `: float` function defined later in the file is typed
  `float`.** The top-level forward-declaration pass had no `float` arm, so such
  a call was typed `int` and `(if c x (g ...))` with a float `x` failed with a
  spurious `if branches have mismatched types: then=float else=int` -- which
  every float-returning mutual pair hits on one side. Functions inside a
  `defmodule`, and `letrec`, already had it right.

## [0.51.0] -- 2026-09-21

### Added

- **`when`, `unless`, `defer` and eleven other forms take a multi-form body.**
  `when` / `unless` are `[test & body]`; `defer`, `reset`, `cloneable-reset`,
  `serial-reset`, `atomically`, the nine `stdlib/effects.tur` handlers,
  `with-capability` and the two `defimage-*-hook` macros no longer need the
  explicit `(do ...)` the guides used to teach as design. A new
  `elab_implicit_do` elaborates forms `[start..len)` as one block -- one form
  stays itself, two or more become an `EX_DO` -- so capture analysis and the
  reset lowerings still take a single expression, unchanged. The elaborator and
  the prelude are shared, so both back ends and both dialects get it at once.
  The match-guard `when` is a positional token, not a macro head, and is
  untouched.
- **Try Turmeric's docs pane leads with the quickstart and a Recently Added
  section.** The pane used to open on whatever guide sorted first in the pack.
  The quickstart is pinned at the top of the nav and is where the pane opens
  when nothing else says otherwise (an explicit `#doc=`, and wherever you were
  last, both still win). Recently Added lists the ten newest pages, dated, flat
  across guides and API modules, in a `<details>` closed on load that springs
  open when you are reading one of its entries. The dates come from git at pack
  time, so a page cannot forget to carry one -- or keep claiming to be new.

### Fixed

- **`(defer a b)` silently dropped `b`.** `elab_defer` elaborated `items[1]`
  and never read 2..n, so the program compiled clean, exited 0, and ran only
  the first form -- on the compiled path and under `--interpret` alike. Fixed
  at both sites (global/`atexit` and in-scope).
- **`atomically` reported `unknown function or operator 'atomically'` on every
  arity but two.** Its dispatch row gated on `len == 2`, so an arity mistake
  fell out of the special-form table entirely and got a diagnostic about the
  wrong thing. The row is ungated now, and the arity error is pinned by a
  fixture.
- **A guide fence marked `no-manifest-check` swallowed the section under it.**
  `genguides` stripped only the `no-check` marker before handing markdown to
  python-markdown, whose `fenced_code` accepts a single bare word; a fence
  carrying the longer marker was not a fence at all, so everything up to the
  next one -- a heading and the block under it -- rendered inside a paragraph.
  Silently. The stripper matches markers generically now.
- **Two docs-nav escaping bugs.** A guide description containing a `"` closed
  the nav's `title` attribute early and scattered the rest of its own text
  across the tag; the front-matter reader kept YAML quotes, so the five guides
  whose titles contain a colon rendered as `"Introducing Saffron"`.

### Docs

- **Plans for R7RS-small as a `#lang` over the Turmeric runtime, and for proper
  tail calls**, in `docs/upcoming/`.
- The continuations guide and tutorial gain 23 sweet-exp siblings (every
  Turmeric block in both files is paired and parse-checked now), Racket
  quotations are highlighted rather than flat, and three prose blocks stop
  claiming to be Turmeric. The old PWA recovery runbook is removed, and
  `binding-forms-guide.md` stops teaching the `(do ...)` workaround.

## [0.50.0] -- 2026-09-19

### Added

- **A variadic rest parameter can be declared `: any`.** It used to be rejected
  at every call site (`rest arg 0 has wrong type (expected any, got int)`), and
  passing already-`any` arguments hit `aggregate value used where an integer was
  expected` -- a rest list was a cons of two raw `int64_t` words and an `any` is
  a two-word tagged box. The rest list of a `: any` rest is now the stdlib
  `(Cons any)` monomorph, built the way source would build it, so each element
  reads back with its own tag and both back ends produce the same cell they
  already produce for `(list 1 "two" 7.1)`. Compiled and interpreted agree.
- **A `:heap` parametric struct can carry a self-typed field** -- the shape a
  typed cons tail needs, previously rejected at elaboration.
- **A GADT index refines exhaustiveness checking.** The check reads the
  scrutinee's index and drops a constructor whose declared index provably
  differs, so a head over `(Vec (Succ n))` no longer demands a `VNil` arm.
  Constructor applications are still typed bare; the guide and the stdlib
  docstring say what is proven and what remains phantom.
- **Saffron: function-valued globals, multi-argument dynamic methods, and an
  `any`-typed `call/cc` receiver.** A thin function-valued global is declared
  and cast at its real type and forward-declared unconditionally; a `kind-*`
  method taking more than the receiver (or returning the class variable) now
  dispatches on an `any` receiver through the same source-level witness the HKT
  classes use; a `call/cc` receiver annotated `: any` gets a value-typed
  continuation, so `(k v)` widens the value.

### Changed

- **`json/bool` takes `:bool`, and `#json(true)` / `#json(false)` read as real
  booleans** rather than `1` / `0`. The `#json(...)` reader macro builds its
  call in `reader.c`, so it never appeared as source text to grep for.
- **`ref`, `chan` and `atomic` carry parametric payloads, and the httpd handler
  seams are typed.** The stdlib int-stand-in audit's S1/S2 work replaces
  `:int`-erased payload and callback parameters with real types, and the
  elaborator now enforces the fat-callback shape. Error diagnostics on these
  modules name the real types.
- **`defdata` over a stdlib type name is refused instead of rewriting it.**
  `elab_defdata` treats a filled definition (`n_ctors > 0`) as a redefinition
  rather than a forward stub, in a whole-program compile as well as a REPL
  session; a same-compile duplicate reports "already defined by an earlier
  form", and an earlier session turn keeps its reuse path.
- **Five more fixtures compile on the JIT engine instead of falling back to
  `cc`** -- any-field reads no longer emit struct casts, and the
  function-pointer global hoist is typed.

### Fixed

- **`future.tur` and `fiber.tur` note the region stores they never had.** Five
  stores -- `promise-fulfill`, `promise-fail`, `future-of`, `future-error-of`
  and `fiber-yield` -- write a caller's word into memory that outlives a
  `with-region` bracket with no `TUR_REGION_NOTE`, which is a silent
  use-after-rewind on the default build. All five parameters arrive erased as
  `:int`, the case the emitter's body-entry note cannot cover. Verified in
  emitted C, and the accompanying fixture was rewritten to actually exercise the
  hooks: an erasing ascription is itself a hooked site, so the old cases passed
  with the hooks deleted.
- **A mutable function cell is fat, and a local's spine drop respects escaping
  aliases.** A capturing closure stored into a function cell no longer loses its
  environment.
- **A monomorph's typedef name is introduced once,** and a local callee reached
  through `emit_call_name`'s early exits is spelled by its declared name --
  both had produced C that did not compile.
- **An erased colored generic no longer rejects an elemented parameter
  signature.** `mono_sig_ok` admits an erased `int64` carrier application in an
  ABI clone's signature, and the clone lookups stop treating a call whose
  arguments are still abstract under an erased outer as the pinned sibling.
- **`adt_app_is_byvalue_product` is guarded against a self-typed field,** and
  by-value recursive ADT lends are handled.

## [0.49.4] -- 2026-09-18

### Fixed

- **Try Turmeric reopens the documentation pane where you left it.** The pane's
  location rode on the `#doc=` fragment, which drives back/forward within a
  session but does not survive a relaunch -- an installed app cold-starts at the
  manifest's `start_url`, with no fragment -- so closing the app inside a guide
  and reopening it from the home screen landed on the editor, with nothing on
  screen saying where you had been. A `tur.try.docs.v1` entry now holds the open
  page and its scroll offset, banked on `pagehide` and on `visibilitychange` to
  hidden (an installed app is backgrounded, not unloaded), and holds null once
  the pane is closed -- closing the docs is how you say you are done with them,
  and the next launch honours that. A `#doc=` link still wins outright; share
  links and tutorial mode skip the restore without clearing the memory; and a
  saved page the current docs pack no longer carries clears itself rather than
  greeting you with "No page ... in this documentation pack".
- **The Contents icon is centered in its button on phones.** `.btn` sets
  `align-items` and nothing about the main axis, which is invisible while the
  button is sized by its own content -- every desktop use of it. The phone rule
  gives it `min-width: 40px` for a thumb, and the 6px of surplus that leaves
  around a 16px glyph all went to the right of it under the default
  `flex-start`, putting the icon 3px left of center.

## [0.49.3] -- 2026-09-18

### Fixed

- **A session endpoint passed to a parameter declaring a different protocol is
  now rejected** -- the one mismatch session types exist to catch. `type_eq`
  had no `TY_SESSION` case, and the saturated positional argument check
  consulted it only for structs and ADTs, matching everything else on
  `TypeKind` alone; every endpoint has kind `TY_SESSION`, so the protocol was
  never compared no matter what `type_eq` answered. Multi-party `Role`
  endpoints had the identical hole (`Role[Ping, A]` compared equal to
  `Role[Ping, B]`). Both are compared now, and the diagnostic names the full
  types -- `expected Session[Close], got Session[Send[int, Close]]` -- instead
  of `Session[?]`. Anything unrecognised (an inference hole, an unresolved
  tyvar, a type past the comparison's fixed bounds) still compares equal, so
  the fix cannot reject code it does not understand. Two fixtures that had
  swapped their two endpoints and compiled only because of this bug are fixed.
- **An installed Try Turmeric notices and applies a new build.** The app had no
  update path at all -- `register()` on `load`, nothing else. A standalone web
  app relaunched from the app switcher is resumed, not navigated, so on the one
  platform where this matters the update check never ran; that is why closing
  and reopening it could not have helped. It now asks on every foreground, and
  a `controllerchange` on a load that started controlled re-navigates once.
  "Force update" navigates rather than calling `location.reload()`, which
  WebKit can answer out of its own page cache -- the same HTML naming the same
  hashed assets, while the UI said "Updating...". The overflow menu names the
  build actually running, read from the live service-worker cache key rather
  than a compiled-in constant.
- **The black band at the bottom of the installed iOS app is gone.** The
  safe-area tokens were gated on `(display-mode: standalone), (display-mode:
  fullscreen)` but the pinned shell that consumes them on `standalone` alone --
  and iOS reports `fullscreen` for a home-screen app with a black-translucent
  status bar, so the shell block never matched. `#app` kept `height: 100dvh`,
  which resolves to the safe viewport, landing 93 CSS px short of the
  `viewport-fit=cover` viewport and painting the shortfall through as the band.
  Both gates are one `html.pwa` class now, set before first paint, so they
  cannot drift apart again; `site-nav` / `.footer` hiding lived in that same
  dead block and applies too.
- **The service-worker kill-switch works, and can be armed at deploy time.**
  `web/public/sw-kill.js` had never been run: `activate` did its work in an
  `async` listener with no `event.waitUntil()`, so the browser was free to
  terminate the worker before a single cache was deleted; `clients.claim()` was
  not awaited; and already-open pages were left alone, which was the move that
  was already not working for a stuck reader. Reloading them unconditionally
  turned out to be an infinite reload loop while the file is deployed at
  `/sw.js`, now guarded by a `swkill` URL marker. Arming is
  `TUR_SW_KILL=1 npm run deploy`, disarming is an ordinary deploy, so the tree
  is never left in the dangerous state. Procedure:
  `docs/guides/pwa-recovery-runbook.md`.

### Changed

- **A REPL or playground turn resolves only its own refinement crossings.**
  `refine_resolve_call_sites` walked the whole `refine_call_sites` array, which
  under a session is state nothing cleared -- so every turn re-resolved every
  crossing collected by every earlier turn, minting a fresh undischarged
  obligation each time (collection does not deduplicate) and re-emitting
  TUR-W0372 on every later turn for a crossing with no runtime backstop or
  under `--strict-refine`. Measured on the libturi wasm preload, 60 turns each
  adding one crossing: 1830 obligations down to 60; turns 400-439 went 48.5 ms
  to 0.23 ms each. A whole-program compile is untouched.

## [0.49.2] -- 2026-09-17

### Fixed

- **A sweet-exp `$` inside `(...)`, `[...]` or `{...}` is now an error
  (TUR-E0332).** The rest-of-line marker belongs to the indentation layer,
  which a bracket switches off, so the token reached the reader as an ordinary
  symbol named `$`: the enclosing form silently gained an extra element and
  meant something the author never wrote. `(fn [msg : cstr] : unit log/error $
  str("a" msg))` read as three flat elements where one call was intended, with
  no error and no warning. Plain `.tur` files are untouched -- a `$` identifier
  there is still legal.
- **Try Turmeric reserves the iOS safe area in every fixed overlay.** Installed
  as a PWA on a notched iPhone the app is served `viewport-fit=cover`, so the
  top ~59 CSS px of the viewport is drawn over by the clock and the battery
  indicator. Only `#app` reserved that strip, and `position: fixed` resolves
  against the viewport, so every overlay escaped it. The docs pane's entire
  topbar -- close, Contents, search -- sat underneath, leaving a reader who
  opened a doc with no way back. `--safe-*` tokens now carry the insets through
  the overlays and the docs sheet, the mobile exit control is a 44px
  leading-edge "< Back", and the standalone shell is pinned so the page cannot
  scroll the status bar out of reach.

### Changed

- **`tur run docs` is roughly 3x faster.** `tools/genguides.py` went from ~38s
  to ~10s by parallelizing the per-guide `git log --follow` creation-date
  lookups and the markdown rendering stage -- it was running at 47% CPU,
  blocked rather than computing.

### Docs

- **The sweet-exp guides give `fn` and `handle` traditional parens, and use `$`
  in the tutorials.** `handle` and `match` take a flat argument list that pairs
  up two at a time, so indentation splits each clause from the body that
  answers it; neoteric `fn(` reads as a call to a function named `fn`. The
  syntax guide now states TUR-E0332 as current behavior and shows the positive
  half -- the same `$` is correct as soon as it is outside every bracket. The
  rationale guide's session example is corrected to the capitalized `Recv` /
  `Send` / `Close` constructors.

## [0.49.1] -- 2026-09-17

### Changed

- **Try Turmeric's Run button runs the program in the editor.** It used to
  evaluate the buffer into a session that still held the previous run, so
  pressing Run twice asked the session to redefine everything, and a name you
  had deleted still resolved. Run now rewinds the session to the preloaded
  stdlib, silently replays the last successful Run of every other tab in the
  same `#lang` dialect, then runs the buffer. Definitions typed at the prompt
  last until the next Run. Re-elaborating the preload measured 11-24 ms per
  Run in desktop Chrome.
- **A later REPL or playground turn can redefine any `def*` form.** `defn`,
  `defclass`, `defopaque` and `defdata` already accepted this; `def`/`define`,
  `defstruct`, `defeffect`, `definstance`, `defmacro` and `defmacro*` refused
  it. That is why the shipped effects example failed on its second Run with
  `defeffect: 'Ask' is already defined`. A definition from an earlier turn is
  now replaced. Still refused, each with a clear message: a duplicate within a
  single turn, redefining a stdlib type or stdlib instance, and the builtin
  `Unsafe`. Compiled programs never continue a session and are unaffected.

### Fixed

- **A failed turn no longer breaks the rest of the session.** After any error,
  the next turn rebuilt the session as one whole program and labeled your
  earlier turns as auto-loaded stdlib. Re-running a program then failed with
  `'main' is already defined by an auto-loaded stdlib module`, and the session
  never recovered. In Try Turmeric, a single doc-panel lookup was enough to
  trigger this. Now the session is rebuilt by replaying your successful turns
  one at a time. Separately, a failed turn after a `defmacro*` used to abort
  the process with `tur: too many source files`, which in the playground left
  a dead eval worker. That abort is gone.
- **The doc panel, `tur doc` and `(doc ...)` read the docstring table
  directly.** The playground's doc panel answered a lookup by evaluating
  `(doc-lookup "name")` in your session. That function was never defined
  there, so every stdlib lookup failed and each failure broke the session.
  `(doc ...)` was also broken everywhere: it did not type-check. All three now
  share one C reader (`src/turi/docstrings.c`). The interpreter gains
  `doc-lookup` and `doc-print` natives, and the compile-time `symbol-name`
  builtin now accepts a quoted symbol, with a new `string?` builtin alongside
  it.
- **`await` and `gen-unwrap` return values at their declared type.** A float
  printed its raw bits (arm64) or a stack address (x86-64), a string printed
  its pointer address, a bool printed as `1`, and a yielded `7.25` came back
  as `7`. `await` now reads the future at the type its thunk declares, and a
  non-int payload is spawned through a wrapper with the thunk's real C
  signature. `gen-unwrap` is now a core form typed by the generator's element
  type. Also, every generator in the interpreter used to trip UBSan on a
  misaligned allocation; that is fixed too.
- **A typed `fmap` on a mixed-type `(Result int cstr)` or `(Either int cstr)`
  printed the `cstr` value's address.** The result type is now worked out
  correctly for this path. The same fix removes a 32-byte-per-call leak when a
  carrier-bodied instance such as `Functor [(Either E)]` passes its closure to
  a `^fat` helper: the box now lives on the stack when the helper provably
  does not keep it.
- **Four emitted-C errors, found while building the `plot` and `nng` spices.**
  The first two are warnings under gcc but hard errors under macOS
  AppleClang; the last two fail with every C compiler.
  - `(:: (f) :int)` over a function returning a pointer, in tail position of a
    CPS body.
  - A `let` binding an `int` word through `(:: words (Vec int))`.
  - A `let` binding a carrier-returning producer inside a self-tail-recursive
    function. This rejected the obvious way to write a retry/poll loop.
  - `(Result nil E)` and `(Option nil)` emitted a `void` struct field. They
    now work as "success, no payload", the natural return type of a setter.

### Docs

- **Sweet-exp versions of s-expression examples in about 20 more guides.**
  Guides that gain them include binding forms, currying, ownership, effects,
  typeclasses, error handling, GADTs and logic programming. Nine guides also
  gain the front matter (title, category, description) the site index uses,
  and rendered docs show `- [ ]` / `- [x]` task lists as checkboxes.

## [0.49.0] -- 2026-09-16

### Added

- **Typeclass superclasses, behind `--enable=class-superclasses`.** A
  `defclass` may list the classes a constraint on it entails, in a
  constraint vector right after the type-parameter vector -- the same
  `[(Class var)]` spelling `definstance` and `defn` already use:

  ```turmeric
  (defclass Monoid [a]
    [(Semigroup a)]
    (mempty [] : a))

  (defn double-up [^Monoid A] [x : A] : A
    (combine x x))          ;; entailed: no separate ^Semigroup A needed
  ```

  Entailment is transitive and covers return-directed methods. It is sound
  because of the paired obligation: `(definstance Monoid [int] ...)` requires
  a `Semigroup [int]` instance somewhere in the program (TUR-E0393), with a
  parametric instance discharging against its own constraints. The graph must
  be acyclic (TUR-E0392), each superclass must resolve with matching arity
  and kinds (TUR-E0391), and the preamble itself -- gate off, malformed,
  naming a non-parameter, or written after the `|` fundep clause -- is
  TUR-E0390. No codegen change: static dispatch still resolves each call
  from the concrete instantiation, and the interpreter binds the superclass
  dictionaries alongside the subclass's. The stdlib's classes stay flat until
  the experiment graduates. `tur experiments` lists the row (prototype,
  introduced 0.49.0, expires 0.55.0);
  `docs/upcoming/typeclass-superclasses-plan.md` is the plan and
  `typeclass-guide.md` the reference.

- **`#s"..."` is always available.** The owned-String literal --
  `#s"text"` reads as `(string/from-cstr "text")`, where bare `"text"` stays a
  borrowed `cstr` -- needs no `#lang` token and no `#use-reader-macros`. Every
  other `#`-dispatch in the language was already unconditional; this one was
  the outlier. `(load "stdlib/string.tur")` is still required, and still for a
  real reason: the dispatch is read-time, the code it expands into is not.

  A free fix rides along. The native `tur lsp` never ran the layer detection
  at all, so `#s"..."` in a `#lang turmeric stringed` file had always been a
  red squiggle in an editor. It resolves now with no LSP change.
- **`cond` and `case` accept a bare `else` as the fallback clause,** alongside
  the `:else` keyword they have always taken. The two spellings name the same
  clause and mix freely within a file. Bare `else` was previously
  `TUR-E0003 unbound symbol 'else'` in both forms -- `cond` fell through to
  `(if else ...)` and `case` to `(= disc else)` -- so nothing that compiled
  before changes meaning, with one exotic exception: a `cond` whose *test*
  position read a variable named `else` now sees a fallback clause instead.
  `else` is still not a global binding; outside a clause head `(if else 1 2)`
  remains an error. The guides, the `:tutorial quickstart` content, and the
  guestbook and snake examples the tutorials walk you through now teach the
  bare spelling; `syntax-guide.md` and `symbols-guide.md` record that `:else`
  is the older spelling and still accepted.
- **C, C++, and GDScript code blocks are syntax-highlighted in the rendered
  docs,** in the same five-color palette the Turmeric blocks use, on the
  guides site, the spice pages, and Try Turmeric's in-app docs pane (one
  tokenizer in `GUIDE_JS_CORE`, so the three consumers cannot drift). A C block
  next to a highlighted Turmeric block used to read as a rendering failure --
  `sandboxing-guide` and `c-integration-guide` for the C, and
  `godot-resource-loader-guide` for the C++ and GDScript. 68 blocks across the
  guides.
- **An inline-C fence inside a Turmeric block is highlighted as C.** Every `;`
  ending a C statement used to read as a Lisp line comment and grey out the
  rest of the line, so the C bodies in `c-integration-guide`, `ffi-guide` and
  27 other guides rendered mostly in comment grey. 92 spans across 31 guides.
- **A typed `session-spawn` / `session-join` pair in `stdlib/session.tur`,** one
  peer spawn that works on both backends: a pthread over
  `tur_session_thread_wrapper` compiled, a scheduler fiber under `--interpret`.
  It takes a `(fn [] nil)` and hands back an opaque `SessionPeer`. All 25
  pthread session fixtures use it, the eight `-turi` twins whose only
  difference was the spawn are deleted, and the stdlib templates (echo, rpc,
  pubsub) plus delegation-over-a-channel get fixtures of their own. The session
  subset under `run-turi.sh` goes from 29 passed, 25 skipped to 57 passed, 1
  skipped. See
  [the plan](https://github.com/rjungemann/turmeric/blob/main/docs/archive/turi-session-expansion-plan.md).
- **TUR-W0043 warns on a session op inside a compiled `async` body.** Compiled
  `async` runs its body synchronously on the spawning thread while the session
  runtime blocks that thread until the peer arrives, so
  `(async (fn [] (recv r)))` hung before `async` even returned its future, with
  no diagnostic. `elab_async` now warns when the body captures a Session/Role
  endpoint or spells a session op itself, names the endpoint, and points at
  `session-spawn`. A warning rather than a rejection, because the peer may
  legitimately be on another OS thread. Not emitted under `--interpret`, where
  the cooperative rendezvous makes the shape correct.

### Changed

- **TUR-E0330 is reworded.** Same code, same failure, same class; it says what
  is actually wrong now: ``` `#lang` takes a single base dialect; unexpected
  trailing token 'x' in path/to/file.tur ```. All five emitters are updated,
  including the REPL's plain-text line.

- **`stdlib/string-reader.tur` is a comment-only no-op.** A file in the wild
  carrying `#use-reader-macros "stdlib/string-reader.tur"` keeps compiling and
  its reader learns why -- re-registering `#s"` against the strict batch
  registry would otherwise turn a working file into a hard "already
  registered" error. Deleted at age-out.

- **The Try Turmeric dialect picker is a single-column popover.** One
  mutually exclusive base, matching the syntax. A returning visitor holding a
  cached older wasm binary whose registry still carries a `layers` key is
  fine: the key is simply never read.

### Deprecated

- **`#lang turmeric stringed` warns (TUR-W0064) and is ignored**, for one
  minor line, so a file that opted in per-file keeps compiling across the
  boundary. Drop the token -- what it turned on is now always on. At 0.50.0 it
  becomes the same TUR-E0330 any other trailing token gets, and
  `stdlib/string-reader.tur` is deleted.

### Removed

- **The `#lang` layer axis is decommissioned.** `#lang` now takes a single
  base dialect and nothing else:

  ```
  #lang <language>[/<reader>]
  ```

  Gone with it: the `LANG_LAYERS[]` registry, the `LangLayerSet` bitset that
  rode through four compile paths, `SourceFile.lang_layers`,
  `TuriEnv.lang_layers`, `detect_lang_layered`, `GRADUATED_LAYERS[]`, the
  `g_manifest_experiments_scoped` global, the wasm registry's `"layers"` key,
  and the Try Turmeric layer checkboxes. In its whole life the axis held two
  rows and never more than one at a time. A one-off syntax convenience belongs
  in a `#use-reader-macros` file; a semantic gate belongs in `EXPERIMENTS[]`
  behind `--enable=`; an always-on `#`-dispatch belongs in the reader's
  built-ins. See
  [the plan](https://github.com/rjungemann/turmeric/blob/main/docs/archive/lang-layers-decommission-plan.md).

- **`tur lang-layers` is replaced by `tur dialects`.** The listing itself
  stays -- it is what the TUR-E0331 diagnostics point at for the valid bases,
  and the only way to ask a build which dialects it accepts -- but it lists
  one axis now and is named for it. `--json` emits `{"dialects": [...]}`.
  Shell completion no longer offers `lang-layers`.

### Fixed

- **`#s(...)` set literals inside a file that also uses `#s"..."`.** The
  reader's no-exact-match path asked "is there a macro with this NAME?" and
  reported `#s(1 2 3)` as "reader string macro '#s' expects string body". It
  was already broken inside a `#lang turmeric stringed` file -- latent because
  almost nobody turned the layer on -- and making `#s"` unconditional would
  have promoted it to every set literal in the language. A reserved
  `(name, delim)` pair now rewinds to the built-in dispatch before either
  targeted diagnostic can fire.

- **A trailing `#lang` token in a manifest.** `build.tur` / `build.tur.sweet`
  went through a detection path that only wanted the strip, so a token
  rejected in every `.tur` file was silently tolerated in the one file that
  configures the build. It is now the same TUR-E0330.
- **A `---` separator in a guide is spaced evenly.** Nothing styled
  `.guide-content hr`, so the page reset zeroed its margins and left the UA's
  `1px inset gray` border: a 2px grey bar whose spacing came entirely from its
  neighbours -- 32px below a heading, 0px below a paragraph, gluing the rule to
  the entry under it. `docs/guides/bibliography.md`, which separates every
  entry with `---`, showed it worst. The rule now carries its own symmetric
  margin and the site's border color.
- **Session payloads are lowered by class onto the int64 rendezvous word.**
  Floats are bit-reinterpreted rather than truncated (`7.25` arrived as `7`),
  pointers cast through `intptr_t` so a `cstr` or a delegated endpoint no longer
  depends on the host `cc`'s `-Wint-conversion`, and by-value structs and ADTs
  are rejected with TUR-E0212 at elaboration instead of inside `cc`. Covers the
  binary templates and the multi-party router on both backends.
- **Two interpreter fiber defects.** `sleep-async`'s fiber arm returned
  `turi_nil()` instead of the future its own contract promised, so `EX_AWAIT`
  failed its tag check and the rest of the fiber body never ran --
  `native_read_async` and `native_write_async` had the identical shape and are
  fixed with it. And a `recv` timeout inside a fiber never observed its
  deadline: the only thing that could resume the park was the peer's deposit,
  so the deadline re-check was unreachable until the value it was meant to
  preempt arrived. The fiber arm now arms a timer future alongside the
  channel's waiter and disarms the loser.
- **Four CPS backend defects, one of them a silent wrong answer.** A
  bind-position call to a colored mono-template clone took the cps->cps tail
  arm while the join classifier still answered only for the binding, so the
  join was emitted inline behind a dead label and the clone's answer went to
  the caller's continuation unchanged -- `false` where the program said `true`.
  Alongside it: the `perform` continuation gated its tail-call arguments as
  cps->direct unconditionally (it asks the callee now); a colored call inside a
  `match` evicted the CPS backend for a by-value record or flat-sum scrutinee,
  and for a by-value aggregate into a cps->direct callee; and the ABI
  specialization scan had no arm for a `perform` argument or a `resume` value,
  so `(perform (EO (some 5)))` implicit-declared `some`. A per-function
  deferred-drop table closes the orphaned drop that made a colored frame leak.
- **`perform` finishes typechecking its arguments.** 0.48.0 covered the
  primitives; the aggregate and pointer-shaped parameter families are checked
  the same way now -- one ordinary-call verdict per shape, measured and made
  into a table. `Point <- Other` used to compile and read `Other`'s field as
  `.x`; it, `ptr<void> <- "hi"`, `cstr <- nil`, `(Option int) <- (some "x")`
  and the rest are TUR-E0001. A `ptr<void>` parameter still admits a fn value,
  nil and any pointer, as a call does, and an `int` parameter still admits an
  aggregate (the carrier word).
- **Six more reports, each pinned by a fixture.** A class default whose result
  is the class variable was spliced into the instance and elaborated at the
  int64 carrier (a silent wrong answer); the interpreter's dispatch gates never
  fired for a receiver that is itself a class-method call, so it picked the
  first instance; `set-add` hashed against the `Hash[int]` representative
  instead of the element's own instance; a `defopaque` over `:cstr` or `:Sym`
  skipped the pointer bridge; a by-value struct parameter in an `if` arm
  dereferenced; and a `float32` block temp widened to double.

## [0.48.0] -- 2026-09-15

### Added

- **An "Introducing Saffron" tour guide.** A first tour of the dialect in the
  order a newcomer meets it -- Try Turmeric, `println`, flow control, functions,
  ADTs, typeclasses, effects -- with all 23 examples written twice, `#lang
  saffron` and `#lang saffron/sweet`, checked to the same AST by
  `tools/check-guide-pairs.py` and compiled and run by
  `tests/fixtures/docs-introducing-saffron-examples`. `saffron-guide.md` stays
  the reference; the two cross-link, and both are now indexed in
  `docs/guides/README.md`. Writing it turned up the five Saffron defects fixed
  below.

### Changed

- **`perform` typechecks its arguments and its arity, like an ordinary call.**
  Neither side of the effect ABI compared itself against the `defeffect`
  declaration: too few arguments segfaulted (the slot array is sized by the
  declaration, and the handler read the uninitialized tail slots), too many were
  silently dropped, and a handler clause binding the wrong number did the same
  in both directions. All four are now `TUR-E0002`. Argument types are checked
  too, applying exactly the rule an ordinary call applies between two plain
  primitives and staying silent wherever a call has a coercion arm.

  **Migration:** an unannotated lambda parameter takes the `int` carrier
  default, and `perform` used to exempt it -- which made the effect ABI the one
  place in the language where a parameter silently escaped its declared type.
  That exemption is gone. Write the annotation the parameter always wanted,
  `(fn [msg : cstr] ...)`; all seven corpus fixtures fixed that way produce
  byte-identical output, and the error carries a note naming the fix.
- **Every sidebar on the site is sans-serif,** set on the container rather than
  on the links, so the generated API reference, the guides, and the
  hand-written pages read as one component; the tour's rail is centered.

### Fixed

- **The five defects found writing the Saffron tour.** `defeffect` hardcoded
  `int` for an unannotated parameter instead of the dialect's own default, and
  `perform` ran neither the `any` widen nor the checked unbox, so an `any`
  argument reaching a concrete parameter printed an empty line; `.method` on an
  `any` holding a pass-by-pointer ADT emitted C that `cc` rejects; a Saffron
  function that both performed and pushed or read a vector element crossed the
  CPS seam with a `tur_tagged_t` where the vector helpers declare the carrier;
  and a dynamic operator node (`+` on an `any`) was invisible to the effect and
  CPS coloring walks, so an enclosing handler was reported unreachable
  (`TUR-W0033`) and the caller went uncolored.
- **A constructor argument no longer loses its effect row.** Neither walk in
  `src/passes/cps.c` had an arm for `EX_MAKE_STRUCT` or `EX_GET_FIELD`, and each
  was a `switch` whose `default` meant "no children" -- the third filing of that
  one shape. Both walks now fall back to a shared child enumeration, so adding
  an arm is a choice about non-uniform treatment rather than a prerequisite for
  being visited. A constructor call is exempt from the hoist probe (it invokes
  nothing and can never reach a `perform`), which keeps the sized-GADT checks
  reading the `size_index` off the un-wrapped node.
- **`__tur_any_drop` is deep.** It freed the payload box without first releasing
  what the payload itself owned, so everything below the first level of a nested
  value leaked with no function call involved anywhere: `(Cons 1 (Cons 2 (Nil)))`
  leaked one box, and a third level leaked two. `EX_UNION_INJECT` also gained an
  arm in two more ownership walks, a wrapper forwarding a class method now owns
  the payload box it returns, and the return-position `any` widen hoist moved
  into its own entry point rather than sitting in the shared helper, where it
  skipped three stamps and reinstated a per-widen malloc.
- **A generic no longer resolves a callee's type variable out of the caller's
  bindings.** `emit_abi_register_call` matched type bindings by NAME, so stdlib's
  `unwrap` quantifying over `A` inside a generic that also quantifies over `A`
  specialized to the wrong type -- on the argument side, and separately on the
  result side, where a bare-tyvar recovery undid the argument fix. A class's type
  parameter carries one name across every instance, so an instance method's own
  instance had the same collision. Call-site argument types now win. A
  constrained generic whose body tail-calls a return-type-directed class method
  also bridges the carrier return its monomorphized spec declares, and a pinned
  `@TypeName` dispatch keeps an opaque result type instead of rendering `<adt>`.
- **An integer literal past the range is an error, not a silent wrap.**
  `read_number` accumulated the literal straight into an `int64_t`, which wrapped
  silently and was UB besides. It now accumulates the magnitude into a
  `uint64_t`, range-checks it once the type suffix is known, and applies the sign
  in unsigned arithmetic; `0x`/`0b` literals stay exempt, so
  `0xFFFFFFFFFFFFFFFF` still means `-1`.
- **`tell` is guarded against the MinGW/MSVC libc name.** MinGW declares
  `long tell(int)` in `<io.h>` and glibc declares it nowhere, so two fixtures
  naming a function `tell` failed to compile on the MSYS2/UCRT64 leg; the
  `libc_names[]` table that already handled `abs` was simply missing it.

## [0.47.0] -- 2026-09-12

### Added

- **Lattice vocabulary in the stdlib.** A new `stdlib/typeclass-lattice.tur`
  lands `Semigroup` and `Monoid` alongside the `JoinSemilattice` /
  `MeetSemilattice` / `Lattice` join-meet family, and `Ord` gains derived
  `max` / `min`. This is the vocabulary the CRDT spice is built on; the plan
  is in `docs/upcoming/lattice-vocabulary-plan.md`.
- **The source fuzzers run in CI.** A `fuzz.yml` workflow exercises the type,
  refine, regions and Saffron source fuzzers, backed by a checked-in seed
  corpus (`tests/fuzz-seed-corpus.txt`) and a `replay-fuzz-seeds.sh` harness
  that `tests/run.sh` replays too -- so a shape the fuzzer crashed on becomes
  a permanent regression test rather than a one-off log line.

### Changed

- **`defer` unwinds innermost scope first on every exit path.** On a `return`,
  a `throw`, or a caught panic, nested scopes used to fire their defers
  OUTERMOST first (the compiled `tur_frame_fire_chain` walked the frame chain
  backwards, and the interpreter had been made to mirror it), while a normal
  exit already unwound innermost first as scopes ended -- so a function's
  cleanup order depended on how it left, and an outer guard was released
  before the inner resource it guarded. Both back ends now unwind innermost
  first with same-scope LIFO, the order every other language's `defer` /
  `finally` / RAII uses. A program that relied on the old cross-scope order
  on an early exit sees its cleanups run in the opposite order. Pinned by
  `tests/fixtures/defer-early-return` and `defer-tail-scope-order`.
- **A duplicate `definstance` is an error (`TUR-E0025`).** A user instance
  for a `(class, type)` the autoloaded stdlib already covers -- `(definstance
  Eq [int] ...)` -- used to be dropped with the stdlib's definition winning
  (silently, then with a warning). It is now rejected, the conventional
  overlapping-instance rule; the message points at the newtype route, and a
  stdlib file loaded twice stays a silent no-op.
- **`bt-scope` and `with-untrailed` are panic-safe.** Both brackets are now
  `defer`-based, so a panic inside the body that an enclosing `catch-unwind`
  catches undoes the trail level (`bt-scope`) and resumes trailing
  (`with-untrailed`). The "a panic inside `body` skips the undo" caveat is
  gone from the docstrings and the backtrackable-state guide.

### Fixed

- **A constrained generic keeps its instance through every calling shape.**
  A `(defn f [A : Show] ...)` lost its dictionary when it was passed as a
  function value, when it was reached through a relay call, and when the
  interpreter re-entered it; inheritance was also keyed on the type-variable
  NAME rather than the class, so two constraints sharing a tyvar letter
  crossed wires. Instance inheritance now flows through the call, the value,
  and the relay alike, on both back ends.
- **A `definstance` is dispatchable from another module.** An instance
  defined in one module of a spice was invisible to a method call in a
  sibling module; covered by a new `cross-module-instance` spice fixture.
- **A phantom-only parametric ADT gets a real base constructor,** and
  `clone_struct_app_type` no longer segfaults on a `TY_APP` with a null
  fn/arg.
- **`min` and `max` are back in the prelude,** and the Windows `NOMINMAX`
  preamble no longer eats their names.
- **An argument that is already a `tur_poly_fn_t` is not carrier-cast again**
  when forwarded to a later `defn`.
- **A caught panic now propagates through CPS-colored functions.** On the
  DK/CPS path (any function taking a `^fat` thunk, or otherwise
  effect-colored) every per-call-site panic check emitted only a comment, so
  a function whose callee panicked under `catch-unwind` -- or which panicked
  itself -- ran the rest of its body and its whole continuation before the
  catch saw the flag. The interpreter ran none of it.
- **A `defer` in a function whose callee panics under `catch-unwind` now
  fires** on the direct-call path; the panic-signal early return skipped the
  function's open defer frames. This was the report filed against generic
  HOFs; the `[A]` was incidental (the mono twin was on the CPS path).
- **`--interpret` no longer fires a scope's defers before a tail call.** The
  frame-reusing tail call fired the activation's defers as "frame completion"
  before entering the callee, so `(defer (println "d")) (shout)` printed `d`
  first. The reuse is now gated on an empty defer chain, matching the
  compiler's "defers break tail" rule.
- **A declared all-`any` function parameter is usable compiled.**
  `(defn tany [f : (fn [any] any)] : any (f (:: 4.5 any)))` failed in `cc`
  (the CPS backend spelled the parameter as a named callee), and handing an
  `any`-held function to such a parameter panicked at the seam (the cast
  checked the bare fn id where the box carries the fat all-`any` one). Both
  fixed; the Saffron fuzzer's `route_seam_fn` shape is back in its pool.
- **A capturing closure passed through a type parameter instantiated to a
  function type no longer crashes (SIGBUS).** A lambda declared
  `: (fn [int] int)` whose body is a capturing closure now marks that result
  `boxed` like a `defn` already did, so the consumer dispatches through the
  fat thunk protocol instead of calling the env box as code.
- **`--interpret`: `type-of` on an `any` holding an inline-C-built opaque value
  answers the opaque's name** (`Route`), as the compiled path does, instead of
  `adt`; the inline-C result path no longer re-tags an opaque's word as a
  struct pointer, which also removes the last way that path could dereference
  a large integer.
- **Widening a let-bound function to `any` no longer leaks a box per widen.**
  `(let [f (fn ...)] (peek f))` malloc'd a 24-byte shim box that nothing freed,
  unbounded in a loop; the binding is now recorded as an alias of the lifted
  global, and the widen hoists the same static box a direct widen uses.
- **Module-level `def`, `^thread-local` init and `set!` stores bridge the
  int64/pointer carrier duality,** so `(def hub-mutex (:: (mutex-new) :int))`
  no longer emits `int64_t g = <void * temp>;` -- a hard error under GCC >= 14
  and clang >= 21. The `let` binder already bridged it; the four store sites
  now share its helper.

## [0.46.1] -- 2026-09-11

### Changed

- **`tests/run-jit.sh` honours `TUR_TEST_SHARD="i/N"`,** so the JIT corpus can
  be split across N runners the way `tests/run.sh` already could. The parsers
  are deliberately identical, clamping included (`0/3` and `abc/3` both mean
  `1/3`, `4/3` means `3/3`, and `1/0`, `1/1` or a value with no slash mean "not
  sharded"), because CI sets the variable once per job and both harnesses may
  read it. Ordinals advance over the full corpus rather than over admitted
  fixtures, so `TUR_TEST_FILTER` never shifts shard membership; happy and error
  fixtures round-robin on their own counters, so each shard holds within one
  fixture of an equal share of both.

  This is aimed at the slowest leg in the matrix. `macos-latest` hands out both
  3-core and 5-core machines, and measured off the `ci-metrics` branch
  `tur_jit_fixture_tests` runs ~440s median on Linux, ~348s on a 5-core mac and
  ~730s median / ~940s p90 / 1078s max on a 3-core one -- which CI draws about
  97% of the time, and which has grown from a 593s median on 08-26. Throughput
  is linear in cores (the serial suites show the 5-core box is only ~1.2x
  faster per core), so raising `TUR_TEST_JOBS` on a 3-core runner buys nothing
  and only risks the per-fixture timeouts it is capped to avoid. More
  parallelism there means more machines.

  **No CI matrix uses this yet.** A shard axis renames the check
  (`JIT engine (macos-latest) 1/3`), so any branch-protection rule naming the
  current check has to move in the same change -- a gating decision, left to a
  human, and the reason the `test` job kept its own name when it was split.
- **A sharded run's suite timings no longer collide.** `collect-suite-timings.py`
  tags each row with `shard_index`/`shard_total` (null when unsharded, so every
  row written before this stays comparable), and the `/ci` dashboard keys each
  slice as its own series. Without that, N shards publish N rows carrying the
  same `(run_id, suite)` and ~1/N of the duration each, which reads as one
  suite that suddenly got faster and started reporting N times per run. This
  was the stated prerequisite for sharding anything, recorded in `ci.yml`
  against the `test` job; it is now met for both harnesses.
- **`tests/run-jit.sh` gained `TUR_TEST_LIST=1`,** which prints the fixture
  names an invocation would run -- after filter and shard -- and exits without
  running any. It is how `tests/run-shard-partition.sh` asks the harness what
  is in a shard, rather than keeping a second copy of the enumeration that
  would agree with itself forever while drifting from what CI runs.

- **All four release archives now unpack to the same prefix layout** --
  `bin/`, `lib/`, `include/turi/`, `share/turmeric/stdlib/`. `windows-x86_64`
  has always used it; `linux-x86_64`, `linux-aarch64` and `macos-arm64` shipped
  flat (`tur`, the `.a` files and `stdlib/` at the archive root) through
  v0.46.0. That split existed because a flat tree once matched none of
  `locate_runtime_lib`'s probes, so Windows was packaged the only way that
  could compile; `<exe_dir>` has been probed since, making the difference
  vestigial. The prefix layout is the one `tur` resolves natively --
  `<exe_dir>/../lib` and `resolve_stdlib_root` step 3 -- rather than by
  accommodation. **Extract-and-symlink instructions gain a `bin/`:**
  `ln -s ~/.local/turmeric/bin/tur ~/.local/bin/tur`.
- **Archives published before this keep working.** The `<exe_dir>` probe in
  `locate_runtime_lib` stays exactly as it was, and `resolve_stdlib_root`
  checks both `stdlib/` and `share/turmeric/stdlib/` at every level of its
  walk-up, so an older tarball still compiles with a newer `tur`.

- **The compiler runs on a stack sized for its own recursion, and the emitter's
  expression-depth cap is gone.** `EMIT_MAX_EXPR_DEPTH` was 40 and was wrong in
  both directions at once. As a ceiling it did not hold: it was calibrated
  against one host's Debug+ASan stack cliff, and the `emit_value` frame later
  grew a 256-byte region-walk array that moved the cliff *below* 40 -- so
  `tur emit-c` on a deeply nested expression aborted with
  `AddressSanitizer: stack-overflow` instead of printing TUR-E0712. As a floor
  it rejected working code: 40 was checked against the deepest *hand-written*
  nesting in the tree (20), but macro expansion is not hand-written, and a
  12-component `for-each` in the `ecs` spice expanded past it and could not be
  compiled at all.

  Depth follows the input, so `tur` now trampolines its whole driver onto a
  stack sized by `TUR_STACK_MB` (default 256 MiB) rather than rationing depth
  with a constant -- the same thing `jit_engine.c` has always done for a JIT'd
  program's entry stack behind `TUR_JIT_STACK_MB`. A 12-component `for-each`
  compiles and runs; 120-level nesting, 3x the retired cap, is unremarkable.

  Sizing only *emission* was not enough: at ~400 levels the abort moved to
  `elab_call -> elab_form`, which had **no depth guard at all**, so deep
  nesting crashed the compiler with no diagnostic whatsoever. It now carries
  the same backstop the emitter and macro expansion do.

- **TUR-E0712 names the real quantity.** It used to say "expression nesting
  exceeds the emitter's depth limit (40)"; with the cap retired that would have
  been a claim about a number that no longer exists. It now reports the stack
  actually exhausted and the depth reached, and points at `TUR_STACK_MB`.

- **A typeclass method call resolves against the class, not just the instances.**
  Dispatch walked the elaborated instance table for a name match and never
  consulted the `defclass` or the caller's own constraint, so an *unconstrained*
  generic body calling a class method passed `tur check` with rc=0 and then died
  inside `cc` with `passing 'int64_t' to parameter of incompatible type`. It is
  now rejected at check time, naming the class, the type variable, and the
  constraint to add. A check/build divergence is the worst place for this, since
  `check` is what editors and the CI type-check step run.
- **`Sym` is a `defstruct`/`defdata` field type.** `(defstruct S [k : Sym])` was
  `defdata: field has unrecognized type :Sym`, so an interned symbol could not be
  stored in a record at all. A Sym is a pointer-sized scalar carrier like the
  `cstr` beside it in both tables, so neither layer needed a representation, only
  its row -- the second one to stop emitting a ctor that takes `int64_t` while its
  caller passes `const struct __tur_sym *`.
- **`tur build --shared <dir>` honours the project's `:build-opts` link flags.**
  `:link-libs` and `:link-flags` were parsed and then dropped on the `--shared`
  project path; `tur build <file>` had honoured them since the ffi-spices S1 fix,
  but `cmd_build_multi_files` carried a hand-copied twin of the manifest read
  taken before it. Found from the Godot AOT stager on Windows, where PE has no
  `-undefined dynamic_lookup` equivalent and the staged library must resolve
  `godot_*` at link time.

### Fixed

- **A shard could have silently corrupted the JIT cc-fallback ratchet.** Its
  *reclaimed* half compares the fallbacks a run observed against the whole
  baseline, so a shard -- which exercises 1/N of the corpus -- would have
  reported the other N-1/N as reclaimed by the engine. It is now suppressed
  under a shard exactly as it already was under a filter. The half that
  protects, the new-fallback check, still runs per shard and still holds across
  the job: every name is in exactly one shard, so their union checks each name
  exactly once. Regenerating the baseline from a shard is refused outright
  (exit 2) rather than warned about, because the file is rewritten whole -- a
  shard would delete every name it did not run, and nothing about the result
  would look wrong.

- **Try Turmeric's service worker can serve the REPL's own workers on WebKit.**
  Inside a service worker, WebKit -- real Safari included -- rejects
  `fetch(request)` with `TypeError: Load failed` when the request carries
  `destination: 'worker'` and that script is already in the browser's HTTP
  cache, which is exactly a returning visitor. `cacheFirst` now retries such a
  request as a plain URL fetch, which every engine accepts. Chromium was never
  affected, and neither was the built site -- the symptom reproduced only
  against the dev server -- but nothing except a lucky cache hit was keeping it
  away from a real reader.
- **`/eval-worker.js` and `/lsp-worker.js` are precached.** Both are built from
  string literals, so nothing in the shell's markup names them and the install
  step could not find them. An offline visit loaded the page and then could not
  evaluate anything, which made "installing Try Turmeric means having the
  compiler" untrue for the one asset that makes it a REPL.
- **`tvm install` produced a toolchain that could not compile anything.** tvm
  normalized only the binary -- it moved a flat archive's `tur` down into
  `bin/` and left `libturt_runtime.a` at the version root, which matches none
  of `locate_runtime_lib`'s probes once `tur` has moved. `TUR_RT_AUTO` then
  fell back to source mode and wanted `src/runtime/*.c` that no archive ships,
  so `tur run` died with `no such file or directory: .../src/runtime/hamt.c`.
  This was the `release-archive-cannot-compile` failure reintroduced by tvm's
  own restage: the extracted archive worked, the tvm install of it did not.
  `tvm install` now normalizes the whole tree, accepts either archive shape,
  and reads either shape back off disk so versions installed by an older tvm
  are not stranded.
- **`tvm use` could leave `TUR_STDLIB_DIR` pointing at the previous version.**
  It exported only on a hit, and the variable outlives the switch, so a version
  whose stdlib it did not recognize silently compiled against another release's
  -- which `tur` honors, since the directory has a readable `macros.tur`. It
  now unsets on a miss. `tvm run` and `tvm exec` no longer export a path that
  does not exist, which had made every invocation print
  `ignoring TUR_STDLIB_DIR=...`.
- **`tvm install --build` never copied `libturt_runtime.a`,** giving a
  source-built version the same cannot-compile defect from a different
  direction.

- **A deeply nested expression no longer aborts the compiler.** What remains at
  each recursive walk is a backstop on real stack headroom
  (`tur_stack_nearly_exhausted`, `src/compiler/stack_guard.h`), shared by the
  emitter, the elaborator and macro expansion, so an unbounded walk gets a
  diagnostic rather than a sanitizer abort. Measuring the actual resource is
  the only bound that cannot rot the way the constant did -- it is correct at
  any frame size, on any host, under any sanitizer. `errors/expr-nesting-depth-limit`
  (which asserted the retired cap) is replaced by `tests/fixtures/expr-nesting-deep`
  plus `tests/run-compiler-stack-guard.sh` (ctest `tur_compiler_stack_guard`),
  which drives the backstop by shrinking the stack instead of growing the
  program.

- **The Saffron `any` seam marshals where it used to reinterpret.** Eight fixes
  from the dynamic-surface pass: a dynamic `.bind` marshals its continuation
  instead of erasing it; the seam into a typed `fn` parameter marshals rather
  than reinterpreting the word; a concrete value widens into an `any`-valued
  container parameter; `while` applies truthiness to an `any` condition; `=` on
  two strings through an `any` answers `Eq[cstr]`; a dynamic field read reaches
  generic and `:heap` ADTs; an `any` `defstruct` field lowers and a dynamic read
  of one compiles; and a partially-applied HKT instance head dispatches on an
  `any` on both back ends.
- **A leading keyword in a constructor call can be a `Sym` value, not only a
  field name.** A record whose first field is `Sym` is built as
  `(make-struct P :kw)`, and the leading keyword was always read as a field name,
  so the call died on `keyword construction needs :field value pairs`.
  `(Circle :diameter 2.0)` (a typo) and `(make-struct Q :label 9.75)` (a Sym value
  followed by a float) are the same shape syntactically, so the disambiguation is
  by field type rather than by argument count, which leaves the `errors/` fixtures
  pinning the unknown-field diagnostic intact.
- **The `any` -> scalar cast no longer miscompiles in the JIT engine.** A
  statement expression binding a 16-byte `tur_tagged_t` from a call that itself
  takes `tur_tagged_t` arguments reached its callee with the first argument
  replaced by the second -- a two-float function through an `any` answered 3
  where 5 was expected under `tur jit`, and correctly under `tur run`. It is
  shape 4 of `jit-x86-64-struct-valued-statement-expression-miscompiles`, and
  reproduces from a hand-written `(cast (g 3.5 1.5) float)` with no Saffron code
  involved.
- **The JIT's `cc` fallback no longer inherits monomorph state.** `cmd_jit`'s
  step-6 fallback re-enters `cmd_run` in the same process, so the van-Laarhoven
  spec registry survived the abandoned engine attempt and the re-entered compile
  emitted a call to a spec nothing defines
  (`call to undeclared function 'over_px__lens_...'`). `mono_specs_reset()` had
  existed since the registry was added and was never called from anywhere in the
  tree; this is the call site it was written for.
- **`call-ptr` marshals a record field of concrete parametric-record type, and
  says what is actually wrong when it refuses one.** The interpreter's struct
  marshaller refused a `defstruct` field whose type is an application of a
  parametric record (`(Box float)`, `(BoxW int32)`) while the compiled path
  accepted the same program. All four layout walkers now thread the same
  `(def, args)` view, so the signature, the flatten and the rebuild cannot
  disagree.
- **A `Vec` out-of-bounds names `vec`, not `tvec`, on both back ends.** The
  compiled side printed a leftover internal name; it now takes the interpreter's
  spelling at both `stdlib/vec.tur` sites. 148 snapshots regenerate, two lines
  each, with no codegen drift.
- **Try Turmeric's favicon paints its `t` instead of knocking it out.** The glyph
  was a counter in a single even-odd path, so it showed whatever sat behind the
  icon -- right on the site's own near-black ground, but 2.16:1 and effectively
  an empty tile in a light browser tab strip at 16px. Filled at the bg-base
  value, it is 9.17:1 and identical on any backdrop.

## [0.46.0] -- 2026-09-09

### Changed

- **`#lang saffron` is on by default.** The dynamically typed dialect graduated
  out of the experiment registry: no `--enable=`, no `:experiments` entry, and no
  `TUR-W0060` on every compile. It is an ordinary base dialect on the same footing
  as `#lang turmeric`, a project manifest can no longer refuse it, and
  `tur lang-layers` reports all eight bases as `stable`. `tur experiments` now
  lists nothing at all. A lingering `--enable=saffron` is accepted as a TUR-W0063
  no-op, so a `build.tur` that named it keeps compiling.
- **Try Turmeric drops the `experimental` badge** from the four Saffron rows in
  the language picker. The picker renders that badge from the `#lang` registry, so
  it followed from the graduation rather than a separate UI change.

### Fixed

- **`(doc 'map-get)` and the web doc panel answer with the current text.**
  `stdlib/docstrings.tur` had drifted a release behind `stdlib/map.tur`, so the
  `any`-miss paragraph added in v0.45.1 was missing from the generated lookup
  table.

## [0.45.1] -- 2026-09-09

### Added

- **A source-level differential fuzzer for Saffron.** `tests/saffron-fuzz-src.py`
  generates random dynamically typed programs and diffs the interpreter against
  the compiled back end; a ctest smoke target runs a short session. It found most
  of what this release fixes -- including one bug in plain typed Turmeric. The
  refine/regions/type fuzzers now report each case as it finishes rather than
  only at exit.
- **`tur just` -- a synonym for `tur run`.** Same recipe dispatch and the same
  Justfile, with shell completion and the guide covering both spellings.

### Fixed

- **Saffron's dynamic surface, from the first fuzzing pass.** A capturing lambda
  keeps its environment and a `call/cc` receiver keeps its scalar return; an
  unannotated lambda's return defaults to `any` the way a `defn`'s does, as does
  a pass-1 forward declaration; a named typed function is reachable through a
  dynamic call; dynamic operators cover containers and shifts; a nullary
  parametric constructor builds at the `any` instantiation; a dynamic field read
  publishes instance rows for its field types; and elaboration seams see through
  macros and bind a map's key type from its siblings.
- **A `Sym` inside an `any` is a `Sym` on both back ends.** The `any` instances
  gained a `Sym` arm, and the interpreter names a receiver before unboxing it.
  `map-get` on a `(Map K any)` miss answers with the nil box on both back ends
  instead of diverging.
- **A parametric ADT constructor with an underscore in its name emitted invalid
  C.** Two local "non-alnum -> `_`" folds in `src/compiler/types.c` kept `_` raw
  while every access site used the injective mangler (`_` -> `_un`), so the union
  member had two spellings and the C compiler rejected the result with no
  Turmeric diagnostic. Typed Turmeric, not Saffron-specific.
- **Saffron compiles under the JIT engine.** Dynamic calls and the `any` bridges
  no longer force a fallback to the interpreter.
- **Try Turmeric's language picker offers the Saffron bases.** `#lang saffron`
  (and its curly-infix / neoteric / sweet readers) worked when typed into the
  playground but could not be selected: `WASM_LANG_BASES[]` in
  `src/web/wasm_glue.c` was a hand-kept copy of the base set that still listed
  the original four. The registry now walks `lang_base_at` -- the same
  (language x reader) cross-product `tur lang-layers` prints -- and an
  experiment-gated base is badged rather than hidden. `turi_wasm_set_lang` and
  `turi_wasm_get_lang` carry the LANGUAGE axis too, so selecting a Saffron base
  no longer leaves the session elaborating as Turmeric.
- **The playground's service worker evicts on every deploy, not every release.**
  `CACHE_VERSION` was keyed on `VERSION` alone, so an out-of-band deploy at the
  same version reused the cache name, `activate` evicted nothing, and returning
  visitors kept being served the previous bundle and wasm cache-first.
  `vite.config.js` now stamps `tur-try-v1-<VERSION>-<short-sha>`, falling back to
  a timestamp outside a git checkout -- never a constant.

## [0.45.0] -- 2026-09-09

### Added

- **`#lang saffron` -- a dynamically typed dialect.** Every unannotated
  parameter and return in a `#lang saffron` file defaults to `any`, and the
  file gets a dynamic operator layer, dynamic calls and dynamic field access on
  both the interpreter and the compiled back end. An entry file auto-loads the
  Saffron prelude. `tur init --saffron` scaffolds one, `tur repl --lang saffron`
  opens a prompt in it, `tur fmt` and the LSP understand it, and the editor
  syntax packs ship. See docs/guides/saffron-guide.md.
- **`any` became a usable type, not just a widening.** `is?`/`cast` work on
  parametric receivers and narrow inside an `if` guard, `type-of` answers
  alike on both back ends, a boxed function keeps a per-signature id and
  survives the round trip, and box type-ids agree across translation units.
  Containers widen too: `[...]` is a `(Vec any)`, `#map{...}` has `any` values,
  `(list 1 "two" 7.1)` widens its elements, and each element reads back with
  its own tag.
- **Typeclass default method bodies.** A method with a default body now works
  and dispatches dynamically -- including on an `any` receiver and on a
  parametric (HKT) receiver keyed on the head -- and an instance may omit it.
  The `{class, tag, dict}` instance registry is published so the dispatch can
  find them.
- **`--runtime=split` (opt-in).** The runtime preamble compiles once instead of
  once per program. Not yet correct for the whole suite: CI runs it ratcheted
  against a named failure list, with a Windows leg where the interesting
  failures actually are.
- **`set-of` checks its element types**, and `any` gets `Hash` and `MapKey`
  instances so it can be a set element or a map key.
- **A top-level `(do ...)` of definitions inside `defmodule`.** A macro
  expanding to several definitions wraps them in an implicit `do`, which was a
  hard TUR-E0711 reject -- so every turmeric-spices file calling `derive-json`,
  `defworld-box-helpers` or `defmirror` at module top level failed CI.

### Changed

- **`::` is refused on an `any` operand**, and the diagnostic names `cast`.
- **A duplicate `definstance` warns** when it is dropped, instead of vanishing.
- **An unknown `#lang` base is named** in the diagnostic, which points at
  `tur lang-layers`.
- **A rank-2 local fn value passes through as a fat closure**, not a by-name
  wrapper.

### Fixed

- **A bad subcommand flag exits nonzero.** `tur build --typo`, `tur repl
  --bogus`, `tur run --bogus` and every other subcommand printed a usage error
  and then exited 0, because the error paths reused the `--help` helper and
  inherited its status -- so `tur build --typo || exit 1` reported success.
  Every usage error path (unknown flag, missing or duplicated input, bad
  `--lang`) now exits 2; `--help` still exits 0 everywhere, including
  `tur expand --help` (exited 2) and `tur smt --help` (exited 3), which had
  the inverse bug.
- **`tur init` no longer destroys a working tree.** `tur init --help` did not
  print help -- the flag fell past the project-name slot, so the current
  directory's basename was used and the scaffold overwrote an existing
  `.gitignore` and `README.md`. `--help` is handled, unknown flags are refused,
  and scaffolding now refuses pre-flight if any of its five targets exists,
  naming them, with `--force` as the opt-in.
- **`tools/gendocs.py` reads the spaced annotation form.** `(defn f [a : int]
  : int ...)` -- the spelling 612 stdlib defns and the style guide use -- was
  parsed as parameters `a` (typed `:`) and `int` (untyped) with a return type
  of `:`, so 53% of the API reference would have rendered with a wrong
  signature on the next `tur run docs`.
- **`tur fetch` tells an optional-dep failure from a required one.** An
  `:optional true` spice that cannot be fetched is reported and skipped and the
  command exits `1`; a required `:spices` entry, `:cmake-deps` build, manifest
  or lock-write failure exits `2`; a clean fetch is `0`. Every failure used to
  be `1`, so CI could only warn-and-continue on all of them.
- **Emitted `any` drops no longer trip `-Wfree-nonheap-object`.** A `defopaque`
  over an immediate widened to `any` through an inlined callee made gcc warn
  `'free' called on a pointer to an unallocated object '7'`, though the runtime
  guard never freed anything. The drop is `noinline, noclone` now, and the
  fixture suite's emitted-C warning ratchet fails on that warning.
- **Three leaks on the `any` and container paths**: a map literal's
  intermediate generations, the spine of a non-escaping by-value recursive ADT
  local, and the Saffron `any`-widen leak (the frame-box rule fires again).
- **`forall-dict` no longer truncates a float result** through the dict
  carrier, and guards a by-value receiver instead of emitting bad C.
- **The interpreter no longer segfaults on `type-of` over an inline-C
  opaque**, a natively-built ADT value gets its `ctor->adt` link, and a
  `(Map K any)` value reports its own tag rather than `int`.
- **`tests/run-jit.sh` notices when the engine is off.** A trivial program must
  run natively before the fixtures start, and the fixtures allowed through the
  cc fallback are listed by name in `tests/jit-fallback-baseline.txt`; a new
  fallback fails the run. An emitter construct c2mir could not parse once put
  every fixture on the cc path and the suite still reported green.
- **`add_test` under `src/CMakeLists.txt` registers.** `enable_testing()` ran
  after `add_subdirectory(src)`, so `tur_trail` built, passed by hand, and was
  never listed by `ctest -N` or run in CI. A new lint
  (`tur_ctest_registration_lint`) fails if that happens again.

## [0.44.2] -- 2026-09-06

### Fixed

- **A downloaded release archive can compile a program.** The published
  linux-x86_64, linux-aarch64 and macos-arm64 tarballs have never shipped
  `libturt_runtime.a` -- the only runtime archive `TUR_RT_AUTO` will link --
  so `tur run` inside an extracted tree fell back to recompiling
  `src/runtime`, which the archive does not contain. The archives now ship
  it, and `locate_runtime_lib` gained a fourth candidate, `<exe_dir>` itself,
  for the flat layout those tarballs use. Windows keeps the prefix layout it
  shipped in v0.44.1, so nothing downstream changes. Every leg of
  `release.yml` now unpacks the artifact it just built and compiles a
  two-line program with it; the previous `tur --version` smoke test proved
  only that the binary starts. The remaining shape disagreement between the
  four archives is tracked in
  `docs/reported/unify-release-archive-layout.md`.
- **`tur lsp` resolves cross-module names on Windows.** `find_spice_root`
  canonicalises through `_fullpath`, which answers with backslashes, then
  walked up looking only for `/` -- so it broke at depth 0, contributed no
  spice include paths, and left every `(import sibling)` unresolved, which is
  why definition, completion and rename all answered "nothing here".
  `find_project_root` and the `tur docs` root probe carried the identical
  bug; all three now share `last_path_sep()`. Separately, `lsp_path_to_uri`
  percent-encoded the drive colon and every backslash -- self-consistent
  through this codebase's own decoder, and matching nothing an editor spells
  -- while `lsp_uri_to_path` turned `file:///C:/dir/x.tur` into
  `/C:/dir/x.tur`, which no Windows API opens. Both now speak
  `file:///C:/dir/x.tur`. POSIX is untouched.
- **`tur dap` keeps the debuggee's output off the protocol channel on
  Windows, and `tur trace` records it.** Neither captured the debuggee's
  stdout there, because `fcntl`/`O_NONBLOCK` has no counterpart for a Win32
  anonymous pipe and a blocking drain would hang -- so a program's `println`
  landed between two DAP messages, desynchronised the framing, and surfaced
  as a replay timeout. `PeekNamedPipe` answers exactly the question
  `O_NONBLOCK` was wanted for, so the read is capped at what is already
  buffered and can never block. The DAP and LSP harnesses now run on Windows
  CI, translating MSYS paths to native ones before handing them to `tur.exe`.
- **Regions: closed several silent wrong-answer and hard-build bugs** in the
  `with-region` / `bt-scope` escape lock. The runtime note was widened from
  the bracket's result word to every word a store or an erasure can carry
  out of a generation, catching a node stored via `vec-push!` into an outer
  collection, an erased field, a value handed to inline-C, or a parametric
  result the static walk couldn't see through -- each previously either
  dangled after a rewind or was wrongly retained. Separately, a carrier/
  pointer type mismatch at `do` result joins broke native compilation on
  newer compilers (clang, GCC >= 14) and macOS, and a related emitter fix
  (dropping a GNU-only `__auto_type` from the region note) had been silently
  disabling the JIT engine tree-wide.
- Fixed a generic function returning a heap-boxed record reading back the
  wrong data when instantiated alongside a sibling generic returning a
  plain by-value record with identical argument types.
- Fixed `vec-get-byval` failing to compile on a `Vec` of a by-value struct.
- `tur smt` and `--dump-refine=json` now read and write SMT-LIB `div`/`mod`
  with correct Euclidean semantics; some divmod-based obligations previously
  produced wrong satisfiability results.

### Changed

- Substantial performance improvement to recursive substitution over boxed
  recursive-field records (SR4): removed a hidden non-tail recursion so the
  by-value path returns to parity with the carrier path (was up to 4.77x
  slower, now ~1.01x).

## [0.44.1] -- 2026-09-05

### Fixed

- **`tur lsp` and `tur dap` speak to editors on Windows.** Both framed
  messages with a literal CRLF CRLF terminator, but Windows opens fd 0 in
  text mode and strips the CR on the way in, so a correct
  `Content-Length: N\r\n\r\n` header arrived as `\n\n`, the terminator never
  matched, and both servers ran to EOF and exited 0 having said nothing --
  no error, no diagnostic. The stdio transport (`src/lsp/lsp_io.c`) now puts
  its descriptors in binary mode; `tur format` and the REPL keep the platform
  text conventions, and `tur mcp` was never affected. A
  `tests/lsp/stdio-smoke.py` guard asserts each server answers an
  `initialize` with a well-framed reply, wired into ctest and the Windows CI
  job -- which ran `tests/run.sh` only and had never invoked ctest, so the
  existing LSP/DAP coverage never executed there. `run-mcp-lsp.sh` now
  honours `$TUR` and the `.exe` suffix instead of hardcoding `./build/tur`.
- **`spice_root_of` handles backslash-spelled paths.** Its walk-up searched
  only for `/`, answering "not in a spice" for every Windows path. Third
  instance of this bug class after `find_stdlib_beside_exe` and
  `rewrite_autolink_relative_paths`. Known remaining Windows gaps in LSP
  cross-module resolution and DAP time-travel replay are tracked in
  `docs/archive/lsp-dap-windows-gaps.md`.

## [0.44.0] -- 2026-09-05

### Added

- **Turmeric runs natively on Windows.** `tur.exe` builds and passes the
  fixture suite under MSYS2/UCRT64: the reactor gets a real `select()`-based
  I/O backend over Winsock, generated programs get real fiber-based context
  switching (a register-snapshot `ucontext` shim, replacing the earlier Win32
  Fibers spike) so multishot and multithreaded effects work, and the JIT
  executes natively through the S2 tier with lazy thunks and safe unwinding
  through fiber stacks. CI now builds and runs the JIT suite on Windows, and
  release artifacts ship a self-contained Windows x86_64 build.
  `TUR_JIT_DUMP_C` dumps the exact source handed to c2mir for debugging the
  JIT pipeline.
- **`tur smt` runs a script as a session, with a real assertion stack**
  (solver-extension plan, SX8b). `(push [n])` and `(pop [n])` scope the
  assertions, each `(check-sat)` is answered where it appears, `(get-model)`
  reprints the last witness and `(exit)` ends the session; the exit code is
  the last answer. `tur smt --interactive` reads the same commands from stdin
  and answers as they arrive, which makes `tur` usable as a backend for
  anything speaking the protocol subset.

  Two limits are deliberate and documented: only hypotheses are scoped (a
  `declare-fun` survives its `pop` -- an unconstrained variable can never turn
  a `sat` into an `unsat`), and the assertion set is incremental while the
  solver state is not (each check rebuilds its DNF cubes). A script with no
  `(check-sat)` is still decided once at the end, so SX8a's contract and the
  corpus replay are unchanged. Guide:
  `docs/guides/refinement-solver-internals-guide.md`.
- **The refinement solver is answerable from the browser** (SX8c):
  `turi_smt_check` runs an SMT-LIB2 script through the same reader, chain and
  bounded model search as `tur smt`, including the `(push)`/`(pop)` assertion
  stack, and returns JSON. The solver was already in the WASM module -- the
  refine sources are core sources -- so this exposes what was shipping rather
  than adding weight. Read-only: nothing reachable from it can elide a runtime
  check.
- **Two more solver caps are now instrumented** under `TUR_REFINE_STATS=1` and
  in `benchmarks/run-cap-sweep.sh`: `path hyps` (`RT_CS_PATH_MAX_HYPS`, the
  branch guards recovered per call-site crossing -- the tightest cap in the
  refinement path, and previously invisible) and `model vars`
  (`MODEL_MAX_VARS`, the counterexample search's width). The latter carries a
  second `model vars run` line counting the subset a higher cap would actually
  help, since a VC over the cap may also carry a non-int variable the sort gate
  declines at any limit. The solver's integer tail is also closed (S2c-lite,
  div/mod axioms, budgeted counterexamples), and the refinement chain now
  builds its DNF once per run instead of once per stage.
- **`with-region`: the lifetime-only region bracket** (regions plan, RM3 R5
  graduation item 1 -- the blocker). Under `--enable=regions`, `bt-scope`
  opens an arena generation as well as a trail level, which is right for a
  solver but left a program with no backtracking no way to bound a lifetime
  except by calling a search primitive. `with-region` (`stdlib/region.tur`,
  autoloaded) is the same bracket with the region only: same reclamation,
  no mark, no undo, and so no `#fx{Bt}` -- a `#fx{}`-declared function may
  call it. Each corner of the trail-level/region square now has one
  spelling: `bt-scope` (both), `with-region` (region only), the
  `bt-mark`/`bt-undo-to!` halves (trail only). Without the flag it is an
  identity call. Measured on the pinning fixture: live blocks at exit 908
  -> 5, allocations 1,424 -> 521, values identical both arms. The region
  stays a gated prototype; this removes the graduation blocker, it does not
  flip the default. The new autoloaded module moved all 148 codegen
  snapshots (binding-id renumbering plus one defn), regenerated in the same
  commit. Backtrackable State Guide has the square.
- **The region rewind admits a scalar-field ADT result** (RM3 R5 graduation
  item 2, first shape). The escape walk refused any constructor field with a
  NULL `full_type`, which is also NULL for an ordinary `:int`, so every
  ADT-returning bracket retired instead of rewinding. It now consults the
  declared field FORM: a bare scalar keyword reaches nothing and is admitted;
  an ADT name, `ptr`, a type variable or a compound form still refuses. This
  admits `(RxIP :int :int)` -- `re.tur`'s `re-find-from` result -- and keeps
  the mutual-recursion result refused; a kind-based accept that was reverted
  for a use-after-free is not reintroduced. A `Vec` of scalars and a
  sum-of-scalars shape are admitted the same way (R5 item 2's second batch),
  and the three R4 residues are closed (R5 item 3).

### Changed

- **Regions are on by default; `--enable=regions` graduated.** Every
  `bt-scope` and `with-region` bracket now opens an arena generation, and a
  `:heap` ADT node allocated inside it is reclaimed in one rewind when the
  bracket exits -- provided the compiler can prove the returned value reaches
  nothing allocated inside (a static walk over the result type plus a runtime
  escape check; a shape neither can clear retires the generation, which is the
  old behaviour exactly). The regions plan held this at prototype on one
  blocker, the boundary form; with `with-region` landed, the static walk
  widened across four pinned batches, and the three R4 residues closed, the
  four graduation requirements are met and the default flipped. Measured on
  the solver workload the phase was built for: the whole per-link spine goes
  to zero leaked at exit and peak RSS drops 47%; on the pinning fixtures, live
  blocks at exit 908 -> 0. `--enable=regions` is accepted as a `TUR-W0063`
  no-op for one minor line; `TUR_REGIONS=0` restores the pre-graduation build
  (plain malloc, no brackets, no shutdown) for bisection, and
  `tests/run-regions-seam.sh` is inverted onto that off path with a canary
  that the hatch bites, per the flags guide. The flip regenerates all 148
  codegen snapshots (region externs, routed ctor allocations, the atexit
  shutdown, and a bracket at each boundary now appear in every program). Plan:
  `docs/archive/regions-plan.md`; the GC guide documents the mode beside RC
  and the cycle collector.

  The emitted C stays self-contained: `src/runtime/{arena,region}.{h,c}` are
  embedded into the compiler at build time (`cmake/embed_region_runtime.cmake`)
  and pasted verbatim into the preamble on the same DEDUP-4b archive posture
  the rc/GC runtime follows -- declarations only when `libturt_runtime.a` is
  on the link line, bodies in the owner TU otherwise (one region stack per
  program, hidden in a `.so`). Before that a project build, a `--shared`
  library, the REPL's spice cache and a bare `cc` of `tur emit-c` output all
  linked with `undefined reference to tur_region_shutdown`. The multi-module
  executable link, which chose the archive posture and then named no archive,
  now links `libturt_runtime.a` like the single-file path does.
- **The release workflow is dry-runnable**, so a workflow change can be
  validated without cutting a real tag.
- **`st-bind`'s callback parameter is now a real function type**, not an
  opaque `StThunk` carrier -- removing the untyped carrier also fixed an
  aliasing bug it was hiding.

### Fixed

- **A broad set of Windows-specific portability fixes** landed alongside the
  bring-up: shell-string subprocesses now work under `cmd.exe`; `C:\dir` is
  recognized as absolute when anchoring `-I`/`-L`; the stdlib walk-up steps up
  a directory correctly; the temp directory and `fs/tmpfile` are resolved at
  run time instead of a hardcoded `/tmp` (which resolved to the MSYS drive
  root); every emitted `setjmp`/`longjmp` routes through `TUR_SETJMP`; `-ldl`
  is dropped from autolink flags (Windows has no `libdl`); `.text` is restored
  after the `ucontext` asm block; a Win64 aggregate return now gets its typed
  `sret` shim; effects inside a fiber no longer die on `STATUS_BAD_STACK`; and
  paths embedded in generated `(load "...")` source and REPL paths are
  correctly escaped.
- **The JIT's native tier (S2) was disengaged on every platform, not only
  Windows** -- an unguarded `dlfcn.h` include broke the Windows JIT build, and
  the S2 engage probe never matched on a host without the runtime archive.
  Both are fixed.
- **The reactor's source-slot reuse orphaned the outgoing source's callback
  box**, surfacing as a hang under sustained async load; slot recycling is
  now deferred by one poll so an in-flight callback is never freed out from
  under itself.
- **Several defects in the union/niche carrier representation**, found via a
  restored SR2 seam harness: three defects in the `tur_tagged_t` round trip;
  a nested-carrier-match binder that failed to deref the Option/Result
  pointer-box slot; a niche-typed comparator parameter that read a caller's
  carrier box instead of unboxing it (now generalized to every word-passing
  container, not just `Vec`); an `rc`-of-by-value-sum monomorph that read the
  first word of the wrong representation (two defects); and three hard
  compile errors in union widening at `let`/return position. The frozen
  `g_adt_app_byvalue` gate, no longer needed, was removed.
- **A CPS call's ABI specialization is now selected by its result type, not
  just its arguments**, fixing a miscompile where two calls sharing an
  argument list but differing result types picked the same specialization.
- **Several closure and dispatch correctness fixes**: a let-aliased function
  parameter captured in a lambda; a directly-applied lambda's calling
  convention now read off its own argument; both parameters of a nested
  binary class-method spec now resolve; a fallback spec clone must belong to
  the call's own callee; and a second zipper's leaked box plus a null-handle
  crash in `zipper-free`.
- **The refinement solver's declared-row purity veto had a second defect**
  that hid behind the first: it ran once at the top of the walk and was
  skipped on every memo hit, and the memo was written first by the
  predicate-impurity walk, so it never fired -- for a resolved `#fx{Unsafe}`
  either. It now applies where the verdict is memoized, so it holds on every
  lookup and is transitive (an unannotated wrapper around a `#fx{Bt}`
  function is not proven pure). Pinned by `sx1-bt-row-checked` and
  `errors/sx1-bt-row-pure-caller-rejected`; the Backtrackable State Guide has
  a section, and the Effects System Guide now documents the silent-drop trap.
- **The internals guide's solver documentation was a release cycle stale.**
  The caps table gave `NO_MAX_SHARED` as 8 where the source says 16 (raised
  2026-08-25), in two places; the S1 section still described the
  rebuild-per-cube EUF state that incremental EUF replaced; and
  `TUR_REFINE_EUF=rebuild`, the seam for bisecting the incremental path, was
  undocumented.
- **The trail's `#fx{Bt}` effect row is now checked; it used to resolve to
  nothing.** Every mutator in `stdlib/trail.tur` has carried `#fx{Bt}` since
  0.41, but no `defeffect` declared `Bt`, and an undeclared uppercase name in
  a row is silently dropped at resolution -- so the annotation checked as
  `#fx{}` and `--dump-effects` showed `bt-set! : #{}`. `Bt` is now a
  `^capability` effect declared in `trail.tur` (the autoloaded module, so it
  is declared wherever the row is written), which makes it propagate from a
  callee's declared row like `#fx{FS}`: a caller that declares `#fx{}` (or
  any row without `Bt`) and reaches the trail is `TUR-E0009`. Unannotated
  callers stay unchecked, as for every capability tag. `bt-scope`,
  `with-untrailed`, `dfs-solve` and `dfs-choose-go` carry the row too; the
  DFS goal constructors deliberately do not (they only build a closure).

## [0.43.0] -- 2026-09-03

### Added

- **Serializable continuations gained a documented typed surface.** The
  guestbook example was rewritten as a spice with one continuation per page
  to demonstrate it, and the serial runtime now emits whenever a serial-cont
  builtin is referenced (rather than unconditionally), with the
  colored-receiver rejection tightened to the actual soundness rule
  (colored, not merely capture-free, is what's refused).
- **Image globals**: a `defimage-global` registry and a second image
  section, with `TUR-W0706` diagnosing misuse.
- **`tur dap` and `tur trace` now instrument top-level programs, not only
  `(main)`.** A program whose work happens at the top level previously ran
  with the debugger idle (no `stopped` event, `0 steps` recorded); the
  launch path now pre-scans for a top-level `main` and arms the debugger
  around the file load itself when there isn't one.
- **`#json-file<T>` readers** for loading typed JSON directly from a file
  path.
- **A `stdlib/VERSION` stamp is checked at resolve time**, so a `tur` binary
  built against one stdlib version and pointed at another's
  `TUR_STDLIB_DIR` now fails closed with a version-mismatch diagnostic
  instead of silently compiling against the wrong stdlib.
- **A real miniKanren example** over `stdlib/logic.tur`.

### Changed

- **The Option niche is the default representation, and `(some p)` over a
  `:non-null` payload can no longer carry NULL.** `--enable=option-niche`
  graduated on 2026-09-03 (the name is accepted as a `TUR-W0063` no-op for
  one minor line; `TUR_OPTION_NICHE=0` restores the tagged form for
  bisection, and `tests/run-option-niche-seam.sh` keeps that path green).
  An `(Option P)` for a `:non-null` opaque (`String`) or a compiler-lowered
  heap collection (`(Option (Vec int))`) is carried AS its payload pointer --
  16 bytes to 8, `(none)` as NULL, no tag word -- and a `(Vec (Option
  String))` stores each element as that word in its slot with no per-element
  box (2e6 elements: 17.8 MB / 0.018 s against 79.7 MB / 0.08 s before). The
  representation spends the bit pattern `0` on `None`, so a `Some` whose
  payload is null has nowhere left to live.

  **Breaking for inline-C that builds an Option over such a payload.**
  `tur_some_ptr(0)` produces a legal value today that `some?` answers true
  on; under the niche it is a diagnostic naming the type and the violated
  declaration (`a carrier Some with a NULL payload crossed into a
  niche-represented Option -- the payload type's :non-null declaration was
  violated`), raised at construction or at the carrier crossing, whichever
  comes first. It is a diagnostic with a message, not a crash. The fix is one
  line and is almost always what the code meant: return `tur_none()` for the
  absent case. If a payload type genuinely has a valid null, drop `:non-null`
  from its `defopaque` -- that un-elects it from the niche and restores the
  16-byte tagged form, at no other cost.

  A *provable* violation (the literal `0` ascribed into a `:non-null` handle)
  has been `TUR-E0303` at elaboration since 0.41.0 and is unaffected. One
  shape is a known residue rather than a bridge: an UNTYPED closure parameter
  that a `vec-eq?` comparator ascribes back to `(Option String)` reads the
  slot word as a carrier box -- write the element type on the parameter
  (`docs/reported/erased-closure-param-over-niche-vec-slot-reads-box.md`).
  Plan: `docs/archive/sr3-option-niche-plan.md`.

- **Recursive sum types now default to by-value representation (RM4/SR4
  flip)**, matching the non-recursive default and avoiding the carrier
  allocation cost measured to have no compensating benefit.

### Fixed

- **`(eq? (some s) (some t))` over a String or struct payload compared
  pointers.** The `Eq[Option]` and `Eq[Result]` instance bodies dispatched
  the payload `eq?` on a match binder the elaborator had typed by the
  representative (`int`), so every ABI specialization ran `Eq[int]`:
  `(eq? (:: (some "aa") (Option String)) (:: (some "aa") (Option String)))`
  was false, with a `-Wint-conversion` warning in the emitted C as the only
  symptom. The binders are now ascribed to the class var, which re-drives
  the dispatch per specialization. Independent of the niche (both paths).
- **`(eq? v w)` over a `(Vec (Option String))` read each slot word as a
  carrier box.** The constrained-Eq synthesizer's `vec-eq?` comparator
  names its parameters as slot words now (`__cmp_slot_a`), so the bridge
  reinterprets the word exactly as a hoisted `vec-get` temp is. Pinned by
  `tests/fixtures/option-niche-vec-closure-cmp`, which joins the seam
  population with the typed-comparator and let-bound-word shapes.
- **A fresh `Option`/`Result` over a value struct passed to a class method
  no longer leaks its payload box.** `(enc (some (make-struct Box ..)))`
  kept the `Box` copy for the process lifetime although the instance only
  read its argument: the drop-after-consumer stamp lived in the ordinary
  call path and a class-method call never ran it. The resolved instance's
  inferred non-retention mask now decides there (per monomorph at emit when
  the receiver is abstract). A let binding through `ok-val` / `err-val` /
  `unwrap` of a fresh producer is freed at scope exit as the payload's only
  holder, and a return-dispatched producer's carrier cell is drained against
  the instance that ran, so the struct copy inside it is freed too. Closes
  `docs/archive/value-struct-payload-sum-monomorph-box-has-no-owner.md`.

- **A callee that stores its `Option`/`Result` argument is no longer inferred
  non-retaining.** `(defn stash [o : (Option Box)] : int (vec-push! store o) 1)`
  got the non-retention bit because the confinement walk modelled a callee
  only through its result, so the caller freed its fresh `(some Box)` after
  the call and the container read it back freed (a use-after-free since
  2026-09-02, silent without ASan). Under the sum walk a hand-off to a callee
  that is neither an audited reader nor proven non-retaining is an escape.
  Pinned by `sum-payload-stashing-callee-not-dropped`.

- **A class method whose result is the class variable can mint an `Option`
  over a value struct in a constrained instance.** Three `cc` errors on a
  shape `tur check` accepted: the ctor-argument seam deref'd a re-dispatched
  by-value result as the carrier; a dictionary slot's base clone was declared
  with a by-value result while its tail spilled to the carrier; and
  `[(Option Box)]` as an instance head read `Box` as a type variable and made
  the method's own parameter move-only. All three fixed;
  `docs/archive/return-dispatched-sum-mint-in-constrained-instance-miscompiles.md`.

- **An inline-C body that boxes a value-struct payload into a declared
  `(Option T)` / `(Result T E)` hands that payload over with the box.** Such a
  producer is fresh by declaration now, so the payload is freed with the cell
  instead of leaking; the guide states the contract (the payload must be a
  fresh allocation, like the box).

- **A niche `(Option P)` Vec element is stored as its payload word (CE1/CE2
  of the container-element-form plan; default, since the niche graduated in
  the same release).** A `(Vec (Option String))` used to pay a heap carrier
  box per element; the element now lands in its slot as the String pointer
  (None is 0), and every read hands it back. 2e6 elements: 17.8 MB / 0.018 s
  against 79.7 MB / 0.08 s. `TUR-E0714` refuses a niche element stored
  through a fully erased receiver, the one shape that cannot decide the slot
  convention. Also fixed on the way: a generic
  `(defn push-it [A] [v : (Vec A) x : A] (vec-push! v x))` specialized for an
  Option element double-wrapped the value -- a silent blank read under the
  niche and a `cc` error on the default path.
  `docs/archive/container-element-form-plan.md`.
- **ADT and constructor names could collide when emitted to C.** The
  separator fold and the emitted joiner shared the same alphabet
  (`_`/`__`), so distinct ADT/constructor name pairs could mangle to the
  same C symbol; two ADTs sharing a constructor name could also emit the
  same C function twice (`redefinition of 'ctor_Mk'`). Constructor symbols
  are now qualified by their owning ADT and mangled injectively, and a bare
  ctor alias whose name mangles like another's now fails closed at compile
  time instead of picking one silently.
- **`bit-shr`'s `>>` did an arithmetic (sign-extending) shift** instead of
  the documented logical right shift, reproducible identically under
  compiled output, `tur jit`, and `--interpret`. Also under `--interpret`:
  `vec-new-filled`'s native override was silently lost after preload (a
  registration-ordering bug), and `int->float` had no builtin entry or
  preload stub at all and was unreachable.
- **`(defn f [] : int nil)` was accepted and returned 0** instead of being
  rejected like every other wrong tail (`"hello"`, `1.5`, `true`); a nil
  literal tail under a declared non-nil return is now a compile error.
- **Several leaks and miscompiles in the sum/carrier reclamation sweep**,
  found via a corpus-wide ASan sweep (2,186 fixtures, 31.4 MB -> 608 KB of
  leaks): a stackless let-bound caught box now frees through its machine
  variable; a catch-unwind is now treated as a fresh sum producer;
  value-struct payload boxes are now freed in argument position and under
  void consumers; bind-chain boxes and envs are now owned at statically
  resolved dispatch sites; a let/do wrapping an `if` no longer
  double-unboxes carrier arms; a fat-closure shim box handed to a proven
  non-retaining sink (e.g. `result-eq?`, `vec-eq?`) no longer leaks; and
  cstr readers, reinterprets, and boxed carrier arms are now correctly
  modeled in the escape walk.
- **`tur fmt` now preserves comments in header/arm gaps, around `^mut`
  pairs, and immediately before a closing form**, which it previously
  dropped.
- **`:c-sources` now propagates across the whole `:spices` dependency
  closure**, not just direct deps.
- **Shared static buffers were retired from C-name accessors**, which were
  not stable across calls in the same compilation (a latent aliasing
  hazard).

## [0.42.2] -- 2026-09-01

### Added

- **`tur dap` serves a recording as a timeline**, not just as a sequence of
  steps. DAP describes execution as one step after another and has no
  vocabulary for an axis -- correct for a live debuggee, where there is nowhere
  to scrub to, and wrong for a recording, which is an axis. Three custom
  requests add one, advertised as `supportsTurmericReplayTimeline`:
  `replayInfo` (how many steps, where the cursor is), `replaySeek` (jump to
  step N, clamped, reporting where it landed) and `replaySites` (where steps
  are and how deep, by explicit index or downsampled to a bucket count). All
  three refuse in a live session naming the reason, because a client that asks
  has a scrubber in mind.

  `replaySites` returns position and depth **together**, which is the shape Try
  Turmeric's `trace-site-at` already uses: a timeline's cursor readout wants
  `file:line` and a depth ribbon wants `depth`, over the same steps. A bucket
  reports its range's *maximum* depth and the site of the step where that
  maximum occurred, so a deep call between two samples is not erased and
  clicking a ribbon spike lands where the spike is.

  None of this is new capability in the reader -- `turi_trace_replay_seek`,
  `_steps`, `_depth_at` and `_site_at` already existed, and the last two are
  index reads. What was missing was any way to reach them over the wire, and
  the alternatives are worse than they look: a slider whose range is a guess,
  and a seek approximated by repeated `stepBack`, which rebuilds state from the
  start of the stream once per candidate and turns a scan of an 80k recording
  from milliseconds into a hang.

- **The replay console rewinds.** Forward motion still appends through ordinary
  `output` events. Backward motion could not: the transcript at the new cursor
  is a prefix of what the client was already sent, and a delta cannot express a
  truncation -- so the old code sent nothing and left the console showing
  output from steps the cursor had rewound past. A backwards seek now emits a
  `replayOutput` event carrying the whole transcript to be used in its place.
  Whole-transcript rather than a cut offset, because a client that missed an
  earlier event would otherwise cut in the wrong place and never know. Clients
  that do not recognise the event are exactly as they were.

### Changed

- **The website is rebuilt around one canonical site map.** The topbar,
  sidebar, mobile drawer and footer are generated from a single pair of lists
  shared by `web/site.js` and the three page generators (`tools/gendocs.py`,
  `tools/genguides.py`, `tools/genspices.py`), so hand-written pages and
  generated ones -- guides, API docs, spices -- can no longer disagree about
  what is on the site. Every chrome link carries a `title` describing where it
  goes, the mobile drawer shows the same site map the desktop rail does, and
  the home page's install step became a tabbed set of install methods. The
  tour was reworked to match.

### Fixed

- **A recording's last step now shows what the program printed.** A replay
  transcript holds the output produced strictly before the cursor's step, and a
  program whose final act is a `println` drains it after the final STEP -- so
  the last index reported an empty transcript. Measured: `outputLength: 0` at
  step 24020 of 24021 for a fixture that prints "done" and exits. An empty
  console at the end of a run that printed reads as a broken timeline rather
  than a precise one. The final step now concatenates every OUTPUT record, the
  same special case and for the same reason as Try Turmeric's
  `turi_wasm_trace_output_full`.

## [0.42.1] -- 2026-08-31

### Changed

- **`tur trace` records one step per expression, not per source line.** The
  recorder drove the debugger with step-in, whose stop predicate is
  line-granular, so a recording's resolution was a source line -- the wrong unit
  in a Lisp, and more so in Turmeric, where neoteric `f(g(x))` and sweet-exp `$`
  chains exist to put more on a line rather than less. A loop whose body fit on
  one line collapsed into a single step, with the induction variable jumping
  from its first value to its last in one delta and every iteration's output
  arriving in one drain; and fidelity depended on formatting, the same loop
  recording 3 steps on one line and 23 across four. Both spellings now record
  58. `tur debug` stepping is unchanged -- line granularity is what a human
  drives by hand and what DAP speaks -- and `--lines` selects the old
  granularity.

- **The `.turtrace` format is v2.** A site carries a column range rather than a
  bare column, and the header records which granularity a recording was taken
  at. The column was always in the format but named nothing under line stepping:
  it was whichever node landed first on a newly entered line. v1 recordings
  still read back. The step cap moved with the unit (200,000 -> 1,000,000
  native, 50,000 -> 250,000 in the browser) -- a cap bounds the recording, but
  what it means is how much of a program fits under it.

- **`tur run` matches attributes by name and refuses unknown ones**, and aborts
  on a builtin failure rather than continuing with an empty string.

### Added

- **`tur run` gains Justfile parity on parameters, modules and builtins**: named
  and flag parameters via `[arg(...)]`, `mod` and imports, backtick evaluation,
  and `os_family` / `path_exists` / `replace` / `join` / `error`.

- **The Try Turmeric timeline highlights the expression** inside the current
  line, which is the visible half of recording per expression; the toolbar
  scrolls when it overflows.

### Fixed

- **Two silent-ignore holes in `tur run`** where a parameterized attribute was
  accepted and then quietly dropped.

- **`tur run` runs recipes from the Justfile's directory** and honors `[no-cd]`.

- **A node was hooked twice by the interpreter's debugger** when the driver
  handed a black-box node to `eval_expr`. Line-granular stepping hid it -- the
  duplicate shares a line -- so it surfaced as doubled records the moment the
  recorder began asking for every node.

## [0.42.0] -- 2026-08-30

### Added

- **`tur trace` -- a time-travel recorder, and reverse execution in `tur dap`.**
  `src/turi/trace.c` records every node the tree-walker evaluates as deltas
  rather than states: a step carries only the bindings whose rendered value
  moved, so 80006 steps cost 1.2 MB -- 15 bytes a step. `tur dap` `launch` with
  `"replay": true` then serves the whole session from that recording, so
  `stackTrace`, `scopes` and `variables` answer from a trace cursor. That is
  what makes `stepBack`, `reverseContinue` and `reverseNext` answerable at all
  -- a pause cannot go back. VS Code and nvim-dap draw the reverse-execution UI
  off `supportsStepBack`, so there is no editor-side widget here.

- **A time-travel timeline in Try Turmeric.** Trace is a second button beside
  Run (and a `:trace` command at the prompt): it records the tab's program under
  the interpreter and turns the console area into a scrubber -- a slider over
  the run, step forward and backward, the editor gutter following the cursor,
  each live frame's bindings at that point, and the transcript rewinding with
  it. The recorder was already compiled into the wasm module and simply
  unexported; the module is byte-identical in size either way, so this costs no
  download.

- **The Try Turmeric prompt gets completion, hover and its own LSP document.**
  The `turi>` prompt is a single-line Monaco editor backed by
  `file:///project/repl.tur` rather than an `<input>` -- not for the look, but
  so that completion, hover and signature help at the prompt are the providers
  that already exist instead of a second widget stack built over a text field.

- **Lexical scope in the language server -- scope-aware highlight, rename and
  references.** The symbol index knew only global bindings, so every consumer
  answered a textual question when it had been asked a lexical one: a parameter
  named `x` highlighted every `x` in the file, and rename could not be written
  at all. Each local binding now carries the region it is visible in, as two
  ranges whose gap is the binding's own initializer -- so `(let [x (+ x 1)] x)`
  resolves the init's `x` to the OUTER binding while a caret on the binder still
  resolves to the inner one.

- **Serializing a continuation inside an open `bt-scope` is refused.** Both the
  host codec and the emitted `tur_serial_cont_serialize` now report the trail
  depth and the count of outstanding trailed writes instead of producing a blob.
  A serialized continuation carries control; the undo information that would put
  the scope's writes back is process-local and does not travel with it, so such a
  blob would deserialize into a world where those writes either never happened or
  can never be unwound.

### Changed

- **`backtrackable-state` graduated -- `stdlib/trail.tur` is always available.**
  The trail (mark/undo cells with per-cell, per-write and per-level opt-out) no
  longer needs `--enable=backtrackable-state`; the experiment row is deleted and
  the module is an ordinary autoload. What held it at `prototype` was one open
  question -- plan 3.5's multi-shot re-entry -- now decided: **the checked error
  is permanent and snapshotting the live trail segment on capture is declined.**
  The SX0 curve settled the cost half (a snapshot is at best at parity with
  replaying the writes at 5.0 ns each, and is paid at *every* capture rather than
  only on the branch taken), and the semantics half went the same way (a
  symmetric snapshot restores the learned clause away, which is the one thing a
  backjump must keep). Writing the fixtures turned up a stronger guard than
  either: `Mark`, `BtCell` and `GCell` have no `Clone` instance, so a multi-shot
  `cloneable-shift` **cannot capture a trail handle at all** -- `TUR-E0014` at
  compile time, now pinned so it cannot regress.

  Because trail.tur now prepends to every compile, 148 codegen snapshots moved.

- **The trail works under `--interpret`.** `stdlib/trail.tur` is entirely
  inline-C, which the tree-walker cannot execute, so making it an unconditional
  autoload would have left a whole module resolving to nothing outside the
  compiled path. It is now shimmed for the interpreter -- and shimmed by
  *calling the same `src/runtime/trail.c`* rather than reimplementing it, so
  there is one trail, not two that can drift. `tur --interpret`, `tur eval`,
  `tur repl` and the web REPL all have the full surface; five fixtures now run
  under both harnesses and produce byte-identical output, including the
  `bt-depth` counts that pin "a thousand writes cost one trail entry".

  Serializing a continuation is the one compiled-only behavior, and it is not a
  gap: under `--interpret` `tur_serial_cont_serialize` is an in-process deep
  copy rather than a byte codec, so no blob outlives the trail and there is
  nothing to refuse.

- **`::` between an integer kind and a float kind is refused, and both meanings
  are named.** It meant two different things depending on where the value came
  from -- `(:: (.n m) :float)` means the NUMBER, `(:: (list-head c) :float)`
  means the BIT PATTERN -- and both operands are statically `:int`, so the
  same-size rule answered "bits" for both, silently. A small integer read as
  IEEE bits is a denormal, so `mixedfold` returned 0.25 instead of 3.25: a
  dropped term, not a garbage one. Say which you mean -- `int->float` /
  `float->int` (`stdlib/math.tur`) to convert the number, the new `float->bits`
  / `bits->float` (`stdlib/bits.tur`) to reinterpret. Integer literals stay
  exempt and keep converting. The interpreter registers the new pair as natives,
  which closes a compiled-vs-interpreted divergence rather than adding one: the
  tagged model could never implement a bit-reinterpreting `::`, but once the
  author has said which reading they meant there is nothing left to guess. All
  four spellings now agree on both paths.

- **`stdlib/arrow`: real `ArrowLoop` feedback via `LoopCell`.** The fed-back `d`
  of `ArrowLoop` at `(->)` was a sentinel `0`, so the instance was only correct
  when the looped arrow never read it. Turmeric is strict, so `d` cannot simply
  BE the value the same run is about to produce -- but indirecting through a
  two-word heap cell `{ filled, value }` splits "the value is written" from "the
  value is read", which is the only thing laziness was buying here.
  `arrow-loop` / `arrow-loop-lazy` fill it with the `d` output when the run
  returns (knot-tying, so a deferred read observes the `d` this run produced);
  `arrow-loop-fix` seeds it and refills per pass until `d` stops moving or fuel
  runs out. One shared protocol, so a single looped arrow works under any of
  them.

### Fixed

- **`any` payload boxes leaked once per widen.** A value RETURNED as `any`, or
  handed back by a callee that boxed it, leaked 16 bytes a turn under
  LeakSanitizer in three residual shapes -- the earlier argument-position fix
  could not help there, because a caller-frame copy would dangle. Ownership is
  now settled where the value lands: a non-escaping local's box dies with its
  scope, an owned temporary is dropped after the call that consumes it, the drop
  fires at early exits as well as the normal one, and a non-retained widen stays
  in the caller's frame. `any` also joins the CPS subset, so a `perform` beside
  one lowers.

- **A local callee was spelled as two different C identifiers.** Calling an
  ascribed `:fn` param -- `((:: f (fn [int] int)) v)` -- hoists the callable head
  into a synthetic binding whose declaration and use went through different
  naming rules, so cc rejected the undeclared one: a clean build break on a
  documented spelling. Both ends now name it by the same rule.

- **Wide by-value aggregates crossing the poly-to-fat boundary.** The three
  poly-to-fat ABIs disagreed about an argument too wide for the carrier; they now
  bridge it through the fat-box carrier and agree.

- **`bt-level` and `bt-depth` read an unspecified upper half.** Both C functions
  return `uint32_t` while `stdlib/trail.tur` declared them `:int` (`int64_t`), so
  the result's high 32 bits were whatever the callee left in the register -- a
  level could in principle read as 4294967297. Found while writing the serialize
  guard. Both now go through `tur_trail_level_i64` / `tur_trail_depth_i64`,
  matching how a mark is already packed across that boundary.

- **Editor-side resolution fixes.** DAP breakpoints match against the recorded
  site rather than a rebuilt state; the language server resolves a buffer's
  spice from where the file lives rather than where it was written; a local
  whose scope start cannot be computed is dropped instead of indexed. Try
  Turmeric starts a recording from a fresh session, aligns the dialect button
  with the rest of the toolbar, and does not register the service worker on
  localhost.

## [0.41.0] -- 2026-08-28

### Added

- **Three structural diagnostics for shapes that previously compiled into
  nothing.** `TUR-E0711` rejects a non-definition form at `defmodule` top level
  (it was silently never evaluated); `TUR-E0713` rejects a definition in tail
  position of a function body (there is nothing to return); `TUR-E0712` bounds
  the emitter's expression walk so a pathologically nested expression reports
  instead of running the C stack out. See `docs/guides/syntax-guide.md`.

- **`:non-null` is declarable on a `defopaque`,** replacing the option-niche
  allowlist's hand-maintained opaque rows. Ascribing the literal `0` into such a
  type is now `TUR-E0303` at elaboration rather than an abort at the niche `Some`
  constructor at runtime; a *computed* zero is not provable there and still falls
  through to the runtime check. `tur explain TUR-E0303` carries the rationale.

### Changed

- **A `defopaque` over a pointer c-names as `void *`, not `int64_t`.**
  `(defopaque String :ptr<void>)` used to lower to the same `int64_t` carrier
  word as every other handle, so an opaque handle was byte-indistinguishable
  from a tagged carrier box at the emitter's ~94 `strcmp(cname, "int64_t")`
  sites. It now says what it is. The declared base type had to start being
  recorded for this: `defopaque` parsed it only to find where the trailing
  `:linear` / `:affine` / `:sealed` attributes start, and then threw it away, so
  `:ptr<void>` and `:int` were indistinguishable downstream.

  **Breaking for inline-C over a pointer `defopaque`**, and deliberately loudly
  so: a body that ends `return (int64_t)(intptr_t)p;` in a function returning
  such a handle is now a `-Wint-conversion` (which the suite's ratchet fails on),
  and a hand-written `extern` declaring the handle as `int64_t` is a hard
  `conflicting types`. Both fixes are one line -- return `(void *)(intptr_t)p`,
  declare the parameter `void *`. Stdlib took 66 such edits across 21 files;
  `stdlib/string.tur` took none, because it was already written as
  `(:: (tur_string_adopt_cstr s) String)` over `ptr<void>`-typed externs, which
  is the pattern to copy. Gate results:
  `docs/archive/opaque-pointer-c-spelling-gate-results.md`.

- **SR3's Option niche is unshelved, as `--enable=option-niche`.** The
  0.40.0 entry below shelved it because `String` -- the whole of the phase's
  census -- could not take the niche while it c-named to `int64_t`. The change
  above removes that, so `(Option String)` is carried as its payload pointer:
  16 bytes to 8, `(none)` as NULL, no tag word, and no
  `tur_adt_Option__String` typedef emitted at all. It stays behind a flag
  because "this payload's valid values exclude 0" is a hand-maintained
  allowlist rather than something the type system records; graduating it means
  making non-nullness declarable. `Cons` remains ineligible for the reason the
  0.40.0 entry gives. Plan: `docs/archive/sr3-option-niche-plan.md`.

### Fixed

- **`:cmake-deps` link lines are resolved by CMake instead of guessed
  downstream.** `INTERFACE_LINK_LIBRARIES` is now walked recursively inside the
  generated `CMakeLists.txt`, where `if(TARGET ...)` can tell a library name
  (`m` -> `-lm`) from a CMake target name (raylib lists `glfw`) -- identical
  shape, opposite handling, and the C-side guess emitted a `-lglfw` that does
  not exist. An Apple framework arrives as an absolute path to the `.framework`
  *directory* and is now respelled `-framework <name>` rather than passed as a
  link input, which `ld` rejects with `file cannot be mmap()ed, errno=22`.
  Alongside: the walk is scoped to the declared `:spices` closure instead of
  every workspace member (building `spices/opengl` configured 15 unrelated
  native deps, and a single one that could not configure aborted the whole
  build); a transitive `:path` dep is no longer absolutized and then
  re-prefixed; a shared-library dep gets its `-rpath`; `:path` resolves against
  `cmake/`; and CMake 4's policy floor is passed through. A raylib spice now
  builds and runs on macOS with no `cmake-deps/` shim.

- **An inline-C body that builds an `(Option T)` produced the wrong value under
  the niche.** `tur_some_ptr` returns the carrier -- a pointer to a tagged box --
  and a niche consumer read that word as the payload, so
  `(string/to-cstr (unwrap o))` printed blank rather than the string. Silent,
  not a crash. The let-binding and call-argument crossings now bridge through
  `emit_carrier_bridge` like every other. Only reachable with
  `--enable=option-niche`.

- **`: nil` forward declarations no longer collapse to the `int` placeholder.**
  A statement-position call to a `: nil` function the elaborator had not yet
  reached was emitted as `__auto_type __ps_N = <void call>;`, which `cc` rejects
  with "variable has incomplete type 'void'" and no `.tur` attribution. `: void`
  was immune, and moving the callee above the caller made it disappear. The root
  cause was in the elaborator: the reader parses a bare `nil` in type position
  as `F_NIL`, not `F_SYM`, so the forward-declaration pre-passes did not match it.

- **A module-level `def` of an opaque-typed value emits its global.**

- **Two diagnostics now name what they could not find:** a hoisted inline-C
  `#include` that does not resolve names the header, and `tur` says so when
  `TUR_STDLIB_DIR` overrides the stdlib sitting beside the binary.

## [0.40.0] -- 2026-08-28

### Added

- **Try Turmeric navigation (M0-M5, F1).** A minimap with blocks and a
  three-lane overview ruler, gated on measured editor width with a persisted
  override; a Symbols popover fed by the `documentSymbol` provider, sorted by
  position, kind-labelled and caret-tracking; go-to-definition into the stdlib,
  opening a read-only padlocked buffer excluded from downloads, persistence and
  the server's document set, with F12, Cmd+click and a Back button that appears
  only when there is somewhere to go; `documentHighlight` that skips comments,
  strings and inline-C fences; hover that falls back to the documentation table
  when the checker has nothing, marked as docs-sourced; and `builtin_describe`,
  so `println`, `+`, `=` and `not` hover to something at all. A C-interpreter
  link joins the site footer, the `/try` footer and the sidebar's Ecosystem
  list.

- **CI metrics page at `/ci`.** Every push to `main` publishes each ctest
  suite's wall time to the `ci-metrics` branch; the page reads one build
  environment at a time and shows duration trends, per-suite sparklines and the
  skip ledger.

### Changed

- **Parametric sum monomorphs flow by value by default (SR2a graduated).**
  `--enable=parametric-sum-byvalue` is retired -- `(Option int)`,
  `(Result float cstr)` and every other concrete parametric sum monomorph are
  aggregates with no per-ctor malloc, where they used to ride the int64 heap
  carrier. Measured on the seam: 3.6x faster and 71x less peak RSS on a
  narrow-sum loop, 3.2x/145x on a wide one, and one leaked allocation per
  construction eliminated. Shapes the predicate declines (self-recursive,
  `:heap`, GADT, fixpoint partners) and erased generic bases still use the
  carrier. `--enable=parametric-sum-byvalue` remains accepted as a TUR-W0063
  no-op for one minor line; `TUR_SR2_APP_SUM_BYVALUE=0` restores the carrier
  for bisection. Plan: `docs/archive/sum-representation-plan.md` (SR2c).

- **`ok?` and `err?` take `(Result A B)`, not `:int`.** They were the last
  carrier-typed Result accessors; `some?`, `ok-val` and `err-val` were already
  parametric. An `:int` parameter stops being a harmless erasure once the value
  flows by value, so inline-C code that hands back a carrier value declared
  `: int` now names its type at the boundary -- `(ok? (:: r (Result int int)))`
  -- exactly as it already did for `some?`.

- **SR3's Option niche is shelved, not shipped.** The representation was gated
  behind `TUR_SR3_OPTION_NICHE=1` (default off) and measured. It works, and one
  erased-crossing bug it exposed is fixed regardless -- a typeclass `Eq`
  dictionary read the low half of a spilled niche pointer as a tag and returned
  a silent wrong answer for two equal `(some v)`. What shelves the phase is the
  population: the niche claims `0` for `None`, so every payload that has already
  spent its null (`Cons`'s `nil` *is* `0`) is ineligible.

### Fixed

- **Seven representation-crossing defects** the by-value default exposed, each
  one a place where two spellings agreed only because a parametric sum monomorph
  and a type variable both c-named to `int64_t`. The sharpest: the
  carrier-to-by-value readback's NULL guard lived in the pre-sum record branch,
  so reading a `(none)` built by inline-C dereferenced the null carrier. Also a
  match on an erased instance base's parameter binding an aggregate from an
  `int64_t` slot, a match resolving its element from a different instantiation
  than the active specialization, an Option/Result pointer-box payload bound as
  a value, an argument spilled to a carrier sink at its static rather than its
  specialized type, a poly-wrapper argument unboxed twice, and the catch-unwind
  group trampoline saving an aggregate-returning member through a scalar cast.

- **CI suites that were passing by not running.** The browser job's desktop step
  runs a fixed spec list, so `minimap.spec.js`, `footer.spec.js` and
  `lsp.spec.js` asserted nothing until they were named (51 tests -> 87). The
  mobile project is WebKit but the job only installed Chromium, so all 32 of its
  tests died at launch behind a `continue-on-error`. Also `tur fmt`'s canonical
  one-line form for the retyped `ok?`/`err?`, and a `docs/reported/README.md`
  row left pointing at an archived report.

## [0.39.0] -- 2026-08-27

### Added

- **Offline documentation, in the browser and out (OD1-OD5).** The guides and
  API pages are rendered once and wrapped twice: the site keeps its chrome, and
  a chrome-free *docs pack* feeds an in-app docs browser in Try Turmeric, so
  reading a guide no longer navigates away from your editor buffer, console
  history, or WASM session. The pack is precached unconditionally on install --
  no toggle, no first-run prompt -- with `#doc=guides/...` deep links, one
  search box over pages and symbols, "Load into editor" on every runnable code
  block, and a remembered scroll position per page. Outside the browser,
  `tur doc <symbol>` answers from the stdlib docstring table rather than just
  the builtin list, and `tur docs [--open|--serve]` locates the rendered
  documentation (`$TUR_DOCS_DIR`, then `<prefix>/share/doc/turmeric`, then
  `<repo>/docs/html`); `--serve` is a loopback-only, GET-only static server for
  the browsers that refuse `file://` navigation.

- **`--enable=parametric-sum-byvalue` (beta).** Parametric sum monomorphs
  (`(Option int)`, `(Result float cstr)`, ...) flow by value with no per-ctor
  malloc, instead of riding the int64 heap carrier. The default path is
  unchanged; the experiment is the staging ground for making by-value the
  default (SR2 graduation). Plan: `docs/archive/sr2-gate-results.md`.

- **Lazy solution streams in `stdlib/logic.tur`.** `Stream` gained the immature
  constructor `(StInc :StThunk)` plus `st-force` / `st-pull`, so `run-logic n`
  now costs n solutions instead of running the whole search and truncating the
  result. `st-append` swaps on an immature stream, which is fair interleaving;
  `st-bind` defers through one rather than forcing it. **A relation with
  infinitely many solutions is now expressible at all** -- previously
  `(defn nats [] (disjoined (succeed) (nats)))` did not merely diverge, it
  SIGSEGVed while the goal was being *built*.
- **`zzz`, a delay macro for recursive relations.** `(disjoined (succeed) (zzz
  (nats)))` terminates where the undelayed form crashes. It is a macro by
  necessity: a function would evaluate its argument at the call site, which is
  the divergence it exists to prevent.
- **`disjoined-dfs` / `st-append-dfs` -- depth-first search that is still
  lazy.** `disjoined` interleaves and is complete; this pair keeps depth-first
  order for goals whose left branch is known finite, reaching a solution at
  depth 18 in 0.012s against 3.5s interleaved. Incomplete by construction -- it
  never reaches the right branch of a goal whose left branch is infinite.
- **`stdlib/trail.tur` -- backtrackable state**, behind
  `--enable=backtrackable-state`. Trailed cells whose writes undo to a mark,
  with the opt-out at three granularities: per cell (`g-cell-new`, never
  trailed), per write (`untrailed-begin` / `untrailed-end`), and per level
  (`bt-commit-to!`). `BtCell` and `GCell` are distinct types so opting out is
  visible in a signature. Recording and undoing state measures ~5ns per write,
  roughly 5x cheaper per unit of live state than capturing and restoring
  control.
- **`tur smt <file.smt2>`** runs an SMT-LIB2 script through the refinement
  solver's staged chain and prints `sat` / `unsat` / `unknown`, which stage
  decided, and a model when the bounded search finds one. Exit codes mirror the
  answer (0 unsat, 1 sat, 2 unknown, 3 error) so a shell harness can branch on
  `$?` without parsing stdout.
- **`--dump-refine=json`** emits one record per refinement obligation --
  location, predicate, verdict, deciding stage, counterexample, the replayable
  VC as SMT-LIB2, and which caps bit for that obligation. Works on `check` as
  well as `emit-c`. The schema is explicitly unstable and says so in every
  record (`"schema": 0`).
- **`tests/run-leak-check.sh`** runs compiled fixtures under LeakSanitizer, opt
  in per fixture with a `requires.leak-check` marker. This generalizes three
  bespoke per-regression harnesses; coverage went from 2 fixtures to 54.

### Changed

- **`Option` and `Result` are real sums now** (SR2b,
  `docs/archive/sum-representation-plan.md`). `(defdata Option :copy [A]
  (None) (Some A))` and `(defdata Result :copy [A B] (Ok A) (Err B))` replace
  the discriminated records; every stdlib accessor and instance is
  match-based, and you can `match` the variants directly. The runtime layout
  is the tagged monomorph `{ int tag; union { ... } as; }` -- 16 bytes for
  both (`Result` down from 24), tags in declaration order, payload at offset
  8. The dead-arm write is gone, so an error type no longer needs a zero
  value. **Inline-C contract:** hand-rolled `{ bool is_ok; ... }` /
  `{ bool is_some; ... }` structs read the wrong bytes now -- build and read
  through the preamble helpers (`tur_box_ok` / `tur_is_ok` / `tur_box_some` /
  `tur_is_some` / ...), which carry the canonical layout and a
  `_Static_assert` pinning it. The interpreter builds and matches the same
  constructors, with the legacy box shapes still readable.
- **`(none)` allocates nothing** (SR3 slice A). The carrier `None` is the
  null pointer -- every reader already treated NULL as none, so the tagged
  box whose only content was `tag = 0` was pure allocation. A 2e6-iteration
  `(none)` loop peaks at 10.3 MB RSS where the still-boxing `(some i)` twin
  peaks at 64 MB. A tagged None box remains valid on the read side.
- **`Option` and `Result` monomorphs lower by value when a type argument is
  itself a monomorph.** `option<list<int>>`, `result<vec<T>, cstr>` and
  `option<(Pair a b)>` previously fell back to the heap carrier -- a silent
  representation downgrade that reintroduced a `malloc` per construction on some
  of the most common shapes in the language.
- **`NO_MAX_SHARED` raised 8 -> 16.** It was the only cap in the refinement
  solver with a live signal, turning away eligible terms on four units and
  always by exactly one. No verdict moved on the 125-benchmark corpus or the 89
  in-tree refinement fixtures, and the corpus replay did not slow measurably.
- **`-main` is no longer documented as an entry point.** Nothing ever called it:
  both shipped examples and the snake tutorial used it, and `examples/minikanren`
  built, linked, ran, exited 0 and printed nothing. Both examples and all 24 of
  the tutorial's listings now use `main`.

### Removed

- **Twelve graduated `--enable=` compatibility shims retired.** A `GRADUATED[]`
  entry is a migration window, not a permanent alias: a name ages out one minor
  line after graduation and goes back to being the hard `TUR-E0310` an unknown
  name gets. Five backend names went first (`cps-effects`, `cps-tramp-resume`,
  `cps-async`, `owning-cloneable-capture`, `closure-drop-glue`) -- no source
  syntax to adopt, so nobody had reason to name one in a build. Then the seven
  that gated source syntax and had each had a full minor line: `refined`,
  `cycle-gc`, `jit`, `sealed-opaque`, `global-state`, `write-frames`,
  `checked-reads`. `#lang turmeric refined` is `TUR-E0330` again -- a semantic
  layer *is* its experiment, so the two shims retire together. `jit-ffi`
  graduated at 0.38.0, so its window opens now and it is the sole survivor.
  **If a `build.tur` or `experiments.tur` still names a retired flag, drop the
  name** -- the feature it gated needs no enable at all.

### Fixed

- **Statement position deleted wrapped expressions -- a high-severity
  miscompile.** `emit_stmt` treats four pure *wrappers* (`EX_REINTERPRET`,
  `EX_CAST`, `EX_ASCRIBE`, `EX_POLY_WRAP`) as pure in statement position and
  emitted nothing for them. The wrapper is pure; the call inside it is not, so a
  discarded parametric call -- whose result rides the int64 carrier and is
  therefore wrapped by elaboration to restore its instantiation type -- vanished
  along with its effects.
- **The aarch64 HFA ABI is correct in the JIT.** AAPCS64 passes a struct or
  array of 1-4 same-typed FP members in `v0..v7`, one per register; MIR's
  aarch64 back end had no HFA concept and routed every aggregate through the
  integer argument registers. Self-consistent inside one c2mir compilation and
  wrong the instant c2mir code met natively compiled code. The MIR pin moves to
  the upstream fix, and both interim refusals it forced are dropped. The
  compiled `tur jit` path, which had no check at all, miscalled an ordinary
  `extern-c` with a record parameter; it now refuses (`TUR-E0711`) where the fix
  does not reach.
- **One convention for wide by-value aggregates at every fat boundary.** A
  lifted thunk and its dispatch site could disagree about how a >8-byte
  by-value aggregate parameter crosses a fat-closure boundary: a hard `cc` error
  through a `^fat` sink, a **silent wrong answer** through the untyped `:fn`
  carrier, and a SIGSEGV through a typed fn-field, whose cast is exactly what
  hid the disagreement from the C compiler.
- **`tur fmt` silently deleted comments inside bracket vectors.** Every `;` /
  `;;` / `;;;` comment inside a `defstruct` field vector, a `defn`/`fn`
  parameter vector, or a `let`/`loop` binding vector was dropped in place, exit
  0, no diagnostic. The non-idempotence (`tur fmt` then `tur fmt --check`
  exiting 1 on the file `fmt` just wrote) was the downstream symptom.
- **`(fn name [...])` points at `letrec` instead of a bracket error.** The
  Scheme/Racket/CL spelling of a self-recursive lambda was rejected with
  "parameter list must be a vector", caret on the name, with a well-formed
  vector sitting one token later -- a message that invited the reading that
  Turmeric has no recursive lambdas. It has two, and the diagnostic now names
  both.
- **Two modules could silently share one API page.** `just docs` wrote 147 pages
  for 148 modules: the filename-derived fallback name keys on the *basename*, so
  `stdlib/capability.tur` and `stdlib/test/capability.tur` both rendered to
  `tur-capability.html` -- the shipped page held the test mocks and the real
  module had none, while the index still showed both cards.
- **`vec-of` over a parametric sum monomorph no longer ICEs.**
  `(vec-of (Yep 8) ...)` over a two-variant sum died at the let binder on the
  default path (the Vec registration and the binder disagreed about the
  element's representation); `vec<option<T>>` is that shape. Fixed by the
  SR2b representation predicates; the report is archived.
- **`rc/of` did not release a multi-variant ADT payload.** It allocated a second
  box for the carrier word and freed only that wrapper, so the payload leaked --
  code doing exactly the documented thing lost 16 bytes per value.
- **`ref/from-rc` and `(upgrade w)` leaked at the ownership handoff.** Two
  unrelated call sites, one shape: a heap allocation handed across an ownership
  boundary to something that never freed it.
- **A closure stored in an ADT field emitted C that warned.** A `defopaque`
  over `:ptr<void>` is a named `int64_t` carrier, and a closure ascribed to one
  still lowers to a pointer, so the store was an int/pointer straddle the
  emitted C complained about. This is what any ADT holding a callback hits.
- **`fat_captures_borrowed` was read out of uninitialized memory.** The flag
  suppresses one specific use-after-free; 60 fixtures were reading it as garbage
  on every compile, and nothing failed because UBSan here prints and continues
  rather than aborting.

### Docs

- **Eight dead guide cross-links fixed, and `--strict-links` armed** so the next
  one fails the docs build instead of shipping a 404 to turmeric-lang.com.
- **The logic guide no longer tells you to hand-write an interleaving `mplus`.**
  `st-append` is the interleaving one now, and the section explains why the
  hand-written version it used to recommend could never have worked: it swapped
  its arguments but built a strict cell, and interleaving without an immature
  result is half a mechanism.
- **The test-suite portability guide gained a section on what the sanitizers
  actually catch** -- ASan aborts, UBSan does not, and the suite now collects
  UBSan findings from the compiler and reports them after the summary
  (`TUR_SANITIZER_GATE=1` makes them fatal).

## [0.38.0] -- 2026-08-21

### Added

- **Dynamic FFI carries by-value records in every direction.** `call-ptr` and
  `callback-ptr` take and return records (including nested ones), a callback can
  receive and return aggregates, and `extern-c` gained by-value record
  parameters and returns. Under `--interpret` this routes through the c2mir
  thunk provider and needs a `-DTUR_JIT=ON` build; compiled code needs nothing.
- **Nested constructor patterns in `match` arms**, and `#json-str?<T>` -- a
  `Result`-returning typed JSON decode.
- **`:global` spice dependencies** -- consume a globally installed spice as a
  library.
- **`mw-recover` in httpd**, and a panic boundary around every task: a panic
  inside an `(async ...)` body no longer unwinds the spawner and aborts the
  process; `await` re-raises it instead.

### Changed

- **`jit-ffi` graduated: `call-ptr` and `callback-ptr` need no `--enable`.**
  They are ordinary `unsafe` forms (a lingering `--enable=jit-ffi` is a
  `TUR-W0063` no-op). The gate existed only so the signature vocabulary could
  move; that vocabulary is now settled and measured. The `-DTUR_JIT=ON` build
  gate on the interpreter path is unchanged, and `unsafe` is still required.
  `EXPERIMENTS[]` is now empty.
- **`stdlib/args` names real handle types.** `ArgSpec` / `ArgResult` are
  `defopaque` instead of bare `:int` across all 18 entry points, and an option
  default is `(Option cstr)` rather than a `cstr` smuggled through `:int`.
- **Float division no longer emits the integer divide-by-zero guard**, so IEEE
  inf/NaN semantics survive instead of trapping.

### Fixed

- **`(cast a OtherStruct)` on an `any` succeeded and reinterpreted the
  payload.** Every struct boxed as `TY_STRUCT` and every ADT as `TY_ADT`, so the
  tag check compared equal between unrelated types, and `type-of` answered
  "struct" / "adt" for all of them. A struct/ADT payload now interns its own
  type.
- **A narrow C return value was read as garbage.** A callee returning `int`
  leaves the upper half of the return register unspecified, so a thunk declared
  `long long` read whatever was there -- `neg_int(1234)` came back as
  `4294966062`. The FFI signature vocabulary is exact-width now.
- **Nested by-value record fields were marshalled to the wrong shape under
  `--interpret`**, on every architecture -- `{{ww}w}` passed as `{qw}`, silently
  wrong answers. Found by the x86-64 verification that had never been run.
- **`match` binds the variable of an ADT catch-all arm.**
- **`catch` keeps the payload's representation on both engines**, including an
  aggregate-returning thunk and a float payload read from the erased `Result`
  carrier.
- Codegen: a divergent-tail function's trailing `return`, inline C naming a
  local, forward-declared globals a lifted lambda reads, and the fn-typed
  if-merge temp.

### Docs

- Every example that compiles is now also run in CI, and the ECS benchmark
  landed.

## [0.37.0] -- 2026-08-20

### Added

- **`tur audit` -- where this build fetches code from.** Reads `build.tur` plus
  `tur.lock` and prints every origin, spices and `:cmake-deps` in separate
  sections, with URL, ref, subdir, and the resolved commit and SHA-256 where
  the lock has pinned it. Unpinned origins are called out with the fix. It
  verifies nothing, and says so on every run.
- **A concurrency stdlib layer**: `arc.tur` (the language surface over the Arc
  runtime), `barrier.tur` (a reusable counting barrier), `stm-sync.tur`
  (`TMVar` and `TChan` over `tvar` + `check`), and `with-lock` /
  `with-read-lock` / `with-write-lock`.
- **`(export-from <mod> name ...)`** -- re-export a name from another module
  without importing it locally.
- **`:entry` in `build.tur`**, `#map{}:(K V)` typed-empty map literals, and
  `cstr-eq?` / `cstr-free` in `stdlib/cstr`.
- **A verification tier for `#reads` frames.** A deferred footprint walk
  reports VERIFIED / EXCEEDED / UNVERIFIED per frame via `--dump-read-frames`,
  and an EXCEEDED reached through a callee's own frame -- a read the
  definition-site scans cannot see -- joins the `TUR-W0383` evidence tier.

### Changed

- **`write-frames` graduated: a `#writes` frame is checked without
  `--enable`.** WF2's three verdicts (VERIFIED, EXCEEDED -> `TUR-E0382`,
  silent UNVERIFIED) and WF3's borrow widening are now unconditional. WF4's
  entry-check elision was retired before graduation -- the check it proposed
  to elide does not exist -- so what graduated is a checker that reports a
  broken promise, not an optimization acting on one.
- **`checked-reads` graduated: a broken `#reads` frame no longer buys a
  proof.** When a measure's body demonstrably reads mutable state its frame
  omits, the congruence override is refused and the crossing becomes the
  ordinary `TUR-W0372` (a hard error under `--strict-refine`). Refusal keys on
  "saw a read", never "could not see", so an inline-C measure -- essentially
  every measure predating mutable globals -- is unaffected. Note what this does
  not do: a `#reads` crossing is proof-only, so refusing buys a diagnostic
  rather than a check, and outside `--strict-refine` the program still runs.
- **`schan-recv` returns `(Pair T (SChan R))`** instead of writing through an
  out-parameter.
- **An int literal ascribed to a float is the number, not its bits.**
  `(:: 3 :float)` printed `1.4822e-323` -- the double whose bit pattern is
  `0x3` -- and now folds to `3.0`. The tell that this was an accident rather
  than a semantic: `(:: 3 :float32)` already printed `3`, because the
  same-width reinterpret rule missed at 8 != 4.
- **The Send-across-await check runs at every await point**, not just the
  first.

### Fixed

- **A SIGSEGV passing a let-bound non-capturing lambda as a `:fn` argument.**
- **`TUR-W0033` fired on the very `(unsafe ...)` block it requires.**
- **`tur run test` now reaches ctest** and passes 108/108.

### Docs

- **A repo-wide documentation accuracy pass.** The guides that had drifted from
  the shipping API were rewritten against it (performance, logic,
  checkpointing, the quickstart tutorial, the datalog examples), and 33 reports
  were filed for what could not be fixed in place.

## [0.36.0] -- 2026-08-19

### Added

- **`defmacro*` -- procedural macros that run on a macro-time interpreter
  environment.** A macro body is ordinary Turmeric evaluated at expansion time
  over a first-class `Syntax` value (a new `TY_SYNTAX` compile-time kind, syntax
  natives, and quasiquote that produces `Syntax`), with the stdlib preloaded
  into the macro env; the derive-family migrated onto it as proof.
- **`(import m :for-macros)` -- cross-module macro-time dependencies.** A module
  imported for macros is loaded into the expansion env rather than the runtime
  one, so a procedural macro can call helpers defined elsewhere. Macro-time I/O
  is denied by default.
- **Procedural reader macros by composition, plus R3-bounded reflection** -- a
  reader macro is an ordinary `defmacro*` composed into the read step.
- **`tur expand` and REPL `:expand`** -- one expansion step at the command line
  and at the prompt. `defmacro` bodies may now hold multiple forms, and gensym
  is unified across the expander.
- **jit-ffi F4/F5 -- struct-by-value through `call-ptr`, and callbacks.** A
  `(unsafe (call-ptr ...))` signature can now pass and return structs by value,
  and a Turmeric function can be handed to C as a callback on both the compiled
  and the interpreted path. The aarch64 FP-aggregate ABI wall F4 hit is
  reported in `docs/reported/mir-aarch64-fp-aggregate-abi.md`.
- **`tur completion <zsh|bash>` -- shell completion.** Completes subcommands,
  per-subcommand flags, and `.tur` file arguments; for `tur run` it completes
  recipe names out of whatever Justfile encloses the directory being completed,
  using their doc comments as descriptions. The scripts are embedded in the
  binary, so `source <(tur completion zsh)` bootstraps anywhere with no
  install-prefix lookup. The Homebrew formula installs both.
- **`tur run --list --all`** shows recipes that are normally hidden.

### Changed

- **`#reads` may name multiple parameters** (`#reads [a b]`), and a mutable
  global is never frozen by a `#reads` frame -- a callee's write to a global the
  frame named no longer survives as an unearned congruence grant (soundness).
- **`[private]` and `_`-prefixed recipes are honored rather than rejected**,
  matching `just`: hidden from `tur run --list`, still runnable by name.

### Fixed

- **Two Result box/struct bridging codegen bugs** -- a CPS-path Result unbox was
  dropped, and a by-value product tail in a `result` block was double-unboxed.
  Every parametric by-value product tail is now covered; the remaining
  non-parametric shape is reported rather than miscompiled.
- **A heap join whose body escapes to an enclosing join** emitted an invalid
  assignment; it is now evicted to the direct emitter.
- **`__TUR_CNAME_` broke on leading underscores**, and a `let` binding of a
  `:void` expression emitted invalid C -- now a clean TUR-E0023.
- **Malformed or duplicated `#reads` frames** are diagnosed (TUR-E0024).
- **`tur run --list` omitted aliases.** `alias b := build` is runnable --
  `find_recipe` resolves it, and the "recipe not found" error even printed
  aliases in its `available:` line -- but the listing walked only the recipe
  table, so aliases were invisible to any tooling built on it.
- **One unsupported Justfile feature blanked the whole listing.** A `[private]`
  recipe or a `mod` line -- both fine under real `just` -- aborted the parse
  with exit 2 and no output. Unsupported features now degrade `--list` (note on
  stderr, remaining recipes still listed) while staying fatal when a recipe is
  actually executed.
- **`tur run --list --json` escaped only `doc`.** Recipe names and parameter
  defaults were emitted raw and control characters passed through, so a default
  like `flags='-DFOO="bar"'` produced invalid JSON.

## [0.35.0] -- 2026-08-18

### Added

- **`(unsafe (call-ptr p [T1 T2 -> R] args...))` -- call a raw function pointer
  through a JIT-compiled thunk**, behind `--enable=jit-ffi`. The thunk is
  rendered from the signature string, compiled through c2mir, and cached per
  unique signature, so a JIT build calls a C function of any arity with no
  `--max-arity` ceiling. It is not a new expression kind -- the signature hangs
  off the ordinary call node, so every walker traverses it unchanged. F1-F3 of
  docs/upcoming/jit-ffi-c2mir-plan.md; struct-by-value (F4) and callbacks (F5)
  deliberately trail.
- **`extern-c` stops lying under `--interpret`.** In a JIT build an `extern-c`
  registration resolves the symbol via `dlsym` and binds a thunk-backed native,
  so `(strtol "123abc" 0 10)` is `123` where everything outside a 7-entry known
  table used to silently return nil. The known table stays as the
  semantics-bearing override (`free` no-op, `exit`, `printf` marshalling).
- **A dialect picker and layer toggles in Try Turmeric.** The editor header
  gains a Language control -- a radio group for the four base dialects and
  checkboxes for the curated `#lang` layers -- rendered from a new WASM registry
  export so the UI never becomes a second source of truth. The `#lang` line
  stays authoritative: typing it by hand and using the picker are the same
  operation, and one Ctrl+Z undoes a switch. Turning a reader layer off now
  genuinely deactivates its `#`-dispatch, where `#s"..."` used to keep reading
  as `String` after `stringed` was switched off.
- **Variadic spice exports are callable from the REPL**, and a spice that
  declares its C dependency the recommended way (`:cmake-deps` / `:link-libs`,
  no `__tur_autolink__` marker) now loads through the REPL's in-process JIT
  hook, falling back to the subprocess build when the hook cannot handle it
  (vendored `:c-sources`, static-only cmake deps).
- **Rust and Haskell benchmark columns, and `tur jit --timing-json`.** 21
  programs each, validated byte-for-byte against the existing goldens, plus a
  phase record (`compile_ms` / `run_ms` / `engine`) so a chart can subtract
  compile time and a `cc` fallback is detected rather than averaged in. The
  existing language list is unchanged; the new columns ride on top.

### Changed

- **`global-state` graduated -- four mutable-global features work without a
  flag.** A `#writes` frame may name a mutable global, an exported global is
  read-only outside its defining module (write it from another module and you
  get a diagnostic naming the owner and `(export (mut g))`), and `^atomic` /
  `^thread-local` are ordinary annotations on a top-level `def`. Every phase of
  docs/archive/mutable-globals-plan.md had landed, so the row had nothing left
  to decide. A lingering `--enable=global-state` is a `TUR-W0063` no-op for one
  minor line, not an error.

  One tightening rides along, and it is confined to `--enable=write-frames`
  (still experimental, and what *checks* a frame at all): a body that declares a
  frame and writes a global the frame does not name is now `TUR-E0382`, where it
  previously just declined to verify.

### Fixed

- **The macOS CI 45-minute hang.** `httpd-stop-async` left the listen fd open
  until `httpd-async-free`, so the kernel kept completing handshakes into the
  backlog after the stop; a client that got one blocked forever in `recv()`,
  deadlocking `main` in `pthread_join`. Both stop paths now close the listener,
  so pending backlog connections are reset and late connects refused rather than
  black-holed. Reproduced 100% by delaying one client 300ms, 0% after, and
  stressed 200 runs at 6x thread oversubscription.
- **`kqueue` write knotes were never deleted.** `EV_DELETE` passed
  `EVFILT_READ | EVFILT_WRITE`, but kqueue filters are enum values (-1, -2), not
  a bitmask, so the OR collapsed to `EVFILT_READ` and a stale WRITE knote could
  deliver a wake on a reused fd.
- **`:build-opts :link-libs` reaches the link line.** It was parsed, documented,
  and round-tripped by `tur init` -- and consumed by nothing.

### Docs

- **A dynamic FFI guide**, with a worked `libzmq` example, plus a plan for
  spice-level FFI integration.

## [0.34.0] -- 2026-08-17

### Added

- **`TUR-W0383`: a `#reads` frame that omits mutable state the body reads now
  warns at the definition.** `#reads` is trusted, and its one consumer grants
  congruence -- so a measure declared `#reads w` whose body also reads a
  mutable global was silently buying proofs it had not earned (the elided
  caller-side crossing check the `refine-reads-frame-omits-global` fixture
  pair pins). The warning is gateless and changes nothing proved: it reports
  positive evidence of the broken promise (a direct read of a `^mut` global in
  the elaborated body) without yet refusing the override. An inline-C body
  yields no evidence and stays silent, so every pre-existing measure is
  unaffected. `tur --explain TUR-W0383` has the full story.
- **`--enable=checked-reads`: refuse the `#reads` congruence override on
  broken-frame evidence.** The gated escalation of TUR-W0383: on the same
  positive evidence (the measure's body directly reads a mutable global), the
  refinement encoder declines the congruence grant, so a crossing that used to
  be proved from the broken promise becomes an undischarged TUR-W0372 -- with
  wording that says the *frame* failed, not the region ("fix the frame, not
  the region"), since the usual "guard it inside a `frozen` region" advice is
  misleading when the region is present. A hard error under `--strict-refine`.
  Refusal keys on "saw a read", never "could not see": an inline-C measure --
  essentially every measure that predates mutable globals -- carries no
  evidence and keeps today's trusted behavior even with the gate on. R2 of
  docs/upcoming/trusted-refinement-claims-plan.md.
- **An execution engine can be selected per project.** `:engine "cc" | "jit" |
  "interp"` in `build.tur`, `--engine <name>` on the command line, or
  `TUR_ENGINE` in the environment, resolved in that precedence with `"cc"`
  last -- the same ladder `:build-dir` already used. `tur init` round-trips the
  key. There is **no silent substitution**: an unknown value is a hard error
  (`TUR-E0311`, with its own `tur explain` entry) and asking for `"jit"` on a
  build without the engine names `-DTUR_JIT=ON` and the override spellings
  rather than quietly falling back, because the engines differ in *semantics*
  and not just speed. Unknown manifest *keys* are still silently ignored, which
  is the documented compatibility story.
- **`tur repl --engine <name>`.** Selects the engine that builds the enclosing
  spice: `"cc"` (the `tur build --shared` subprocess, still the default) or
  `"jit"` (compile the whole spice in process through MIR, no `.so`, no
  `dlopen`). Reads the same precedence ladder as above, so `TUR_ENGINE=jit` or
  `:engine "jit"` in `build.tur` work too.
- **An error inside macro-generated code now names the call that generated
  it.** Template spans survive expansion, so a diagnostic used to point into
  the `defmacro` body with nothing tying it to the code the user actually
  wrote. One note is appended at the call site -- "in expansion of macro
  'name' -- the diagnostics above are inside code this call generated" -- on the
  outermost frame only, so nested macros get a single note at the user-visible
  call and a clean expansion followed by an unrelated error gets none.
- **`maximum macro expansion depth exceeded` carries a hint** naming the two
  measured causes of a base case that never fires: `nil?` on an empty `^syntax`
  rest (an empty rest is an empty *list*, so `empty?` is the predicate), and
  counting-driven recursion (the compile-time evaluator has no arithmetic, so a
  spliced `(- n 1)` recurses on the unevaluated form).
- **`^thread-local` on a top-level `def`**, behind `--enable=global-state`.
  Each thread gets its own copy, materialized on first access and initialized by
  running the declared initializer *on that thread* -- so
  `(def ^thread-local buf (make-buf))` gives each thread its own buffer rather
  than sharing one. Lowered to a `pthread_key_t` holding one per-thread block,
  not `__thread`: C has no dynamic thread-local initialization, and the JIT's
  c2mir has no thread-local storage at all (it would silently share one slot
  across threads). The key's destructor frees the block on thread exit. Under
  `tur --interpret` it is a plain global -- turi has no user-reachable thread
  spawn, so there is no second thread for it to differ on. Does not combine with
  `^atomic`, and its initializer may not reference another `^thread-local`.
- **`^atomic` on a top-level `def`**, behind `--enable=global-state`. Every read
  of a `^atomic ^mut` global lowers to a sequentially-consistent load and every
  `set!` to a sequentially-consistent store. The practical benefit is as much
  the *load*: a bare global read in a loop may be cached in a register, so a
  spinning reader would never observe another thread's store however atomically
  it was made. **It does not make `(set! c (+ c 1))` safe** -- that is a load
  then a store, not an atomic read-modify-write, and two threads still lose
  updates; use `stdlib/atomic.tur`'s CAS/fetch-add or a lock. Eight-byte scalars
  only (`:int`, `:float`, `:cstr`, `:ptr`); anything else is refused with a
  reason. `^atomic` does not imply `^mut`.
- **An exported global is read-only outside its defining module**, behind
  `--enable=global-state`. A module that exports a counter for reading no longer
  thereby exports it for writing; `set!` on another module's global names the
  owning module and both ways out. The permission is granted at the definition
  site with `(export (mut g))` -- reusing the structured-export form
  `(export (effect Name))` established, rather than adding an annotation -- so
  the decision sits with the code that owns the invariant. Only bites across a
  real module boundary: single-file programs and in-module writes are
  untouched. `(mut ...)` on a function or an immutable global is rejected by
  name rather than left inert.
- **A `#writes` frame may name a mutable global**, behind
  `--enable=global-state`. `(defn bump! [] #writes [hits] : void ...)` lets a
  body that maintains global state carry a *checked* frame instead of being
  declined outright, and a frame may mix the two (`#writes [a hits]`). Coverage
  works as it does for parameters: writing a global the frame does not name is
  `TUR-E0382` naming the global, declared-but-unwritten is fine (a frame is an
  upper bound), and an unresolvable body is UNVERIFIED. Naming an immutable
  global is `TUR-E0381` with its own reason. Deliberately `#writes` only --
  `#reads` grants congruence, so a global there would let a promise about
  mutable global state pay out in proofs.
- **`--dump-write-frames`** prints the checked verdict for every declared
  `#writes` frame, with the frame's own verdict and the global-write answer as
  separate columns (`frame=VERIFIED global=YES`). A diagnostic knob, not an
  experiment: it reports what the checker decided and changes nothing.

- **`^mut` on a top-level `def` -- mutable globals.** `(def ^mut hits 0)` gives
  static storage that `set!` may write. This closes a dead end: `set!` on a
  global already advised "use `^mut` at the binding site", and the binding site
  rejected `^mut`, so the only fix the diagnostic named did not exist. Without
  the annotation a global stays immutable and `set!` on it is still an error.
  A `^mut` global is process-wide mutable state with no synchronization --
  nothing checks that it is shared safely across threads.
- **`def` annotations may appear in any order**, matching `let`:
  `(def ^mut ^persistent c ...)` and `(def ^persistent ^mut c ...)` are the
  same declaration.

### Changed

- **Three experiments graduated: `cycle-gc`, `sealed-opaque`, and `jit`.** Each
  gate had nothing left to decide. Existing opt-ins keep working and can be
  deleted at leisure -- `--enable=<name>`, `:experiments [<name>]` in
  `build.tur`, and the user experiments file are all accepted as no-ops with
  `TUR-W0063`.

  - **`(gc-auto!)` is an ordinary call form.** What graduated is the *call*, not
    a default. `GC_AUTO` remains strictly opt-in, permanently: a program that
    never calls `(gc-auto!)` still runs the pure-RC path with no collector
    overhead, and the AUTO-only allocation cost is conditional on the mode at
    run time. **Automatic GC is not becoming the default in this language**,
    before or after v1. That the two decisions were separable is the whole
    reason ungating cost a non-calling program nothing. Baked from 0.30.8 across
    the 0.31-0.33 lines, with pause time fixed, the allocation-path cost
    measured (~10%, fixed overhead rather than per-byte), and steady-state
    residue on a real-shape workload measured at ~60 blocks.
  - **`:sealed` on a `defopaque` now enforces on every compile.** Outside the
    declaring module, `::` refuses both the unwrap and the fabricate direction
    (`TUR-E0302`), closing the extract-and-re-wrap aliasing hole that otherwise
    bounds every guarantee built on an opaque handle. Unusually low-risk for a
    graduation: with the gate off `:sealed` already parsed and imposed nothing,
    so this reaches only code that deliberately *wrote* `:sealed`. The
    two-direction rule the gate existed to question survived the one spice that
    adopted it, so it graduates as designed rather than narrowed to
    fabrication-only. Still a compile-time discipline over the `::` surface --
    inline-C can cast an `int64_t` to anything, so this raises the bypass from
    "one `::` away in ordinary code" to "requires deliberate inline-C" and does
    not claim more.
  - **`tur jit` no longer needs a run-time flag.** The parity condition the gate
    was holding for is discharged: the whole fixture corpus runs through the
    engine on both hosts with an empty denylist. **The build-time gate stays and
    is now the only one** -- `-DTUR_JIT=ON` vendors MIR at configure time, a
    default build carries neither the fetch nor the dependency, and `tur jit` on
    such a binary says so. `cc` is still the default engine; the JIT runs when
    you invoke `tur jit` or when engine selection asks for it.

  One thing moved rather than being deleted: `--enable=jit` was the only switch
  that turned on the in-process REPL spice loader, so removing it would have
  forced a choice between making that path the default and losing it. It now
  hangs off engine selection (`tur repl --engine jit`), which is why `tur repl`
  grew `--engine` in this release. Unset, the subprocess path is unchanged.
- **A capturing closure passed to an effect-row'd `(fn ...)` parameter is now
  `TUR-E0007`.** Such a parameter keeps the thin calling convention, which has
  nowhere to carry a closure environment, so the callee jumped into the
  environment box as code -- a clean compile and a SIGBUS at run time. Only the
  capturing, non-performing shape reached the crash (a capturing callback that
  *performs* already died loudly at CPS-subset eviction). Concrete, empty, and
  row-variable rows all rode thin and all crashed; all three are now refused at
  the call site. Annotating the parameter `^fat` is the way to accept one.
- **`tur build <dir>`, `tur check <dir>` and `tur test <dir>` walk
  subdirectories.** They used a flat `readdir`, so a spice whose modules live
  one level down -- `src/demo/lib.tur`, the layout `:exports "demo/lib"` implies
  -- reported `no .tur files found in 'src/'`. That was the exact invocation the
  `module not found` diagnostic recommends, so the advertised recovery from one
  confusing error produced a second one. Project mode already recursed, so the
  two spellings of "build this spice" disagreed. `<dir>` also goes on the
  include path as its own module root now, without which finding the files
  merely moved the failure to `module 'demo/lib' not found`.
- **`def` and `define` are one form; position, not spelling, selects the
  meaning.** `def` at the top level is a global binding (unchanged, and
  redefining is still an error); `def` in a body is a binding scoped over the
  rest of the body -- what `define` has always done. `define` is an accepted
  alias for `def` in both positions. Nothing that compiled before compiles
  differently: every change is a position or spelling that used to be an error
  becoming legal. See
  [docs/archive/def-define-consolidation-plan.md](docs/archive/def-define-consolidation-plan.md).
- **A name defined at the REPL prompt with `define` now survives to the next
  turn.** `define` used to error at the top level, so the REPL worked around it
  by wrapping each turn containing one in an implicit `(do ...)` -- which also
  scoped the binding to that single turn. A top-level `define` is a real
  top-level binding now, so the wrap is gone. Anyone relying on the
  turn-scoped behaviour was relying on a workaround.
- **A body binding takes a `: type` ascription**, in either the spaced
  (`(def x : float 7.1)`) or fused (`(def x :float 7.1)`) spelling, matching
  top-level `def` and `let`. It used to be an error.
- **A `def`/`define` in an expression position gets an explanation instead of a
  rule.** `(if c (def x 1) ...)` now says the binding has nothing to scope
  over, and names the positions that would work.

### Fixed

- **Performing an effect inside a fiber no longer kills the process on
  Windows.** The DK tail-resume trampoline lands via `longjmp`, and on win64
  that is a real SEH unwind: `RtlUnwindEx` validates every frame against the
  thread's stack bounds as recorded in the TEB, and a fiber stack is `malloc`'d
  and invisible there. Every frame on it was out of bounds, so the unwind raised
  `STATUS_BAD_STACK` and the program died before printing anything. The landing
  now uses `__builtin_setjmp`/`__builtin_longjmp` on Windows -- a plain
  SP/FP/PC save-restore with no unwinder, which is all the trampoline ever
  wanted. Linux and macOS are byte-identical. Not fixed on the JIT path: c2mir
  has no such builtin, and both halves of a split-runtime program must agree on
  the mechanism, so the split keeps plain `setjmp`. `call/cc` and panic-in-fiber
  have the same defect and are still open.

- **A runaway macro on a sanitizer-instrumented (Debug) build now reports
  `maximum macro expansion depth exceeded` instead of aborting with an ASan
  stack-overflow.** The 256-level depth counter is a proxy for stack
  headroom, and ASan's redzone-inflated frames could exhaust the real stack
  first (observed on macOS/arm64 Debug; reproducible anywhere with
  `ulimit -s 4096`).  The guard now also measures the thread's actual
  remaining stack (glibc/macOS/Windows queries; the SP register is read
  directly because ASan's fake stack makes local addresses useless for this)
  and raises the same diagnostic pair -- plus a note naming the early stop --
  when headroom is nearly gone
  (docs/archive/macro-depth-guard-loses-race-with-asan-stack.md).
- **`tur emit-c` output now links at `-O0`.** The dead base generic thunk
  chain (a generic fn returning a closure over a type application, e.g.
  `(fn [] (Cons A))`) referenced the base `ctor_X` of a heap parametric ADT,
  a symbol that is never defined -- only per-spec monomorphs are.  `-O2`
  dead-stripped the chain, but a hand `-O0` compile of `emit-c` output died
  with `undefined reference to ctor_Cons`.  The emitter now flushes static
  trap stand-ins (fprintf + abort naming the ctor) for those never-defined
  base ctors into the forward-decl band, covering the n-arg and 0-arg ctor
  branches and both drivers (whole-program and per-TU), so the emitted C is
  self-contained at any -O level.  A genuinely live base-ctor call -- a
  compiler defect, previously an unconditional link error -- now aborts
  loudly at runtime instead
  (docs/archive/dead-base-thunk-chain-references-undefined-ctor.md).
- **A dynamic variable's `pthread_key_create` failure now aborts with a
  message instead of being ignored.** On `EAGAIN` (the process key budget --
  `PTHREAD_KEYS_MAX`, 1024 on glibc, one key per `defdynamic` plus one shared
  by every `^thread-local` -- is exhausted) the key was left uninitialized and
  every later `pthread_getspecific` on it was undefined behaviour: a silent
  wrong-value failure. The emitted `_dynvar_init_*` now checks and aborts,
  mirroring what `^thread-local`'s key init already did
  (docs/archive/mutable-globals-plan.md section 13.3).
- **The rational/complex numeric tower now runs on all three engines.** Measured
  under the MIR engine for the first time: every rational and complex fixture
  passes with **zero** `cc` fallbacks, from one pure-Turmeric implementation
  against the same expected output. The 16-byte-struct ABI problem this was
  expected to hit never materialized, so the by-pointer workaround sketched for
  it was never needed. The standing rule that no `_Complex`, `<complex.h>`, or
  `__mul*c3`/`__div*c3` reaches the generated C is now enforced by every plain
  `bash tests/run.sh`, not only by its own ctest target.
- **The interpreter resolves typeclass dictionaries the same way the compiler
  does.** All three recovery heuristics turi used to guess an instance from a
  receiver's runtime tag are retired, the last one covering an unascribed
  carrier-helper read inside a constrained container instance. Where the
  compiled path had solved these statically, the interpreter was pattern-matching
  on runtime tags and could disagree with it; dictionary passing now carries all
  of them, so the two engines agree by construction rather than by coincidence.
- **Several float and carrier miscompiles.** A method result declared `:float`
  keyed off the *body* rather than the declared result type; a float literal
  ascribed to a narrower float width was not retyped in place; a `float32`
  generic-call result had no admitted carrier pair; and the int-slot/float-body
  engine divergence (`TUR-E0707`) was asymmetric between engines. Also a silent
  per-push leak in the container-element path, and seven mismatched fat-closure
  function-pointer types.
- **A multi-shot resume across a nested handler delivers once.** The handler
  chain carried two separate spines, so a resume crossing a nested `handle`
  could deliver twice or not at all. `while` loops and statement-position
  conditionals inside a handler clause work now too -- the latter used to hit an
  internal compiler error rather than a diagnostic.
- **A malformed `build.tur` fails the command instead of vanishing.** A manifest
  that failed to parse was treated as absent, so the command proceeded with
  whatever defaults applied and the real problem never surfaced.
- **Assorted correctness fixes.** A `^borrow` parameter passed where `^unique
  ^mut` is required is rejected; an explicit `: nil` return on a lambda is
  honoured; generic instance resolution survives a `#lang` switch; an HKT type
  variable is pinned from the argument type across a rank-2 `forall`; a return
  type disagreeing with an aggregate body is rejected; `definstance` constraint
  types resolve through the real type resolver; `println` prefers a resolved
  `bool` shape over the runtime tag; a hoisted inline-C include no longer
  disables the JIT's split-preamble fast path; the REPL's source-file registry
  survives incremental eval turns; elaborator-minted names stay out of the LSP
  symbol index; and `term/set-cooked` restores the saved terminal mode rather
  than a zeroed one.
- **A `#writes` frame is no longer VERIFIED when the body writes a mutable
  global** (behind `--enable=write-frames`). A frame's vocabulary is
  *parameters*; a global is written by name rather than passed, so `#writes []`
  means "writes none of my arguments", not "writes no storage anywhere" -- and
  a body declaring it could mutate global state and still be stamped VERIFIED,
  which is the tier an optimization may act on. The verdict now downgrades to
  UNVERIFIED, silently: a global write is outside the frame's vocabulary rather
  than outside the declared frame, so no program stops compiling and no
  diagnostic is added. The fact propagates through callees, including callees
  that receive none of the caller's parameters. An EXCEEDED frame is still
  reported -- a global write does not launder TUR-E0382. See
  [docs/archive/mutable-globals-plan.md](docs/archive/mutable-globals-plan.md).
- **Statements above the first body-level `define` are no longer silently
  dropped.** The define splice built its `let` from the first `define` onward
  and discarded everything before it, so in
  `(defn main [] : int (println "before") (define x 1) (println x) 0)` the
  first `println` never ran and the program printed only `1`. It now prints
  both lines.
- **`^persistent` and `^deprecated` on a body binding are rejected rather than
  quietly ignored.** `^persistent` was accepted by the splice and demoted to an
  ordinary `let` binding -- i.e. it silently did not do what it said. Both are
  top-level-`def` annotations and now say so.
- **Every `def` annotation is now either accepted or rejected by name.**
  `^linear`, `^relevant`, `^affine`, and `^unique` on a top-level `def` used to
  fall through to a generic arity diagnostic that never mentioned the
  annotation. Each is now refused with its own reason: `^linear` and
  `^relevant` are verified when a binding's scope ends and a global's never
  does; `^affine` would count elaboration sites across the program rather than
  uses at run time, so two functions naming the global would be rejected even
  if only one ever ran; `^unique` asserts no aliasing, which a name every
  function can reach cannot have. An unrecognized `^`-led annotation says
  "unknown annotation" and lists what `def` accepts.

## [0.33.2] -- 2026-08-02

### Added

- **`#writes` write frames -- a checked per-argument write declaration**, behind
  `--enable=write-frames`. `#writes w` / `#writes [a b]` on a `defn` declares
  which arguments the body may write through, and a deferred pass checks the
  declaration against the body: VERIFIED (a fact an optimization may act on),
  EXCEEDED (`TUR-E0382`), or UNVERIFIED (no diagnostic -- the declaration still
  documents intent, and nothing acts on it). The bracket form gives
  `#writes []` -- "writes nothing" -- a spelling, and "no frame" never collapses
  into "empty frame". `#reads` and `#writes` may appear in either order. Gated
  because the checking can reject a body that compiles today.
- **A borrow that provably reaches no writer no longer sinks a refinement
  hypothesis.** WF3 declined a caller body the moment an assigned name was also
  borrowed, because there was no way to ask what a callee does with the borrow.
  With checked frames there is: the decline lifts when every borrow of the
  assigned name is write-free, which is exactly the `frozen`-region idiom whose
  `(& w)` exists only to lock `w` down. "Cannot be written" has three sources
  and no others -- a `^borrow` parameter, a CHECKED `#writes` frame excluding the
  slot, and nothing else; an unresolvable callee or a DECLARED-but-UNCHECKED
  frame both answer "assume it writes".

### Fixed

- **The interpreter copies by-value struct arguments on parameter bind.**
  Turmeric passes structs by value, so a write through a struct parameter is
  invisible to the caller -- the compiled backend has always done this, but turi
  bound the heap `TuriStruct*` straight through, so the same program printed `0`
  compiled and `3` interpreted. The copy recurses through by-value struct fields
  (a nested `(set! (.n (.inner o)) v)` is invisible too) and stops where sharing
  *is* the semantics: an `__rc` wrapper, a `:heap` struct, and any non-struct
  value such as a `&Struct` borrow. stdlib's `Cons` and `Vec` are both `:heap`,
  so list and vector arguments are not walked at all.

### Docs

- **The guides index gets a search filter**, generated by `tools/genguides.py`.
- **WF4 retired** from the checked-write-frames plan -- its premise was false --
  and the stateful-refinements guide updated for the landed WF1/WF2/WF3.
- New reports: the intermittent macOS JIT-leg 45-minute CI hang (with an
  end-to-end confirmation that the `coreutils` timeout contains it), and a
  `definstance` constraint type defaulting to `:int`. New plan: reflected
  measures.

## [0.33.1] -- 2026-08-02

### Changed

- **A refinement hypothesis now survives an assignment that provably cannot
  disturb it.** A crossing's path conditions were dropped wholesale the moment
  *any* `set!`/`swap!`/`reset!` appeared anywhere in the caller body -- the
  coarsest correct rule, and the binding constraint on two shipped surfaces:
  `for-each-alive!` accepts only pure bodies, so an accumulator `set!` about
  `acc` dropped hypotheses about the world and the entity, and every
  `while`-lowered loop lost facts its counter never touched. A hypothesis now
  survives iff every assignment in the body targets a plain symbol the
  hypothesis does not mention, and the body never borrows that symbol. A
  place-expression target (`(set! (.n w) 9)`), an assignment symbol the scan
  cannot attribute, depth or slot exhaustion, or a borrowed target all restore
  the old whole-body decline. The assignment's *value* needs no check: a
  hypothesis is only believed when its terms are congruent, and congruence is
  granted only to a pure measure or to a `#reads` measure inside a region
  freezing its argument -- neither of which a call in value position can stale.

### Fixed

- **`TUR-E0371`'s explainer no longer recommends a retired flag.** It closed
  with "Enable with: `tur build --enable=refined myfile.tur`" -- user-facing
  text pointing at a flag that has been a `TUR-W0063` no-op since refinement
  types graduated in 0.33.0.

### Docs

- **Type-Level Rows (`#row{...}`) added to the HKT guide**, plus a followups
  plan for row types and a report on the stale mono-specs header comment.
- **The refinement solver's shipping status is stated correctly.**
  `advanced-type-system-rationale.md` claimed "there is no SMT dependency. No
  shipped artifact links a solver." The staged decision procedure
  (`refine_solver{_s0,_euf,_arith,_no}.c`) ships compiled into `tur` and runs on
  every compile; what no artifact links is a *third-party* prover -- no libz3 at
  configure time, no subprocess, nothing a user installs. Corrected there and in
  the 0.33.0 CHANGELOG bullet that had inherited the sentence.
- **Refinement plans reconciled with the graduation.** Several documents still
  read as open work or as gated behind `--enable=refined`; status banners,
  prerequisite rows, and struck-through constraints are now dated and accurate.
- A report filed on `pkg_manifest_read` conflating "no manifest" with "broken
  manifest", which degrades a manifest typo into an unrelated `module not found`
  whose hint leads away from the cause -- and on `tur check` printing
  `TUR-E0620` at `error:` severity while exiting 0.

## [0.33.0] -- 2026-08-01

### Fixed

- **A refinement in type-argument position no longer breaks the program.**
  `(Box #refine{ v : int | (> v 0) })` stored the contract node whole and
  nothing peeled it, so the payload a `match` arm binds stayed contract-typed
  and every ordinary use of it failed -- `(+ v 1)` was `TUR-E0006` "first arg
  type { v : int | ... }", `println` found no overload, and a float base was
  `TUR-E0707` "declares float but returns { v : float | ... }" because the
  register-class check compares kinds without peeling. The annotation broke the
  program rather than merely failing to help it. It is now peeled to its base,
  with a new **`TUR-W0380`** stating that the payload predicate is not enforced
  -- inert, but not silently so. Actually checking a container payload needs the
  refinement to survive to the unpacking binder, which is a feature and is not
  built. `TY_CONTRACT` now also delegates to its base in
  `type_has_concrete_codegen_layout`, which this was the last thing blocking.

### Changed

- **Refinement types graduated: `#refine{...}` predicates are now discharged
  statically on every compile.** The `refined` experiment gate is gone. The one
  user-visible consequence: a refinement that is violated on *every* execution
  reaching it is now a compile error (`TUR-E0371`) rather than a runtime
  contract failure. Nothing else changes -- an obligation the solver cannot
  decide still falls back to exactly the runtime check it would have had
  anyway, which is why turning this on cannot make a correct program wrong.
  Graduation covers the stateful slice (`#reads` / `frozen`) as well as the
  pure core. Preconditions were measured rather than assumed: the in-tree blast
  radius was one fixture, and compile cost on a real ~5400-line program was
  1.004x with zero `TUR-E0371`.

  Existing opt-ins keep working and can be deleted at leisure:
  `--enable=refined`, `:experiments [:refined]` and the user experiments file
  are accepted as no-ops with `TUR-W0063`; `#lang turmeric refined` with
  `TUR-W0064`. Both shims age out one minor line from now. `--strict-refine` is
  unaffected and remains a real flag.

### Docs

- **The three refinement guides are reachable by browsing.**
  `refinement-types`, `stateful-refinements`, and `refinement-solver-internals`
  existed but were absent from the guides index -- only `contract-types` was
  listed, so the whole feature was unreachable. They get their own section. The
  advanced-type-system rationale's "refinement types were correctly deferred
  for v1.0.0" section is rewritten as "deferred, then built": the deferral
  reasoning was sound on its premises, but it assumed entailment meant an SMT
  dependency on the compiler's critical path. Because every refinement is a
  contract type first, it already has a runtime meaning, so a partial
  discharger may answer `Unknown` on any obligation and stay sound -- which is
  what made an in-house solver shippable, and why the *external* SMT dependency
  the deferral feared never materialized. Dependent types remain deferred on
  unchanged grounds.

## [0.32.8] -- 2026-08-01

### Fixed

- **`$` in sweet-exp no longer double-applies a rest-of-line that is already
  one complete expression.** `println $ g(7)` expanded to `(println ((g 7)))`
  and failed with "expression in call head has type `int`, which is not
  callable" -- so `$` composed with a bare token sequence but not with a
  neoteric call, a parenthesised form, a curly-infix group or a data literal,
  exactly the spellings the rest of the sweet-exp style encourages. The chained
  form the guides teach, `println $ normalize $ vec3(1.0 0.0 0.0)`, did not
  compile. The wrap is now suppressed when the rest is already exactly one
  balanced, delimited expression. A bare atom is deliberately untouched:
  `f $ g` stays `(f (g))`, SRFI-110's zero-argument reading.

- **A capturing closure passed to a tyvar-signature fn parameter no longer
  segfaults.** `(defn run [R] [body : (fn [] R)] : R (body))` compiled clean
  and then jumped into the closure's env struct, because a tyvar-signature
  parameter kept the thin representation, which has nowhere to put an
  environment. Fat normalization now admits tyvar signatures on the *parameter*
  side; the result side deliberately keeps the concrete-only claim, since the
  poly-call protocol boxes returned closures itself. Two missing shim sites came
  with it: rank-2 forall params called through the carrier, and normalized
  params stored into fat struct fields, which were boxed a second time. An
  effect-row signature is still excluded and tracked as a fuzz `--known-probes`
  row.

- **A `{ shim, orig }` fat box no longer leaks once per call at a normalized fn
  parameter.** Nothing frees a box handed to a normalized nominal param, so
  `(apply1 add3 acc)` in a loop leaked 24 bytes an iteration -- 5e6 iterations
  peaked at 122 MiB. Where the boxed value is a file-scope function the contents
  are constant, so the box is allocated once at file scope, filled from
  `__tur_static_init`, and given a no-op drop glue so every drop path correctly
  does nothing. Peak RSS 122 MiB -> 1 MiB; a 2e7-iteration loop 0.533s ->
  0.042s. The hoist is opt-in per shim site and only the normalized-nominal-param
  site takes it -- `^fat` sinks and owning struct fn-fields keep the heap box,
  since a `^fat` callee may drop its argument. The same per-call box at a `^fat`
  sink leaks too and is filed as its own report.

- **Generated C no longer straddles the int64 carrier and a pointer at a
  monomorphized constructor's field slot or at a fn-value return site.** Four
  programs (`conv-defstruct-option-fn-element`, `hkt-ap-fn-in-container`,
  `defalias-composite`, `fn-value-matrix-ok-rows`) failed to compile on any
  toolchain that promotes `-Wint-conversion` to an error -- Apple clang >= 15
  and gcc >= 14 -- while the older gcc on the CI Linux leg merely warned. This
  was the sole remaining cause of the standing red `Test (macos-latest)` job.
  Both halves come down to not re-deriving an emitted C type from a `Type`: the
  constructor's real parameter C type is now recorded when the ADT application
  is registered and looked up at the call site, and the return-site bridge asks
  the typed AST whether the tail expression emits the carrier rather than
  pattern-matching the emitted string. No codegen snapshot moved.

- **A `(c-fn ...)` and an ordinary `(fn ...)` of the same signature no longer
  collide on one monomorph C name.** The type checker already holds the two
  distinct whenever the latter is a capturing closure -- a fat closure must
  never flow into a raw C callback sink -- but the type mangle did not carry
  the `cfnptr` flag, so `(Option (c-fn [int] int))` and `(Option (fn [int] int))`
  produced two registry entries under one name. The second `#ifndef` block was
  preprocessed away and the `c-fn` view silently adopted the closure view's
  `void *` constructor slot in place of its own function-pointer typedef, with
  **no diagnostic from any compiler**. Benign in practice only because every
  function-value representation is a same-bits 8-byte word.

  Splitting the names then exposed a latent ordering bug the collision had been
  hiding: function-pointer typedefs are now written before the monomorphized ADT
  definitions that reference them (they are still *generated* after, since
  emitting a monomorph is what registers them). `type_eq` is deliberately
  unchanged.

- **A generic whose result family is instantiated at a parametric ADT no longer
  calls the wrong monomorph.** Both specializations of `vec-empty-like__` called
  the `int`-element `vec-new` clone; the `(Map sym int)`-element one was never
  interned or emitted. A zero-argument, return-only-polymorphic call records no
  type-variable bindings at elaboration, so its callee monomorph is recovered
  from the enclosing specialization's result family -- and that recovery gated
  each element on `type_has_concrete_codegen_layout`, which returns false for
  every type application by design (its own comment names
  `type_app_is_concrete_adt` as the companion predicate for a concrete
  parametric ADT). Consulting only the first meant every ADT-application element
  was silently declined; the `int` clone worked only because `int` is not a type
  application. Runtime-benign where it was found, purely because `vec-new`'s body
  is element-agnostic.

### Internal

- **Six GC / `Rc` / weak-reference fixtures assert on the CG6 collector counter
  (`gc-live-blocks`) instead of a process-wide malloc probe.** The probe equals
  the program's heap only on an unsanitized `cc` build that owns its process; it
  was already known to be vacuous under ASan on glibc and quarantine-inflated
  under ASan on Darwin, and under one-process `tur jit` it reads the compiler's
  heap. That last mode was the whole of the `JIT engine (macos-latest)` redness
  -- never a GC, `Rc`, weak-reference, or JIT-codegen defect.

  The counter is program-scoped, exact rather than tolerance-based, portable,
  and identical across every linkage mode, so the six now run under the JIT and
  under `cc` with the same output. The assertions got stronger: the
  collector-off control moved from *impossible* (identical output either way on
  glibc) to `10000` against a tolerance of `0`. Five of them also stopped
  falling back to `cc` under the JIT engine, since the probe's
  `#include <malloc/malloc.h>` was what dragged in the `TargetConditionals.h`
  the MIR front end rejects.

  `tests/run-gc-leak-gate.sh` gained four real assertions (14 passed / 2 skipped
  -> 18 / 2) because `gc-collects-strong-cycle` could move from its
  probe-output exemption list to the controls. The byte-level probe survives as
  `gc-collects-strong-cycle-heap-bytes`, `cc`-only, since bytes still catch a
  payload leak the block counter cannot see.

- **Interpreted trampoline fixtures cap their RSS, and a per-fixture timeout is
  reported as a timeout rather than a stdout mismatch.** The tree-walking
  interpreter retains roughly 4 KiB per trampolined step, so a fixture's step
  count is a memory multiplier under `--interpret` and two co-scheduled large
  ones were memory pressure, not a CPS defect. Diffing stdout first had been
  turning a killed fixture's partial output into a claim about the answer.

- **CI**: the Windows leg installs `wineditline` and compares the smoke fixture
  in bash (`diff` is not present on the runner); the macOS JIT baseline is
  re-measured in the job's own configuration, and the JIT corpus counts are
  published on a passing run.

## [0.32.7] -- 2026-08-01

### Added

- **A `windows-latest` CI job.** The Windows build is covered on every push
  rather than rediscovered by hand, so a break in that path surfaces in CI
  instead of at release time.
- **`tur experiments` and `tur lang-layers` appear in `tur --help`**, alongside
  the `--enable=<name>` global flag. All three shipped without a listing.

### Fixed

- **The Windows build works again.** The emitter, LSP, arena, REPL, and the
  generated runtime-split sources build on Windows, and `platform_fs.h` grows
  the shims that path needed.
- **Winsock socket options.** `setsockopt`/`getsockopt` are shimmed in the
  Winsock compatibility layer, and `SO_REUSEADDR` -- whose Windows semantics
  are not the POSIX ones -- is no longer set there.
- **`stdlib/fs` and `stdlib/term` on Windows.** Their inline-C bodies are
  ported off POSIX-only APIs; fixtures that genuinely require POSIX I/O now
  carry a `requires.posix-apis` marker and skip rather than fail.
- **Nested `bind` over `result` no longer segfaults at a typed boundary.**
  The emitter now agrees with itself about the carrier across the boundary.
- **turi resolves return-directed class methods from the frame's pinned type
  variables**, instead of keeping an instance baked in from an earlier call.

### Internal

- `tests/run-jit.sh` times negative fixtures through `_run_timed` rather than a
  bare `timeout`, cutting the JIT harness's false failures from 407 to 6.

## [0.32.6] -- 2026-07-31

### Added

- **`tur jit <file>` -- an in-process MIR JIT engine.** Behind two gates
  (`-DTUR_JIT=ON` at build time, `--enable=jit` at run time), `tur jit`
  compiles a program's emitted C with c2mir and runs it in process -- no `cc`
  subprocess, no linker. Any engine failure prints `TUR-W0070` and delegates
  to the existing `cc` path, so the subcommand never fails where `tur run`
  would have succeeded. A default build vendors nothing and carries no new
  dependency.
- **A split runtime: the JIT compiles against declarations, not the preamble.**
  `tur jit` swaps the emitted all-gates preamble for a committed declarations
  region under an xxh64 hash guard and resolves the runtime by address into
  the host; on a mismatch (emitter drift, knob drift, missing archive) the
  full preamble is used unchanged. `arith` end to end drops ~278ms -> ~200ms.
  `TUR_JIT_NO_SPLIT=1` opts out.
- **The REPL builds spices in process.** `tur --enable=jit repl` replaces the
  `tur build --shared` subprocess + `dlopen` + `dlsym` pipeline with the
  engine; cold spice load drops ~850ms -> ~260ms on a two-module probe.
- **The JIT engine is reachable from a libturi embedder.** A C host linking
  `libturi` probes for `TUR_HAVE_JIT` (a PUBLIC compile definition, so the
  probe resolves whether or not an engine was built) and drives the engine
  through `jit_engine.h`, falling back to `turi_eval` rather than to `cc`.

### Fixed

- **Emitted C is portable to a strict C11 front end.** `__auto_type`,
  `__attribute__((constructor))`, `__attribute__((cleanup))`, and
  `__extension__ ({...})` no longer appear in the emitted program -- replaced
  by named types, an explicit `__tur_static_init()`, an explicit scope-exit
  pop, and a bare statement expression. This is what unblocked the JIT, but it
  makes the `cc` path's output more portable too.
- **Invalid C from two emitter defects.** A call temp now records its
  *declared* C type rather than a re-derived one, and a `TUR_APPLY` argument
  is no longer cast to an aggregate parameter type.
- **`return <void expr>;` is no longer emitted in `:void` functions**, and
  hoisted `#include`s are ordered before hoisted code.
- **Persistent-map `cstr` keys hash and compare by content**, not by pointer
  identity, in the P3 `^persistent` lowering.
- **Dynamic bindings pop on an early return**, not only via the cleanup path.
- **Self-TCO fires through a capturing named `let`.**
- Two arm64/macOS emitter defects: the `xxh64` prototype and the
  `TaskGroupBlock` layout.
- Monaco editor colors in the web REPL.

### Internal

- CI covers the MIR JIT engine (`tests/run-jit.sh`), and a parity harness plus
  an engine benchmark triangle compare `tur jit` against the `cc` path across
  the fixture corpus.

## [0.32.5] -- 2026-07-30

### Removed

- **The Z3 refinement oracle scaffold is retired.** The dev-only Z3 backend
  (`refine_libz3.c`), the `TUR_REFINE_Z3_ORACLE` CMake option, its
  `find_package(Z3)` block, and the `tur_refine_fuzz` VC-level differential
  fuzzer are deleted, having met both retirement criteria. No shipped artifact
  ever linked Z3 -- the option defaulted off and refused Release and WASM
  builds -- so the compiler is functionally unchanged. Solver soundness is now
  guarded by the labelled SMT-LIB corpus (`tur_refine_corpus`: 125 benchmarks,
  no solver linked, 0 soundness failures) and the source-level fuzzer
  `tests/refine-fuzz-src.py`, which never needed an oracle. The internal
  diagnostic `TUR-I0379` is retired with it; its code stays reserved.

### Docs

- **Automatic GC is permanently opt-in.** The `cycle-gc` plan's CG8 phase no
  longer conflates ungating `(gc-auto!)` with making `GC_AUTO` the default;
  only the ungate is on the table, and the default-on option is recorded as
  rejected rather than deferred.
- `expires_at` is documented consistently as advisory -- it never blocks a
  release cut.

## [0.32.4] -- 2026-07-30

### Fixed

- **Function-typed values now carry a single ABI shape.** Fn-typed values are
  fat-normalized at return, ascription, and concrete-signature nominal
  parameter positions, and carrier/fat provenance is tracked through aliases
  and type joins. This closes a family of miscompiles where a closure passed
  or returned across a typed boundary produced invalid C or a SIGBUS.
- **`bind` continuations pair with the selected entry point.** Monadic `bind`
  at a typed boundary (e.g. `Result`) no longer emits a continuation whose ABI
  disagrees with the callee it was selected for.
- **Class-method results into generic positions.** `__inst_` callees with
  by-value results are no longer treated as carrier producers, fixing invalid
  C from typeclass method results flowing into generic contexts.
- **Width-independent container elements.** By-value struct elements in `vec`
  and narrow struct values in `map` round-trip correctly under both `tur` and
  `turi` instead of depending on the element's machine width.
- **Heap-record bindings consult the representation spec.** Concrete heap
  bindings (including wide lens families) get correctly typed pointer
  bindings, and transparent int newtypes are treated as their payload.

### Internal

- `repr_of(type, position)` is now the single decision function for value
  representation, backed by a one-row-per-`TypeKind` table, shadow
  instrumentation, and a representation-decision ratchet in the test suite.

## [0.32.3] -- 2026-07-30

### Added

- **Numeric tower: `Rational` and `Complex`** (`stdlib/rational.tur`,
  `stdlib/complex.tur`). `Rational` is an exact `num`/`den` pair over int64,
  always normalized, so structural equality is mathematical equality;
  `Complex` is a plain `re`/`im` pair of doubles. Both are pure Turmeric with
  no inline C, so they behave identically under `tur` and `turi`. Complex
  division uses Smith's algorithm, and the emitted C never contains
  `_Complex`, `<complex.h>`, or the `__mul*c3`/`__div*c3` compiler-runtime
  helpers. Complex deliberately has **no** `Ord` instance. See
  [docs/guides/numeric-tower-guide.md](docs/guides/numeric-tower-guide.md).
- **Operator overloading via `Num`**: when no builtin operator row matches the
  argument types, `+`/`-`/`*`/`/` now fall back to `Num` typeclass dispatch --
  so any user numeric type with a `Num` instance works with the bare
  operators. Variadic calls left-fold into nested binary method calls, and
  unary `-` reaches `neg`. Primitive arithmetic is untouched: the builtin row
  still wins whenever it matches.
- **`#rat{3/4}` and `#cx{3.25 -1.5}` reader literals**, alongside `#map{...}`
  and `#set{...}`. `#rat{...}` reads its body raw (curly-infix would otherwise
  make `{3 / 4}` infix division) and normalizes at read time, so `#rat{6/8}`
  and `#rat{3/4}` are the same literal; a zero denominator is a read-time
  error (`TUR-E0284`). `#cx{...}` reads two ordinary expression slots, so
  `#cx{3.25 {1.0 + 0.5}}` composes (`TUR-E0285` on any other arity).
- **`exp`, `log`, `sin`, `cos`, `atan2` in `stdlib/math.tur`**, with matching
  interpreter natives; `fabs`, `ceil`, and `pow` gained the interpreter
  natives they were missing.
- **`Num [float]` instance**, which the class was missing.
- **Editor intelligence in the browser**: the LSP gained a WASM bridge and a
  browser analysis backend, so Try Turmeric now offers completion, hover,
  diagnostics, and go-to-definition without a server. Transport is split from
  dispatch, with transport-free session unit tests behind it.
- **`defalias` accepts a full type expression** -- not just a type constructor
  -- so composite aliases (function types, rows, applications) are
  expressible.
- **`defopaque :sealed`**: `::` cannot cross the representation boundary of a
  sealed opaque type.
- **`:tur-version` in `build.tur`**: a spice can declare which compiler
  versions it is known to work with.
- **Shadowing diagnostic**: a `defn`/`defmacro` that shadows a special form
  now warns at the definition site rather than surprising you at the call.

### Changed

- **`Num` class methods are closed over the instance type**: `add`/`sub`/
  `mul`/`div`/`neg` now take and return `a` instead of returning `:int`. The
  existing instances needed no body changes, but emitted dictionaries and
  instance methods now carry the instance's own type (e.g.
  `__inst_Num_add_int8` returns `int8_t`, not `int64_t`). This is what makes a
  `Num` instance for a non-integer type -- Rational, Complex, a user newtype --
  usable at a call site.

### Fixed

- **Higher-kinded dispatch inside constrained-polymorphic bodies** now goes
  through dictionary passing ("Route B"). This fixes return-directed method
  resolution on a constrained abstract type constructor, aggregate
  continuation returns at the dict-dispatch carrier, and the
  partial-application hole in the `Type`.
- **ICE when taking an effectful function's address** -- it was counted as
  performing the effect.
- **SIGSEGV calling a `^fat` parameter with a non-empty effect row.**
- **Name mangling**: `append_type_mangle` is now injective (its default arm is
  gone), and C reserved words are guarded with a `tur_u_` prefix.
- **Codegen**: a stored `fn` element is spelled as a handle with its type
  variables substituted, and `:Sym` is treated as a concrete codegen layout.
- **Row algebra threads field names through**, so typed-field rows survive row
  operations.
- **`load` honors a loaded file's inline `#lang`**, and `--interpret` now
  routes through the same path.
- **The transitive `:spices` include-path walk terminates on cycles.**
- **`turi` shows collection elements by their own `Show` instance** rather
  than always using `Show[int]`.
- **`build.tur` bare-brace manifest syntax** in the docs and the manifest hint.

### Docs

- New guides for type-level rows, value representations, and the numeric
  tower; `effects-vs-monads` rewritten around the present-tense choice.

## [0.32.2] -- 2026-07-27

### Added

- **LSP: formatting, signatureHelp, and `$/cancelRequest`**: `tur lsp` now
  answers `textDocument/formatting`, `textDocument/signatureHelp`, and
  `$/cancelRequest` -- the largest protocol gaps found by building a real
  editor client (Trowel) against the server.
- **REPL: `:load-string` and richer shell-integration markers**:
  `:load-string "<src>"` evaluates a literal without a disk round-trip; OSC
  133 `C`/`D` now bracket evaluation so a host can distinguish idle/busy/done,
  and `TUR_SHELL_INTEGRATION=1` forces markers on when driving the REPL over
  a pipe.

### Fixed

- **LSP completion no longer goes blank while typing**: the symbol index is
  retained across a failing compile and primed even for a file that has
  never parsed, so an unbalanced paren -- the normal state mid-edit -- no
  longer drops completion to zero.
- **LSP hover, diagnostics, and sync papercuts**: hover honors
  `hover.contentFormat`, zero-width diagnostic ranges are widened
  server-side, `didChange` reads the last (not first) content-change entry,
  and the analysis temp file no longer hardcodes `/tmp` (which broke on
  Windows).
- **`\uXXXX` JSON escapes decoded correctly** in LSP messages, including
  surrogate pairs -- previously the backslash was dropped, corrupting text
  and shifting every later diagnostic's byte offset.
- **REPL working-directory reporting uses `pwd -L` semantics**: a
  symlink-resolved `getcwd()` no longer disagrees with a host's own path
  tracking (e.g. `/tmp/demo` vs `/private/tmp/demo` on macOS).
- **`tur fmt` no longer mangles a `defn` with a type-parameter vector**;
  stdlib reformatted to match.
- **`build.tur` manifests with map values no longer error** during parsing.

### Changed

- **LSP analysis is debounced and negotiates `positionEncoding: utf-8`**,
  cutting redundant compiles on rapid edits and making the byte-offset
  contract explicit instead of a silent mismatch with the LSP default.

## [0.32.1] -- 2026-07-27

### Added

- **REPL `:cd` and `:pwd`**: `:pwd` prints the working directory and `:cd
  [dir]` changes it (bare `:cd` goes to `$HOME`). This moves the *running*
  process, so definitions and session state survive -- unlike a host-side
  "set directory", which can only restart the REPL. A successful `:cd` also
  emits an OSC 7 `file://` report when shell integration is active, alongside
  the existing OSC 133 prompt markers, so a host editor can track the working
  directory without restarting or scraping output; the initial directory is
  reported at startup for the same reason.
- **`stdlib/rcvec`**: a flat vector of `rc<A>` that the cycle collector can
  trace. A plain `Vec[rc<T>]` is refcount-correct but invisible to the
  collector, so a cycle through a slot strands; an `RcVec` carries its own
  walk/drop hooks, so cycles through it are reclaimed.

### Changed

- **Long-lived interpreter sessions bound their memory**: tracked collection
  buffers (Vec/Set/Map wrappers, TVar cells) are swept at the eval boundary
  right after a successful scratch-promotion rewind, instead of accumulating
  until teardown. Measured over 5000 transient-vec evals: 5000 tracked boxes
  retained before, 0 live after.

### Fixed

- **`rc` scalar default drop glue no longer frees its inline payload**: a
  decrement-to-zero on a scalar `rc` allocated with default glue freed an
  interior pointer, aborting under ASan. Scalars now default to a no-op
  inline drop; the separate-payload entry points keep the freeing default.
- **TVar cells survive promotion rewinds**: cells were scratch allocations
  that promotion could not see, so the first rewind after
  `(def t (tvar/new 0))` poisoned the cell -- a live use-after-reset in the
  REPL. Cells are now tracked boxes that survive rewinds and sweep when
  unreachable.

## [0.32.0] -- 2026-07-26

### Added

- **`#reads` stateful refinements (experimental)**: extend `--enable=refined`
  with a stateful surface -- `#reads` annotations on parameters (parsed,
  congruence-granted at call sites, and backed by codegen), the frozen-region
  form realized as a linear mutation cap, and boolean-sorted measures in
  refinement predicates. Macro templates can emit `#reads` annotations and
  `#refine` contract types.
- **`tur test` directives**: per-test flags and expected-error negative
  tests, so refined/negative fixtures run under plain `tur test`.
- **Arena debug diagnostics**: debug-poisoning and guard diagnostics that
  surface uninitialized-arena reads early.

### Changed

- **Inline-C HKT instances return real results**: the by-value HKT result
  limitation is lifted -- an inline-C `definstance` body can return a
  carrier-width or `:heap` ADT result without heap-boxing, `stdlib/rc.tur`
  compiles again, and TUR-W0042 diagnoses the remaining unsupported shape
  at the `definstance`.

### Fixed

- **Refinement guard discharge**: crossing guards are now collected from the
  whole function body (not just the return form) and from inside macro
  expansions, so macro-generated guards and frozen-region crossings
  discharge; unproven `#reads` crossings surface in non-strict mode; the
  refine memo resets per compile (fixes multi-compile corruption).
- **defstruct/ADT field elaboration**: fields resolve assoc-type projections
  and Size literals, fixing the `(Storage T)` compiler skew.
- **Arena crashes**: two uninitialized-arena-read crashes.
- **Stdlib load dedup**: `(load "stdlib/X")` resolves via the stdlib dir
  first, so it no longer double-loads against the auto-loaded stdlib.
- **Weak refs**: execute the stdlib weak-ref audit (WR1, WR3, WR4).

## [0.31.1] -- 2026-07-26

### Added

- **`Show [Sym]`**: add a Show instance for runtime symbols, and close
  deeper sym-show display gaps.

### Changed

- **Cycle-GC pause bounds**: bound linearization, add measured caps,
  memoize macro expansion, and make the `rc<T>` free-queue drain linear
  (no per-link recursion) so large cycles collect without a stack overflow.

### Fixed

- **`set!` rc<T> leak**: `set!` now releases the `rc<T>` value it
  overwrites and normalizes what it stores.
- **Cycle-GC pause time**: fix the collector's real pause-time term (a
  quadratic over the candidate set, not the candidate set itself).
- **Refinement types**: cross the caller's whole body at a call site,
  sort measures by return type, type SMT-LIB corpus numerals by the
  declared logic, and round-trip refined syntax correctly through `fmt`.
- **Linearity**: a closure that captures and consumes a linear value is
  now itself linear.
- **`#lang` reader switch**: keep the preloaded stdlib across a reader
  switch.
- **wasm32**: mix `promo_hash` at a fixed 64 bits so wasm32 no longer
  shifts past the hash width.

## [0.31.0] -- 2026-07-25

### Added

- **Refinement types (experimental)**: predicate-refined types behind
  `--enable=refined` (or `#lang turmeric refined`). Ships RT0-RT7 -- refined
  parameters/results, call-site and let/match path-condition crossings,
  typeclass method result-refinement enforcement (TUR-E0374/E0375), and a
  three-valued purity classifier. Backed by an in-house SMT solver plus an
  optional Z3/cvc5 oracle, a source-level fuzzer, and a labelled SMT-LIB
  corpus replayed against the chain. Runs on wasm32.
- **Cycle-collecting GC (experimental)**: a Bacon-Rajan trial-deletion
  collector behind `--enable=cycle-gc` reclaims strong `rc<T>` reference
  cycles via `(gc!)` / `(gc-auto!)`.
- **`rc<T>` in collections**: `Vec`, `Map`, and `HAMT` can own `rc<T>`
  elements, with caller-supplied value ownership; adds `stdlib/rcchain.tur`,
  a collection the cycle collector can trace.

### Changed

- **turi REPL incremental elaboration**: persistent elaboration/parsing
  session takes long-lived envs from O(N^2) to ~O(N); the incremental path is
  now ON by default.
- **Single runtime GC**: compiled executables link the runtime collector
  instead of replicating it per module, reconciling the two `RcControlBlock`
  layouts and guarding against future drift.

### Fixed

- **Refinement codegen defects**: fix two codegen bugs in `match` on a
  non-ADT scrutinee, `rc/of` over a multi-variant ADT (released/traced
  nothing), `rc<T>` over a `:heap` defstruct, and a compiler stack overflow
  on a disjunctive refinement goal.
- **Web REPL**: invoke top-level `main` so Run shows output (not
  `#<fn main>`), unstick the service-worker cache so the REPL loads the
  current wasm, and add a "Force update" command.

## [0.30.8] -- 2026-07-24

### Fixed

- **Interpreter (`--interpret`) parity**: retain `rc<T>` on closure capture,
  resume multishot/cross-fn continuations correctly, and close String-parity
  fixture gaps so the tree-walking interpreter matches compiled behavior.
- **Async catch-unwind**: keep `catch-unwind` stackless inside async programs
  (fiber-rec).
- **Nested effect handlers**: resolve non-termination when handlers are nested
  (effect-rec).
- **macOS build & codegen**: broad macOS compatibility fixes across the
  compiler, runtime, and stdlib, plus CPS cons/cstr argument casting and
  by-value aggregate frame-escape codegen fixes.

## [0.30.7] -- 2026-07-23

### Added

- **Curated `#lang` layers**: `#lang <base>[/<dialect>] <layer>*` selects one
  mutually-exclusive base reader (`turmeric`, `turmeric/curly-infix`,
  `turmeric/neoteric`, `turmeric/sweet`) plus an order-independent set of
  additive layers. Ships the `stringed` reader layer (`#s"..."`) and validates
  every layer token against the curated `LANG_LAYERS[]` table -- an unknown
  layer is a hard error, not a silent ignore.

### Docs

- Reader-forms and syntax guides document the base/layer split; the web REPL
  handles the new `#lang` line form.

## [0.30.6] -- 2026-07-23

### Fixed

- **Composed lens codegen**: lower composed struct-of-closures lenses end to
  end -- specialize every closure of a struct-of-closures return and mangle
  apostrophes in ADT monomorph names (Blocker 2b/2c).
- **Effectful fn-value params**: thread effectful callbacks correctly through
  fat-closure function-value parameters (E2).
- **catch-unwind leaks**: reclaim the caught result box and panic-message
  string on unwind.
- **`#lang` line parsing**: strip trailing tokens on the `#lang` line before
  handing source to the reader.
- **Separate compilation (`--shared`)**: unblock `--shared` spice builds, order
  base ADT typedefs ahead of the monomorph flush in the header, and mirror the
  direct forward-decl param ABI in the CPS entry-wrapper.
- **libc symbol collisions**: mangle user globals whose names collide with libc
  symbols.
- **Imported-module defns**: forward-declare load-spliced top-level defns in
  imported modules.
- **stdlib/httpd leaks**: free per-limiter `RateLimit` state at process exit and
  per-request cookie/form accessor strings.
- **stdlib/image**: hoist platform executable-path includes to file scope.
- **REPL builtins**: inject native-function stubs so `cons`/`head`/`tail`
  resolve at the prompt.

## [0.30.5] -- 2026-07-22

### Fixed

- **REPL `:reset` / `:run` restore the full stdlib surface**: recreating the
  session env (via `:reset`, or an editor "Run" that resets first) no longer
  drops the interactive stdlib preload. `list-head`/`list-tail` and other
  carrier helpers stop emitting spurious `TUR-W0040` warnings, and `#map{}` /
  `#set{}` and typeclass `Show` work again after a reset instead of failing
  with "unknown function or operator".

## [0.30.4] -- 2026-07-22

### Added

- **`tur compile` / `tur link` subcommands**: `tur build`'s compile and link
  phases are now separately invokable, and `tur build --runtime=lib` links a
  prebuilt `libturi.a` instead of recompiling the runtime. A lean, non-ASan
  `libturt_runtime.a` ships so `--runtime=lib` is defaultable.

### Changed

- **Automatic closure/Drop reclamation graduated**: the drop-glue header ABI is
  now the default (closure-drop-glue R4). The compiler emits scope-exit
  auto-drop for move-only `Drop`-instance opaque let-bindings, letting httpd
  retire its manual `httpd-mw-drop` / `httpd-mw-free-chain` markers and reclaim
  runtime-built middleware chains automatically.
- **Default runtime linkage is now auto**, preferring the lean prebuilt archive.
- **Interpreter FFI arity ceiling lifted** via per-export shims.

### Fixed

- **stdlib `String` load in imported modules**: fixed via a forward-decl
  pre-pass (#705).
- **Leak of module-private env keys** in the interpreter (LSan-reported) (#704).
- **Flag-on crash clusters** cleared during graduation prep (33 -> 3), plus
  `^fat` handler drops on httpd construction-failure paths.

## [0.30.3] -- 2026-07-21

### Added

- **Owned `String` builders and optional accessors**: `stdlib/str-build-string.tur`
  adds `str-concat-string` / `cstr-sub-string` (owned-`String` wrappers over the
  `cstr` builders), with wrap-vs-build guidance steering multi-join accumulation
  to `StringBuilder` (linear) rather than an O(n^2) fold. `stdlib/httpd-string.tur`
  adds `httpd-req-cookie-opt` / `httpd-req-form-opt` returning `option<String>`, so
  "present but empty" (`some ""`) is distinct from "absent" (`none`) (#701).
- **Owned `String` sibling modules for the stdlib**: opt-in `*-string` modules
  wrapping each freshly-allocated-`cstr` function in `string/adopt-cstr` --
  `json-string`, `csv-string`, `term-string`, `re-string`, `range-string`, and
  `schema-string` (Bucket A) (#702).

### Changed

- **Leak-clean fixtures**: dropped 6 stale `requires.no-leak-check` markers now
  that the S1/S2 fat-closure drop machinery reclaims the escaping / HOF-passed
  value-closure envs the opt-outs were guarding (#703).

### Fixed

- **Macros invisible across stdlib re-elaboration**: fixed macros not resolving
  across stdlib re-elaboration (`elab_core.c`, `eval.c`, `fiber.h`).

## [0.30.2] -- 2026-07-21

### Fixed

- **`void*`/`int64_t` carrier straddles for `String` returns**: compiled
  `String`-returning functions (and taskgroup handles) emitted C that straddled
  `void*` and `int64_t`, which clang's default `-Wint-conversion` and GCC 14+
  `-Werror` reject as a hard error -- blocking AOT-compiled use of the owned
  `String` type and reddening the macOS CI leg. The `Show [..] : String`
  inline-C bodies, the `emit_expr`/`emit_fns` return paths, and the
  phantom-witness carrier now bridge through `(int64_t)(intptr_t)` /
  `(void*)(intptr_t)` (#699). This also fixes interactive `show` of results in
  `tur repl`: expressions such as `(+ 40 2)` previously displayed a corrupted
  value (e.g. `#<fn main>`) instead of `42`.
- **Additional macOS codegen fixes**: further carrier-straddle and CPS-IR emit
  fixes surfaced by the macOS toolchain (`emit_expr.c`, `emit_cps_ir.c`,
  `main.c`).

## [0.30.1] -- 2026-07-21

### Fixed

- **Generic `show`-wrapper monomorphization + `rc`-field double-free**: fixed a
  monomorphization bug in the generic show-wrapper and a double-free of
  refcounted struct fields (#697).
- **CPS effect-loop leak**: plugged an O(N) DK-node leak on tail-resumed effect
  re-opening (#696).
- **`Real-Random`/`Seeded-Random` codegen**: fixed nested static-fn C emit for
  the random instances (#693).

### Changed

- **CPS `shift`/`reset`**: serial and cloneable continuations are now folded
  into `shift`/`reset` by receiver-continuation capability (#695).

### Added

- **Trowel landing page**: a new web landing page for the Trowel toolchain.

### Docs

- **Strings guide**: added `docs/guides/strings-guide.md`, cross-referencing
  `cstr` vs owned `String` ownership (#698).

## [0.30.0] -- 2026-07-20

### Added

- **Owned String type**: a new owned, immutable, refcounted `String` type
  (owned-string-type-plan), with an opt-in `#s"..."` owned-String literal and
  a stdlib reader-macro path.
- **StringSlice**: zero-copy, bounds-checked, safe ranged views into a `String`.
- **`ShowString` typeclass**: an owned-String show surface, plus a
  `derive-show-string` derive macro, `ptr<void>`/`Bound` instances, and
  `Debug`/`Display` instances for `cstr`, `bool`, and the numeric types.

### Changed

- **`Show` returns an owned String**: the `Show` typeclass now returns an owned
  `String` rather than a `cstr`. `derive-show` is now the owned-String deriver;
  the old `cstr` path moves to `derive-show-cstr`. Interpreter Show is at parity.
- **stdlib String adoption**: stdlib migrates onto the owned `String` (owned
  bridge + path cluster, digest, and httpd CORS capture).

### Fixed

- **`rc<int>` angle-bracket annotation**: no longer silently becomes a type
  variable (#692).
- **Generic typeclass dispatch on opaque carriers**: fixed, along with derive
  alias labels.

## [0.29.1] -- 2026-07-19

### Fixed

- **gcc14 clean build**: a tree-wide representation-tracking sweep now casts
  int64<->pointer carriers correctly throughout codegen (spec-dispatch,
  cps->direct tail calls, ctor field args, inline-C anon-structs, control-form
  result assignments), so the compiler builds clean under gcc14 -- the
  `-Wno-error=int-conversion` and `-Wno-error=incompatible-pointer-types`
  escape hatches have been dropped.
- **effectful-fnvalue miscompile**: fixed a miscompilation of a polymorphic
  function value passed through the CPS ABI.
- **cps->direct dispatch**: cps->direct LETCALL now resolves to the defined
  monomorph clone, fixing an unmangled tcons reference.

### Changed

- **CPS owning-env teardown graduated (E3a/E3b)**: capturing an owning value
  into a multi-shot cloneable continuation is now always-on, including deep-copy
  clone of captured heap handles and auto-deferred scope-exit drops across a
  cloneable reset.
- **cps-async graduated**: the remaining fiber-interop gaps are closed and async
  CPS is now always-on.
- **Relaxed cloneable-shift requirements (E4a)**: an owning or non-Serializable
  value merely in scope at a cloneable shift no longer requires `Clone` /
  `Serializable`.

## [0.29.0] -- 2026-07-18

### Removed

- **The fiber-based effect runtime is deleted**: the legacy fiber effect
  handler runtime -- the second effect-lowering substrate that ran
  alongside the CPS/DK backend -- has been fully removed (Stage G). The
  delimited-continuation (DK) backend is now the sole lowering path for
  effects and handlers, and the dead `tur_effect_cont_resume` /
  `emit_effects_*` machinery is gone (#685).

### Changed

- **The CPS/DK backend is the sole effect-lowering path**: effectful
  handles (top-level, macro-expanded, and inside `defmodule`), first-class
  and dynamic handler values, effect-polymorphic higher-order functions,
  nested handles, escaping continuations, and self-handling async closures
  now all lower onto the DK backend instead of evicting to a fiber.
- **`cps-tramp-resume` graduated to always-on**: trampolined tail-resume
  is no longer experimental, giving flat (heap-bounded) effectful
  tail-recursion at scale (e.g. 1e6 iterations) with no flag required.

### Added

- **Windows bringup**: the compiler and runtime now build and run on
  Windows (#682).
- **`stdlib/logic.tur` is pure Turmeric**: the miniKanren logic engine was
  reimplemented with no inline C (#679).
- **`stdlib/re.tur` is a pure-Turmeric regex engine** (#676).
- **Cooperative session-channel runtime** under the interpreter (#677).
- **Shallow effect handlers (F2) and async/await on heap continuations
  (F3)** (#674).
- **Unbounded function arity**: the hard positional-parameter cap is
  removed; wide functions store args out-of-line, with a `TUR-W0041` lint
  nudge past 16 params (#669).

### Fixed

- **Numerous effect-runtime memory leaks closed**: DK continuation-chain
  nodes, straight-line and heap-join perform-continuation frames, shift
  receiver closure environments, and multi-shot resume snapshots are now
  freed; the compiler/codegen path is leak-clean under ASan/LSan
  (#681, #664, and follow-ups).
- **`Eq [Bound]` no longer misdispatches to bare-tyvar instances**; adds a
  `str-build` leaf (#673).
- **Owning `rc` capture into a multi-shot handler-case environment** is
  handled correctly (#680).

## [0.28.2] -- 2026-07-12

### Fixed

- **CPS backend temporaries are now `__`-prefixed to avoid user-code
  collisions**: the CPS-IR-to-C backend previously emitted temporary
  variables under bare names that could collide with identifiers from user
  code; every generated temporary is now `__`-prefixed, closing that
  namespace hazard (#661).

## [0.28.1] -- 2026-07-11

### Added

- **Debug sanitizers are now configurable via `TUR_DEBUG_SANITIZE`**: the
  ~40 hardcoded `-fsanitize=address,undefined` sites in the Debug build now
  route through a single `TUR_ASAN_FLAGS` variable driven by a new
  `-DTUR_DEBUG_SANITIZE=ON/OFF` CMake option (also gating the `eval_import`
  test's fixture-compile flags), so ASan/UBSan coverage can be toggled without
  editing the build. Adds a tight-timeout `tur --version` CI smoke check on
  Linux and macOS to catch startup-hang regressions (#660).

## [0.28.0] -- 2026-07-11

### Changed

- **CPS-IR-to-C backend is now the always-on codegen path**: the emitter
  that lowers a colored function's ANF/CPS IR directly to C -- under the
  ratified DK-threading ABI, covering the direct<->CPS boundary edges,
  letcont join points, and the load-bearing shift/reset case where a
  callee's shift is delimited by a caller's reset -- is no longer
  experimental. `emit-c` emits `__cps` bodies unconditionally, with no
  flag required. This release retires the last of its experiment-flag
  surface (#658), completing the graduation begun in the previous cycle.

### Removed

- **`--enable=cps-backend` flag surface fully removed**: with the backend
  always-on, the residual experiment-flag plumbing is gone.
  `--enable=cps-backend` now reports the standard unknown-experiment error
  (TUR-E0310) instead of an accept-and-warn shim (#658).

### Added

- **`--dump-direct-lowering-callers` metric**: new diagnostic counter
  reports the residual eviction and direct-dispatch caller populations
  reaching the direct-style delimited-control lowering, measuring progress
  toward retiring the legacy direct lowering (#659).

## [0.27.7] -- 2026-07-10

### Added

- **Eq/Show for every single-word collection element type**: `Vec`,
  `Set`, and `Map` are now `Eq`- and `Show`-able over any single-word
  element type, not just `:int` (#654).
- **Collection Show in the WASM REPL**: the Try Turmeric REPL renders
  bare collection results via `Show` instead of raw handles (#651).

### Changed

- **Delimited-continuation runtime relocated**: the DK runtime moves
  into place and the `reset`/`shift` and `call/cc` implementations
  close previously open gaps (#656).

### Fixed

- **Grounded tyvar ascriptions for string keys**: `Show[Set]` and
  `Show[Map]` over `cstr` keys now render the strings instead of raw
  values (#655).
- **Bare-head constrained instance dispatch**: the interpreter binds
  constraint tyvars for bare-head constrained instances at dispatch
  time (#652).

## [0.27.6] -- 2026-07-09

### Added

- **CPS-IR-to-C backend (experimental)**: new emitter lowers a colored
  function's ANF/CPS IR directly to C under a ratified DK-threading
  ABI, covering the direct<->CPS boundary edges, letcont join points,
  and the load-bearing shift/reset case where a callee's shift is
  delimited by a caller's reset. Gated behind `--enable=cps-backend`;
  off by default and neutral to the suite until a fixture opts in
  (#649).

## [0.27.5] -- 2026-07-09

### Added

- **Show instances for typed collections**: `Vec`, `Set`, and `Map`
  now have `Show` instances (in a new opt-in
  `stdlib/typeclass-show.tur`), and the REPL renders bare collection
  results as `[1 2 3]` / `#set{...}` / `#map{...}` instead of raw
  handles (#648).
- **Content-keyed `Set[A]`**: `Set`'s element API is generalized off
  `:int` and dispatches `Hash`/`MapKey` on the concrete element type,
  so `Set[cstr]`, `Set[Sym]`, and content-equal elements at distinct
  addresses now behave correctly. `Set[int]` output is byte-for-byte
  unchanged.

### Docs

- New plans for container Eq/Show element dispatch, boxed multi-word
  elements, and an owned `String` type; new reports on the WASM REPL
  Show-preload gap and the web REPL inline-C native gap.

## [0.27.4] -- 2026-07-09

### Added

- **Legible character literals**: `#\a`, `#\space`, `#\newline`,
  `#\u41`, and friends now read as ints holding the Unicode code
  point, matching Scheme-style character-literal syntax (#647).

### Docs

- Parser combinators tutorial refresh; new plan doc for legible
  character literals under `docs/upcoming/v1/`.

## [0.27.3] -- 2026-07-08

### Added

- **Stdlib preload for REPLs**: the WASM (Try Turmeric) REPL and
  `tur repl` now preload the stdlib on startup, so `(head ...)`,
  `(tail ...)`, and other stdlib forms are available at the prompt
  without a manual import.
- **Web deploy-gate CI**: a new CI workflow runs a Playwright smoke
  spec against the deployed web REPL to catch stale wasm /
  cache-version drift.

### Fixed

- **Try Turmeric stdlib gap**: the deployed web REPL was missing
  stdlib natives; the preload path and `sw.js` `CACHE_VERSION` bump
  restore parity with the CLI REPL.

## [0.27.2] -- 2026-07-08

### Added

- **Pure-reader admissibility (BR3b/c)**: by-ref aggregate params and
  transitive pure-reader borrow chains are now admitted in the
  catch-unwind trampoline.
- **Stackless catch-unwind aggregate returns**: support aggregate
  Result return types with correct box lifecycle and branching tails.

### Changed

- **Collection natives relocated into libturi** for interpreter /
  compiled-backend parity.
- **Effect-op seed locking** via mechanical coloring (F1) and the
  effect-rec probe (F4).

### Fixed

- **Interpreter buffer reclamation**: Vec, Set, and Map buffers are
  now reclaimed at env teardown (previously held for process
  lifetime).
- **HAMT delete**: refcount handling and double-free of shared
  siblings on lineage free.
- **Generator resume** no longer corrupted by intervening top-level
  evaluations.
- **Value-position if-branch** panic caused by miscompiled
  `(null) = ((void)0);`.
- **Panic / catch-unwind memory**: non-heap free of opaque panic
  payload, free-nonheap-object warnings, discarded Result box leak,
  and work-stack perform-capture on the panic path.
- **Escaping work-stack effect continuations** relocated during
  scratch promotion.
- **Interpreter**: list-concat crash and Option heap-payload unwrap.

## [0.27.1] -- 2026-07-07

### Added

- **Stackless catch-unwind** graduated to always-on. The compiled
  backend now models `catch-unwind` on the driver work-stack with
  panic-as-signal semantics (Phases C1/D1 through D3-G), surfaces the
  result as `(Result A B)` so `ok-val`/`err-val` infer, widens
  by-const-pointer aggregate param eligibility (BR3), and aggregates
  params in the group driver.
- **Van Laarhoven lenses**: nested-mapper dict dispatch lowered
  (Phases 1-3) via `forall-dict-pass`, which also graduates to
  always-on with multi-constraint dict-clone frame support.
- **Four HKT/forall experiment flags** graduated to always-on.

### Fixed

- **macOS-only dispatch-array overflow** from uninitialized main-body
  `EmitCtx`; zero-init on entry.
- **Panic-unwind box leak**: free the discarded catch-unwind Result
  box and the caught panic payload; free the work-stack perform
  capture on the panic path.
- **Async fiber stacks** are reclaimed on completion instead of held
  until env teardown.
- **Dict-clone result-type threading** for polymorphic methods.

### Docs

- Fix drifted sweet-exp toggle pairs in guides.

## [0.27.0] -- 2026-07-05

### Added

- **Manifest `:exports` accepts `#map{...}` literal syntax**. The
  `:exports` clause in `build.tur` now recognizes the `#map{k v ...}`
  data-literal form alongside the existing plain-map spelling, with
  dedicated diagnostics for malformed keys/values. See
  `docs/upcoming/exports-map-syntax-tighten-plan.md`.

### Fixed

- **REPL reload** rebuilt against the current wasm; `mise.toml` trimmed
  of stale entries.

### Docs

- Lens guide typo fixes.

## [0.26.6] -- 2026-07-05

### Added

- **turi value-pool: scratch/permanent split with escape promotion**
  (Phase A, #609). Bounds steady-state memory for a single long-lived
  `TuriEnv` (notebook-kernel pattern) without changing interpreter
  semantics. Split `TuriEnv.value_arena` into `value_scratch` (default
  alloc target) and `value_perm` (promoted survivors). Opt in with
  `turi_env_set_scratch_promotion`; at each `turi_eval` top-level
  boundary a Cheney-style two-pass deep copy relocates provably-safe
  escapees (scalars, strings, closures + captured frames/bindings/
  tyvars, structs) into perm, then rewinds scratch. Conservative bail
  on carrier-encoded bare-int pointers, live continuations/generators/
  handlers/futures, and scratch-resident refs. Off by default; the
  existing per-unit-env path is unchanged.

### Fixed

- **CI regen-snapshots `--check` now honors per-fixture `flags`
  files**. Previously ran `tur emit-c` without them, mis-reporting
  experiment-gated snapshots (e.g. `van-laarhoven-lens-wide-*`) as
  drift and threatening to regenerate them to empty.

### Changed

- **Docs**: the value-pool scratch-promotion plan is archived
  (executed); a follow-up
  `docs/upcoming/turi-value-pool-carrier-relocation-plan.md` captures
  the remaining carrier/live-C-state relocation tail.

## [0.26.5] -- 2026-07-05

### Added

- **Consumer monomorphization graduated; `vl-wide-mono` retired**
  (CM1-CM4). By-value HKT across the van Laarhoven lens boundary
  (Path B) is now unconditional; the `--enable=vl-wide-mono`
  experiment is gone. CM1 resolves each consumer's lens param to the
  full set of concrete lenses reachable at its call sites via a
  fixpoint. CM2 emits box-free consumer clones
  (`<consumer>__lens_<hash>`) for ambiguous (|set|>=2) params. CM3
  rewrites those call sites to the clones, dropping the lens arg.
  CM3-transitive specializes forwarding consumers through the call
  graph. CM4 gates composed lenses back to Path A via a
  `has_composed_lens` poison so the graduation is safe.
- **By-value propagation for composed van Laarhoven lenses** (CB1-CB5).
  A composed lens (`line-a-x`, whose body tails into another lens via an
  adapter lambda) and any consumer passed one now thread `(f a)` by value
  end to end on Path B -- no carrier box at any composition crossing. The
  resolve pass re-admits composed lenses and registers their nested lenses'
  `<lens>__mono` bodies; the poly-call emit redirects each nested
  application to the nested mono body; VBM2b mints a by-value twin of the
  adapter closure. `van-laarhoven-lens-wide-compose` runs by value with an
  `expected.c` codegen snapshot.
- **REPL terminal escape-sequence handling** for cursor / key events in
  `src/turi/repl.c`.

### Changed

- **Docs**: `docs/guides/lens-guide.md` now documents Path B as always-on
  with the simple-vs-composed distinction and the two Path A fallbacks
  (runtime-selected + composed lenses). Resolved `docs/upcoming/` plans
  moved to `docs/archive/`; the residual by-value-propagation slice for
  composed lenses is captured in
  `docs/upcoming/v2/van-laarhoven-composed-byvalue-plan.md`.

## [0.26.4] -- 2026-07-04

### Added

- **By-value van Laarhoven monomorphization: spec discovery** (VBM1,
  #605). Under `--enable=vl-wide-mono`, wide-functor lens call sites
  register per-concrete-functor specs the emitter will later drive.
- **Cross-procedural concrete-lens resolution** (VBM2a, #606). A new
  `mono_specs_resolve_program` pass joins each abstract lens spec to
  the concrete lens passed at every top-level call of its enclosing
  fn, collapsing abstract `(l g s)` pins to a resolved emit key.
  `--dump-mono-specs` prints both the abstract and resolved tables.

### Changed

- **CLAUDE.md: green-suite gate excised.** `bash tests/run.sh` is a
  signal, not a gate. A red suite -- intermediate or otherwise --
  never blocks a commit, PR, or calling a change done.

## [0.26.3] -- 2026-07-03

### Added

- **User-level experiments file** (UC-2/UC-3). A
  `~/.config/turmeric/experiments.tur` list enables experiments across
  projects; CLI `--enable=` and manifest `:experiments` still take
  precedence.

### Changed

- **`TUR_M7_HKT` feature gate retired** (#603). By-value HKT is the
  only path; the conditional carrier code and env-var branch were
  collapsed.
- **CI skips the CPM.cmake fetch** when running in CI environments,
  avoiding an unnecessary network dependency.

### Removed

- **`--allow-experimental` was retired** (UC-4). Enabling an experiment
  -- via `--enable=<name>`, a `build.tur` `:experiments` list, or
  `~/.config/turmeric/experiments.tur` -- is now itself the
  acknowledgment. The `TUR-W0060`/`TUR-W0061` lifecycle warnings fire
  whenever an experiment is enabled, with no way to silence them.
  Passing `--allow-experimental` is a hard error with a targeted
  "retired" message for one release; remove the flag from any scripts.

### Fixed

- **By-value Option/Result if-join and Result-parameter codegen** now
  produce correct C output (#601).
- **`-Wint-conversion` on HRT poly-calls** with pointer-class return
  types eliminated (#600).
- **Defstruct bracket fields** are no longer dropped when they follow
  a type-param vector (#599).
- **Method-level HKT tyvars** now ground through match-arm unification
  (#598).

## [0.26.2] -- 2026-07-02

### Added

- **Wide by-value van Laarhoven functors through the lens boundary**,
  behind `--enable=vl-wide-functor` (WF1).

### Changed

- **Syntax and LSP tweaks** across the main driver, stdlib docstrings,
  and vim highlighting.

### Fixed

- **Match-arm error path** no longer leaks linear-state snapshot
  buffers.
- **Wide by-value van Laarhoven functors** now report a targeted
  `TUR-E0309` diagnostic instead of miscompiling.

## [0.26.1] -- 2026-07-02

### Added

- **Constrained HKT + rank-2 `forall`, slices 1-4.** Kind annotations,
  constraint propagation, higher-kinded rank-2 quantification, and
  van Laarhoven lenses (`view`/`set`/`over`).

### Changed

- **Class-method result functor inferred from the receiver**, so
  `fmap`/`bind` sites no longer need explicit functor annotations.
- **Generic focus type flows through rank-2 `forall` lens arguments**,
  making van Laarhoven `view`/`set`/`over` inferable at use sites.
- **`tur run` improvements.**

### Fixed

- **Poly combinator element tyvar** grounded through the returned
  closure's application, unblocking inference of polymorphic
  combinators that return functions.

## [0.26.0] -- 2026-07-02

### Added

- **`interpret` CLI subcommand** and REPL `:explain` meta-command
  (native + web), rounding out the introspection story.
- **REPL clickable diagnostics (L3)** and stale-buffer tracking (L4)
  for the web REPL; L5 split out for follow-up.
- **CLI auto-suggest** for mistyped subcommands.

### Changed

- **structdef-retirement, slices 4 + 5 + DS-D.** Lower `:linear`
  defstructs and `defopaque` to record ADTs; remove the `TY_STRUCT`
  type representation and the dead struct-app registry / typedef
  chain; migrate single-occurrence param placeholders to named
  tyvars.
- **element-field plan (EF-1..EF-4).** Route `(arrow ...)`/`(-> ...)`,
  `(handler E V R)`, and `Session`/`project`/`Role` as struct/ADT
  fields; shelve `forall` as a field (design decision); reject bare
  descriptors + `Global` at that position.
- **`with` on narrowed multi-variant ADTs (CONV-S4N)** and unified
  struct/variant construction diagnostic wording (CONV-S6).
- **`cstr` byte primitives reimplemented in pure Turmeric.**
- **turi stdlib inline-C parity** for the list/map/vec/slice
  representation bridges.
- **Test-performance-optimization-plan phases 2 + 3** land, cutting
  suite wall-clock.
- **Parser combinator tutorial rewritten.**

### Fixed

- **Three parametric-ADT / codegen reports** archived after fix.
- **Option-monomorph mangler collision** over unresolved placeholders.
- **`ok`-construct float-payload-into-carrier-box value coercion.**

## [0.25.6] -- 2026-06-28

### Added

- **Web REPL multi-tab editor.** Drag-reorderable tabs, project
  download as zip, project load from zip, mobile overflow menu, and
  Playwright multi-tab specs.
- **Debugger upgrades.** DAP gains an in-frame expression evaluator
  and conditional breakpoints; LLDB pretty-printers for core types;
  VS Code Native Debugging configuration and integration docs.

### Changed

- **`defstruct`-as-`defadt` seam 4 graduation.** Force-lower blockers
  dropped from 212 to 60 across the suite via the seam-4 work
  (adt-recursive, hkt-ap-fn-in-container, Option/Result pointer-slot
  parity, by-value ADT return bridges).

### Fixed

- **Nested-carrier match losing concrete element type.**
- **stdlib `future`/`taskgroup` nested-function hoisting** so the
  GCC nested-function extension is no longer required; `str.tur`
  returns typed correctly.
- **macOS `ucontext_t` ABI mismatch** that made `TuriEnv` layout
  translation-unit-dependent.
- **Apple clang 17 `-Werror=int-conversion`** noise in generated C
  suppressed at the build-system level.

## [0.25.5] -- 2026-06-26

### Added

- **Explicit `#fx{...}` reader form for effect rows.** Phase 1 of the
  fx-row-syntax-rename-plan: `#fx{...}` is the new canonical spelling,
  while legacy bare `#{...}` and `@{...}` effect rows now carry
  provenance tags so elab can emit deprecation diagnostics (TUR-D0002 /
  TUR-D0003) on first consumption.
- **`tools/migrate-fx-rows.py`** automates the rewrite from the legacy
  spellings to `#fx{...}`; applied across the stdlib and the fixture
  suite.

## [0.25.4] -- 2026-06-26

### Added

- **PWA support and mobile split-view for the web REPL.** Service
  worker, install manifest, custom `_headers`, kill-switch SW for
  recovery, and a mobile split-and-PWA Playwright spec covering
  `/try`.

### Changed

- **Curly-infix `{a + b}` is now enabled in the default
  s-expression dialect, not just sweet-exp.** Reader-form change
  in `src/compiler/reader.c` with three new default-dialect
  fixtures (`curly-infix-default-{basic-add,mixed-vars,nested}`).
- **Contract types must use the explicit `#refine{var : T | pred}`
  reader form** instead of the former bare `{var : T | pred}`
  overload, which is freed up for curly-infix. Two new fixtures
  (`refine-basic`, `refine-in-defn`); reader-forms,
  contract-types, and syntax guides updated.

## [0.25.3] -- 2026-06-26

### Added

- **CONV-S1 struct/ADT convergence widens to parametric, fn-field, and
  pointer-field structs (#551, #554, #561, #566, #567).** `defstruct`
  lowering now handles by-value ADT products with nested aggregates,
  rc<ADT> field access, drop-glue, large-ADT pass-by-pointer ABI, typed
  `fn` fields, and parametric record-ADT field access with
  instance-head resolution.
- **Parametric ADT by-value monomorphisation flipped on (P2-P4, #559).**
  Wire crossings landed and the gate is on.
- **Heap-ADT auto-lowering + carrier bridges for parametric `:heap`
  (#568, #569).** Seam 3 of the CONV-S1 ABI work: typed-pointer ADT
  foundation plus auto-lowering and bridges.
- **B4 slice 2: wide by-value ADT closure params via heap-box carrier
  (#563).** Wide ADTs survive fat-closure boundaries.
- **Typed consumers for `Set` (#564) and `Eq[Cons]` via typed
  `(Cons__A *)` consumers (#553).**
- **libturi embed peripherals: Gaps 1-8 (#547, #549) + configurable
  include paths for embedders.**
- **Debugger Phase 4 + 5 (#548, #550).** Native source maps for
  `emit-c`; native type-name audit and gdb pretty-printers.
- **Typed native registration (#552).** Curated facades over embedder
  natives now type-check.
- **TUR-W0040: warn on eval-mode unknown call heads (#560).**

### Changed

- **`Map` is now a non-transparent heap struct backed by a HAMT pointer
  (#555).**
- **Macro args elaborated before expansion; list-macro quote vs
  syntactic-symbol fixes; nested vec literals collapse to a runtime
  vec.**

### Fixed

- **By-value Result/Option field access aggregate-to-pointer cast
  (#557).**
- **Multi-param struct-app spine preservation for param/return types
  (#556).**

### Docs

- **Godot binding guide (G5 prep) + typing follow-ups plan
  (G6.1-G6.3).**
- **Windows cross-compile bootstrap plan (#562).**
- **Cross-script calls section (T4.A starter); Tier 3 and Tier 4 plans
  archived as shipped.**

## [0.25.2] -- 2026-06-24

### Added

- **Interpreter debugger (`tur debug`) + DAP server (#543, #544, #545).**
  Three-phase debugger: source-spans audit and coverage gate, an
  interactive REPL-style stepper over the interpreter, and a Debug
  Adapter Protocol server so editors can attach.
- **By-value ADT representation for leaf-scalar products (CONV-S1/B3,
  CONV-S2/S4, #540, #541, #546).** Struct/ADT convergence:
  record-style variants, keyword construction, match-on-struct, and
  by-value layout for single- and multi-variant ADTs without the
  carrier indirection.
- **Mise plugin.** Install Turmeric via `mise`.
- **Try Turmeric (web playground) overhauled.** PWA manifest +
  apple-touch-icon, non-scrolling mobile layout, editor/console
  persistence across reload, iOS safe-area + focus-zoom handling, and
  a legible mobile Examples dropdown.

### Changed

- **turi interpreter perf.** Env-owned value-arena pool (#537),
  pooled inline-C escaping buffers, and coroutine-stack tracking
  (#539).
- **Spice `__ok` / `__err` migration complete.** All 13 spices
  (json, sqlite, png, rtaudio, osc, template, wav, postgres, valkey,
  ...) now use the canonical Result builders; umbrella plan archived.

### Fixed

- **By-value struct/ADT results now survive the closure ABI (#538).**
- **Invalid C initializer when let-binding a by-pointer struct param
  (#542).**

## [0.25.1] -- 2026-06-24

### Added

- **Typed inline-C Result/Option builders.** New preamble helpers
  `tur_ok_ptr` / `tur_ok_int` / `tur_err_ptr` / `tur_err_int` /
  `tur_some_ptr` / `tur_some_int` / `tur_none` let an inline-C body
  return real `(Result T E)` / `(Option T)` values that flow straight
  into stdlib `ok?` / `ok-val` / `some?` / `unwrap` -- no hand-rolled
  struct, no `:ptr<void>` escape hatch, and no `(int64_t)(intptr_t)`
  cast at the call site. The `_int` and `_ptr` suffixes spell out the
  payload's cast direction; a `_Static_assert` pair pins the
  Result/Option byte layout to the stdlib shape. New guide:
  [`docs/guides/inline-c-results-guide.md`](docs/guides/inline-c-results-guide.md).
- **Struct ergonomics: auto-bound constructor, keyword args, with-update
  (#535).** `defstruct` now generates an auto-bound constructor; struct
  literals accept keyword arguments; new `with` form copies a struct
  with selected fields replaced.

### Fixed

- **Parametric struct fn-field call-through carrier-ptr ABI mismatch
  (#534).** Calling a function-typed field on a parametric struct no
  longer trips the carrier-vs-by-value ABI seam.
- **`(. obj field args)` receiver-first dot routing (#533).** Method-style
  dot syntax now routes through the receiver type's method table even
  when arguments follow the field name.
- **Honor `#[used]` on single-file / whole-program build path (#532).**
  The `#[used]` attribute is no longer dropped when the build is invoked
  on a single file or in whole-program mode.

## [0.25.0] -- 2026-06-23

### Removed

- **`throw` / `try` / `catch` deleted.** The Phase S4 exception forms
  are gone end-to-end: elab arms (`elab_throw`, `elab_try_catch`),
  `EX_THROW` / `EX_TRY_CATCH` IR enum tags and struct members, codegen
  arms in `emit_expr.c` / `emit_stmt.c`, the borrow-check arm, the
  interpreter eval cases plus the `DK_TRY_BODY` / `DK_TRY_CATCH`
  driver continuations, `turi_native_throw` / `make_throw_val`, and
  the `TUR-D0002` deprecation warning. Use `result<T,E>` for
  recoverable failures and `panic` / `catch-unwind` for unrecoverable
  ones. Runtime helpers (`tur_panic_with`, `tur_catch_unwind`, the
  `setjmp`/`longjmp` plumbing) stay -- they're load-bearing for
  `panic`. See
  [`docs/archive/throw-deprecation-plan.md`](docs/archive/throw-deprecation-plan.md).

### Changed

- **Interpreter fiber rejection now flows through a new
  `TURI_REJECTION` value tag.** `(await ...)`, `with-timeout`, and
  `task-cancel` produce `TURI_REJECTION` instead of throwing; callers
  observe rejections with `(error? r)` / `(error-message r)` instead
  of `(try ... (catch [e] ...))`. The new tag is distinct from
  `TURI_ERROR` so binding a rejection in `(let [r (await ...)] ...)`
  does not trigger the interpreter's universal error short-circuit.

### Fixed

- **Opaque / struct tyvar binding lost through closure arg position
  (#530).** Type-variable bindings for opaque newtypes and structs
  now survive being threaded through a closure argument position.
- **Typed inline lambdas as macro arguments now splice verbatim
  (#529).** Typed `(fn [...] ...)` literals passed to a macro are
  preserved through quasiquote expansion instead of being re-parsed
  as raw symbols.

## [0.24.3] -- 2026-06-23

### Added

- **STM finished on TL2 fine-grained locking (#528).** The half-built
  per-TVar-mutex commit path is replaced with a complete TL2
  (Transactional Locking II) discipline in both the reference runtime
  and the inline-emitted runtime compiled programs use. Reads are
  lock-free with version revalidation; commits lock only the stripe
  buckets covering the write set, bump a global version clock, and
  publish writes under a per-TVar lock bit. `retry()` parks on a
  bucket-filtered cond. New `stm-stress` (8 threads x 500 contended
  increments, TSan-clean) and `stm-retry-wakeup` fixtures.
- **Stackless delimited control and `call/cc` on the work-stack
  (#526, #527).** Heap-bound single-operand black-box forms (N3) and
  stackless `shift`/`shift0`/`call/cc` (N4) move off the re-entrant C
  frame onto the driver work-stack, removing two synchronous
  `turi_call` re-entry sites.
- **Experimental feature flag mechanism (`--enable=<name>`) (#524).**
  New `EXPERIMENTS[]` registry in `src/runtime/experiments.c` with
  `name`, `summary`, `plan_path`, `introduced`, `expires_at`,
  `lifecycle`, and `opt_global` fields; lifecycle warnings TUR-W0060
  and TUR-W0061 fire on use. See
  [`docs/guides/experimental-flags-guide.md`](docs/guides/experimental-flags-guide.md).

### Changed

- **`try`/`catch`/`throw` deprecated.** Use sites emit a deprecation
  note pointing at result/option-based error handling. The legacy
  fixture is parked under `_legacy-throw/`.

### Fixed

- **Parametric `defstruct` + fn-typed field gaps (#523, #525).** Type
  arguments for parametric structs are now inferred from fn-typed
  fields, closing the remaining defstruct holes.
- **Multi-param projection + functional dependencies for associated
  types (#522).** Assoc-types-2 Part A lands; multi-parameter
  projection composes with fundeps.
- **Workspace `:members` seed transitive cmake-deps (#521).** Enclosing
  workspace member lists now feed transitive cmake dependencies on
  spice builds.

## [0.24.2] -- 2026-06-23

### Changed

- **Dead `-X` feature-flag guards removed (#520).** Now that every
  gated feature is always-on (v0.24.0), the trivially-true
  `if (g_*_enabled)` guards in elab/emit have been unwrapped and the
  dead `if (!g_*_enabled)` branches deleted. The
  `extern bool g_*_enabled` symbols stay defined in `globals.c`, so
  any external code linking against them keeps working.

### Docs

- Added the experimental-flag-mechanism plan under
  `docs/upcoming/v1/` and refreshed the bundled web/WASM build.

## [0.24.1] -- 2026-06-23

### Added

- **`ref<T>` auto-drop on scope exit (#519).** Affine `ref<T>` values
  are now automatically dropped when they go out of scope, removing
  the manual `(drop! r)` boilerplate previously required.
- **`TurChannel` session type runtime in codegen preamble (#517).**
  Session-typed channels now have their supporting runtime emitted as
  part of the standard codegen preamble.
- **Spice-level vendored C sources via `:c-sources` / `:c-includes`
  (#516).** `build.tur` manifests can now declare bundled C source
  files and include directories that build with the spice.

### Fixed

- **Auto-loaded `defmodule` stdlib macros now promoted to global
  visibility (#515).** Macros defined inside auto-loaded stdlib
  modules are visible at the use site without manual re-import.
- **Always-on session/linear fixtures (#518).** Fixture failures that
  surfaced when session types and linearity went always-on are
  resolved.

## [0.24.0] -- 2026-06-23

### Changed

- **All 16 `-X` feature flags are now accept-and-warn no-ops; every
  gated feature is on by default (#TBD).** The flags
  (`-Xlinear`, `-Xsubstructural`, `-Xunique-types`, `-Xgadt`,
  `-Xunion-types`, `-Xintersection-types`, `-Xeffect-types`,
  `-Xcontracts`, `-Xsessions`, `-Xdynamic-vars`, `-Xcallcc`,
  `-Xsized-types`, `-Xdata-literals`, `-Xjson-reader`,
  `-Xschema-reader`, `-Xsymbols`) are still recognized for source
  compatibility, but each one emits `TUR-W0050` and otherwise does
  nothing. Existing `build.tur` files and CI invocations keep
  compiling unchanged. See
  [`docs/guides/compiler-flags-guide.md`](docs/guides/compiler-flags-guide.md)
  for the removal list and
  [`docs/upcoming/v1/drop-x-flags-plan.md`](docs/upcoming/v1/drop-x-flags-plan.md)
  for the plan.
- **`--strict-effects` no longer auto-on with effect types.** When
  `-Xeffect-types` was a real flag, passing it also enabled
  `--strict-effects`. Effect typing is now always on, but
  `--strict-effects` stays opt-in -- code without explicit `forall [e]`
  annotations no longer gets the nudge unless you pass
  `--strict-effects` explicitly.
- **Partial features go always-on at their current completion level.**
  `-Xunique-types` (UT0--UT3) and `-Xsized-types` (SZ0--SZ9; static
  checking covers folded-constant sizes, runtime assertions cover
  open-expression sizes) now light up for every program. Existing
  not-yet-shipped-bit diagnostics (`TUR-E0260`, etc.) continue to fire
  unchanged.

### Internal

- 283 fixture `flags` files containing only `-X` tokens deleted; the
  rewritten `docs/guides/compiler-flags-guide.md` is now a short
  diagnostic-flag reference plus a "Removed Feature Flags" pointer
  table.

## [0.23.3] -- 2026-06-23

### Deprecated

- **All `-X` feature flags will become accept-and-warn no-ops in the next
  minor release (0.24.0).** Every feature currently gated behind an `-X`
  flag will be unconditionally on; `-X<name>` will still be recognized for
  one full minor line and emit `TUR-W0050` instead. Downstream `build.tur`
  files that pass `-X` flags will continue to compile without modification.
  See `docs/guides/compiler-flags-guide.md` and
  `docs/upcoming/v1/drop-x-flags-plan.md` for the full removal list and
  rationale.

## [0.23.2] -- 2026-06-23

### Added

- **Turmeric Version Manager (`tvm`) (#511).** Install, use, and switch
  between Turmeric releases from the command line.
- **Uniqueness types: UT2 inference + UT3 stdlib patterns (#513).**
  Inference for uniqueness annotations plus stdlib coverage of the
  common patterns.

### Changed

- **R2 dispatch-side constraint-var mapping collapsed to a shared kernel
  (#508).** One audited mapping path instead of per-site variants.

### Fixed

- **`ap` fn-in-container by-value monomorphization (#507).** Regression
  coverage added; plan archived.

### Internal

- **IT4 close-out: `defdata`-as-union fixture; stale parity-check path
  fixed (#514).**
- **Sized-types marked complete; docs and plans archived (#512).**
- Archived turi-parity-post-v1, HKT dispatch options tradeoff, and
  turi-interpreter-gap-closure plans (#506, #509, #510).

## [0.23.1] -- 2026-06-22

### Fixed

- **Route value-side carrier<->concrete recovery through one chokepoint
  (#505).** Consolidates the value-side recovery paths so remaining
  carrier-crossing edge cases land on a single, audited transition rather
  than ad-hoc per-site rewrites.
- **Fix output in Try Turmeric (web REPL).**

## [0.23.0] -- 2026-06-22

### Added

- **Track C / U3 advances to 2/4.** sqlite TStmt landed end to end.
- **Track C / U6 closes.** Typed variadic c-dsl builders finished; v2 plan
  filed for the valkey follow-up.

### Changed

- **`(list ...)` is element-type polymorphic via `tcons-of` (#473, #488).**
  Float heads and other non-int elements now flow through the classic
  list surface without ascription.
- **Applied type constructors accepted in `defdata` constructor fields
  (#483).**

### Fixed

- **Carrier<->by-value bridging across constrained-generic instance
  dispatch (#490, #491, #493-#495, #497, #503, #504).** Ascribed and
  no-int-instance paths, float reinterpretation, by-value field access
  through ascribed receivers, return-dispatched by-value-struct method
  elements specialized into generic loops, struct-headed applied-instance
  matching, and unascribed carrier-helper reads in constrained instances
  all resolved.
- **HKT cata over function-typed carriers (#489, #499).** Fn result
  threading and match-arm payload capture fixed; segfault when the
  function carrier's argument is itself a function eliminated.
- **Captureless algebra arms and letrec self-capture under fat closures
  (#500-#502).** Mixed fn/value carrier env-struct collisions carved;
  captureless arms no longer thicken through fat-closure carriers;
  letrec self referenced from a nested closure now captures correctly.
- **By-value parametric struct field layout (#481, #482).** Fields embed
  the aggregate; constrained generics returning parametric containers
  monomorphize correctly.
- **Nested generic by-value construct in constrained instance bodies
  (#480);** parametric `:heap` struct field extraction no longer
  collapses to the carrier (#479); recursive `(Cons A)` specs no longer
  hijacked by carrier-erasure ascription (#498); control-form result
  temps thread the by-value carrier-ABI aggregate (#492).
- **Multi-index opaque cross-parameter unification for sized matrices
  (#476).**
- **Constrained instance method dispatch on pointer-carried element
  types (#475).**
- **`type_name` emit-path leak on composite type diagnostics (#477).**
- **CI: fmt bootstrap, REPL spice loading, and turi gates greened up
  (#474).**

## [0.22.0] -- 2026-06-20

### Added

- **Turi interpreter parity (Track E closed, #398).** EX_CONS_LIST support
  in the interpreter (#435) plus 7 turi fixture fixes (#457) bring the
  tree-walking evaluator to parity with the compiled path.
- **`#[used]` attribute -- retain a defn with external C linkage (#467).**
  Keeps defns reachable only via raw `extern` (Arrow release fns, qsort
  comparators, signal handlers) from being demoted to `static`. Unblocks
  the `frame` spice's group/interop/reshape suites.
- **Classic Lisp list surface in stdlib (#470).** `car`/`cdr`/`null?`
  alongside the existing `head`/`tail`.
- **Migration diagnostics + guide for legacy `:int`-pointer struct
  forms (#466).**

### Changed

- **End-to-end monomorphization (Track A) lands (#444).** Carrier->by-value
  bridging completes across constrained generics, HOFs, existential
  pack/open, option/result accessors, and tail calls into inline-C ADT
  helpers. Touches dozens of PRs (#395-#469); audit floor moves to 0
  carrier deref-copies on the by-value path.
- **TCO lands in ABI specs.** Eq[Vec]/Eq[Map]/Eq[Set] rewritten as
  pure-Turmeric TCO'd loops (#400, #424); tail calls now bridge to
  inline-C carrier-ABI helpers at the return site (#415, #416).
- **MutableMap retyped to honest `(MutableMap K V)` (#396);** its carrier
  bridge is retired.
- **Vec inline-C producers monomorphized to typed pointers** (Track A
  bucket A follow-ups, #391/#393).

### Fixed

- **Spice-uplift wave.** Two separate-compilation codegen blockers (#465);
  prelude monomorphized specs no longer emit `static` with external
  linkage; file-scope inline-C include guards no longer corrupted by the
  dedup pass (`stats` spice unblocked); ECS E2d typeclass dispatch gaps
  closed (#405); StorageOps bounded-wrapper heterogeneous monomorphization
  gap closed (#447, #448); MutableMap typed-pointer producer
  monomorphization (#411); `time` importable as a module (#410).
- **`-lm` is now linked unconditionally (#471),** so pure spices can use
  libm symbols (`sqrt`/`fabs`/`sin`/...) without a fake cmake-dep.
- **letrec self-recursive float accumulator no longer collapses to int
  carrier (#469).**
- **HKT instance-method spec emitted when consumed by a match scrutinee
  (#468);** Applicative `ap` preserves fn type through polymorphic
  constructors (#438); layer-4 by-value `<|>` / selection-body shape
  (#442).
- **Closure capture of bindings referenced inside open/pack/dispatch
  (W3, #464);** letrec carrier self-call typing and fresh `vec-new` arg
  unification (W1/W2, #463); NULL-deref in `elab_lookup_ctor` on
  malformed defdata field type (#462).
- **Existential pack/open of multi-field struct payloads via heap-boxing
  (#420);** existential `open` dispatch through packed witnesses (#452);
  by-value struct payload rejected in constrained pack with a clear
  diagnostic (#455).
- **Return-type unification: cstr-commit/integer-body (TUR-E0708, #454)
  and float-vs-non-float register-class (TUR-E0707, #453) mismatches now
  rejected.**
- **CT macro evaluator: `map` and nested macros now work in splices
  (#406).**

## [0.21.0] -- 2026-06-15

### Added

- **Sized types ON by default (#392).** `-Xsized-types` flips on, with
  SZ8 recovering size indices from variables and struct projections
  (#367).
- **`EX_CONS_LIST` interpreter support** plus an autolink-duplicate
  fix.

### Changed

- **Vec migrated to the `:heap` typed-pointer ABI (#377).** Carrier-based
  Eq[Vec] retired (audit 98 -> 70, #393) and Vec inline-C producers
  monomorphized to typed pointers (Bucket A, #391).
- **M5 Option C: by-value twin redirect for carrier stdlib accessors
  (#369).** Carrier->concrete return deref for by-value instance
  methods; the EX_ASCRIBE CK_CONCRETE->CK_CARRIER bridge is gone
  (#368).
- **Pure-Turmeric fat-closure dispatch finishes de-inline-C.** Ascribing
  `:int` / `:ptr<void>` carrier to a fn type marks it boxed.
- **cfnptr `:usize` / `:isize` lower to `size_t` / `ptrdiff_t`**, and
  `ptr<const-T>` lowers to `const T*`.

### Fixed

- **Parametric `:linear` opaques now enforce single-use.** A
  `(defopaque Name [T] :int :linear)` previously compiled a double-use
  cleanly under `-Xsubstructural` because the four TY_APP construction
  sites hardcoded `copy_kind = CK_COPY`, dropping the head's substructural
  qualifier. `src/compiler/types.c::propagate_app_discipline` now lifts
  `:linear` / `:affine` from the head onto every application node;
  invoked from `type_app`, both `substitute_*_app_type`, and
  `elab_types.c::type_expr_from_form`. Regression:
  `tests/fixtures/errors/parametric-linear-double-use/`. Unblocked the
  ECS `WriteCap<T>` capability surface (Phase I of the ECS prereq
  plan).
- **Generic-dict dispatch re-resolution under `--interpret` (#386)**,
  carrier-fallback instance method dispatch under `--interpret` (#381),
  and return-dispatch on constrained type variables (with deserialize
  support).
- **Carrier-source Result/Option bridge for sub-word payload types.**
- **Generic-of-generic carrier callees emit via a carrier-relay
  closure.**
- **`make-struct` cstr->int64 carrier field bridge cast (#374)** and
  phantom-typeparam lowering for mixed phantom/by-value structs.
- **c-fn-ptr typedefs emitted to the module header before use (#375).**
- **Workstealing-balance deque race (#372).**
- **`Serializable[int].deserialize` avoids signed left-shift UB.**

### Removed

- **EX_ASCRIBE CK_CONCRETE->CK_CARRIER bridge (#368).**
- **Dead carrier-int `vec-eq-loop`** following Eq[Vec] retirement.

## [0.20.0] -- 2026-06-11

### Added

- **Variadic HKT rows (#330)** -- `^&` row-kinded parameters land all six
  layers; backs the ECS variadic for-each surface.
- **Associated type members on typeclasses (#327)** -- single associated
  type with dictionary-free type-level projection.
- **Application image dumps** -- serializable continuations + image
  dump/restore via the AI1/AI2/AI4/AI5/AI6 plan; resource-reacquisition
  hooks for replay; see [image-dumps-guide.md](docs/guides/image-dumps-guide.md).
- **Sized types for GADTs** -- SZ6-SZ8 type-level index with
  cross-parameter unification; see
  [sized-types-guide.md](docs/guides/sized-types-guide.md).
- **Turi interpreter parity (TI1-TI4)** -- `EX_LETREC` + `EX_SET_FIELD`;
  generators (`gen`/`yield`/`gen-next`/`gen-done?`); abortive delimited
  control + STM.
- **Sweet-exp manifests + codemod (#322)** -- `build.tur.sweet` manifest
  support (SW0-SW8); `fn`-type-colons codemod extended to
  `definstance`/`defclass`/`defprotocol`.
- **`build/` output directory** -- `tur build` routes artifacts under
  `<root>/build/{obj,bin,lib}/`; configurable via `--build-dir` /
  `TUR_BUILD_DIR` / `:build-dir`.
- **Deep transitive `:cmake-deps`** -- recursive walk + fetch-site
  cycle/conflict detection.
- **bare-fat-result-monomorphization (Phase B)** -- non-tail float
  register-class via per-call-site monomorphization.
- **ECS prereq compiler fixes** -- typeclass-constrained `defn` parsing,
  `F_TYPE_ANN` unquote in `substitute_params`, top-level `(def name init)`
  emission into `__tur_module_def_init` constructor, CBLOCK quasiquote
  auto-wrap, top-level `(do ...)` splicing.

### Changed

- **Phase 21: serial-shift capture grammar generalized** to do-sequences
  and beyond.
- **TUR-E0706: hard error for non-capturable serial-shift contexts
  (#331)** -- replaces the silent-0 miscompile.
- **`fn`-type leading colons rejected (Phase 4)** -- inside `(fn ...)`
  type expressions.
- **Per-declaration dedup for file-scope inline-C blocks.**
- **Stdlib consolidations** -- type-erasure-cleanup and hkt-consolidation
  tracks closed.

### Fixed

- **Multi-line inline-C formatting + stdlib layout (#324).**
- **Project-mode spice builds** -- fat-closure codegen + `defstruct`
  typedef emission + RC runtime preamble (T1-T11) all landed.
- **Archive churn sweep** -- 19 resolved plans/reports moved to
  `docs/archive/history/`.

## [0.19.1] -- 2026-06-06

### Added

- **Editor color themes** -- ships Emacs (`turmeric-dark-theme.el`), Vim
  (`vim-syntax/colors/turmeric-dark.vim`), and VS Code
  (`vscode-syntax-ext/themes/turmeric-dark-color-theme.json`) dark themes
  tuned for Turmeric syntax.
- **Reversible name mangling (#301)** -- executes plan stages T3/T5/T6/T7/T8;
  kebab/snake coexistence and operator mangling are now round-trippable.

### Changed

- **`>>>` rewritten to polymorphic typed form** -- removes the float-specific
  `compose-float-*` helpers in favor of a single typed `compose` arrow.
- **Fixture-codegen decoupling (#299)** -- Phase 1 of the fixture-churn-paydown
  plan strips ~200 KLOC of preamble noise from `tests/fixtures/*/expected.c`,
  so future codegen tweaks no longer ripple through hundreds of snapshots.

### Fixed

- **Project-mode `defstruct` typedef missing** -- `emit_module` now hoists
  struct typedefs ahead of the inline-C uses that reference them, fixing
  project-mode spice builds.
- **File-scope inline-C block emit ordering** -- elements in inline-C blocks
  now emit in source order, fixing a latent miscompile.
- **`^fat` let-binding of `ptr<void>` fat closure** -- partially addressed in
  `emit_module`; remaining cases tracked in `docs/reported/` (#305, #306).

### Docs

- Guides refresh for v0.19 (arrows, effects, error-handling, GADTs, httpd
  middleware, session types, compiler flags, delimited control).
- Docs archive curated: completed plans moved under `docs/archive/history/`.

## [0.19.0] -- 2026-06-05

### Added

- **Typed closure invocation ABI (#276)** -- `TUR_APPLY{0..4}_T` macros thread
  declared `fn` argument and return types to the C invocation site, retiring
  int64-only erasure through `TUR_APPLYn` / fat-shims. Closures taking or
  returning `:float`, `:ptr`, `:bool`, and `:cstr` now codegen with correct C
  signatures end-to-end.
- **`Category` typeclass and Kleisli `ArrowZero` (#290)** -- `stdlib/arrow.tur`
  gains an honest `Category` with `id`/`compose` and `ArrowZero` with
  `zero-arrow`; the full `Arrow` hierarchy consolidates into `stdlib/arrow.tur`.
- **stdlib `Functor`/`Monad`/`Alternative` instances for `Option` (T1)** --
  `Option` gains `Functor`, `Applicative`, `Monad`, and `Alternative` instances;
  `Result` gains `Bifunctor`; `do-m`/`for`/`fmap`/`bind`/`alt-or` work over
  stdlib optionals and results without manual imports.
- **`min` and `max` prelude macros** -- available in every module alongside
  `when`/`cond`/`for`/`unless` without an explicit import.
- **Codegen snapshot CI guard and `regen-snapshots` recipe (#298)** -- `tur run
  regen-snapshots` bulk-regenerates fixture snapshots; a CI gate prevents
  snapshot drift from landing undetected.

### Changed

- **Injective Turmeric-to-C name mangling (#275)** -- `-` maps to `_hy`, `_` to
  `_un`, `/` to `_sl`; the scheme is now injective and reversible. Manual
  `extern` declarations in spice inline-C must be updated to use the new
  mangled names.
- **`(fn [...] ...)` leading colons deprecated (TUR-D0001)** -- `(fn [:float]
  :float)` now emits a deprecation warning; the bare-identifier form `(fn
  [float] float)` is the target. A codemod swept `stdlib/` and `tests/` in
  #270.
- **`tur build` compiles dep modules for shared-library spices** -- library
  spices with no `main` (e.g. tourist) previously skipped dep-module header
  generation. Dep headers are now always emitted; dep `.c` files are excluded
  from the link via the new `n_own` parameter.
- **Project-mode prelude auto-load** -- `tur build <dir>` auto-loads the stdlib
  prelude in per-module compile paths, making `when`/`cond`/`for`/`unless`
  available inside `defmodule` bodies without explicit imports.
- **Local typeclass instances shadow stdlib on erased dispatch** -- when
  `.method` dot-dispatch on an erased receiver is otherwise ambiguous and
  exactly one matching instance is user-defined, that local instance is selected
  instead of failing with `TUR-E0020`.

### Fixed

- **Fat-closure / typed-SF dispatch (cluster)** -- resolved interacting closure
  codegen bugs: poly-closure inner dispatch result erasure, `copy_kind`
  initialization, two-level SF return miscompilation, `Vec<SF>` fold blockage,
  `int`-to-`ptr<void>` carrier casts, let-bound SF type-check routing, and
  `^fat` param emission in inline-C bodies (#276, #283, #286, #287, #292, #293,
  #294, #295, #296).
- **Recursive `defn` return type in `defmodule` (#291)** -- `F_TYPE_ANN`
  wrapper was not unwrapped for recursive defn return-type slots, causing
  spurious type errors in self-recursive functions inside a `defmodule`.
- **`definstance` idempotency (#278)** -- repeated `(load ...)` of a file
  containing `definstance` no longer duplicates the instance.
- **macOS codegen miscompile: uninitialized `arg_poly_fn` (#274)** -- silent
  wrong-code on macOS in the poly-function path; covered by a new fixture.
- **`definstance` ergonomics (#284)** -- stdlib helper resolution gains better
  hints, a load fallback path, and corrected effect annotations.

## [0.18.0] -- 2026-06-02

### Added

- **`tur/httpd` standard middleware library (M0-M8)** -- headers (M0), request
  logging + composition (M1/M8), cookies and `Set-Cookie` (M2), urlencoded +
  JSON body parsers (M3), CORS and HTTP Basic Auth (M4/M5), multipart/form-data
  (M7), and body-size, rate-limit, and static-file middleware. Includes
  `docs/guides/httpd-middleware-guide.md`.
- **`tur/httpd` async server (A0-A4)** -- async accept loop and `await`
  primitives (A0/A1), Track-M middleware composes over async handlers (A3),
  in-flight cap with throughput fixtures and `docs/guides/httpd-async-guide.md`
  (A4).
- **`?` query operator and CPS error handling (#187)** -- `?` short-circuits on
  `Result` errors with a dedicated `TUR-E0001` message; `*-must`/`*-expect`
  route through `tur_panic_with` so `catch-unwind` can intercept them;
  `--no-contracts` strips runtime contract checks; `--lint-panic`
  soft-deprecates `*-unwrap` call sites.
- **`catch-unwind` and panic handling on the compiled path (R2 + R6c, #189)**
  -- compiled programs can intercept panics via `catch-unwind`; the
  effect-handler/continuation panic semantics are now normative.
- **CPS transform unification and full `call/cc`** -- continuation-passing
  phases consolidated; `call/cc` works end-to-end on the compiled path.
- **Intersection and union types** -- new type-system constructors for
  combining and alternating type constraints.
- **MCP LSP integration (#173)** -- ships an MCP server that exposes
  Turmeric's LSP for editor agents.
- **`parse-check` subcommand and syntax guide (#182)** -- standalone
  syntax-validation pass and a comprehensive surface-syntax guide.
- **Tourist routing composition** -- `tourist/routing` (sibling spices repo)
  adds `url-map!`, `cascade!`, `cascade-with!`, and `req-full-path` for
  mounting sub-apps and cascading on configurable statuses. See
  `docs/guides/tourist-routing-guide.md`.
- **GitHub Codespaces entry points (#174)** -- homepage and README link
  directly to a configured Codespaces environment.

### Changed

- **`^fat` ABI markers replace sentinel-capture workaround (#190)** --
  function-pointer marshalling now uses explicit `^fat` markers instead of
  the prior closure-sentinel detour; spice code referencing the old path
  may need to migrate.
- **Compiler: variadic-rest function-pointer args (V0-V2)** -- function-typed
  variadic rests are cast correctly at call sites, removing per-call
  adapter shims.
- **Compiler: inline-C by-value struct params (DS0-DS2)** -- call sites for
  inline-C functions taking by-value structs now match the C signature
  directly.

### Fixed

- **Neoteric bracket chaining and curly-infix operator detection (#188)** --
  `f(x)(y)` chains parse correctly under sweet-exp and `{a + b}` operator
  scanning is no longer confused by adjacent identifiers.
- **Sweet-exp polish** -- residual indentation and reader edge cases
  addressed across guides and fixtures.

## [0.17.0] -- 2026-06-01

### Added

- **`tur/httpd` HTTP/1.1 server (H1-H7)** -- `stdlib/httpd.tur` adds a full
  HTTP/1.1 server built on `tur/reactor`: blocking thread-per-connection (H1),
  reactor-backed listener (H2), bounded worker pool with shared fd queue (H3),
  persistent connections (H4), routing DSL with method guards and path parameters
  (H6), and middleware composition via `httpd-call` (H7).
- **`reactor-run-fibers` local fiber driver (F1-F8)** -- `tur/reactor` gains a
  single-reactor fiber group for cooperative concurrency without a separate
  scheduler thread.
- **Data literals `#map{...}` / `#set{...}` (DL0)** -- compile-time reader
  dispatch lowers `#map{:k v ...}` and `#set{a b ...}` to typed map/set
  constructors; composes with neoteric and curly-infix in sweet-exp files.
- **`tur/schema` runtime validation (SC0-SC7)** -- `stdlib/schema.tur` adds
  `HasSchema` typeclass, typed decode, `Validation` applicative, and
  `Functor`/`Applicative`/`Alternative` instances via transparent newtype (SC7).
  Includes `docs/guides/schema-guide.md`.
- **First-class runtime symbols `:Sym` (SYM0/SYM1/SYM4)** -- `-Xsymbols` enables
  interned `:Sym` values, `sym->str`, `sym=?`, and dynamic interning via
  `stdlib/sym-dynamic.tur`. Includes `docs/guides/symbols-guide.md`.
- **Typed `Map[K V]` surface (TMS2-TMS5)** -- unified macro accessors for
  content-keyed maps; `float32`/`float64` map keys via `MapKey` carrier typeclass
  (WKC); string maps via `smap-of` lowering (GMK2).
- **`#json(...)` compile-time reader macro (JR0)** -- parses a JSON literal at
  compile time and lowers it to typed map/vec constructors.
- **`let*` sequential-binding form** -- each binding sees previous bindings in
  scope; complements `let` and `letrec`.
- **`^fat` closure markers (A#1)** -- `^fat` parameter annotation triggers
  fat-closure auto-shim generation; `^fat` return-position marker propagates
  the annotation into nested lambdas.

### Changed

- **`^unsafe-multishot` annotation removed (MS4)** -- the annotation is no longer
  needed; remove any remaining `^unsafe-multishot` from spice code.
- **`#map`/`hamt-of` lowering unified (GHE4+GHE5)** -- all content-keyed map
  builders now use per-`K` fn-value specialization; `smap-*` APIs are removed.
  Migrate `smap-of` call sites to `#map{...}` or `hamt-of`.

### Fixed

- **Nested-closure captures in `#{Unsafe}`/`handle` bodies (KB-IDIOM-1)** --
  captures from outer scopes are correctly threaded into `unsafe`/`handle`
  body environments.
- **Cross-module private-defn C symbol collision (CC0-CC2)** -- private `defn`
  symbols in different modules no longer collide in emitted C.
- **Unnecessary parentheses in codegen** -- `BIN_INFIX`, `PREFIX_UNARY`, and
  `VARIADIC_FOLD` expressions no longer emit spurious wrapping parentheses.
- **Generic dict eq dispatch** -- `dict-eq?` correctly dispatches through
  `Hash`/`Eq` typeclasses for all registered key types.

## [0.16.0] -- 2026-05-30

### Added

- **`tur/reactor` event loop (R1-R8)** -- `stdlib/reactor.tur` adds a
  lightweight single-threaded reactor for multiplexing file descriptors,
  timers (one-shot and interval), OS signals (`reactor-add-signal`), and
  cross-thread channels (`reactor-add-chan`) without requiring the fiber
  scheduler. Backed by epoll on Linux and kqueue on macOS/BSD. Includes
  `docs/guides/reactor-guide.md` and `docs/tur-httpd-plan.md`.

### Changed

- **Phase F poly-dispatch extended to unsigned narrow ints** -- `uint8`,
  `uint16`, and `uint32` now use the concrete-cast fast path for
  `(forall [a] (-> a a))` calls, avoiding the `int64_t` carrier
  round-trip on x86-64.

### Fixed

- **`tur new` scaffold Justfile** -- scaffolded recipes now pass the
  correct targets (`tur build .`, `tur test tests/`, `tur check src/`);
  the CI contract was updated to `clean check test`.
- **`handler` name shadowing (FH2)** -- the `handler` special form no
  longer shadows higher-order parameters named `handler` when a local
  binding is in scope.
- **Reactor fixture leak markers** -- nine reactor test fixtures are
  tagged `requires.no-leak-check` to suppress false-positive
  LeakSanitizer reports from process-lifetime callback closures.
- **LS5 spice resolver memory leak** -- `cmd_run` now frees the
  strdup'd directory strings before freeing the `spice_inc_dirs` array.

## [0.15.0] -- 2026-05-30

### Added

- **`tur run` Justfile task runner** -- `tur run <recipe>` executes Justfile
  recipes with deps, params, `{{ interpolation }}`, `set` directives, and
  dotenv. `tur new` gains `--kind lib|bin`, `--author`, `--license`, CI
  workflow scaffolding, and a standard Justfile.
- **`tur fmt` formatter** -- `tur fmt [paths...]` reformats `.tur`/`.tur.sweet`
  files in-place with recursive directory walking; `--check`, `--stdout`,
  `--stdin`, and `--lang` flags; idempotent on all stdlib sources.
- **First-class handler values (FH0-FH7)** -- `(handler E [params] k body)`
  creates a portable handler value; `(with-handler hv body)` applies it;
  `(compose-handlers h1 h2)` composes disjoint-effect handlers (h1 outer).
  Multi-effect handler types `(handler #{A B} V R)` supported.
- **Explicit lifetime syntax (LS0-LS5)** -- `&'a T` and `&mut 'a T` in type
  annotations; implicit quantification; borrow return types; inter-procedural
  borrow-escape checking (TUR-E0105/E0106).
- **`-Xsized-types` flag** -- promoted to a real compiler flag (implies
  `-Xgadt`); ships type-level size indices (`SizedVec n`), static compile-time
  size checking (TUR-E0260), size inference, and `--dump-sizes` diagnostic.
- **`letrec`, `named-let`, and `(define ...)`** -- `letrec` supports self-
  recursive and mutually-recursive bindings; `named-let` desugars to
  tail-recursive `letrec`; `define` provides `let*`-style body-position
  binding in `defn`, `fn`, `let`, and `do`.
- **Typed variadic rest parameters for user-defined types** -- `& rest :T`
  now fully type-checks opaque, struct, ADT, and type-application rest
  arguments; unknown type names are hard errors; `:int`-and-cast workarounds
  are no longer needed.
- **Cross-module ABI specialization (J1-J7)** -- `tur build <dir>` performs
  a two-pass build that emits owned-clone bodies in the owner module and
  extern forward-decls in borrowers; `.tur-abi-cache/index` persists across
  incremental builds.
- **`any` boxing and if-guard narrowing (TY2/TY3)** -- `(box :any v)`,
  `(cast :T v)`, and `(if (is? :T v) ...)` narrowing in control flow.

### Fixed

- **Memory leaks in composite-type diagnostics** -- `type_name()` result
  strings are now arena-managed; error paths are ASan/LSan-clean.
- **Codegen: Clang int-to-pointer warnings** -- spurious `int<->pointer`
  casts eliminated from all generated C.
- **Runtime autolink path resolution** -- prefix-installed builds resolve
  absolute `-I`/`-L` SDK paths correctly.

## [0.14.6] -- 2026-05-28

### Docs

- **Guide polish across stdlib documentation** -- copy edits and
  formatting fixes for the C integration, cellular-automata comonad,
  custom-effects, HAMT, HKT, and type-erasure guides.

### Internal

- **`scripts/wait-for-release.sh`** -- helper script for maintainers
  to watch a release workflow run from the CLI after pushing a tag.

## [0.14.5] -- 2026-05-28

### Fixed

- **`tur uninstall` rejected names from older `tur list` output** -- prior
  `tur list` rendered entries as `name-version` with no separator (e.g.
  `tur-notebook-0.1.0`), so users copying that string into `tur uninstall`
  got "is not installed". Uninstall now falls back to splitting on a
  trailing `-<digit>...` suffix and matching against the recorded version.
- **`tur list` separates name and version with a space** -- kebab-cased
  package names like `tur-notebook` are no longer visually fused with
  their version (`tur-notebook 0.1.0` instead of `tur-notebook-0.1.0`).

## [0.14.4] -- 2026-05-28

### Fixed

- **`*args*` empty in user-defined `main`** -- a user-defined zero-arg
  `main` now receives `(argc, argv)` at the C level and populates
  `g_tur_args` from the process argv before user code runs, matching the
  synthesized-main path. Previously `*args*` was always empty in
  user-main programs (which broke spice CLI parsing).
- **`tur install` git fetch errors include cache path + hint** -- when
  `pkg_git_fetch` fails on a previously-cloned cache directory, the
  error now reports the destination path and suggests `git -C ... status`
  / `rm -rf ...` to inspect or discard the dirty cache.

## [0.14.3] -- 2026-05-28

### Fixed

- **`tur install` global SDK path resolution** -- prefix-installed builds
  (e.g. Homebrew) now resolve absolute `-I`/`-L` paths to the Turmeric SDK
  when compiling spices that use `-lturi`; previously, the relative paths in
  `__tur_autolink__` failed when the working directory was not the source tree.
  Resolution order: `$TUR_SDK_ROOT` override, then walking up from the
  executable to locate `share/turmeric`.

### Changed

- **VSCode syntax extension** -- corrected the Markdown injection grammar so
  Turmeric code fences in Markdown files highlight correctly.

## [0.14.2] -- 2026-05-28

### Fixed

- **`tur install` spice dependency resolution** -- `tur install` now fetches each
  `:spices` dep declared in `build.tur` into the global spice cache and adds their
  `src/` directories as `-I` paths when building binaries; previously, spices with
  transitive deps (e.g. `tur-notebook` depending on `ansi` and `png`) failed to
  build with "module not found" errors.

## [0.14.1] -- 2026-05-28

### Fixed

- **`tur --help` missing package commands** -- `tur install`, `tur uninstall`,
  `tur list`, and `tur upgrade` were dispatched correctly but absent from the
  help output; they now appear under the "package management" section.

### Docs

- **`tur/hash` API reference** -- docstrings for `hash-int` and all
  sized-integer variants (`hash-int8`/16/32/64, `hash-uint8`/16/32/64)
  added to the stdlib API reference and web REPL doc lookup.

## [0.14.0] -- 2026-05-28

### Added

- **ABI specialization (Phases C--I)** -- the compiler now performs typed ABI
  specialization for typeclass methods, concrete structs, and inline-C bodies.
  Integer hashing, bitwise ops, and pass-by-ptr structs use unboxed calling
  conventions (C/D); function-pointer fields in concrete structs are unboxed (E);
  inline-C bodies opt in via `__TUR_TY_<NAME>__` macros (G); typeclass dispatch
  routes directly to the instance implementation rather than going through the
  generic slot (H). New `--emit-abi-trace` flag reports applied specializations (I).

### Changed

- **Web playground promoted to public beta.**

### Fixed

- **Typeclass and stdlib fixes (KB-021--034)** -- GADT HKT constraint unification
  for `equal-cong` (KB-022); typeclass dispatch ABI mismatch for struct-typed
  instances (KB-021); `rc.tur` HKT instances now dispatch on `rc<T>` rather than
  `ptr<void>` (KB-027); orphan checker credits built-in primitive types to their
  home module (KB-030); `session.tur` return-type annotation corrected (KB-029);
  `gvzip-with` in `gadt-vec.tur` now takes a typed function parameter (KB-034).

- **HKT/Clone fixture conflicts** with auto-loaded stdlib typeclasses resolved.

- **macOS Clang compatibility** -- `run.sh` adds `-Wno-error` gates for
  `int-conversion` and `incompatible-function-pointer-types` when building with
  Clang, fixing CI failures on macOS Xcode 15+.

## [0.13.0] -- 2026-05-27

### Added

- **Spice-aware REPL (RP0--RP8)** -- `tur build --shared` emits a dlopen-able `.so`;
  `tur repl` auto-discovers and dlopens the enclosing spice, binds all exports as
  callables at the prompt, and refreshes them via `(reload)` or `--watch`. Error paths
  surface actionable hints for the three most common failure modes.

- **`#rx` regex literals and `#name"body"` reader macros** -- `#rx"pattern"` compiles
  to a regex literal at read time; `#name"body"` is a general reader-macro hook. `re`
  union helpers round out the regex API.

- **Variadic rest parameters** (`& rest :type`) -- functions now accept an unknown number
  of same-type trailing arguments; rest is a cons-list of the declared type and is `nil`
  when absent.

- **Currying** (`curry` macro, effects, rank-2) -- `curry` macro, algebraic effects in
  curried bodies, and rank-2 support (CY3+CY4).

- **Tuple2--Tuple5 built-in structs** -- `Tuple2` through `Tuple5` are now pre-defined
  in the compiler; pointer-type slots are handled correctly.

- **Sized-primitives mixed-width arithmetic (TUR-E0042)** -- mixed-width expressions over
  `i8`/`i16`/`i32`/`i64`/`u8`/`u16`/`u32`/`u64` now elaborate without manual casts.

- **`vec-of` macro** -- construct a `Vec` from a literal element list.

### Changed

- **Contract macros removed; `--no-auto-stdlib` added** -- the old `assert!`/`require!`/
  `ensure!` contract macros are no longer loaded by default; `--no-auto-stdlib` suppresses
  automatic stdlib injection entirely.

- **macOS release binary is now arm64-only** -- the release matrix is
  `linux-x86_64`, `linux-aarch64`, and `macos-arm64`.

### Fixed

- **Aggregate Carrier ABI (ACB Phase 1 + 3)** -- ACB Phase 1 audited call sites for
  struct-carrying aggregates; Phase 3 completes the carrier-to-struct bridge in
  `EX_ASCRIBE` so aggregate returns through ascription work correctly.

- **GADT `Vec` rename, `Clone` typeclass, `Functor` collision** -- `Vec` GADT renamed to
  avoid shadowing the stdlib type; `Clone` typeclass added; `Functor` name collision
  between stdlib and user definitions resolved.

- **Spaced compound type annotations** -- `f : (-> int int)` and compound annotations
  with internal spaces now elaborate correctly through the full pipeline.

- **`?` operator** -- missing `__tur-q-is-err?` and `__tur-q-ok-val` stdlib helpers
  added so the `?` early-return operator functions correctly.

## [0.12.0] -- 2026-05-25

### Changed

- **Typed stdlib modules now use canonical names**
  - The old `t*` module names were dropped in favor of unprefixed modules such as `vec.tur`, `map.tur`, `option.tur`, `result.tur`, `pair.tur`, `list.tur`, `grid.tur`, `zipper.tur`, `set.tur`, and `mutmap.tur`
  - Compiler preloads and synthesized structural-equality helpers were updated to use the new module and helper names
  - Tests, benchmarks, and generated docs were refreshed to use the unprefixed APIs throughout

- **Docs layout cleanup**
  - `drop-typed-prefix-plan.md` moved into `docs/archive/`
  - Several roadmap docs were promoted out of `docs/upcoming/` into the main `docs/` tree

## [0.11.0] -- 2026-05-25

### Added

- **`defalias` for primitive type aliases**
  - New `(defalias Name :primitive-type)` form for defining named aliases of primitive types such as `:int` and `:float`
  - Function parameter and return annotations now resolve these aliases during elaboration
  - Added coverage for basic aliases, float aliases, and invalid alias targets

- **New guides**
  - `frame-guide.md` -- using the `tur-frame` spice for in-memory columnar dataframes and Arrow interop
  - `tur-logic-guide.md` -- miniKanren-style relational programming with `tur/logic`

### Changed

- Documentation archive reorganized under `docs/archive/` and `docs/archive/history/`, with the archive index refreshed
- The miniKanren tutorial was renamed to `minikanren-1-relations-and-queries.md` to make room for a multi-part guide series

## [0.10.0] -- 2026-05-25

### Fixed

- Higher-order function return types now preserve their full `TY_FN` payload through elaboration and calls, so let-bound functions returned from other functions remain callable instead of degrading to a non-callable shell type.
- The signal spice no longer needs `requires.typecheck-skip`; `signal/core.tur`, `signal/dsp.tur`, `signal/envelope.tur`, and `signal/synth.tur` all typecheck cleanly against the current compiler and signal API surface.

## [0.9.0] -- 2026-05-22

### Added

- **LSP intelligence (LD0-LD4)** (#67)
  - `textDocument/hover` -- returns a Markdown snippet with the symbol's type signature and `;;;` docstring
  - `textDocument/definition` -- returns the source location for the symbol under the cursor
  - `textDocument/completion` -- prefix-filtered `CompletionItem` list from the symbol index; also completes stdlib module paths inside `(import ...)` forms
  - New LSP helpers: `lsp_scan_docs` (re-scans `;;;` blocks without invoking the compiler), `tur_collect_symbols` (elaboration-based symbol index)

- **`Show` typeclass (SI0-SI2)** (#68)
  - `Show [bool]` -- returns `"true"` or `"false"`
  - Fixed `Display [int]` -- was `"<int>"`, now decimal via `snprintf %lld`
  - Fixed `Debug [int]` -- was `"<int>"`, now `"int(N)"` format
  - Fixed `Display/Debug [ptr<void>]` -- correct `ok(N)` / `err(N)` / `Result::ok(N)` / `Result::err(N)` formatting
  - `derive-show` macro: generates a `Show` instance for any struct from a field descriptor list
  - New compile-time built-ins powering `derive-show`: `vec?`, `symbol-name`, `dot-sym`, `str-append`

- **`name : type` annotations** -- the parser now accepts a space before the colon (`name : type`) in addition to the existing `name :type` form

- **Datum comments** (`#;`) -- `#;expr` suppresses the next form without removing it from the source, matching standard Scheme/Racket convention

- **Sweet-expression syntax** (`#lang sweet-exp`) -- opt-in indentation-sensitive syntax; `#lang sweet-exp` at the top of a file (or a `.tur.sweet` extension) enables full t-expression + neoteric + curly-infix mode

- **Devcontainer** -- `.devcontainer/devcontainer.json` for one-click VS Code Remote / GitHub Codespaces setup

- **Spices directory page** -- `tools/genspices.py` generates `docs/html/spices/index.html` from the `turmeric-spices` README (local `../turmeric-spices/` or GitHub fallback) with full Turmeric syntax highlighting; `just spices` runs it; `just docs` now depends on it

- **New guides**
  - `lsp-guide.md` -- setting up the LSP server with VS Code, Neovim, and Emacs
  - `advanced-type-system-rationale.md` -- design rationale for HKTs, GADTs, session types, and sized types
  - `arrows-guide.md` -- composable `Arrow` abstractions and the `>>>` / `***` / `&&&` combinators
  - `sandboxing-guide.md` -- capability-restricted interpreter environments and step-fuel limits

### Fixed

- LSP server: fixed a crash when the client sent a request before the workspace root was known
- `Display [int]` / `Debug [int]` / `Display/Debug [ptr<void>]` now produce correct output (see Show typeclass above)

### Changed

- Guide code examples reformatted throughout to follow the new Clojure-style indentation rules and sweet-exp style guide (Guides, API Docs, and Spices pages all updated)
- Documentation reorganised: several plan documents moved to `docs/archive/`

## [0.8.0] -- 2026-05-22

### Added

- **Language Server Protocol (LSP)** (#62)
  - `tur lsp`: stdio-based LSP server with hover, go-to-definition, diagnostics, and completion
  - `tur check --json`: machine-readable diagnostic output for editor integration
  - VSCode extension updated to launch `tur lsp` and wire hover/definition providers

- **Sandboxed eval (SB0–SB4)** (#65)
  - `turi_env_new_sandboxed()`: capability-restricted interpreter environment
  - `TuriCaps` bitmask (`TURI_CAP_IO`, `TURI_CAP_FFI`, `TURI_CAP_INLINE_C`, `TURI_CAP_ASYNC`, `TURI_CAP_UNSAFE`, `TURI_CAP_IMPORT`)
  - Step-fuel limit (`turi_env_set_fuel`) and max-depth guard (`turi_env_set_max_depth`)
  - `import` and inline-C call sites blocked in sandboxed environments
  - New API: `turi_env_allow`, `turi_env_deny`, `turi_env_has_cap`

- **Lazy sequences (LZ0–LZ1)** (#63)
  - `Seq[A]` pull-based lazy sequence type with short-circuit support
  - Builders: `seq-from-list`, `seq-from-range`, `seq-repeat`, `seq-iterate`
  - Transforms: `seq-map`, `seq-filter`, `seq-take`, `seq-drop`, `seq-flat-map`, `seq-zip`
  - Consumers: `seq-to-list`, `seq-fold`, `seq-for-each`, `seq-count`, `seq-first`
  - `Range` type with constructors and set-algebra operations (`range-intersection`, `range-gap`, `range-span`, `range-encloses`, etc.)
  - Stdlib in `stdlib/seq/core.tur`, `stdlib/seq/builders.tur`, `stdlib/seq/transform.tur`, `stdlib/seq/consume.tur`, `stdlib/range.tur`

- **9 new stdlib modules** (#64)
  - `stdlib/json.tur` -- JSON parse/emit
  - `stdlib/csv.tur` -- CSV read/write
  - `stdlib/fs.tur` -- filesystem operations
  - `stdlib/path.tur` -- path manipulation
  - `stdlib/env.tur` -- environment variables
  - `stdlib/process.tur` -- subprocess spawning
  - `stdlib/re.tur` -- POSIX regular expressions
  - `stdlib/term.tur` -- terminal control (ANSI colours, cursor)
  - `stdlib/digest.tur` -- hashing (SHA-256, MD5)

- **Doctest framework (D0–D5)** (#58)
  - `tools/doctest.py`: extract and run `;;; Example:` blocks from stdlib docstrings
  - `tools/run-doctests.sh`: stamp-cached test runner with SKIP support for interpreter-incompatible modules
  - `just doctest` target; `just test` extended to include doctests
  - Fixed incorrect docstring examples in `stdlib/math.tur` and `stdlib/async_pipe.tur`

- **Emacs major mode** (#61)
  - `emacs/turmeric-mode.el`: syntax highlighting, indentation, and `M-x turmeric-run` for `.tur` files

- **Spice `subdir` support**
  - Spice manifests now accept a `subdir` key for monorepo sub-packages

- **New guides**
  - Generators guide (`docs/guides/generators-guide.md`) -- generator state machine design and usage
  - CLI args guide (`docs/guides/cli-args-guide.md`) -- structured argument parsing with `stdlib/args.tur`
  - Cloudflare deployment guide (`docs/guides/cloudflare-deployment-guide.md`) -- deploying the web REPL to Cloudflare Pages

## [0.7.0] -- 2026-05-21

### Added

- **Sized types (SZ0–SZ1)** (`-Xsized-types`) (#50)
  - `StaticInt` and size arithmetic (`static-int-add`, `static-int-mul`, `static-int-eq`) for phantom size annotations
  - `SizedVec` -- length-indexed vector; `sized-vec-new`, `sized-vec-push`, `sized-vec-get`, `sized-vec-set`
  - `SizedBuf` -- flat byte/word buffers with heap and stack allocation dispatch; `sized-buf-alloc`, `sized-buf-stack`, `sized-buf-get`, `sized-buf-set`
  - `SizedMatrix` -- sized 2-D matrix with row/column shape annotations; `sized-matrix-new`, `sized-matrix-ref`, `sized-matrix-set`
  - `SizedBitVec` -- compact bit array with size annotation; `sized-bitvec-new`, `sized-bitvec-get`, `sized-bitvec-set`, `sized-bitvec-popcount`
  - Stdlib in `stdlib/sized.tur`, `stdlib/sized-buf.tur`, `stdlib/sized-matrix.tur`, `stdlib/sized-bits.tur`
  - Full user guide: [docs/guides/sized-types-guide.md](docs/guides/sized-types-guide.md)

- **Literal match patterns** (Phase S4) (#50)
  - Match arms now accept integer, boolean, float, and string literals directly (no wrapping constructor required)
  - Emits an `if`/`else-if` chain for primitive scrutinees; integrates with the existing ADT elaboration path

- **`async-race` and `with-timeout`** in the `turi` interpreter (#50)
  - `async-race`: race two futures; the first to resolve wins and the loser is cancelled
  - `with-timeout`: run a future with a deadline; cancel and throw on expiry

- **Tail call optimization (TCO)** in `turi` interpreter (#50)
  - Self-tail-recursive and mutually-tail-recursive functions no longer grow the call stack
  - Tail calls through `let` bindings and `do` blocks are optimized

- **`catch-unwind`** boundary in `turi` interpreter (#50)
  - `setjmp`/`longjmp` panic boundary exposed as `EX_CATCH_UNWIND`; `panic?` native predicate

- **Weak pointer upgrade returns Option** (#50)
  - `(weak-upgrade r)` now returns `(some value)` on success and `(none)` on dangling -- previously returned a raw value or zero

- **Args parser stdlib** (`stdlib/args.tur`) (#50)
  - Builder-pattern CLI argument parsing: flags (`--verbose`), options (`--input=file` or `--input file`), positional args, and arbitrarily nested subcommands
  - `args/spec-new`, `args/spec-flag`, `args/spec-option`, `args/spec-subcmd`, `args/parse`, `args/get`, `args/positional`

- **Math stdlib** (`stdlib/math.tur`) (#50)
  - Thin wrappers around libm: `sqrt`, `fabs`, `floor`, `ceil`, `round`, `pow`, `log`, `log2`, `exp`, `sin`, `cos`, `tan`, `atan2`, `hypot`

- **Bits stdlib** (`stdlib/bits.tur`) (#50)
  - `bit-shr` (unsigned right shift), `println-float` (float with precision), and related bitwise helpers

- **Per-subcommand help strings** (#50)
  - `tur build --help`, `tur run --help`, `tur eval --help`, etc. now print usage for each subcommand

- **Performance comparison suite** (`performance-comparison/`) (#50)
  - Multi-language benchmark harness comparing C, Turmeric (compiled), `turi` (interpreted), Rust, Clojure, Racket, and Python
  - `tur --interpret file.tur` flag runs programs through the tree-walking interpreter without compiling
  - Benchmarks cover numerical computation, data structures, string processing, concurrency, I/O, and recursion

- **Cross-language validation framework** (`validation/`) (#50)
  - Python validation scripts that run the same benchmark across languages and verify identical results
  - `validate_fibonacci.py` checks correctness of Fibonacci across all five languages

- **Tier 3 worker pool** (#45)
  - Persistent interpreter processes for test fixtures; dramatically reduces per-test process-spawn overhead

- **`emit_effects.c`** -- effects codegen extracted from `emit_expr.c` into a dedicated translation unit (#50)

- **EAVT / Datalog database tutorial series** (`docs/guides/datalog-*.md`, `examples/datalog/`)
  - Four-part guide: EAVT concepts, minimal implementation, query API, B-tree indexing
  - Five progressive example programs in `examples/datalog/`: `minimal.tur`, `indexed.tur`, `query.tur`, `blog.tur`, `datalog.tur`

- **New and expanded guides**
  - Performance guide (`docs/guides/performance-guide.md`) -- numerical, data structures, concurrency, memory, recursion, I/O, benchmarking methodology
  - Sized types guide (`docs/guides/sized-types-guide.md`)
  - Building for the Web with Emscripten (`docs/guides/web-emscripten-tutorial.md`)
  - Structs guide (`docs/guides/structs-guide.md`)
  - Web continuations tutorial (`docs/guides/web-continuations-tutorial.md`)
  - Dual Turmeric / sweet-expression syntax toggle throughout all guides (`check-guide-pairs.py`)
  - Substantially expanded: threading, STM, session types, HKT, tidal/scscm cookbook guides

### Fixed

- ADT match regression: literal match path no longer incorrectly triggers for ADT matches on unannotated parameters
- Memory leaks and stale codegen snapshots from perf-comparison branch
- `weak-dangling` test updated to reflect Option-returning `weak-upgrade`

## [0.6.0] -- 2026-05-19

### Changed

- Documentation refresh and cleanup
- Regenerated stdlib API reference

## [0.5.0] -- 2026-05-19

### Added

- **Session types -- binary (SS0–SS4)** (`-Xsessions`) (#38, #36, SS0a–SS3c)
  - `Session[P]` type; `make-session`, `send`, `recv`, `close` channel operations
  - `Choose`/`Branch` for internal/external choice; `choose-left`/`choose-right`/`offer`
  - `Rec` equirecursive protocols (co-inductive equality with seen-set guard)
  - Duality checking (`dual(P)`) and protocol-progress enforcement via linear-type machinery
  - Session delegation (protocol ownership transfer) and session subtyping
  - Typed timeout channels (`recv-timeout`, `TY_TIMEOUT`, `Timeout` protocol constructor)
  - C codegen: `TurChannel` struct; synchronous rendezvous via pthread condvars
  - Debug builds embed initial protocol name as `const char* dbg_proto`
  - Error codes `TUR_E0210`–`TUR_E0212`; `tur explain` entries

- **Session types -- multi-party (SS5–SS8)** (`-Xsessions`) (#39, #40, #41, #42)
  - `defprotocol` global protocol declaration with role list and interaction forms
  - `(-> From To MsgType)`, `(choice From [label branch ...])`, `(loop label body)`, `(continue label)`
  - Well-formedness checks (undeclared roles, non-guarded recursion): `TUR_E0223`
  - `(project G R)` type annotation: compile-time projection of a global type onto a role
  - Honda/Yoshida/Carbone projection algorithm (`src/compiler/elab_global.c`); validated by `tools/project.py`
  - Projection failure diagnostic `TUR_E0220`; role/projection mismatch `TUR_E0221`/`TUR_E0222`
  - `make-protocol` allocates N `Role[G, R]` endpoints (one per declared role)
  - `send-to`/`recv-from` route messages through a shared N-party lock-based router
  - Stdlib multi-party templates in `stdlib/session.tur`: `three-way-handshake`, `coordinator`, `ring`
  - Tutorials: Two-Phase Commit and OAuth-Style Auth Flow in `session-types-plan.md`
  - Full user guide: [docs/guides/session-types-guide.md](docs/guides/session-types-guide.md)

- **Dynamic vars (DV0–DV4)** (`-Xdynamic-vars`) (#44)
  - `defdynamic` top-level form declares typed thread-local cells with a root value
  - `binding` form pushes per-thread override frames; cleanup via `__attribute__((cleanup))`
  - Dynamic-var `set!` mutates the current thread's top binding frame
  - `TY_DYNVAR` type kind; `DynVarEntry` struct; codegen using `pthread_key_t` + linked-frame stack
  - `spawn-conveying`: spawn a thread with a snapshot of the parent's current binding frame
  - Stdlib common vars in `stdlib/dynvar.tur`: `*log-level*`, `*locale*`, `*random-seed*`, `*current-module*`
  - Error codes `TUR_E0600`–`TUR_E0605`, `TUR_W0600`; `tur explain` entries
  - Full user guide: [docs/guides/dynamic-vars-guide.md](docs/guides/dynamic-vars-guide.md)

- Datalog database tutorial (#35)

### Fixed
- Scheduler multithread codegen snapshot (#63906e87)

## [0.4.0] -- 2026-05-17

### Added
- Algebraic effects with delimited continuations and handler syntax (#25)
- WASM threads planning and infrastructure

## [0.3.2] -- 2026-05-17

### Changed
- Syntax highlighter improvements
- Documentation cleanup and reorganisation

## [0.3.1] -- 2026-05-17

### Changed
- Homepage layout and copy updates
- Documentation improvements

## [0.3.0] -- 2026-05-17

### Added
- GADTs -- generalised algebraic data types with full elaboration and codegen support (#24)
- Linear types -- linearity constraints enforced by the type system

### Fixed
- Homepage horizontal scroll on mobile (#23)

## [0.2.0] -- 2026-05-16

### Added
- Package manager -- CPM-based dependency management (#22)
- Effect rows -- row-polymorphic effect types
- Substructural and uniqueness types
- Linear types (initial support)
- "Solve this" button in the web REPL

## [0.1.0] -- 2026-05-14

### Added
- Higher-ranked types -- rank-N polymorphism (#18)
- GADTs -- initial implementation (#21)
- Arrows -- generalised computation abstractions
- Structural equality -- deep equality for all types (#17)
- Serialisable continuations -- capture and restore delimited continuations (#16)
- Contracts -- `require!`, `ensure!`, `invariant!`, and `assert!` macros
- Set literals -- `#s(...)` reader syntax
- HAMTs -- persistent hash-array-mapped tries
- STM -- software transactional memory
- Comonads -- comonad typeclass and standard instances
- Numeric types -- fixed-width integers and floats with explicit cast operators
- Auto-formatter -- source code pretty-printer
- Async/await -- fibre-based structured concurrency (#10)
- Unsafe effects -- escape-hatch unsafe block form (#11)
- HKT -- higher-kinded types, kind inference, and type application syntax
- Docstrings -- `;;;` doc-comment standard and `doc` macro
- Markdown Turmeric block syntax highlighting in the web REPL

## [0.0.4] -- 2026-05-09

### Added
- Algebraic effects infrastructure (v1) with delimited continuations
- Capability-passing effects (v1 effect system)
- Exceptions -- `try`/`throw` via `setjmp`/`longjmp`
- Defer expressions -- unified runtime-list-on-frame model
- Multi-file support and mutual recursion across files
- Async/await foundation -- fibre context switching (x64/arm64 asm)
- Compile-time macro evaluation via procedural elaboration
- `cond` as a variadic `defmacro` (removed built-in `elab_cond`) (#4)
- miniKanren-style logic programming example and guide
- Snake game example project
- Phases 0–19 of the core compiler (parsing, elaboration, CPS lowering, codegen)
