# Security audit -- Turmeric as it stands at v0.56.3

> **Status: WP1 and WP2 DONE (2026-09-29); WP3, WP4, WP5 and WP6 DONE
> (2026-09-30); WP7, WP8 PROPOSED.** WP8's decision is made (2026-10-02):
> `#fx{Unsafe}` is Option A, pointer arithmetic only -- see open question 3
> and [effect-row-honesty-plan](effect-row-honesty-plan.md); its lint-default
> bullet is still open. Written 2026-09-30
> against `main` @ 81e12de4 (v0.56.3). Section 2 lists what a one-afternoon
> survey already turned up, so the audit starts from a map, not from zero;
> every row there is a *candidate* until the work package that owns it
> verifies or retires it. Items marked **verified** were reproduced or read in
> the source during the survey; the rest are read-only findings from the
> survey and still need a repro.
>
> **WP1 landed 2026-09-29** against `main` @ dc95b2fdc. Every file the survey
> cited was re-read and is byte-identical between 81e12de4 and dc95b2fdc, so
> the section 2 rows hold as written. WP1's own verification results, three
> re-grades and three findings the survey did not have are recorded in
> section 2a; section 7 Q1 is answered.
>
> **WP2 landed 2026-09-29** on top of it. All nine D rows reproduced, but three
> had the wrong mechanism written down and one of the plan's own prescribed
> fixes turned out not to exist -- corrected in **section 2b** before the work
> started. D-1 through D-9 are closed, pinned by `tests/run-security-driver.sh`
> (27 assertions, ctest `tur_security_driver`). One finding of WP2's own is
> filed: `docs/archive/buf-puts-breaks-the-incidental-nul-invariant.md`.
>
> **WP3 landed 2026-09-30**, branched from WP1's PR (`1d5f533e`). Its research
> pass is section 2c: S-1 reproduced from an embedder AND from `tur check`
> (the survey's macro-time attempts failed only because they went through
> `:for-macros`), two findings the survey did not have (`load` in a sandbox,
> and the R7RS `eval` bridge as a full escape), and a new high, S-5, that the
> capability check cannot close.
>
> **WP4 landed 2026-09-30** against `main` @ 5fd23a65.  M-1 to M-4 are fixed
> with fixtures.  Ten libFuzzer targets now live in `tests/fuzz` and run
> nightly.  Across their 60-second and 10-minute passes they found eight
> defects, all fixed.  WP4's
> research pass is recorded in section 2e: re-grades, the survey line numbers
> that had moved, and fifteen findings the survey did not have.
>
> **WP5 landed 2026-09-30** against `main` @ 5fd23a65. M-5 and M-6 are fixed
> with fixtures, the TSan job and the format-string flags are in; section 2d
> records what the verification found, what the survey had wrong, and what was
> handed to WP3/WP4.
>
> **WP6 landed 2026-09-30.** Its pass is section 2f: W-2 reproduced as a
> working attribute injection from the editor's first line, a same-origin gap
> the survey did not have (every generated doc page ran inline script, so the
> CSP needed the doc generators changed), and a broken Share button filed as an
> ordinary bug.
> **Type:** Security / process / tooling
> **Depends on:** nothing that is not already in the tree. The Debug build's
> ASan+UBSan (`CMakeLists.txt:33`), the four differential fuzzers
> (`tests/*-fuzz-src.py`, `.github/workflows/fuzz.yml`), and the `TSan`
> CMake config (`src/CMakeLists.txt:481`) are the tooling this plan builds on.
> **Related:** [sandboxing-guide](../guides/sandboxing-guide.md) (the
> promise WP3 audits), [consuming-spices-guide](../guides/consuming-spices-guide.md#security)
> (the promise WP7 audits), [ci-release-workflows-plan](ci-release-workflows-plan.md)
> and [release-in-actions-plan](release-in-actions-plan.md) (WP7 lands
> alongside them), `docs/reported/README.md` (where findings go).

## 0. Summary

Turmeric is a compiler, an interpreter, a runtime library, a package
fetcher, a web playground, an installer, and a release pipeline. Each of
those has a different notion of "untrusted input", and today none of them
has a written one. The audit's first job is to fix that: **say which inputs
the project promises to handle safely**, then check the code against the
promise. Its second job is the ordinary one: find the memory-safety and
injection bugs in the C that handles those inputs, and put the cheap
hardening (pinning, permissions, checksums, CSP) in place.

The survey found no evidence of anything malicious and no exploited bug. It
did find that three documents promise more than the code delivers:

- `docs/guides/sandboxing-guide.md` presents `Env/new-sandboxed` as safe for
  untrusted code, but the natives table registered into *every* env
  (`src/turi/env.c:224-251`) includes `process/spawn`, file open/write,
  unlink and raw-fd read/write with no capability check (section 2, S-1).
  **Fixed in WP3**: every native is classified and the dispatch checks it;
  the guide now names the remaining gap (S-5) instead -- itself closed
  2026-10-07.
- `docs/guides/consuming-spices-guide.md:405` says every fetched spice is
  verified and builds fail on mismatch; the lock hash is trust-on-first-use
  and rewritten on every fetch (`src/compiler/pkg.c:2572-2582`), and only
  `tur run` ever compares it (`src/main.c:5737-5750`) (section 2, C-3).
- The install path advertised in `README.md:20` is `curl | sh` into
  `brew install --HEAD`, which builds whatever `main` is at that moment
  (`web/worker.js:1-24`, `Formula/turmeric.rb:5`), not a release with a
  checksum (section 2, C-1). **Re-graded by WP1: not an overclaim.** `README.md`
  says only "installs via the Homebrew formula in this repo", which is true,
  and `releases-and-installation-guide.md:152-154` already disclosed the
  `--HEAD`-only formula. The defect was an *undisclosed material fact at the
  most prominent entry point*, fixed in WP1 by one sentence each in
  `README.md` and `web/index.html`. C-1 (the installer itself) is untouched
  and still WP7's.
- **A fourth overclaim the survey missed, on the same footing as the spice
  one:** `releases-and-installation-guide.md:47` says `tvm install` "verifies
  them against the release's `sha256sums.txt`" (and `:39` comments "SHA-256
  verify"), while `tvm/tvm.sh:262-280` has four paths that skip the check, two
  of them printing nothing at all (section 2a). Corrected in WP1; the
  fail-closed fix stays C-2 in WP7.

The audit is eight work packages (section 3), ordered so that the first
two weeks close the items an outside reporter would find first, and so that
the fuzz harnesses and the CI hardening -- the parts that keep paying after
the audit ends -- land early rather than as an afterthought.

## 1. Trust boundaries -- what the audit is checking against

The audit needs a decision on each of these before it can grade a finding.
Proposed answers are given; section 7 lists the ones the author must
confirm.

| # | Boundary | Untrusted input | Proposed promise |
| --- | --- | --- | --- |
| T1 | **Compiling a project** (`tur build/run/check/emit-c`, `tur repl`, LSP, MCP, `tur run --list`) | a `.tur` tree, its `build.tur`, its `spices/`, its Justfile | Like every compiler: building a project *is* running its code (inline C, macros, Justfile). **No promise** that `tur build` on a hostile tree is safe. But `tur check`, the LSP, `tur run --list` and `tur repl` *auto-discovery* are things an editor runs on a tree you have only opened, and the macro env is documented as capability-denied -- those **should** be safe to point at an untrusted tree, and today are not (S-1, D-2, D-3). |
| T2 | **A compiled program's own inputs** | bytes handed to stdlib readers: `bytes->serial-cont`, image files, JSON, HTTP requests to `httpd`, `read-async` lengths | The stdlib reader must not corrupt memory on any input. A malformed input is a `result` error or a panic, never a wild read or write. This is the ordinary promise every runtime library makes; today the serial/image path does not keep it (M-1). |
| T3 | **The sandboxed interpreter** (`Env/new-sandboxed`, the compile-time macro env, Try Turmeric's wasm env) | the program text evaluated inside it | A capability-denied env can do no I/O, no process, no FFI, no inline C, and terminates under fuel. Today the natives table bypasses the capability check (S-1). Try Turmeric runs `CAP_ALL` and relies on the browser sandbox (`src/web/wasm_glue.c:159-162`) -- acceptable, but it must be written down. |
| T4 | **The supply chain** | the installer, release assets, `tur fetch` of a `:url` spice, GitHub Actions inputs | A user who installs a release gets the bytes CI built, verifiably. A spice pinned in `tur.lock` cannot change under a rebuild without a diagnostic. |
| T5 | **Editor protocols** (LSP, DAP, MCP over stdio) | framed messages from the editor | Peer is semi-trusted (it is the user's editor), but framing must be robust: a bad `Content-Length` must not overflow (M-3). |

Out of scope for this audit: DoS of a developer's own machine by their own
program; the R7RS embedding's memory growth (`docs/reported/r7rs-callcc-memory-never-freed.md`
covers it); Godot bindings (separate repo).

## 2. Findings already in hand

Severity is the survey's estimate against the section 1 promises, to be
re-graded by the owning work package. **verified** = reproduced or read
line-by-line during the survey; otherwise a read-only claim awaiting repro.

### Sandbox (WP3)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| S-1 | high -- **FIXED in WP3** | Natives that reach the OS are registered into every env, including sandboxed and macro envs, with **no capability check**: `process/spawn` (fork+execvp), `fs/tmpfile`, `io-fopen-read/write`, `write-temp-file`, `r7rs-io-open__`, `r7rs-unlink__`, r7rs `getenv`/environ, `json/decode-file!`, `read-async`/`write-async` on raw fds. **verified** in source (`src/turi/interpreter_natives.c:4851-4877, 5199, 4403-4412, 2615, 2892, 2858-2880, 1349`; `src/turi/fiber.c:754-830, 954-955`; registration `src/turi/env.c:224-251`; the sandbox constructor `env.c:277-286`; the macro env `src/turi/macro_env.c:212, 229`). The only cap checks are in `is_blocked_builtin` (`src/turi/eval.c:4070-4104`, println/dlopen/raw-memory only), the FFI thunk paths, inline-C, async and import. **Not yet reproduced from macro time**: four attempts via `defmacro` + `(import process :for-macros)` expanded correctly but never ran the native -- something in name resolution or the inline-C cap intercedes. The `Env/new-sandboxed` embedder API (`stdlib/turi/eval.tur:49`) and `tests/turi/sandbox-eval.c` are the right place for the PoC. |
| S-2 | medium -- **FIXED in WP3** | `extern-c` "known overrides" (`printf`, `printf_s`, `getenv`) skip the FFI cap check the thunk path enforces, and `printf`'s format string is program-controlled (`src/turi/eval.c:360-421`). | |
| S-3 | low (re-graded by WP3: needs `TURI_CAP_INLINE_C`) -- **FIXED in WP3**; a second emulator, `ic_exec_linked_list_print`, put through the same check by WP5 (section 2d) | The inline-C emulator's snprintf pattern hands the program's format string to `snprintf` with every argument coerced to `long long` -- a `%s` in the body dereferences an integer (`src/turi/eval.c:5532-5535`). | |
| S-4 | info -- **documented in WP1/WP3** | Try Turmeric's wasm env is `CAP_ALL` by design (`src/web/wasm_glue.c:159-162`); `tests/turi/sandbox-eval.c:37-88` covers only println/async/inline-C. | The security guide records the posture as intentional; the sandbox test now covers every classified native. Section 7 Q4 stays the author's. |
| S-5 | high (under T3; under T1 for `tur check`) -- **FIXED 2026-10-07** (native channel direction 1 2026-09-30, continuation resume and call targets 2026-10-03, host-exit half 2026-09-30), found by WP3 | Interpreter handles (vectors, maps, HAMTs, strings, conses, continuations) are bare `TURI_INT`s that natives cast back to pointers unchecked, so `(vec-get 4096 0)` in a sandbox or a `defmacro*` was a wild read and the setters a wild write. **verified** under ASan. **Direction 1 (2026-09-30):** a per-restricted-env handle-provenance registry (`TuriProvSet` in `src/turi/eval.c`) plus a per-native handle-kind column (`k_handle_rows[]` in `src/turi/native_caps.c`). **Closed 2026-10-07:** the value-model channel -- every interpreter re-tag of a word (an erasing ascription to `cstr` or a struct, a field read through a bare int, `gen-unwrap`, a `TVar`, a panic payload) checks the registry, and the words it accepts are recorded where a value loses its tag (native arguments, rest-list cells, TVar payloads). Executing the report measured the native channel too: a forked sweep of every capability-free native with forged arguments found **243 that crashed**, from a guard that trusted any tagged string at a handle position, rows limited to 4 positions, whole families with no row, mis-kinded rows (slices minted as Strings, two layouts per kind twice), unchecked stored words (comparators over stored keys, the owned-key flag, list and Vec element links) and unbounded indices. All closed (400 rows, 25 new kinds); the `r7rs-eval-c-*` natives now need every capability, like eval/load. Pinned by `native-sweep` and the `forgery/*` / `handles-ok/*` cases in `tests/turi/sandbox-eval.c`. Not a capability; see section 2c. | [`docs/archive/turi-sandbox-handles-are-forgeable-integers.md`](../archive/turi-sandbox-handles-are-forgeable-integers.md) |
| S-6 | medium -- **FIXED in WP3**, found by WP3 | `(load "path")` in a sandboxed env read the file and echoed its first token in the unbound-symbol diagnostic: `load` expansion (`src/compiler/elab_toplevel.c`, `load_expand_forms`) had no gate while `import` did. | |
| S-7 | high -- **FIXED in WP3**, found by WP3 | `r7rs-eval-c-eval__`/`-load__` evaluate text in the process-global embedded R7RS env (`src/turi/r7rs_embed.c`), which is an ordinary `CAP_ALL` env, so any sandbox reached every capability through it. | |

### Compiler driver and filesystem (WP2)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| D-1 | high (under T1 for `check`/LSP) | The C-compiler command line is assembled by `buf_printf` and run with `system()` **without quoting** any of: `TUR_CC_FLAGS`, output path, input paths, `-I` dirs, `__tur_autolink__` marker text scraped from generated C by `strstr` (`src/main.c:2283-2295`), `:link-libs`/`:link-flags` from the manifest (`main.c:2375-2392`), transitive spices' `:c-includes`/`:c-sources` (`main.c:4352-4390`), and cmake-dep flags (`src/compiler/pkg.c:4118-4136`). **verified** at `main.c:2873-2897`; same shape at `:2957-2977, :3079-3087, :6940-6982, :7595-7608`. `tur_shell_quote` (`src/platform_proc.h:109-159`) exists and is correct; it is just not used here. Any inline-C block in any module (a fetched spice included) can inject shell text through the autolink marker. |
| D-2 | high (under T1) | Justfile backtick assignments are evaluated while *parsing* (`src/compiler/justrun.c:1438-1440, 959, 1892, 1930`), and parsing precedes listing (`:2990` before `:3019`), so `tur run --list` in an untrusted tree executes shell. `find_justfile` walks up from cwd (`:1607-1625`). Recipe `{{ }}` args are unquoted into `system()` (`:2460`). Shebang recipes go to a hardcoded `/tmp/tur-run-XXXXXX` (`:2406-2439`). |
| D-3 | medium (under T1) | `tur repl` auto-discovery walks up from cwd (`src/turi/repl.c:1313-1325`), AOT-builds the tree and `dlopen`s the result; rebuild is decided by mtime, so a committed `.tur-repl-cache/lib-N.so` newer than the sources is `dlopen`ed without a build (`src/turi/spice_loader.c:713-723`). `TUR_NO_AUTO_SPICE=1` opts out. |
| D-4 | medium | `/tmp/tur-build` is `mkdir 0700` with the result ignored and no owner check (`main.c:2242-2253`); C paths are predictable and `fopen(..., "wb")` follows symlinks (`:2255-2273, :3184`); the prelude cache `<tur-build>/prelude/<hash>.o` is reused if non-empty with no integrity check (`:3034-3060`), so a planted object on a shared box is linked into the user's binary. |
| D-5 | medium | Spice destination is `spices/<name>-<ref>` with neither validated (`pkg.c:2531-2536`, `main.c:5720-5725`, `pkg.c:3088-3093`): `..` in a transitive manifest's name or ref escapes `spices/`. **verified** (`snprintf` at `pkg.c:2531-2536`). `:path` deps and `:members` are joined without a containment check (`pkg.c:3066-3110, 2161-2310`); `:build-dir` may be absolute. |
| D-6 | medium | Update-path fetch runs `git -C <dest> fetch --depth 1 origin <ref>` with no `--` before `<ref>` (`pkg.c:2046-2050`, **verified**), so a ref like `--upload-pack=<cmd>` from a transitive manifest is option injection; `tur install`'s `git ls-remote <url> <ref>` has the same shape (`src/compiler/install.c:1375`). The clone path is correct (`--branch <ref> -- <url>`). |
| D-7 | low | `TUR_SHQ` (`src/platform_fs.h:394-398`) is a bare `'` with no escaping; used for the run-binary path and pass-through argv (`main.c:5507-5513, 5572-5578, 6094-6098`), `tur fmt --diff` and `cmd_parse_check` file names (`:7832-7836, 8129-8132`), so a `'` in a path or argument breaks out. `open_in_browser` uses `xdg-open "%s"` (`:10498-10510`). |
| D-8 | low | MCP `execvp("tur", {format|build, <path>})` with no `--` (`src/lsp/mcp.c:232-247, 736, 758`); `try_external_subcommand` honours relative `PATH` entries (`main.c:11462-11508`). |
| D-9 | low | Generated `CMakeLists.txt` for `:cmake-deps` interpolates `:url`, `:ref`, `:options` unescaped (`pkg.c:3526-3569`), then `FetchContent` runs upstream CMake. |

### Memory safety and parsers (WP4, WP5)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| M-1 | high (under T2) | **FIXED (WP4)** -- fixtures `serial-resume-rejects-forged-env`, `image-*`; `tests/image_unit.c`; fuzz targets `fuzz_serial_cont`, `fuzz_image_header`. `tur_serial_cont_deserialize` has **no bounds checks**: frame count, name length, cstr length and env length are trusted from the bytes; raw int64s become frame environments; `__sk_frame_for_tag` takes an unchecked tag (`src/runtime/generated/tur_rt_split.c:2247-2291`; emitted copy `src/compiler/emit_dk_runtime.c:324`). `bytes->serial-cont` validates first but shallowly (`stdlib/serial.tur:598-651`); `resume-cont!` (`stdlib/workflow.tur:61-68`) and `image/blob-resume!` (`stdlib/image.tur:672-677`, via `load-resume-file!` `:699`) skip validation entirely. Image CRC covers the 68-byte header only, and `plen`/`goff` from the file size the `malloc` (`stdlib/image.tur:617-655`, `src/runtime/image.c:136-167`). The guestbook example resumes a continuation from a `POST` token -- this is the one place the project already ships T2 across a network. |
| M-2 | medium | **FIXED (WP4)** -- fixture `json-decode-hostile-input`; fuzz targets `fuzz_json_compiled`, `fuzz_json_interp`. JSON: input ending in `\` steps past the NUL terminator (compiled `stdlib/json.tur:545-563`; interpreter `src/turi/interpreter_natives.c:1211-1230`); no nesting depth limit (`json.tur:616-660`); no `\u`; error paths leak. |
| M-3 | medium (under T5) | **FIXED (WP4)** -- ctest `tur_lsp_io_unit`; fuzz target `fuzz_lsp_frame`. LSP framing parses `Content-Length` with an unchecked `atol`; `-1` wraps `body_len + 1` to 0, `malloc(0)`, then a huge `read` -- heap overflow (`src/lsp/lsp_io.c:71-96`, **verified**); `read_headers` grows unbounded (`:40-68`). DAP reuses it (`src/turi/dap.c:818, 1114, 1340`). |
| M-4 | medium | **FIXED (WP4)**, residue filed as `docs/archive/httpd-residual-request-hardening.md` (all closed 2026-10-03) -- fixture `httpd-request-hardening`; fuzz target `fuzz_httpd_head`. `httpd`: `Content-Length` is `(int)strtol` into `malloc(content_len + 1)` with no cap (`stdlib/httpd.tur:293, 329, 2491`); `Transfer-Encoding` ignored (smuggling behind a proxy); static-file traversal guard is `strstr(path, "..")` with `stat` not `lstat` (`:4143-4200`); binds `INADDR_ANY` by default (`:720`). Multipart (`:2102-2176`) and Basic auth (`:1963, 2014`) unreviewed. |
| M-5 | medium | **FIXED by WP5** (section 2d), with 26 more sites the sweep found. `read-async` does `malloc((size_t)bytes + 1)` with an unchecked, possibly negative `int` (`src/turi/fiber.c:763`); `tur_string_substring`/`slice` compute `start + len > n` with signed overflow (`src/runtime/tur_string.c:183, 300`); `n_from_bytes` accepts `len > strlen` (`src/turi/string_native.c:25-30`); `sb_reserve` doubles unchecked (`tur_string.c:238-243`); `bytes-alloc` `malloc(8 + (size_t)n)` with negative `n` (`stdlib/serial.tur:56-62`); `alloca(n * 8)` with user `n` in `stdlib/sized-buf.tur:445, 484` (gated `#fx{Unsafe}`). |
| M-6 | medium (silent UAF class) | **FIXED by WP5** (section 2d): one survey site confirmed -- as a class, not a site -- three retired, and a second class the survey did not have. Region escape hooks missing, per the CLAUDE.md rule: `tur_hamt_transient_set` (`src/runtime/hamt.c:1879`, from `stdlib/hamt.tur:740`), `tvar/write`/`tvar/swap` (`stdlib/stm.tur:88, 109`; `src/runtime/stm.c` has no note), `sized-buf-set!` (`sized-buf.tur:307`), `sized-matrix-set!` (`:232`), `sized-bitvec-set!` (`sized-bits.tur:139`), `httpd-resp-header-add!` (`httpd.tur:1281`). Each is a candidate use-after-rewind on the default build. |
| M-7 | info | The effect system is not a security boundary today, though one half of it is enforced. **Enforced:** calling an `#fx{Unsafe}` function outside `(unsafe ...)` is a hard error unless the caller declares `#fx{Unsafe}` (which propagates); `(unsafe ...)` discharges the obligation and erases the row, by design (Option A, decided in [effect-row-honesty-plan](effect-row-honesty-plan.md) section 3). **Not enforced by default:** `--strict-effects` is opt-in and warns (`-Werror=strict-effects` makes it fail the build); inline-C outside `Unsafe` is a lint behind `--lint-inline-c-unsafe`, default off (`globals.c:19`, `src/compiler/elab_toplevel.c:875`); the deserializers above infer plain rows. Because wrappers erase the row, `Unsafe` cannot answer "what may corrupt memory on bad input". |

### Web (WP6)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| W-1 | medium -- **FIXED in WP6** | No CSP anywhere: no meta tag, none in `web/public/_headers` (COOP/COEP only) or `web/worker.js:78-92`. | One policy in `web/csp.js`, applied by the dev/preview servers, stamped into the built `_headers`, and set by `worker.js` on its own responses. |
| W-2 | medium -- **FIXED in WP6**, **verified** | `escapeHtml` (`web/main.js:905`) does not escape `"` but is used inside attributes (`main.js:1495, 1498-1499, 4733` -- `data-name="${escapeHtml(item.name)}"`); `escapeAttr` (`:922`) exists and is unused there. `hydrateConsole` re-inserts HTML from `localStorage['tur.try.console.v1']` unescaped (`:184-196`). | Reproduced: see section 2f. `escapeHtml` escapes both quotes; the transcript is stored as data (`tur.try.console.v2`). |
| W-3 | low -- **FIXED in WP6** | No eval timeout or `worker.terminate` for the wasm worker (`web/public/eval-worker.js`), so a runaway program hangs its own tab. `#code=` share links only fill the editor (`main.js:1081-1090`) -- good. | Watchdog plus Stop. The share-link half was moot: Share has never worked (section 2f). |
| W-4 | info -- **documented in WP6** | The docs pane `innerHTML`s same-origin docs-pack HTML (`main.js:5543`), so the trust boundary is `tools/gendocs.py` over `;;;` docstrings -- including third-party spice docstrings via `tools/genspices.py`. | The security guide's Try Turmeric section names the docs as trusted content; the CSP stops injected HTML running script. |
| W-5 | medium -- **FIXED in WP6**, found by WP6 | Every page under `/docs/html/` -- the playground's origin -- ran two inline `<script>` blocks and three `onload="this.rel='stylesheet'"` handlers, emitted by `tools/genguides.py`, `gendocs.py` and `genspices.py`. | The generators write their scripts as files beside the pages and link plain stylesheets, so the doc pages run under the same policy. |

### Supply chain and CI (WP7)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| C-1 | high | Advertised installer is `curl -sSf https://turmeric-lang.com/install \| sh` (`README.md:20`, `web/index.html:331`) served by the Cloudflare Worker (`web/worker.js:1-24`), running `brew install --HEAD` against `main` (`Formula/turmeric.rb:5` is HEAD-only; no `url`/`sha256`). A compromised `main` -- or a bad afternoon on it -- ships to every new install. Release assets exist with `sha256sums.txt` (`.github/workflows/release.yml:349-352`) but nothing installs from them except `tvm`. |
| C-2 | medium | `tvm` checksum check fails open: skipped when `sha256sums.txt` is missing/empty, when the asset has no row, or when no sha tool exists (`tvm/tvm.sh:263-279`, **verified**); the sums file comes from the same origin as the asset, so it is integrity only. Base URLs are env-overridable (`TVM_RELEASE_BASE_URL`, `:205, 209`). |
| C-3 | medium | `tur.lock` is trust-on-first-use: the tree hash is recomputed and **overwritten** on every fetch, never compared (`src/compiler/pkg.c:2572-2582`); `:resolved` is never used to check out (clones track the branch/tag in `:ref`); verification happens only in `tur run` for already-present dirs, skipped for legacy/git-SHA hashes (`src/main.c:5710-5760`, `pkg.c:1744`). `consuming-spices-guide.md:405` overclaims. `tur audit` "lists; it does not verify" (`:420-424`). |
| C-4 | medium | Release assets are unsigned: no Sigstore/cosign, no GitHub build-provenance attestation, tags are `git tag -a` not `-s` (`.claude/commands/cut-*-release.md`). `softprops/action-gh-release@v2` runs floating with `contents: write` (`release.yml:338-355`). |
| C-5 | medium | No workflow pins an action to a SHA; `mymindstorm/setup-emsdk@v14` with `version: latest` (`ci.yml:134, 1470`); `msys2/setup-msys2@v2`; `pip install` unpinned (`ci.yml:105, 1467`; `release.yml:286`); `ci.yml:152-157` clones `turmeric-spices` default branch unpinned and compiles it; `ci.yml` has no top-level `permissions:` (default token scope everywhere except `publish-timings`' `contents: write`, `:762-765`); ccache `restore-keys` prefixes (`:112, 1454`). |
| C-6 | low | **`inputs.seed` half FIXED (WP4)**, in passing, while adding the parser job to the same file: the seed goes through `env:` with a digits check. The `issues: write` scope is WP7's. `fuzz.yml:75` interpolates `${{ inputs.seed }}` directly into a `run:` block (dispatch-only, so needs write access already; `inputs.n` at `:87` uses the safe `env:` form). Workflow has `issues: write` and `GITHUB_TOKEN` for `gh issue create` (`:41-43, 122+`). |
| C-7 | low | MIR comes from the personal fork `rjungemann/mir.git`: until 2026-10-02 `cmake/mir.cmake` fetched it at configure time (SHA-pinned; JIT-only, default off); since then a copy is vendored under `external/mir/` (fork commit in `external/mir/UPSTREAM`, re-synced by `tools/update-mir.sh`) and `TUR_JIT` defaults ON, so it ships in every default build. `examples/snake` pins raylib by tag. `Dockerfile` uses `ubuntu:22.04` by tag; `.devcontainer/Dockerfile` has two `curl \| bash` installs. |
| C-8 | low | Committed to git: `.claude/settings.local.json` (with a broad `Bash(xargs cat *)` allow), a `.claude/projects/.../memory/project_er6.md`, and `TEMP.md`. Missing: `SECURITY.md`, `CODEOWNERS`, `.github/dependabot.yml`, CodeQL/scanning workflow, and any private-vulnerability-reporting setting. |

## 2a. WP1's verification pass (2026-09-29)

Everything below was read in the source against `main` @ dc95b2fdc. No file the
survey cited moved between 81e12de4 and dc95b2fdc.

### Rows confirmed, with the mechanism tightened

- **S-1 confirmed structurally, and the sweep is now exhaustive.** A grep of
  `TURI_CAP_` across `src/turi/*.c` finds every capability check in the
  interpreter, and there are nine: the FFI thunk (`ffi_thunk.c:330`), three
  FFI sites in `eval.c` (`:470, :1963, :2045`), `is_blocked_builtin`
  (`:4070-4101`), inline-C (`:9897, :9911, :11524`), async (`:10696, :11577`),
  and import (`:14319`). **Exactly one native consults caps:** `native_doc_print`
  (`interpreter_natives.c:5569`). Everything else in the table is unchecked.
  `turi_env_new_sandboxed` (`env.c:277-286`) is `turi_env_new()` plus
  `caps = TURI_CAP_NONE`, and `turi_env_new` registers the whole table at
  `env.c:224-251`, so the sandbox is porous by construction rather than by
  oversight.
- **The `read-async` route needs no `(async ...)` at all.** `native_read_async`
  (`fiber.c:754`) does its `read(fd, buf, bytes)` eagerly in the non-blocking
  fast path at `:763-764`, before any future is created, so the
  `TURI_CAP_ASYNC` gate on the `(async ...)` form never intercedes. This is the
  cheapest PoC for WP3 to write first, and it is also the M-5 negative-`int`
  `malloc` in the same two lines.
- **The macro env is the sharper half of S-1.** `macro_env.c:222-232` calls
  `turi_env_deny(env, TURI_CAP_ALL)` under a comment promising "no I/O, FFI,
  inline-C, async, unsafe, import" -- ten lines *after* `:212` registers the
  unchecked native table into that same env. The intent is on the page; the
  enforcement is not.
- **D-2 confirmed, and the fix is one struct field.** `re_primary`
  (`justrun.c:954-969`) runs `jr_capture_command` -> `popen` the moment it sees
  a backtick, and it is reached from `eval_rhs` at `:1439`, which runs inside
  the line-by-line parse loop for a `name := value` assignment. `parse_justfile`
  (`:2990`) strictly precedes the `--list` branch (`:3017`). Deferring is
  scoped: give `JVar` the unevaluated text plus a lazily-filled `value` and
  evaluate on first use by a recipe. It also makes `--set` cheaper -- today a
  `--set` override at `:2996-3011` pays for the backtick and throws the value
  away.
- **D-3 confirmed.** `needs_rebuild` (`spice_loader.c:210-216`) is a bare mtime
  comparison, so a committed `.tur-repl-cache/lib-N.so` newer than the sources
  is `dlopen`ed with no build. The `.gitignore` handshake in `ensure_cache_dir`
  (`:222-250`) only appends to an existing `.gitignore` and does nothing
  against a repo that commits the object deliberately.
- **D-1's manifest half confirmed, with the analogy that explains it.**
  `append_manifest_link_flags` (`main.c:2380-2394`) documents `:link-flags` as
  "the verbatim sibling -- no prefix is added" and `buf_printf`s it straight
  into the flag string that later reaches `system()`. `:build-dir`
  (`pkg.c:808-810`) is parsed with `form_str_dup` under a comment saying
  "relative path" with nothing enforcing relative, absolute, or `..`.
- **C-3 confirmed, four ways.** `pkg_hash_comparable`/`pkg_hash_dir` have
  exactly one comparison call site in the tree: `main.c:5737`, inside `tur run`.
  `tur build` never checks. The check is skipped when the dep dir is absent
  (that branch fetches, then `pkg.c:2576-2582` unconditionally `free`s and
  overwrites `le->sha256`) and when the hash predates `PKG_TREE_HASH_TAG`.
  Because the hash is rewritten from the tree just fetched, it can only catch a
  local post-fetch edit -- never upstream drift.
- **C-2 confirmed, and it is four skip paths, not one.** `tvm/tvm.sh:262-280`:
  an empty-or-unreachable sums file (`2>/dev/null` swallows the fetch error)
  skips **silently**; an asset with no row leaves `$_want` empty so the `elif`
  is false and it falls through **silently**; no sha tool logs a skip; `--from`
  bypasses the block. Only the third says anything.

### Rows re-graded

- **C-1 -> the README half is a disclosure gap, not an overclaim.** See
  section 0. The installer fix is unchanged.
- **C-8 -> low, and no history rewrite is needed.** All three files were read:
  `.claude/settings.local.json` holds no credentials, only a machine's absolute
  paths (`/Users/.../turmeric2/...`), a broad `Bash(xargs cat *)` allow and two
  MCP server names; `project_er6.md` is a stale status note; `TEMP.md` is 34
  commits deep and was not opened. So `git rm --cached` plus `.gitignore` closes
  it completely -- **no rotation, no rewrite.** The one live risk is real
  though: that committed allowlist is pre-approved in any contributor's agent
  session after a clone.
- **The `--macro-caps=io` flag points the wrong way for WP3.** It is the only
  macro capability flag that exists (`main.c:9190-9193, :12001`;
  `globals.c:201`) and it *grants*. There is no `--no-macros` equivalent, so
  `tur check`'s macro exposure has no opt-out today.

### Findings the survey did not have

- **The VS Code LSP extension lets a repository redirect the language server
  binary.** `vscode-syntax-ext/package.json` contributes the setting
  `turmeric.serverPath` ("Path to the tur executable used for the language
  server", default `tur`), and neither extension declared
  `capabilities.untrustedWorkspaces`. A workspace `.vscode/settings.json` could
  therefore name any executable and have it started on folder open. **Fixed in
  WP1** by declaring both extensions `"supported": "limited"` and listing
  `turmeric.serverPath` in `restrictedConfigurations`, so Restricted Mode
  ignores the folder's value while highlighting and formatting keep working.
  `editors/vscode-turmeric` takes its adapter path from the launch
  configuration (`extension.js:14-16`), which Restricted Mode covers by
  refusing to debug.
- **`main` has no branch protection** (`GET /branches/main/protection` ->
  `404 Branch not protected`). Worth a decision given that the workflow is
  PR-only and that CI does not run for PRs not based on `main`. Not assigned to
  a package; the author's call.
- **Secret scanning and push protection are disabled**, both free on a public
  repo, as is Dependabot security updates (separate from WP7's
  `dependabot.yml`). Suggest folding the repo-settings half into WP7.

### Not reproduced

The survey's four failed attempts to reach a native from macro time were not
retried here -- WP3 owns the PoC. **WP3 reproduced it** (section 2c): the
route that works is the native's bare name in a `defmacro*` body, not a
`:for-macros` module; the attempts failed on the import, not on any check.

## 2b. WP2's verification pass (2026-09-29)

Every D row was re-read against `main` @ dc95b2fdc (the WP1 base). All nine
reproduce, but three had the wrong mechanism written down and one of the
prescribed fixes does not exist, so they are corrected here before the work
starts rather than after.

### Rows confirmed as written

- **D-1.** `link_command_run` (`main.c:2866-2897`) is one `buf_printf` chain
  into `system()`: `cc`, `cc_flags`, `out_path`, `inputs`, `aux_includes`,
  `aux_sources`, `autolink`, `cmake_flags` and every `-I` dir, none quoted.
  The same shape repeats at `:2961-2977` (the split prelude compile, which also
  builds a `&`-backgrounded shell pipeline), `:3087` (prelude whole-unit),
  `:6982` (multi-module link) and `:7608` (`tur compile`'s `cc -c`).
  `scan_autolink_markers` (`:2284-2295`) takes the marker text with `strstr`
  and `buf_write`s it through unexamined.
  `append_manifest_link_flags` (`:2380-2394`) `buf_printf`s `:link-flags`
  verbatim; `collect_spice_aux_c` (`:4372-4385`) does the same for a transitive
  spice's `:c-includes`/`:c-sources`; `pkg_cmake_manifest_append_cc_flags`
  (`pkg.c:4118-4137`) for the cmake manifest's dirs and flags.
- **D-2.** `eval_rhs` runs inside the line-by-line parse loop
  (`justrun.c:1437-1440`); `re_primary` (`:951-969`) `popen`s on sight of a
  backtick; `parse_justfile` (`:2990`) strictly precedes the `--list` branch
  (`:3017`). Recipe `{{ }}` args land unquoted in `system()` (`:2460`), and a
  shebang recipe writes to a hardcoded `/tmp/tur-run-XXXXXX` (`:2406`) while
  every other temp path in the tree goes through `tur_temp_dir()`.
- **D-3.** `needs_rebuild` (`spice_loader.c:210-216`) is a bare mtime
  comparison; the `.so` is `dlopen`ed at `:717-720` with nothing else checked.
- **D-4.** `stable_c_prefix` (`main.c:2242-2253`) is `mkdir(dir, 0700)` with the
  result discarded and no owner check; `stable_c_path` maps an input to a
  predictable name and the `.c` is written with `fopen(..., "wb")`
  (`:3184`); the prelude cache reuses `<tur-build>/prelude/<hash>.o` on
  `st_size > 0` alone (`:3040`).
- **D-5.** `snprintf(dest, ..., "%s/%s-%s", spices_dir, it->name, it->ref)`
  (`pkg.c:2531-2536`) with neither validated; `parse_spices` (`:243-248`) takes
  `name`, `:url`, `:ref`, `:path`, `:subdir` straight out of the form with no
  grammar, and `:build-dir` (`:810`) is `form_str_dup` under a comment that
  says "relative path" and enforces nothing.
- **D-7.** `TUR_SHQ` is a bare quote character (`platform_fs.h:387-396`) used at
  `main.c:5507, 5510, 5573, 5576, 6096`; the two `tur fmt --diff` commands
  hardcode `'%s'` four times each (`:7833-7835, :8129-8131`); `open_in_browser`
  is `"%s \"%s\""` (`:10506-10507`).
- **D-9.** `pkg_write_cmake_lists` interpolates `:url`, `:ref` and every
  `:options` key and value into `GIT_REPOSITORY`/`GIT_TAG`/`set(...)` lines
  with no quoting or escaping (`pkg.c:3543-3570`).

### Rows whose mechanism was wrong

- **D-6 is option injection only -- the shell half was already fixed.** The
  survey read `pkg.c:2046-2050` as unquoted. It is not: `pkg_cmd_arg`
  (`:2005-2013`) is a thin wrapper over `tur_shell_quote`, and *both* the url
  and the ref go through it on every branch of `pkg_git_fetch`, as does
  `upgrade_ls_remote` (`install.c:1370-1372`). So a ref of `a; id` is inert.
  What is missing is the **`--` end-of-options separator**: git still reads a
  ref of `--upload-pack=<cmd>` as an option, quoted or not. Verified that both
  commands accept the separator -- `git fetch --depth 1 origin -- HEAD` and
  `git ls-remote <url> -- HEAD` both succeed -- so the fix is two tokens, and
  the severity stays medium for the reason the survey gave even though the
  quoting claim was wrong. The clone path already had `--`.
- **D-8 cannot be fixed with `--`.** `tur format -- <path>` and `tur build --`
  both print usage and exit 0: neither subcommand's argument parser knows the
  separator, so adding `"--"` to the MCP argv would break the tool rather than
  harden it. The faithful minimal fix is to make the path non-optional-looking
  at the call site -- a leading `-` becomes `./-`, which names the same file --
  and to reject an empty path. Teaching every subcommand `--` is a CLI change
  and belongs with a CLI plan, not here.
- **D-1's prescribed fix (b) -- "build an argv, never a string" -- is the wrong
  call for the cc invocation, and the plan's own preference is withdrawn.**
  Three reasons, all verified in the tree:
  1. `TUR_CC_FLAGS` is **documented shell syntax**. A user who sets
     `-I"/My Projects/inc"` is relying on `/bin/sh` to split it. Tokenizing it
     into an argv ourselves changes a published interface, and getting the
     quoting rules subtly wrong there is a worse failure than the one being
     fixed.
  2. The autolink string has a **space-joined contract** that three separate
     passes depend on: `append_include_tokens` (`main.c:2439-2453`) re-splits
     it to pull `-I` tokens for the `cc -c` line, `autolink_has_bare_c_source`
     (`:2456+`) and `autolink_drop_bare_sources` re-split it to drop bare `.c`
     args superseded by `-lturi`, and the ASan probe (`:2745-2760`) scans it for
     `-L`. Shell-quoting the string wholesale breaks all three.
  3. `TUR_SHOW_CC` prints the assembled command for a human to paste into a
     shell. That is the mechanism that found the doubled `-L` behind
     `release-archive-cannot-compile`, and an argv dump is not pasteable.

  So D-1 splits into two fixes at two different boundaries, which is a better
  fit for the threat model anyway (section 7 Q1: `tur build` keeps no promise;
  what must not happen is a *fetched spice* smuggling shell text into the
  toolchain of a project that merely depends on it):
  - **a grammar at the untrusted boundary** -- the autolink marker text and the
    manifest's `:link-libs`/`:link-flags`/`:c-includes`/`:c-sources` -- which
    rejects anything outside the documented link vocabulary with a diagnostic;
  - **quoting for the paths the driver itself owns** -- `-o`, the inputs, the
    `-I` dirs, the aux sources -- which is a correctness fix as much as a
    security one, since a checkout under a directory with a space in its name
    cannot link today.
- **The autolink diagnostic cannot name the module.**
  `scan_autolink_markers` runs over the *assembled* generated C, where nothing
  records which module emitted which inline-C block, so the plan's "naming the
  module" is not available at that point. The diagnostic names the rejected
  token and quotes the marker it came from, which is what locates the source in
  practice (the marker text is distinctive).

### What the survey did not have

- **`export NAME := `...`` evaluates its backtick at parse time too**, not just
  on use: `justrun.c:1451` calls `setenv(var->name, var->value, 0)` the moment
  the assignment line is read. So D-2's fix has to defer the `setenv` as well,
  and a lazily-evaluated `JVar` must therefore know whether it was exported and
  publish itself when it is finally forced.
- **`--set` pays for a backtick it then discards.** The override loop
  (`:2996-3011`) runs after `parse_justfile`, so `tur run --set x=1` on a
  Justfile whose `x :=` is a backtick has already run the command. Deferring
  makes the override free, which is the second argument for the same change.
- **`:link-flags` "verbatim, no prefix is added" is load-bearing.** The comment
  at `main.c:2375-2378` says it is the only way to spell `-framework Cocoa`,
  and `pkg.c:4104-4110` respelling a `.framework` path proves the case is live.
  Any grammar must admit `-framework <name>`, `-L<dir>`, `-l<name>`,
  `-Wl,<...>`, `-pthread` and a bare object/source path, or it breaks macOS
  spices.
- **`buf_puts` and `buf_printf` differ in whether the Buf is incidentally a C
  string, and the driver depends on the difference.** `buf_vprintf`
  (`src/runtime/buf.c:46-59`) reserves `n + 1` bytes and lets `vsnprintf` write
  its NUL at `data[len]`, so a Buf built entirely out of `buf_printf` can be
  read as a C string *before* anyone appends an explicit terminator --
  and `link_command_run` does exactly that with the `aux_includes` and
  `aux_sources` buffers (`buf_puts(&cmd, aux_sources->data)`). `buf_puts`
  reserves only `n`. Replacing one `buf_printf` with a `buf_puts` in
  `collect_spice_aux_c` therefore read past the allocation; ASan caught it in
  `tests/spice-c-sources-tests.sh` (heap-buffer-overflow, `strlen` from
  `buf_puts`) while the 3405-fixture suite stayed green, because no fixture
  builds a spice with vendored `:c-sources`. Worth a report of its own: the
  invariant is real, undocumented, and one character away from being violated
  again.
- **`/tmp/tur-build` is not the only unowned shared path, but it is the only
  one that is *reused*.** The three `mkstemp` sites (`justrun.c:2406`,
  `main.c:7824, 8115`) are individually safe -- `mkstemp` is `O_EXCL` -- and
  differ only in that the Justfile one ignores `TMPDIR` while the other two
  already call `tur_temp_dir()`. The reuse is what makes D-4 the live one: the
  prelude object is keyed by a content hash and then trusted on `st_size > 0`,
  so a planted file with the right name is linked into the user's binary
  without a single check.

## 2c. WP3's verification pass (2026-09-30)

Read and run against WP1's head (`1d5f533e`, on `main` @ dc95b2fdc). No file
section 2's S-rows cite moved.

### PoCs, before the fix

From an embedder (`turi_env_new_sandboxed()` + `turi_eval`), every one of
these ran:

| Text | Result before WP3 |
| --- | --- |
| `(io-fopen-write "/tmp/...")` | created the file |
| `(process/spawn "touch" 0)` | forked; returned the child's pid |
| `(r7rs-unlink__ "...")` | deleted the file |
| `(r7rs-getenv__ "HOME")` | returned it |
| `(r7rs-exit__ 3)` | the **host** exited with status 3 |
| `(read-async 0 -1)` | the host aborted (`*** buffer overflow detected ***`) -- M-5's negative `malloc` and S-1 in one call |
| `(println-float 7.1)` | printed, although `println` itself was refused |
| `(load "/etc/hostname")` | read the file; its first token came back as an unbound symbol |

And from `tur check` -- no embedder, no flags -- a `defmacro*` body calling
`(r7rs-unlink__ "...")` and `(process/spawn ...)` by bare name deleted the file
and spawned the process at expansion time, with only a TUR-W0040 "will
runtime-dispatch" warning. The survey's four attempts went through
`(import m :for-macros)` and failed on the import; the bare name needs none.
So **S-1 was exploitable under T1**: `tur check` on an untrusted tree ran
shell. Plain `defmacro` is template substitution and never reaches the macro
env.

The existing sandbox test passed two of its cases for the wrong reason:
`sb-println` and the mixed-caps I/O check on "unknown function
'println-int'" (the name no longer exists), and `sb-import` on "import is only
allowed inside defmodule".

### The inventory

A static extraction of every `turi_env_register_native*` call (656 names
across `interpreter_natives.c`, `collections_native.c`, `string_native.c`,
`fiber.c`, `eval.c`, `macro_env.c`, `ffi_thunk.c`, `main.c`) matched a runtime
enumeration of a fresh `turi_env_new()` exactly, plus the three registered
conditionally (`break`, `reload`, `syntax-struct-fields`). Each body was
scanned for OS calls, transitively through its static callees, and every hit
read by hand. 63 are not pure; the rest are. The table is
`src/turi/native_caps.c`; the classes and the reasoning are in the sandboxing
guide.

The two native dispatch sites are both in `eval_apply_driven` -- the direct
native call and the inline-C override -- and `turi_call`, the fiber thunk and
every higher-order native reach natives only through it. One check there
covers them all, which is what made the choke point cheap.

### Findings the survey did not have

- **S-5 (high; fixed 2026-10-07)** -- handles are forgeable integers; see the row. The
  capability check is sound and this is underneath it. Its host-exit half was
  fixed in a follow-up the same day. Gating the collection
  natives behind `TURI_CAP_UNSAFE` would make a sandbox without vectors, which
  is not a sandbox anyone can use, so WP3 filed it rather than paper over it.
- **S-6** -- sandboxed `load`. Fixed.
- **S-7** -- the R7RS `eval` bridge. Fixed by classifying it as requiring
  every capability; making the embedded env inherit its caller's capabilities
  is the better fix and is not needed while nothing sandboxed wants R7RS
  `eval`.
- **S-3's snprintf also over-read.** `snprintf` returns the length it would
  have written, and the emulator copied that many bytes out of its 1024-byte
  buffer. Fixed alongside.

### Re-grades

- **S-3 -> low.** It needs `TURI_CAP_INLINE_C`, and a program with inline C
  was already trusted with its own format strings; it stays worth fixing
  because the emulator is the only thing between inline-C text and libc.
- **The `--macro-caps=io` direction (2a) is resolved by addition, not
  reversal.** WP3 adds `--no-proc-macros`; `--macro-caps=io` still grants, and
  now grants I/O plus path-based file access, because WP3 split
  `TURI_CAP_FS` out of `TURI_CAP_IO` and the flag is documented for
  embed-file macros.

## 2d. WP5's verification pass and execution (2026-09-30)

Everything below was read in the source, and where it could be run, run,
against `main` @ 5fd23a65.

### M-6 -- the region store hooks

Every survey site was probed with `TUR_REGION_STATS=1`, the only instrument
that sees a missed hook (stdout does not: a rewound generation is not
necessarily reused before the read).

| Survey site | Verdict |
| --- | --- |
| `sized-buf-set!` | **Confirmed, and it is a class, not a site.** A node stored from inside `with-region` read back the arena poison (`-2387225703656530210`; `rewinds=2 retires=0`). `sized-buf-set!` is a Turmeric-bodied wrapper over `__sized-buf-set!-raw`, and the implicit node -> `:int` erasure was noted only when the *callee* was inline C: inside the wrapper the word is already an `:int`, so the store it forwards to sees no node. Any user wrapper of that shape had the same hole. Fixed in the rule (`elab_call.c`): the erasure is now noted at any callee for a `:heap` node word, constructors excepted. |
| `tvar/write`, `tvar/swap` | **Retired as a hook gap.** `val : ptr` -- a node reaches it only through an explicit `(:: node ptr)`, which is noted at the ascription. A transaction inside a lambda did not compile when this was written ([stm-inside-closure-captured-tvar-undeclared](../archive/stm-inside-closure-captured-tvar-undeclared.md), since fixed); `tests/fixtures/region-escape-via-store` case 11 now runs it -- a chain written to an outer TVar inside `with-region`, read after the pop. |
| `tur_hamt_transient_set` | **Missing in C, unreachable today.** The stdlib entry takes `ptr<void>`, noted at the erasure. The C setter now carries the note as `tur_hamt_set` does, and that function's "every public setter funnels through here" comment, which was false, is corrected. |
| `sized-matrix-set!` | **Retired.** An inline-C callee with an untyped `v`, so the existing implicit-erasure note already covers it (the probe retires). The survey cited `sized-buf.tur:232`; it is `sized-matrix.tur:232`. |
| `sized-bitvec-set!` | **Retired, false positive.** It sets a bit; no caller word is stored. |
| `httpd-resp-header-add!` | **Retired, false positive.** It copies both strings. |

Two findings the survey did not have:

- **An inline-C callee's type-variable parameter** was never noted. The
  body-entry note skips a def-less tyvar on the theory that the node "was noted
  at its ascription", but a generic call has no ascription. Reproduced (poison
  read) and fixed at the call site (`emit_expr.c`), the one place the concrete
  argument type is known.
- **A constructor must not be noted**, learned by measurement: the first cut of
  the widened rule noted every node field handed to a constructor and took
  typed trees built in a bracket from 4/4 rewinds to 0/4. A constructor's box is
  routed into the same generation (or, malloc'd, notes its own fields), so its
  store is region-to-region. The one known remaining cost is `tcons`, whose
  tail is `t : int`: a list built with it inside a bracket now retires, where
  `tcons-of` (typed tail) keeps its rewind. The regions fuzz passes its
  rewind/retire model at seeds 1 and 7.
- **The widening exposed a pre-existing CPS emitter gap.** The CPS lowering
  types a call-argument binder from the argument but binds the value with its
  ascriptions peeled, so an erasing `(:: node :int)` argument to a CPS callee
  put a typed pointer into an `int64_t` uncast (-Wint-conversion). It was
  already reachable with an explicit ascription; the implicit one made it
  common (`list-eq?` over typed lists: four fixtures). The delegated-let
  emission casts a pointer-typed value into an `int64_t` binder now.

Pinned by `tests/fixtures/region-escape-via-wrapper`, a `hook.sh` fixture that
asserts the stats line (`pushes=6 rewinds=2 retires=4`) and the
`TUR_REGIONS=0` values; all four stores read back poison before the fix. The
plan asked for cases in `region-escape-via-store`; they went in their own
fixture because that one asserts stdout only, which, per
[stdlib-region-store-hooks-unswept](../archive/stdlib-region-store-hooks-unswept.md),
tests nothing about a hook. CLAUDE.md's hooked-set paragraph names both new
routes.

On the way: `stdlib/sized-matrix.tur` and `stdlib/sized-bits.tur` did not load
at all (an untyped parameter handed to `size-eval`); both are typed now.

### M-5 -- sizes that reach an allocation

All six survey rows confirmed and fixed. The `malloc(.*\*` / `alloca(` sweep the
package asked for covered 58 runtime sites, ~300 interpreter sites and 486
stdlib inline-C sites, and turned up 26 more; each was re-read before it was
fixed. `tests/fixtures/size-arith-guards` runs ten of them in their own
processes: before the fix, a substring with a huge length and
`(sized-buf-with-stack -1 f)` segfaulted, `fs/read-text` on a pipe was a heap
overflow glibc's fortify happened to catch, and the other seven accepted the
bad size silently.

Fixed, by kind:

- **Window arithmetic that overflowed its own clamp:** `tur_string_substring`,
  `tur_string_slice`, `tur_slice_sub` (`off + len > n` skipped the clamp for a
  huge `len`); `sb_reserve`'s doubling; `trace.c`'s OUTPUT record, whose 32-bit
  `1 + 4 + n` let a user-loaded recording claim 4 GiB and stall replay;
  `tur_hamt_show`'s 32-bit `count * 48`; r7gc's large allocation, which rounded
  a near-`SIZE_MAX` request to zero chunks and returned a live-looking pointer.
- **A product that wrapped to a tiny block under a huge recorded size:**
  `sized-buf-new(-zeroed)` (stdlib and interpreter twin), `sized-matrix-new(-zeroed)`,
  `sized-bitvec-new`, `grid-new` (twin too), `chan-new`/`async-chan-new`/`schan-new`
  (twin too; a capacity under 1 is now 1 on both paths, as the interpreter
  always read it), `work-queue-new-bounded`, `thread-pool-new(-dynamic)`,
  `httpd-new-pool`'s worker count, `vec-new-filled` (interpreter),
  `tur_uf_new`, `zipper-new`.
- **A negative count:** `read-async` (range-checked before its narrowing to
  `int`), `bytes-alloc`, `bytes-concat`, the `ptr<void>` Serializable pair
  (the deserializer's `dlen` is off the wire and a negative one overflowed a
  0-7 byte block), `sized-buf-compute`.
- **A caller-sized `alloca`:** `sized-buf-with-stack` is range-checked and moves
  to the heap past 512 elements, same scoped lifetime.
- **No check at all:** `grid-get`/`grid-set!`, `#fx{}` functions that indexed
  any `(x, y)`.
- **`ftell`'s -1:** `fs/read-text`, `read-file` and `csv/read-file-with-delim`
  sized a buffer by it and `fread` a pipe or FIFO into zero bytes.
- **A panic that returned:** the interpreter's r7rs `substring` fell through
  `turi_runtime_panic` (which returns under a catch boundary) into a
  `SIZE_MAX` copy.
- **A stack over-read:** the inline-C `snprintf` emulator copied `rlen + 1`
  bytes out of a 1024-byte buffer whatever `rlen` was. WP3 found and fixed the
  same line independently; the two fixes merged as one.
- **The wire's own lengths:** `serial_cont_from_bytes`'s string and byte-field
  readers allocated a uint32 length before checking the input held it (9 bytes
  could ask for 4 GiB per field), and `slen + 1` wrapped. The rest of that
  reader stays M-1's.
- **Past a cstr's NUL:** the interpreter's `tur_string_from_bytes` native caps
  the length at the string for a `cstr` argument (the no-FFI path the sandbox
  and the wasm build use). The compiled and FFI paths keep the raw
  (pointer, length) contract.

Handed on, not fixed here:

- `httpd`'s `Content-Length` parsed as `(int)strtol` with no body cap (DoS, no
  corruption), and its response writer, where a failed `realloc` leaves `hlen`
  past `hbuf_cap` -> **M-4, WP4**.
- `lsp_read_message` -> **M-3, WP4**, unchanged.
- Raw-pointer natives with no length to check against -- `flat-get`/`flat-set!`
  (caller-supplied width), and the private r7rs helpers `r7rs-io-peek__`,
  `r7rs-io-byte-ref__` and `r7rs-environ-name__` (unchecked index). Not a size
  check. WP3, landing alongside, classes `flat-get`/`flat-set` `UNSAFE` and
  `r7rs-environ-name__` `ENV`, so a sandbox cannot call them; the two io
  helpers stay pure-classified, and an out-of-range index or a forged handle
  there is **S-5** (fixed 2026-10-07: both are guarded handles and the index
  is bounds-checked).

### TSan

`tests/run.sh` has had `TUR_TSAN=1` since T19, and eighteen fixtures carry
`requires.tsan` -- which *skips* them in every ordinary run -- but nothing ran
them. `tests/run-tsan.sh` now selects those plus the STM, async, fiber,
channel, select, future, mutex, atomic, scheduler and thread-pool families (69
fixtures, about 20 s), and `.github/workflows/tsan.yml` runs it nightly on a
Release `tur` (a Debug one is ASan, which cannot share a process with TSan),
filing an issue on a finding as `fuzz.yml` does. The first run was not green:

- **A fiber read another worker's thread-local.** A sanitizer's
  instrumentation needs a thread-local's address, so gcc computes it once and
  reuses it across a fiber switch, exactly as clang does in every build:
  `fiber-scheduler-mt-migration` read the first worker's slot from the second
  (2-3 TSan reports a run). `TUR_TLS_FRESH`, the accessor clang already gets,
  now covers `__SANITIZE_THREAD__` and `__SANITIZE_ADDRESS__`. An
  uninstrumented gcc build on ELF still addresses each access through `%fs`
  and is unaffected.
- **`select`'s sleeper read `selected_idx` with a plain load** while the
  signaller CASes it: a C11 data race. Both reads are atomic acquires.
- **The handler-group id counter** (`g_dk_hgroup_ctr`) was a plain `++` from
  every thread. A lost update can move it backwards and give one thread the
  same id twice, so two handles' cases read as siblings. Atomic now; not
  per-thread, because a fiber's chain can collect ids on more than one worker.
- **`select-send-block` hung about one run in three, TSan or not** -- a fixture
  bug: each consumer stopped after exactly three values, while the select may
  route four to one channel. The consumers drain to a sentinel now.
- **`future-basic` and `future-error` no longer built** (a local `result-free`
  collides with the stdlib's); renamed.
- `threads-effects-tail-resume` is excluded, with the reason in the script:
  TSan's own trace allocator faults about 16k longjmp-driven tail resumes into
  a worker. The race it reported first, the counter above, is fixed.

### Format strings

`-Wformat-security` was already clean on gcc and clang, so the exit criterion
held before the package started. `-Wformat=2` was not: its
`-Wformat-nonliteral` found 13 sites under gcc, and clang -- which also refuses
a `va_list` wrapper that forwards a caller's format -- 14 more. Both flags are
now on the three `-Werror` targets:

- The printf-style wrappers carry `TUR_PRINTF_FMT` (`format(printf, ...)`,
  off on Windows): `buf_printf`/`buf_vprintf`, `diag_emit`/`diag_emitv`/
  `diag_emit_with_code`, `turi_errorf` and six file-local helpers. So every
  call's arguments are checked against its format now, which is the "checked
  and sized" list the package asked to confirm, kept by the compiler instead of
  by a survey. Neither compiler found a call that disagreed.
- Four sites chose a format at run time from a table of literals; each spells
  its literal now.
- The interpreter's `printf` and inline-C `snprintf` emulation format with a
  copy of the *program's* string that WP3 rebuilds and checks against the
  arguments (its S-2/S-3 fix); a checked buffer is still not a literal, so
  those sites are silenced by name. Merging WP3 also turned up a **third**
  emulator it had not reached: `ic_exec_linked_list_print` formatted the
  program's string unchecked, so a `%s` still dereferenced the integer it was
  handed. It goes through the same check now.

Checked with gcc and clang, Debug, with and without the JIT.

### Cost

Every `expected.c` snapshot moved twice: `grid.tur` and `zipper.tur` are
prelude modules, so their new bodies are in every emitted program, and the
preamble's thread-local guard, `select` read and handler-group counter changed.
The split-runtime artifacts were regenerated with them. Under the 500-fixture
coordination bar, so regenerated in the same change.

## 2e. WP4's research pass (2026-09-30)

Everything below was read in the source against `main` @ 5fd23a65 before any
WP4 change was made. The survey's WP4 rows still stood. Three of its
mechanisms were sharper than it said, one claim was wrong, and several line
numbers had moved.

### Rows confirmed, with the mechanism tightened

- **M-1: the validating route was not safe either.** `bytes->serial-cont`'s
  validator (`stdlib/serial.tur:598-651`) checked every length, but it never
  compared a call record's env kind with the kind the frame was *registered*
  with. The env kinds are:
  - `SK_ENV_INT` only ever carries a `TY_INT` operand. `cps_scalar_kind_ok`,
    `src/passes/cps_ir.c:1109`, admits int and cstr alone, and
    `check_serializable_capture_precise` rejects any other capture with
    TUR-E0018.
  - `SK_ENV_CSTR` carries a pointer.

  A buffer that labels a cstr-env frame's record `SK_ENV_INT` therefore hands
  that frame an attacker-chosen integer, which it dereferences as a string. The
  validator accepted this, and `resume-cont!` / `image/blob-resume!` never
  validated at all. The "raw int64s become frame environments" line in the
  survey is exactly this route, and it is the only one: a genuine int env is an
  int. Reproduced by fixture `serial-resume-rejects-forged-env`.
- **M-1's image half.** `image/read-image-file` sized its `malloc` from the
  header's `payload_len` without comparing it to the file.
  `image/loadable?` checked less than `image/load-resume-file!`, so "loadable"
  could still fail to load. The CRC covered 68 header bytes only. The runtime's
  `tur_image_read_header` ignored the flags word, which the header documents
  as "must be 0".
- **M-2 in both decoders, plus two more.**
  - `\u` was not decoded at all: `A` came back as the literal `u0041`.
  - The encoder wrote control characters other than `\n\r\t` raw, which is
    invalid JSON.
  - On an error part-way through a container, the parser freed the vector but
    not the elements it had already parsed.
- **M-3 exactly as filed, plus one.** The `strstr(headers, "Content-Length:")`
  lookup also matched the name inside another header's value.
- **The TSER wire codec has no caller.** `serial_cont_from_bytes`
  (`src/runtime/serial.c`) is compiled into `tur_core` and nothing calls it. It
  was still worth fixing, because it is public API in `serial.h`:
  - A u32 string or bytes length reached `malloc` before it was checked against
    the remaining input, so four bytes could ask for 4 GiB.
  - `n_fields` sized a `calloc` unchecked.
  - Decoded field names leaked on every frame.

  Deleting the codec is the author's call.

### Re-graded

- **"The guestbook resumes a continuation from a `POST` token -- a forged
  token is a forged continuation" was already false when the survey was
  written.** Since `01e7fa71` (2026-09-25), the token is a random 64-hex
  *name*, HMAC-signed with `GUESTBOOK_SECRET`. The continuation bytes never
  leave the server (`data/conts/<token>.bin`), and the load goes through
  `bytes->serial-cont`. The example had already answered section 7 Q2 the
  "application's job" way.

  Its one real gap was that a correctly signed token was used as a file name
  without checking its shape. With the dev default secret, which anyone can
  read, a signed `../../x` named any `.bin` file outside `data/conts/`.
  `verify-token` now requires the 64-hex shape. After M-1's fix, such a file
  could only have rebuilt this program's own frames anyway.

### Survey line numbers that had moved (M-4)

- `stdlib/httpd.tur:329` is the `body_in_buf` clamp, not a `Content-Length`
  parse; the `malloc` is at `:330`.
- The third parse is `httpd-mw-content-length` (`:3736`), not `:2491`, which is
  the async copy of the core parse.
- The request parser existed twice: `httpd-handle` (`:175-546`) and
  `httpd-async-fiber-body` (`:2356-2694`). The copies had drifted: the async
  cleanup never freed multipart parts or attributes, and the blocking path's
  status table had no 401.

### Findings the survey did not have

httpd, from a full read of `stdlib/httpd.tur`:

- **A stack buffer overrun in `httpd-set-cookie!`.** `n += snprintf(buf + n,
  sizeof buf - n, ...)` over a 2 KiB stack buffer never clamped `n`. Once
  `name=value` passed 2 KiB, `sizeof buf - n` wrapped and the next attribute
  was written past the buffer. It is reachable whenever a handler reflects
  request data into a cookie.
- **Response splitting.** Response headers were written with `"%s: %s\r\n"`
  and no CR/LF check.
- **`Content-Length` was `(int)strtol`.** `4294967306` became 10, `10abc`
  became 10, and duplicate headers were last-wins. Together with ignoring
  `Transfer-Encoding`, that is a request-smuggling kit behind a proxy.
- **A short body was dispatched.** A body shorter than its `Content-Length`
  (a timeout or peer close) reached the handler truncated. The
  `httpd-mw-body-size` fixture relied on exactly that, after a 5 s stall.
- **`mw-body-size` is not a pre-read check.** Its comment said it "prevents
  the body read entirely", but it runs in the handler chain after the core
  has read the body.
- **The async server waited forever.** `tur_local_park_fd(..., -1)` let a
  half-sent request hold its fiber indefinitely.
- **Static files.**
  - `stat` and `open` both follow symlinks, so a symlink inside the root that
    points outside it was served.
  - Dotfiles (`.env`, `.git/config`) were served.
  - The query string was part of the file name.
  - There was a `stat`/`open` TOCTOU window.
- **Basic auth.** A decoded NUL truncated the credentials the verifier saw.
- **Multipart.** A repeated part header leaked the earlier value.

The compiler's own front door, found by the new fuzz targets. The first two
came from the 60-second pass and the rest from the 10-minute passes:

- **The reader `free()`d arena memory.** `read_neoteric_bracket`
  (`src/compiler/reader.c:2988`) called `free(call_items)` on an
  `arena_alloc` pointer for `f[x]` (the `bracketapply` form). Any neoteric or
  sweet-exp file containing `f[x]` crashed `tur check` and the language server
  at read time: an invalid free, or glibc abort or heap corruption in a Release
  build. T1, high, now fixed. Fixture `errors/neoteric-bracket-call-reads`.
- **A Justfile parser leak.** `parse_recipe_header` leaked the parameters it
  had parsed when the line turned out not to be a recipe header. Low; fixed.
- **A Justfile parser hang.** In a dependency with arguments, `(a #` never
  terminated: `parse_value` consumes nothing at a `#` or `\r`, and the argument
  loop did not stop on either. The loop spun forever and allocated an empty
  argument on every turn, so `tur run --list` on such a Justfile hung until it
  ran out of memory. T1, medium (denial of service); fixed. Pinned by a case
  in `tests/run-tur-run-attrs.sh`.
- **Stack exhaustion in the Justfile evaluator.** Parentheses and call
  arguments recurse through `re_expr`, so an assignment nested a few thousand
  deep overflowed the C stack while the file was only being parsed, and
  `tur run --list` crashed. T1, medium; fixed with a nesting cap of 256
  ("expression nested too deeply"). Pinned in `tests/run-tur-run-attrs.sh`.
- **Manifest reader leaks.** A repeated key leaked the earlier value, both for
  scalar keys (`:version`, `:description`, `:build-dir`, `:engine`, ...) and
  for vector keys (`:exports`, `:authors`, `:spices`, `:build-opts`, ...).
  Only `:name` freed before overwriting. Each slot now releases its earlier
  value through the helpers `pkg_manifest_free` also uses, so last-wins is
  unchanged. Separately, the `#lang` trailing-token rejection (TUR-E0330)
  returned without `symtab_free`. Low; fixed.
- **Undefined behaviour on an empty sweet-exp file.** `sweet_preprocess`
  called `memcpy(dst, NULL, 0)` when the preprocessed text was empty. It is
  harmless in practice, but UBSan stops on it and a compiler may exploit it.
  Fixed.

### Deliberately not done here

- The httpd items that need a design choice or are not parser work are filed
  as `docs/archive/httpd-residual-request-hardening.md` (all closed 2026-10-03):
  - the quadratic header scan
  - async writes that park forever
  - no default in-flight cap
  - a rate limiter that fails open when full
  - oversized request fields silently read as `""`
  - the `Connection` prefix match
  - the Basic-auth doc example leaking username validity
- `stdlib/async_socket.tur` still binds `INADDR_ANY` by default. It is a raw
  socket API, where the POSIX default is the expected one, so it was left
  alone. httpd's default moved; see WP4.

## 2f. WP6's verification pass (2026-09-30)

Read and run against `main` @ 1ef4cd2f. `web/main.js` had moved since the
survey, so the W-rows' line numbers are the survey's; the code they point at
was unchanged.

### W-2 reproduced

The language picker renders a row for whatever base the buffer's first line
names, and interpolated that name with `escapeHtml` into `value="..."` and
`title="..."`. `escapeHtml` went through `textContent` -> `innerHTML`, which
leaves quotes alone. A first line of

```
#lang x"data-injected="1"onfocus="window.__pwned=1"autofocus="
```

rendered, before the fix, as

```html
<input type="radio" name="lang-base" value="x" data-injected="1" onfocus="window.__pwned=1" autofocus="">
```

No spaces are needed: the HTML tokenizer starts a new attribute after a closing
quote. The buffer's first line is user data from a pasted file, a restored tab
or an opened project zip, so this was script execution from content the user
did not write, one hover or focus away. The survey's two other attribute sites
(`aria-label` from the registry's language names, `data-name` from the docs
index) carry repo data only. The console key was the persistence half: anything
that once reached the transcript as markup came back as markup on every load.

### A same-origin gap the survey did not have (W-5)

The survey read `web/`, but `/docs/html/` is served from the same origin as
the playground and was not in it. Every generated guide, API page and spice
page carried inline scripts and `onload` handlers, so a site-wide
`script-src 'self'` would have broken them all, and before WP6 any injection
into a doc page ran with the playground's `localStorage`. Cloudflare's `_headers`
could have given `/docs/html/*` a laxer policy with its `! Header` detach, but
whether detach-then-reattach in one rule works is not documented, and a laxer
policy is exactly the one worth not having. The generators changed instead.

### Deviations from the WP6 text

- `connect-src` is `'self'`, not `'self' https://raw.githubusercontent.com`:
  the CI timings are proxied by the Worker, so the browser never fetches that
  host.
- `script-src` names `https://cdn.jsdelivr.net/npm/mermaid@11/dist/`. The guide
  runtime imports mermaid on demand (`tools/genguides.py`, `MERMAID_SRC`), in
  the docs pane as well as on the doc pages. It is a path, not the host, so no
  other package on jsDelivr may load: verified in Chromium with the real
  `mermaid@11.17.2` dist served at that path (diagrams render, no violation) and
  an import from `/npm/lodash-es@4/` refused as `script-src-elem`.
- `style-src` keeps `'unsafe-inline'` (Monaco, and the doc pages' `<style>`
  blocks) and names the doc pages' font hosts. The console's inline
  `style="margin:0"` became a class, because the persisted transcript keeps no
  attributes but `class`.
- The watchdog stops the worker on its own at 30 s (`CONFIG.EXECUTION_TIMEOUT`,
  a 5 s value nothing read until now) and offers Stop after 1 s. The worker is
  serial, so the oldest outstanding request is the one it is running.

### Found in passing

- **Share has never worked.** `encodeState`/`decodeState` call `pako`, which
  nothing loads; both swallow the `ReferenceError`, so Share reports "Failed to
  encode code" and a `#code=` link decodes to nothing. Not a security defect --
  it removed a vector rather than adding one -- so it is filed as
  [try-share-links-never-encode](../archive/try-share-links-never-encode.md).
- **The service-worker kill switch could not be built from a clean tree.**
  The Cloudflare plugin builds the Worker as its own Vite environment and that
  bundle closes first, before `dist/client/` exists, so `TUR_SW_KILL=1 npm run
  build` on a fresh checkout threw "the kill-switch was NOT deployed" -- it only
  ever worked over a stale `dist/`. The CSP stamp hit the same ordering; all
  three `closeBundle` hooks in `web/vite.config.js` now skip the Worker pass
  (`isClientBuild`). Fixed alongside, verified both ways from an empty `dist/`.
- **mermaid is loaded from a floating range.** `mermaid@11` resolves to
  whatever 11.x jsDelivr serves, with no integrity check (a dynamic `import()`
  cannot carry one). The CSP scopes it to that package; pinning the exact
  version in `MERMAID_SRC` and in `web/csp.js` together belongs with WP7's
  pinning work.

## 2g. WP7's verification pass (2026-09-30)

Read and run against `main` @ 82ccdc555 -- after WP1 and WP6 landed. Two of
the eight C rows are now **substantially closed by WP1's collateral**, and
three of the remaining six had a materially different mechanism than the
survey recorded. The live repo settings had moved the most: three of the four
the survey called out have since been turned on, and the one that mattered
most for C-5 turns out to point the other way. `.github/workflows/ci.yml` has
grown since the survey, so every `ci.yml` line number in the C-5/C-6 rows is
stale; the code they point at is unchanged and the current numbers are used
below.

### Rows confirmed as written

- **C-1 confirmed, installer half only.** `web/worker.js:3-26` still serves an
  `INSTALL_SCRIPT` whose payload is `brew install --HEAD "$TAP/turmeric"`, and
  `Formula/turmeric.rb:5` is still `head ... branch: "main"` with no `url` and
  no `sha256`. So `curl | sh` still builds whatever `main` is at that instant,
  and the release assets' checksums (`release.yml:348-352`) are still installed
  by nothing but `tvm`. **The disclosure half is already fixed**: WP1 rewrote
  `README.md:22-27` and `web/index.html:332` to say in so many words that the
  formula is `--HEAD`-only, "builds whatever is on `main` at that moment and
  verifies no checksum", and to point at `tvm` or a release tarball for a
  pinned install. What is left is the fix, not the honesty.
- **C-2 confirmed, and WP1's count of four skip paths is exact.**
  `tvm/tvm.sh:262-280`, re-read line by line. `_sums` empty -- whether the sums
  file is missing, empty, or the fetch failed into `2>/dev/null` -- skips the
  whole `if [ -n "$_sums" ]` block **silently**. An asset with no row leaves
  `_want` empty, so `elif [ -n "$_want" ] && [ "$_want" != "$_got" ]` is false
  and it falls through **silently**. No sha tool (`__tvm_sha256` returns 2)
  logs a skip, the only one of the four that says anything. `--from` (`:250`)
  skips the block by construction. Note also that `_got` is computed *inside*
  the `[ -n "$_sums" ]` arm, so the `_rc = 2` branch is unreachable when the
  sums fetch already failed -- the "no sha tool" message is not printed in the
  case where it would matter most.
- **C-3 confirmed, four ways, and WP1's reading holds exactly.**
  `pkg.c:2813-2820` unconditionally `free`s `le->sha256` and rewrites it from
  the tree just fetched, so the recorded hash can only ever describe the last
  download. The single comparison call site in the tree is `main.c:6001`,
  inside `tur run`; `tur build` never checks. It is skipped when the dep dir is
  absent (`main.c:5990`, which routes to a fetch that rewrites the hash) and
  when `pkg_hash_comparable` rejects a pre-tag hash. `:resolved` is recorded
  (`pkg.c:2807`) but never checked out: `pkg_git_fetch(it->url, it->ref, dest)`
  at `:2779` passes the **manifest** ref, and the clone is
  `git clone --depth 1 --branch <ref>` (`:2266-2268`), so a `:ref` naming a
  branch tracks that branch forever. Grepping `le->resolved` finds it used only
  to print (`:2762`, `main.c:6205`) and in `pkg_cmake_verify_lock`.
  **The guide overclaim is already fixed** -- `consuming-spices-guide.md:404-419`
  now states the TOFU behaviour, the `tur build` gap and the skip conditions
  explicitly, and cites C-3. Only the code fix is outstanding.
- **C-4 confirmed.** A grep for `attest`/`cosign`/`sigstore`/`provenance` over
  `.github/workflows/` and `.claude/commands/` returns one hit, and it is the
  word "attested" in an English sentence (`ci.yml:696`). All three
  `cut-*-release.md` say `git tag -a`, never `-s`.
  `softprops/action-gh-release@v2` still floats, in the one job that holds
  `contents: write` (`release.yml:338-355`).
- **C-5 confirmed on pinning, and it is total.** 56 `uses:` lines across the
  four workflows, **zero** pinned to a SHA. The inventory also shows the repo
  is running two majors of three actions at once -- `actions/checkout` at v4
  (14x) and v5 (3x), `upload-artifact` at v4 (11x) and v5 (3x),
  `download-artifact` at v4 and v5 -- so a pin pass is also a consistency pass.
  `mymindstorm/setup-emsdk@v14` is `version: latest` at **both** `ci.yml:155`
  and `:1520`; the second one carries a comment reading "Pinned to a modern
  release" directly above the word `latest`, which is the kind of thing that
  reads as done in review. `pip` is unpinned three ways: bare `pyyaml`
  (`ci.yml:126`), `--upgrade -r tools/requirements.txt` (`:1517`), and
  `-r tools/requirements.txt` (`release.yml:286`), where the requirements file
  itself carries `markdown>=3.4` -- a floor, not a pin, and no hashes.
  `ci.yml:178` clones `turmeric-spices` at `--depth 1` off its default branch
  and compiles it. The ccache `restore-keys` prefixes
  (`:138, 442, 733, 1403, 1460`) are as described.
- **C-7 confirmed, and it is the mildest row in the table.** MIR is already
  SHA-pinned (`cmake/mir.cmake:141`) with a paragraph above it explaining that
  an existing build dir keeps fetching its cached pin -- the pin is the
  careful part of that file, not the loose part. The fork is the author's own.
  `Dockerfile:18,34` uses `ubuntu:22.04` by tag; `.devcontainer/Dockerfile:15`
  and `:20` are the two `curl | bash` installs (NodeSource and just.systems).
  Both are developer-facing, not on any user's install path.

### Rows whose mechanism was wrong

- **C-5's permissions half points the other way. This is the biggest re-grade.**
  The survey reads "`ci.yml` has no top-level `permissions:` (default token
  scope everywhere...)" as an over-grant. It is not, today:
  `GET /actions/permissions/workflow` returns
  `{"default_workflow_permissions":"read","can_approve_pull_request_reviews":false}`.
  The repo default is already read-only, so every job in `ci.yml` and
  `release.yml` that does not declare `permissions:` is **already** running on
  a read-only token, and the two jobs that need more declare it locally
  (`ci.yml:812` `contents: write` scoped to `publish-timings` with a comment
  saying why it is not top-level; `release.yml:338` `contents: write`).
  So the finding is **defense in depth, not a live over-grant**: the guarantee
  currently lives in a repo *setting* that one checkbox in the web UI silently
  reverts, with nothing in the tree to notice. Writing the top-level block
  moves that guarantee into version control where a diff shows it. Worth doing
  and cheap -- but it is **low**, not medium, and it should not be described in
  a changelog as closing an over-permissioned token.
- **C-6's injection half is fully fixed, not half.** The survey (and the WP4
  annotation) say `inputs.seed` was half-fixed and `fuzz.yml:75` still
  interpolates it into a `run:` block. It does not: all three inputs now go
  through `env:` -- `INPUT_SEED` (`:89`), `N` (`:104`), `SECS` (`:222`) -- and
  a grep for `inputs\.` over the file returns exactly those three lines and
  nothing else. **Nothing in C-6 is left but the `issues: write` scope**, which
  is a different question from injection and is re-framed below.
- **C-8 is mostly already closed, by WP1's collateral rather than by WP7.**
  Of the three committed files, **all three are gone**: the tracked set under
  `.claude/` is six paths and `settings.local.json` is not among them, the
  `project_er6.md` memory file is not in the tree, and `TEMP.md` does not
  exist. Of the five missing files, **`SECURITY.md` and `CODEOWNERS` both now
  exist**. What is genuinely left is two files -- `.github/dependabot.yml` and
  a CodeQL workflow -- plus the repo-settings half, which has also moved
  (below).

### Repo settings: three of four have moved since WP1 read them

WP1 recorded "`main` has no branch protection", "secret scanning and push
protection are disabled", and suggested folding the repo-settings half into
WP7. Re-read today, that ledger is stale in the repo's favour, and the one
remaining gap is not the one WP1 named:

| Setting | WP1 (2026-09-29) | Now | Left to do |
| --- | --- | --- | --- |
| Branch protection on `main` | `404 Branch not protected` | **exists** | tighten -- see below |
| Secret scanning | disabled | **enabled** | -- |
| Push protection | disabled | **enabled** | -- |
| Private vulnerability reporting | (not read) | **enabled** | -- |
| Dependabot alerts | (not read) | **enabled** (204) | -- |
| Dependabot security updates | disabled | **still disabled** | turn on |
| Default workflow token | (not read) | **`read`** | mirror into the tree |

The branch protection that now exists is close to empty:
`required_approving_review_count: 0`, `required_status_checks.contexts: []`,
`enforce_admins: false`, `required_signatures: false`. What it does buy is
`allow_force_pushes: false` and `allow_deletions: false` -- which is the half
that matters for C-1, since `--HEAD` installs track `main` and a force-push to
`main` is the cheapest way to make every new install build attacker code.
Worth saying out loud: **C-1's severity is bounded by exactly that setting**,
and it was not in place when the survey graded the row.

Raising `required_status_checks` is the one that needs the author's judgement
rather than a patch, because of the standing position that a red suite never
blocks landing, and because CI does not run at all for PRs not based on `main`
(so a required check would hard-block every stacked PR). Recommendation: leave
required checks empty, and do not add required reviews on a solo repo; the
force-push and deletion locks are the load-bearing part and they are on.

### Findings the survey did not have

- **The private-triage posture is largely symbolic on a public repository, and
  that is the real C-6 finding.** The first draft of this section said the gap
  was that `fuzz.yml:184` and `tsan.yml:111` file public issues while WP4's
  parser job does not -- two policies in one file. Checked before acting, that
  framing is wrong in the way that matters. This repo is public, so a
  **scheduled run is already world-readable the moment it finishes**: the run
  page returns HTTP 200 to a signed-out client, the workflow-run list is
  anonymously readable over the API (`total_count` comes back without a
  token), and the parser job's own failure path writes the failing target
  names into `$GITHUB_STEP_SUMMARY` and emits `::error::parser fuzzing found
  something in: $FAILED` -- both of which land in those public logs -- then
  uploads `fuzz-parser-findings`, an artifact any signed-in GitHub user can
  download. Not filing an issue withholds a **title, an index entry and a
  notification**. It does not withhold the finding.

  So the two-policy split is not an oversight to patch; it is a partial
  mitigation whose limit was not written down. And section 6 item 3's claim
  that a parser finding "is triaged privately" is an **overclaim of the same
  shape as the three WP1 corrected** -- it describes an intent the mechanism
  does not deliver. Anyone who accepts it at face value will believe a nightly
  memory-safety finding in an untrusted-input parser is embargoed when it has
  in fact been public since 04:30 UTC.

  Genuinely private triage would mean the fuzz search not *running* in public
  -- a private mirror of the repo, or a job whose only output is a
  notification to the maintainer -- which is a scope and cost decision for the
  author, not a patch. **Deliberately not changed here**: the issue-filing in
  job 1 is something the author built on purpose after four nights of findings
  reached nobody (the comment at `fuzz.yml:145-152` records it), and removing
  it unilaterally would trade a working signal for a privacy guarantee the
  platform is not providing either way. Recorded as an open question in
  section 7 instead, with section 6 item 3 corrected to say what is true.
- **`markdown>=3.4` is a floor with no ceiling and no hash**, and it is
  installed in `release.yml:286` -- i.e. inside the job that produces the
  published docs tarball. `pip install --upgrade` at `ci.yml:1517` takes
  whatever PyPI serves that morning. This is the one C-5 sub-item with a path
  into a release artifact rather than into CI only.
- **WP6 handed WP7 a pin.** `tools/genguides.py:877` imports
  `https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs` -- a
  floating major -- and `web/csp.js:32` allows that whole path. A dynamic
  `import()` cannot carry an SRI hash, so the CSP path scope is the only
  control, and the version must be pinned in **both** files together or the
  CSP stops matching. Section 2f flagged this and assigned it here.
- **Nothing pins the `turmeric-spices` clone, and nothing can, cheaply.**
  `ci.yml:178` clones the default branch. Pinning it to a SHA means a file in
  this repo that some human bumps, which in practice goes stale and then gets
  bumped blind -- the failure mode `cmake/mir.cmake`'s comment block describes
  from experience. The honest framing is that `turmeric-spices` is the same
  owner under the same account, so it sits **inside** the trust boundary `main`
  already draws, not outside it. Recorded here as a deliberate non-pin with a
  reason rather than left as an open row.

## 3. Work packages

Each package names its scope, method, deliverable and exit criterion.
Effort is in focused engineer-days for someone who knows the tree; double
it for someone who does not. Findings are filed the usual way, under
`docs/reported/<slug>.md` with a row in the index -- **except** an
exploitable one, which goes to the private channel WP1 sets up until the
fix lands, then is archived normally.

### WP1 -- Threat model, SECURITY.md, disclosure channel (1 day) -- DONE 2026-09-29

- [x] `docs/guides/security-guide.md` -- section 1's five boundaries, each with
  the promise and a **status today** block naming the open defect where the
  promise is not kept. Written so a closed gap is deletable in one piece, per
  the no-archeology rule for guides.
- [x] Root `SECURITY.md` -- the private advisory form, 7-day acknowledgement /
  14-day assessment, no fix deadline promised, scope and non-scope.
- [x] GitHub private vulnerability reporting **enabled** (was `false`).
- [x] `CODEOWNERS` at the repo root, grouped by the boundary each path sits on
  rather than by directory, and covering the survey's seven plus
  `justrun.c`, `macro_env.c`, `interpreter_natives.c`, `src/lsp/`, `dap.c`,
  `tvm/` and `Formula/turmeric.rb`.
- [x] Four overclaims corrected (the survey's three, re-graded, plus the `tvm`
  one it missed -- see section 2a): `sandboxing-guide.md` (the false
  "`read-async`/`write-async` are similarly blocked" claim, the I/O row of the
  capability table, and a header warning), `consuming-spices-guide.md` (TOFU
  stated plainly, `tur build` named as unchecked), `releases-and-installation-guide.md`
  (the `tvm` verify claim, with the four skip paths tabulated; the `--HEAD`
  consequence), `README.md` + `web/index.html` (the install disclosure).
- [x] C-8: the three files untracked and `.gitignore`d. Re-graded low -- none
  held a credential, so no rewrite (section 2a).
- [x] Both VS Code extensions declare `capabilities.untrustedWorkspaces`, with
  `turmeric.serverPath` restricted. This was a finding of WP1's own, not a
  section 2 row (section 2a), and it is what makes T1's editor promise
  enforceable rather than aspirational.
- [x] README links the guide and the advisory form from a new `## Security`
  section.
- **Exit met.** Later packages grade against the guide's boundary table; each
  open item in the guide names its section 2 id, so closing a row means
  deleting that guide block.

**Left for others deliberately:** the repo-settings half of the new findings
(branch protection, secret scanning, Dependabot) is folded into WP7 rather than
flipped here, and the four T1 code fixes the promise depends on (defer Justfile
backticks, a `--no-macros` equivalent, the direnv-style repl trust prompt, the
`:link-flags`/`:build-dir` grammar) stay with WP2 and WP3.

### WP2 -- Compiler driver: command construction and filesystem (4-5 days) -- DONE 2026-09-29

Re-scoped before execution against the verification in section 2b. The method
changed in one place: **(b) "build an argv, never a string" was withdrawn for
the cc invocation** (2b gives the three reasons), and D-1 became a grammar at
the untrusted boundary plus quoting for the driver's own paths.

- [x] **`src/tur_argcheck.h`** -- one header holding the grammars: what a
  contributed link token may look like, what a path a manifest supplies may
  look like, and whether a string is free of shell syntax. `main.c`, `pkg.c`
  and `mcp.c` all call into it, so the rules cannot drift between them the way
  the two link-flag readers once did.
- [x] **D-1a, the grammar (the untrusted half).** `scan_autolink_markers` now
  checks every token of a marker body and **fails the build** on anything
  outside the vocabulary; `pkg_manifest_read` checks `:link-libs`,
  `:link-flags` and `:c-flags`; `pkg_cmake_manifest_append_cc_flags` drops a
  cmake-manifest token carrying shell syntax with a diagnostic. The diagnostic
  names the token and quotes its marker -- not the module, which is not
  recoverable at that point (2b).
- [x] **D-1b, the quoting (the driver's own half).** `buf_put_quoted` wraps
  `tur_shell_quote`; `-o`, the inputs, the `-I` dirs, the aux paths and the
  `cc -c` input/output are quoted at all five `system()` sites that assemble a
  cc command, and a path too long to quote refuses the command rather than
  running a truncated one. `TUR_CC_FLAGS` stays shell syntax by contract. This
  is also a correctness fix: `tur build "My Dir/ok.tur" -o "My Dir/my out"`
  failed before it (`clang: no such file or directory: 'Dir/old'`) and works
  now.
- [x] **D-2, the Justfile.** `JVar` keeps the unevaluated RHS and
  `jvar_force` evaluates it on first use, with a cycle guard. `--list` forces
  nothing; `--set` drops the `expr` so an override no longer pays for a command
  it discards; the recipe environment binds lazily, so a recipe that never
  mentions a variable never runs its backtick; `export` publishes at force time
  (2b). A failed force is fatal at interpolation rather than silently empty.
  The shebang script honours `TMPDIR`. `{{ }}` args stay raw shell **by
  decision, with the reasoning in the code**: `just` splices them as shell text,
  so quoting would make `ls {{ flags }}` one argument and break every Justfile
  that passes flags through a variable -- the boundary that matters is that
  reaching a recipe body now requires naming a recipe.
  - *Behaviour change, deliberate:* a forward reference to a variable defined
    later in the file now resolves, where it used to be "unknown variable".
    `just` resolves forward references too, so this is convergence;
    `tests/run-tur-run-rhs-eval.sh` case 7 was updated and a cycle case added.
- [x] **D-3, the repl cache.** A `.built-by` sidecar records the compiler's
  version, path, size and mtime, and `needs_rebuild` rebuilds when it does not
  match -- so a committed `.tur-repl-cache/lib-N.so` with a convenient mtime is
  not `dlopen`ed. The check is "did I build this", not "is this file
  trustworthy": an attacker can write the stamp, but not one matching the `tur`
  binary on the machine they are attacking.
- [x] **D-4, the shared temp dir.** `<tmpdir>/tur-build` is `lstat`ed for a
  real directory we own that is not group/world-writable; one that fails is not
  repaired (we do not own it) but replaced by a private `tur-build-<uid>`, with
  a diagnostic. The generated `.c` and the prelude sources open `O_NOFOLLOW`,
  and the prelude object must be a regular file we own rather than merely
  non-empty.
- [x] **D-5/D-6, the package paths.** Spice `name`, `:ref`, `:url`, `:subdir`,
  `:path`, `:members` and `:build-dir` are validated at manifest-parse time.
  `:path` is the deliberate exception to containment -- it is documented as a
  sibling/monorepo pointer (`../leaf`) and is a place we READ, never one we
  create, so it gets shell-safety only. `--` was added before the positional
  ref in `pkg_git_fetch`'s update path and in `upgrade_ls_remote`.
- [x] **D-7, `TUR_SHQ`.** Deleted. Its five uses, the four hand-written `'%s'`
  interpolations in the two `tur fmt --diff` commands and the URL in
  `open_in_browser` all go through `tur_shell_quote` now. The macro's comment
  is replaced by a note saying why a quote *character* is not quoting.
- [x] **D-8, MCP.** `--` is unavailable (2b): a leading `-` in the path becomes
  `./-`, which names the same file, and an empty path is refused.
- [x] **D-9, cmake.** A `:cmake-deps` entry's `name` and `:cmake-name` must be
  cmake identifiers (they land in bare positions no quoting would save), its
  `:url`/`:ref`/`:cmake-version` are validated and now written quoted, and an
  `:options` pair carrying cmake syntax is skipped with a diagnostic.
- [x] **Tests:** `tests/run-security-driver.sh` (ctest `tur_security_driver`),
  27 assertions over `tests/fixtures/security-driver/`. Every rejection fixture
  asserts **both** a diagnostic **and** the absence of the file its payload
  creates -- "failed to build" and "ran, then failed to build" are otherwise
  indistinguishable, which is the trap a fixture like this usually falls into.
  `good-framework-flag` is the counterweight: `-framework Foundation` and
  `-L<dir>` must still build, since a grammar that rejected them would break
  every macOS spice.
- **Exit met.** Every `system()` that assembles a cc command quotes its paths
  and admits only grammar-checked contributed tokens; `tur run --list` runs no
  shell; a manifest cannot write outside `spices/`; the fixture is green, as
  are `tests/run.sh` (3405/0), `regen-snapshots --check` (156 up to date),
  `run-fmt.sh` (34/0), the reported-index lint, and the 28 driver-related ctest
  targets.

**Filed rather than fixed:** `buf-puts-breaks-the-incidental-nul-invariant`
(section 2b). It is a `src/runtime/buf.c` API defect, not a driver one, and the
preferred fix touches every `Buf` in the tree -- out of WP2's scope, and it
wants its own change.

**Left for others, unchanged:** the `--no-macros` equivalent and the
direnv-style repl trust prompt stay with WP3 and the author's decision on
section 7 Q1 respectively. D-3's stamp converts the *accidental* load into a
rebuild; it does not make auto-discovery opt-in, which is the separate design
question that answer raised.

### WP3 -- Interpreter sandbox capability audit (3 days) -- DONE 2026-09-30

Research first; section 2c has the PoCs and the inventory. What landed:

- [x] **The classification.** Every builtin native has one row in
  `src/turi/native_caps.c` naming the capabilities a caller must hold. Three
  classes were added to `TuriCaps` because the builtin set never needed them:
  `TURI_CAP_FS` (by-path file access), `TURI_CAP_PROC` (spawn, wait, exit the
  host) and `TURI_CAP_ENV` (getenv/environ), plus `TURI_CAP_EVERY` for a
  native that evaluates with every capability. The non-pure rows are tabled
  in the sandboxing guide's "Capability classification" section.
- [x] **One choke point (S-1).** `turi_env_register_native` stamps the row's
  bits on the closure; `eval_apply_driven` refuses a native call whose env
  lacks any of them, at both native dispatch sites, before the native runs.
  Unclassified names -- an embedder's own natives -- carry no requirement, and
  `turi_env_register_native_caps` states one explicitly either way.
  `is_blocked_builtin` stays as it was: the builtins are a closed enum checked
  where they are evaluated, and folding them into the name table would add a
  lookup without closing anything.
- [x] **S-2.** The seven `extern-c` overrides need `TURI_CAP_FFI` like every
  other `extern-c`, plus the class of what they do (`exit` proc, `getenv` env,
  `printf`/`printf_s`/`puts` io). The interpreted `printf` re-emits the
  program's format through a checker: one conversion, of the kind matching its
  argument, length modifiers normalised, `%n`/`%p`/`*`/`$` refused.
- [x] **S-3.** The inline-C `snprintf` emulator uses the same checker: one
  conversion per argument, an integer conversion for an int and `%s` only for
  a string parameter. The over-read found alongside is clamped.
- [x] **S-6, S-7** (section 2c): sandboxed `load` is refused in the
  elaborator; the R7RS `eval` bridge requires every capability.
- [x] **The PoC the survey could not build**, both routes, recorded in
  section 2c; each is now a fixture (below).
- [x] **`--no-proc-macros`**, the opt-out section 7 Q1 said WP2/WP3 owed:
  refuses every `defmacro*` definition, call and `:for-macros` import with a
  diagnostic, so no macro-time code runs. Template `defmacro` still expands.
- [x] **The generated test.** `tests/turi/sandbox-eval.c` now (a) requires
  each fixture to fail for its stated reason, via a captured diagnostic sink;
  (b) walks the table: strictly sorted, every native a fresh env holds has a
  row, and every non-zero row is refused in a sandboxed env -- a
  conditionally registered one through a trap native, which also proves the
  stamping; (c) checks a grant admits exactly its class, the explicit
  registration overrides the table, and pure natives still run. Ten new
  sandbox fixtures, `errors/macro-native-denied`, `errors/no-proc-macros`,
  and `no-proc-macros-template-still-expands`.
- [x] **Try Turmeric (S-4)** -- the security guide already records `CAP_ALL`
  behind the browser sandbox as intentional; nothing in WP3 changes it.
  Section 7 Q4 remains the author's call.
- [x] Guides: the sandboxing guide's warning, capability list, reference table
  and native-registration section; the security guide's T3 status and T1
  macro subsection; the macros guide.
- **Exit met, with one open row.** The generated sandbox test is green and
  the sandboxing guide's claims match the table. The T3 promise is still not
  made, because of S-5, which is filed in `docs/reported/` and indexed.

**Left for others deliberately:** S-5's forged-handle half (a per-native
handle-kind column plus a provenance set per restricted env, or tagged handles;
the report has the measured scope and the design). Its host-exit half landed
in the same PR as a follow-up. Also left: making the embedded R7RS env inherit
its caller's capabilities instead of requiring all of them.

**Decided by the author 2026-09-30:** the language server does **not** pass
`--no-proc-macros` by default for now; the question stays open for
reconsideration.

### WP4 -- Deserializers, parsers, and the fuzz harnesses (5-6 days) -- DONE 2026-09-30

This package is the one with the most lasting value: the libFuzzer targets it
leaves behind run nightly under ASan and UBSan. Its research pass is section
2e.

- [x] **Serial continuations (M-1).**
  - Validation lives inside `tur_serial_cont_deserialize` (the emitted
    `tur_serial_cont_check`, `src/compiler/emit_dk_runtime.c`), so no entry
    point can skip it.
  - The check covers every length, every tag, the frame registry, and the
    record's env kind against the frame's registered kind. That last check is
    the one the old validator missed.
  - `bytes->serial-cont` maps the check's codes to its existing `Err`
    messages. `resume-cont!` and `image/blob-resume!` panic, and a caught panic
    yields the identity continuation.
  - No frame-count cap is needed: every record consumes at least 8 bytes or
    fails the walk, so a passing `n` is bounded by the input.
  - `cont-from-file` holds its stored length to the file size.
  - The JIT runtime split (`src/runtime/generated/`) was regenerated.
- [x] **Images (M-1).**
  - Header `flags` bit 0 is now `TUR_IMAGE_FLAG_PAYLOAD_CRC`: a CRC-32 of the
    payload trails it. The writer always sets it; an unknown bit is refused.
  - `payload_len` is held to the file's real size.
  - The continuation is checked before an image counts as loadable, so a
    damaged image is a cold start rather than a panic.
  - `image/loadable?` runs the full load-time check.
  - `tur image-verify` checks the payload (`tur_image_verify_payload`).
  - The CRC is corruption detection, not authentication; the guide says so.
- [x] **The guestbook as worked example (M-1).** It was already HMAC-signing
  a server-side name, so the survey's claim did not hold (section 2e).
  `verify-token` now also requires the 64-hex shape, closing the one real gap.
- [x] **JSON (M-2), in both decoders, kept step-for-step identical.**
  - The trailing-backslash over-read is fixed.
  - Nesting is capped at 256.
  - Decided on `\u`: it is decoded to UTF-8, surrogate pairs included. A lone
    surrogate is an error, and so is `\u0000`, because the value is a C string.
  - Partial trees are freed on every error path.
  - The encoder escapes control characters.
  - `turi_json_decode_cstr` exposes the interpreter's decoder to the fuzz
    target.
- [x] **LSP/DAP framing (M-3).**
  - `Content-Length` is matched per line and case-insensitively, and must be
    digits only.
  - It must be non-zero and at most 64 MiB, and repeated headers must agree.
  - The header block is capped at 8 KiB.
  - ctest `tur_lsp_io_unit`.
- [x] **httpd (M-4).**
  - One head parser, `httpd-parse-head`, is shared by both server loops and is
    pure over a buffer.
  - `Content-Length` must be digits only and repeats must agree (400).
    `Content-Length` together with `Transfer-Encoding` is 400, and
    `Transfer-Encoding` alone is 501.
  - The body cap is `httpd-set-max-body!`, 8 MiB by default, and it is checked
    before anything is allocated (413). A short body drops the connection.
  - Servers **bind 127.0.0.1 by default**. `httpd-set-bind-any!` or
    `TUR_HTTPD_BIND_ANY=1` opts in to 0.0.0.0, and `TUR_BIND_LOOPBACK` still
    forces loopback.
  - One response-head builder drops headers that would split the response.
  - One per-request release fixes the async path's leaks.
  - Async reads wait 5 s per park.
  - `mw-static` checks `realpath` containment (symlinks included), opens what
    it stats, strips the query string, and refuses dot-segments (except
    `.well-known`) and backslashes.
  - Also fixed: the `httpd-set-cookie!` overrun, Basic-auth NUL truncation,
    and the multipart repeated-header leak.
  - Reviewed but deliberately not changed: see
    `docs/archive/httpd-residual-request-hardening.md` (all closed 2026-10-03).
- [x] **Harnesses.** `tests/fuzz/` has ten libFuzzer targets behind
  `-DTUR_FUZZ=ON`, which is clang only and instruments `tur_core` with
  `-fsanitize=fuzzer-no-link`:
  - `fuzz_serial_cont`, `fuzz_serial_wire` and `fuzz_image_header`
  - `fuzz_json_compiled` and `fuzz_json_interp`
  - `fuzz_httpd_head` and `fuzz_lsp_frame`
  - `fuzz_reader` (all six dialects), `fuzz_manifest` and `fuzz_justfile`

  How they are built:
  - **The three stdlib-inline-C targets include `tur emit-c` output generated
    at build time**, so they fuzz the code a compiled program ships, not a
    copy. `hoist-includes.py` applies the `__tur_include__` hoist that
    `tur build` does and `emit-c` does not.
  - **`fuzz_justfile` renames `popen`, `system`, `setenv` and `chdir` before
    it includes `justrun.c`.** The parser still evaluates backticks (D-2), and
    a fuzzer must never run a command it made up.

  How they run:
  - Each target is a ctest that replays its committed seeds.
  - `run-fuzzers.sh` adds the tree's own inputs (every fixture, every
    `build.tur`, every `.json`, the Justfile) to the seeds.
  - The nightly `fuzz-parsers` job in `fuzz.yml` runs at 600 s per target
    with a `contents: read` token.
  - **On a finding, the job fails rather than filing a public issue.** These
    parsers take untrusted bytes, so an auto-filed issue would be public
    disclosure. Triage starts private, per `SECURITY.md`.

  Reference: `tests/fuzz/README.md`.
- [x] **Findings of the harnesses themselves**, all fixed with the reproducer
  committed as a seed:
  - the reader's arena `free()` (T1, high; fixture
    `errors/neoteric-bracket-call-reads`)
  - the Justfile `(dep #` hang (T1, medium; `tests/run-tur-run-attrs.sh`)
  - Justfile expression stack exhaustion (T1, medium; same script)
  - the Justfile parameter leak
  - the manifest repeated-key leaks, for scalar and vector keys, and the
    `#lang` rejection's symbol-table leak
  - the empty-sweet-exp `memcpy(NULL, 0)`
- **Exit.**
  - **Tests for the fixes.** Every fixed memory-safety bug and the hang have
    a fixture, a unit test or a harness case that exercises the input. Three
    of the lesser fixes are covered only by the fuzz seeds and the replay:
    the two leaks and the TSER allocation caps. One has no dedicated test at
    all: the Basic-auth NUL refusal, because `fuzz_httpd_head` does not
    drive the verifier.
  - **The 10-minute runs.** Each target ran for 600 s under ASan and UBSan
    on a 4-core box, four at a time, and all ten finished clean.
    - The first 10-minute pass stopped `fuzz_manifest` and `fuzz_justfile`
      on findings, and the reruns found more. Each finding was fixed and the
      target rerun, until both went the full 600 s clean.
    - Executions in 600 s:
      - `fuzz_serial_cont` 182M
      - `fuzz_image_header` 39M
      - `fuzz_serial_wire` 31M
      - `fuzz_httpd_head` 18M
      - `fuzz_json_compiled` 8.9M
      - `fuzz_lsp_frame` 4.5M
      - `fuzz_json_interp` 4.0M
      - `fuzz_reader` 1.1M
      - `fuzz_manifest` 1.8M, in its final clean run
      - `fuzz_justfile` 1.0M, in its final clean run
- **Decisions this package took, for the author to overrule:**
  - **The httpd bind default moved to loopback.** It is the one change a
    deployed server notices. The plan asked for it, the opt-in is one call,
    and `docs/guides/httpd-guide.md` says so.
  - **`\u0000` is an error**, not U+FFFD.
  - **Section 7 Q2** is answered provisionally as "the application's job";
    see there.

### WP5 -- Runtime memory safety (3 days) -- DONE 2026-09-30

- [x] **Region escape hooks (M-6):** every survey site probed with
  `TUR_REGION_STATS=1`. One confirmed, and it was a class (a Turmeric wrapper
  over an inline-C store); three retired; a second class the survey missed (an
  inline-C callee's type-variable parameter). Both routes are closed in the
  compiler rather than per site, constructors excepted, and pinned by
  `tests/fixtures/region-escape-via-wrapper` on the stats line. The HAMT
  transient setter carries the note too. Section 2d.
- [x] **Integer arithmetic on sizes (M-5):** the six survey rows plus 26 the
  `malloc`/`alloca` sweep found, each range-checked with plain comparisons
  (stdlib inline C goes through c2mir, so no `__builtin_*_overflow`), pinned by
  `tests/fixtures/size-arith-guards`. Three sweep findings handed to WP3/WP4
  with reasons. Section 2d.
- [x] **TSan:** `tests/run-tsan.sh` + `.github/workflows/tsan.yml`, nightly on
  Release, 69 fixtures. Its first run found a sanitizer-build thread-local
  bug, two races in the emitted runtime and three broken fixtures, all fixed;
  one fixture excluded with the reason in the script.
- [x] **Format strings:** `-Wformat=2 -Wformat-security` on the `-Werror`
  targets, the printf-style wrappers annotated so every call is checked, the
  interpreter's vetted-format sites silenced by name, and the one emulator
  WP3's S-3 check had not reached put through it.
- **Exit met.** The region fixture covers every route the survey and the
  verification found; the TSan run is 69/69 (four runs, Debug and the workflow's Release);
  `-Wformat=2` is clean under gcc and clang.

**Left for others deliberately:** out-of-range indices in pure-classified
interpreter natives (S-5; fixed 2026-10-07), httpd's body cap and response-writer
`realloc` (WP4, M-4), and the rest of `serial_cont_from_bytes` (WP4, M-1).

### WP6 -- Try Turmeric and the web worker (1-2 days) -- DONE 2026-09-30

Section 2f has the repro and the deviations. What landed:

- [x] **W-1, the policy.** `web/csp.js` is the one definition, with each
  allowance's reason beside it. `vite.config.js` sends it from the dev and
  preview servers and stamps it into the built `dist/client/_headers` (the build
  fails if it cannot); `worker.js` sets it on every response it produces,
  which `_headers` never reaches. Verified under `wrangler dev` against the
  built site: one copy of the header on an asset, a Worker route and the
  asset-passthrough 404.
- [x] **Whatever it broke.** The `/try/` shell's pre-paint PWA snippet is now
  `public/pwa-shell.js` (precached by `sw.js`). The three doc generators emit
  their scripts as files beside the pages and link plain stylesheets (W-5).
- [x] **W-2.** `escapeHtml` escapes both quotes, so it is safe in any context;
  the attribute sites say `escapeAttr`. The transcript is stored as runs of
  text and `[tag, class, runs]` under `tur.try.console.v2` and rebuilt with
  DOM calls; the HTML-era `v1` key is deleted unread.
- [x] **W-3.** A watchdog over the eval worker's outstanding requests offers
  Stop after 1 s and stops it at 30 s; stopping terminates the worker, fails
  every pending and queued request, and boots a fresh one. A stopped
  `:type`, `:explain` or Trace reports in the console rather than as an
  unhandled rejection.
- [x] **W-4** documented in the security guide's Try Turmeric section.
- [x] **Tests:** `web/tests/security.spec.js`, added to CI's desktop list --
  every shipped page and one of each generated kind load with the policy
  enforced and zero violations; the REPL evaluates under it; an injected
  handler does not run; the `#lang` attribute injection (failing before the
  fix); both console-persistence paths and a reload round trip; Stop, the
  automatic stop, and no Stop on a quick run.
- **Exit met.** With the policy enforced, CI's desktop list plus the new spec
  is green: its one red, `lang-picker`'s grouped-list case, failed on `main`
  too -- it predated the `#lang r7rs/sweet` base v0.57.0 added -- and this PR
  updates its expectation. The mobile project passes 41 of 41 on WebKit in CI.
  Against the built site under `wrangler dev`, the deploy gate, smoke, LSP,
  docs-pane and security specs pass (77). The desktop specs CI does not list
  pass 37 of 40; the three `repl-intelligence` failures fail identically on the
  unmodified tree.

**Left for others deliberately:** pinning mermaid's exact version (WP7), and
fixing Share (filed).

### WP7 -- Supply chain, release, CI (3 days) -- DONE 2026-09-30

Verification pass in section 2g; it re-graded three rows and found two the
survey did not have, so read that before this list. What landed:

| Row | Outcome |
| --- | --- |
| C-1 | **Fixed.** `/install` bootstraps `tvm` (the plan's "or, better") and installs a checksum-verified release; works on Linux, which it never did. `--HEAD` stays as the documented opt-in, so `Formula/turmeric.rb` is unchanged. Answers section 7 question 5. |
| C-2 | **Fixed.** All three refusals plus `--insecure`; `--from` warns. A mismatch is deliberately not `--insecure`-able. |
| C-3 | **Fixed.** Drift is detected, `tur build` and `tur audit` verify, and a *third* defect turned up: `tur fetch` was a no-op on a fresh clone. Checking out `:resolved` landed 2026-10-01 ([lock-tracks-ref-not-resolved-commit](../archive/lock-tracks-ref-not-resolved-commit.md)): a fetch checks out the locked commit, fails rather than falling back to `:ref` when it is gone, and `tur fetch --frozen` holds a fetch to the lock. The guide half was already done by WP1. |
| C-4 | **Fixed.** Sigstore keyless attestation on every asset. Tag signing deliberately not switched on, with the reason recorded in all three `cut-*-release` commands; answers section 7 question 6. |
| C-5 | **Fixed**, except the `turmeric-spices` clone, which is a recorded deliberate non-pin (same owner, inside the trust boundary; see 2g). 56 actions SHA-pinned, emsdk and pip pinned, both top-level `permissions` blocks added -- the last as defense in depth, not the over-grant the survey described. |
| C-6 | **Injection half was already fixed** by WP4. The `issues: write` half is a design decision, not a patch: see 2g and section 7 question 8. |
| C-7 | **Closed as accepted.** MIR was already SHA-pinned; the two `curl \| bash` are developer-facing, in `.devcontainer/`, on no user's install path. Dependabot watches both Dockerfiles now. |
| C-8 | **Fixed.** `dependabot.yml` and a CodeQL workflow; the committed files and the two missing docs were already handled by WP1. Repo settings had moved -- three of the four the survey named are on; only Dependabot security updates is still off (owner action, not a patch). |

Two things WP7 added that were not in the list: **the tvm suites ran nowhere
automatic** before tvm became the install path (a new `tvm` CI job runs both on
Linux and macOS), and the mermaid pin WP6 handed over in section 2f.

- **Installer (C-1):** make `/install` fetch the latest *release* tarball
  and verify `sha256sums.txt` against a value pinned *in the script* for
  that release (or, better, make `tvm` the installer and have `/install`
  bootstrap `tvm`); give `Formula/turmeric.rb` a stable `url`+`sha256`
  block with `head` as the opt-in; update `README.md:20` and
  `web/index.html:331`.
- **`tvm` (C-2):** fail closed -- missing sums, missing row, or missing
  sha tool is an error unless `--insecure` is passed; pin the release base
  URL unless an env override is explicitly acknowledged.
- **Lock verification (C-3):** compare the fetched tree hash with the lock
  row and fail with a diagnostic on mismatch unless `tur fetch --update`;
  check out `:resolved` when present rather than tracking `:ref`; run the
  check in `tur build` as well as `tur run`; make `tur audit` verify. Then
  fix the guide to say what the code does.
- **Release (C-4):** `actions/attest-build-provenance` on every asset,
  `git tag -s` in the cut-release commands (or document the key decision),
  pin `softprops/action-gh-release` to a SHA.
- **Workflows (C-5, C-6):** pin every action to a full SHA with a version
  comment (Dependabot can keep them fresh), add a top-level
  `permissions: contents: read` to `ci.yml` and `release.yml` and raise it
  per job, pin `pip` requirements with hashes, pin the `turmeric-spices`
  clone to a SHA the repo records, move `inputs.seed` to `env:`, set
  `version:` on `setup-emsdk`. Add `.github/dependabot.yml` for actions,
  npm (`web/`), and pip; add a CodeQL workflow for C and JavaScript.
- **Exit:** every action SHA-pinned; release assets carry attestations; the
  installer verifies a checksum; the lock check has a fixture. **All four met**
  -- 56 of 56 pinned, `actions/attest-build-provenance` on every asset,
  `tvm/tests/install-script.sh` (11 assertions) driving the Worker's real
  `/install` route, and seven new cases in `tests/run-spice-fetch.sh` (17
  total; 5 of the 7 fail against the pre-fix binary). The lock cases went into
  that existing harness rather than a new one -- it is already wired into ctest
  AND the Windows CI leg, which is where the package manager's coverage gap
  was, so a parallel harness would have been the thinner half of the pair.

### WP8 -- Effects as a stated boundary (1 day, decision-heavy)

- ~~Decide what `#fx{Unsafe}` promises (section 7).~~ **DECIDED 2026-10-02:
  Option A, pointer arithmetic only** ([effect-row-honesty-plan](effect-row-honesty-plan.md)
  section 3). The marker describes a body, `(unsafe ...)` discharges it and
  erases the row, so the M-1/M-5 deserializers are not retro-tagged. The
  security guide says so in one section, "What the effect system does and
  does not promise", including what *is* enforced.
- Consider defaulting `--lint-inline-c-unsafe` on once `stdlib/` is clean
  (a follow-on to `docs/reported/stdlib-int-stand-in-audit.md`'s sweep).
- **Exit:** one paragraph in the security guide, and a decision recorded
  in this plan's status line.

## 4. Sequencing

| Week | Packages | Why this order |
| --- | --- | --- |
| 1 | WP1, ~~WP7~~ **done 2026-09-30, all eight C rows**, WP4 (M-3 LSP framing; M-1 bounds checks) | The cheapest changes with the largest blast radius: what a user installs, what CI can do with its token, and the two overflows a reporter would demo first. WP7 came in larger than "installer, tvm, workflow pins, permissions": the lock work (C-3) turned up a third defect of its own, and two of the eight rows were already closed by WP1's collateral. |
| 2 | ~~WP2 (D-1, D-2, D-5, D-6)~~ **done 2026-09-29, all nine D rows**, WP3 (S-1 choke point) | The compiler-driver injection class and the sandbox bypass. Both are one design change each plus a sweep. WP2 came in as one header plus a sweep, as predicted; the sweep was the larger half. |
| 3 | WP4 (harnesses, JSON, httpd), WP5 (**done** 2026-09-30) | The fuzz targets need WP4's fixes landed to seed sensibly; region hooks and integer checks are independent. |
| 4 | WP6, WP8, WP2/WP3 remainder, re-grade section 2 | Web hardening, the effects decision, and closing the long tail. |

Roughly 22-25 engineer-days of focused work, spread over four weeks with
the fuzzers running nightly from week 3. Nothing here blocks the v1
track: every package is independently landable, and the plan is a
checklist, not a gate.

## 5. Method and tooling

- **Static:** the grep inventories in section 2 (subprocess, `getenv`,
  `malloc(.*\*`, `alloca`, `innerHTML`), plus `clang-tidy` with the
  `cert-*`, `bugprone-*` and `security.*` checkers over `src/` and CodeQL
  in CI (WP7). `-Wformat=2 -Wformat-security` are on the `-Werror` targets
  (WP5); `-Wshadow` is not.
- **Dynamic:** the Debug build's ASan+UBSan (already on, leak detection on
  for the compiler path), the new libFuzzer targets (WP4), the nightly TSan
  job (WP5, `tests/run-tsan.sh`), `tests/run-leak-check.sh` for emitted
  programs.
- **Manual review checklist** for each boundary crossing: who supplies
  each byte, every length used for allocation or indexing is checked
  against the remaining input, every string that reaches a shell is
  quoted or is an argv element, every path is canonicalised and contained,
  every native declares its capability.
- **Filing:** one `docs/reported/<slug>.md` per defect with the usual
  repro/root-cause/fix-direction shape and a `security-` tag in the index
  row; exploitable ones through the private channel first (WP1).

## 6. Definition of done

1. The security guide and `SECURITY.md` exist and the three overclaims are
   corrected.
2. Every section 2 row is either fixed with a fixture, filed as an open
   report, or retired with a one-line reason in this plan.
3. `tests/fuzz/` targets run nightly under ASan and flag crashes. **Met by
   WP4.** The job fails rather than filing a public issue.
   **Corrected, then made true, by WP7.** The line used to end "so that a
   finding in an untrusted-input parser is triaged privately", which the
   mechanism did not deliver: on a public repository the scheduled run is
   world-readable the moment it finishes, so the step summary naming the
   failing targets, the `::error::` repeating them, and the downloadable
   findings artifact were all public -- not filing an issue withheld a title
   and a notification, not the finding (section 2g has the measurement).
   **Since 2026-09-30 it holds**: findings go to Sentry and nothing else, the
   artifacts are not uploaded, and the summary says only that something
   failed. See section 7 question 8 for the decision and its cost.
4. The generated sandbox test pins every native's capability.
5. Release assets are attested and the installer verifies a checksum.
   **Met by WP7 (2026-09-30):** `actions/attest-build-provenance` signs every
   asset through Sigstore keyless, and `/install` installs a checksum-verified
   release through `tvm`, which fails closed. Both covered by
   `tvm/tests/install-script.sh`.
6. Every workflow action is SHA-pinned with least-privilege permissions.

## 7. Open questions for the author

1. ~~**T1 line:**~~ **ANSWERED 2026-09-29.** Not "safe" -- clangd's line:
   *`tur check`, `tur run --list` and the language server do not execute
   repo-supplied code or shell unless you asked them to.* Consequences the
   answer settles:
   - **D-2 stays high.** `--list` reads as an inventory command in every
     toolchain, so evaluating Justfile backticks at parse time fails the
     surprise test regardless of what the guide promises. The fix is scoped
     (defer to recipe invocation), not a rewrite -- see section 2a.
   - **Macros at check time are Rust's proc-macro problem**, and the promise
     is deferred until S-1 lands rather than made now. A genuinely
     capability-denied macro env would be a better story than Rust's and is
     worth promising *then*. Until then the guide says `tur check` expands
     macros, and WP2/WP3 owe the `--no-macros` equivalent that rust-analyzer
     ships as `procMacro.enable`. **Delivered by WP3 as `--no-proc-macros`;**
     S-1 landed too, so what now deferred the promise was S-5 -- resolved
     2026-10-07, and the security guide now describes the macro env as
     capability-denied and handle-checked.
   - **`tur repl` auto-discovery gets no promise** -- it compiles and
     `dlopen`s, which is Gradle-tier. But `TUR_NO_AUTO_SPICE=1` is
     default-allow, which points against where pnpm, Bun, Deno and Neovim have
     all moved. direnv's model (hash `build.tur`, ask once, remember) is cheap
     and converts D-3 from a defect into a documented design.
   - **`build.tur` is Turmeric's `.cargo/config.toml`.** `:link-flags`,
     `:c-sources` and an absolute `:build-dir` are repo-supplied toolchain
     redirection, and Cargo's answer to the identical hazard is "building is
     running". So `tur build` keeps no promise, and the guide names the
     analogy -- which does more work than a severity number.
   - **The LSP declares its trust support.** Whatever the CLI promises, the
     editor integration should let the editor enforce it; done in WP1, and it
     buys Restricted Mode users the protection Go's extension gives them.
2. **Continuations over the network:** should `bytes->serial-cont` accept a
   key and verify an HMAC, or is it the application's job (the guestbook
   example would then need to show it)?
   **Proposed by WP4, pending the author: the application's job.** The
   guestbook already shows it, and shows the better design: it never sends
   the bytes at all, only an HMAC-signed name for a server-side file (section
   2e). What the runtime now guarantees is memory safety. Every rebuild route
   checks the buffer against this program's frame registry, which is the T2
   promise. The security guide's T2 section says in so many words that a
   checked buffer is not an authenticated one. A keyed
   `bytes->serial-cont/verified` could still be added later without breaking
   anything. It would be the natural home for the HMAC the guestbook
   hand-rolls in inline C, if a second program ever needs one.
3. ~~**`#fx{Unsafe}` semantics** (WP8): pointer arithmetic only, or "may
   corrupt memory on bad input"?~~ **ANSWERED 2026-10-02: pointer arithmetic
   only (Option A).** It is what is built and what every stdlib call site
   assumes, and `(unsafe ...)`-as-discharge is load-bearing; the two readings
   are mutually exclusive on one marker. A propagating "tainted input" marker
   would be a separate feature under its own name.
4. **Try Turmeric:** keep `CAP_ALL` behind the browser sandbox (proposed),
   or run the wasm env sandboxed too for defence in depth?
5. **Installer:** keep Homebrew as the primary channel (with a stable
   formula), or make `tvm` the advertised path?
6. ~~**Release signing key:**~~ **ANSWERED 2026-09-30 by WP7, as proposed:**
   Sigstore keyless via GitHub OIDC. `actions/attest-build-provenance` signs
   every release asset with a short-lived certificate minted from the release
   job's OIDC token, so there is no long-lived key to hold, rotate or lose.
   Tags stay annotated rather than signed, and the three `cut-*-release`
   commands record why: the attestation is what protects a downloader, and a
   `git tag -s` whose key is absent would turn every future cut into a hard
   failure partway through. A maintainer GPG key can still be added later; it
   would prove who cut a release, which is a narrower claim than where the
   bytes came from.
7. **Who** runs the audit -- one person over four weeks, or the packages
   handed out? WP2 and WP3 want someone who knows the driver and the
   interpreter respectively; WP4 and WP7 do not.
8. ~~**Is private triage of fuzz findings worth what it costs?**~~
   **ANSWERED 2026-09-30 -- the "make it real" option, via Sentry rather than a
   private mirror.** The fuzz search, the libFuzzer parser targets and the TSan
   run now report to Sentry and to nothing else: `gh issue create` is gone from
   both workflows (and with it `issues: write`), the findings artifacts are no
   longer uploaded, and the job summaries say only that something failed.
   Details -- target names, the sanitizer report, and the reproducer as an
   attachment -- go to a place only the maintainers can read.

   Three things that made this cheaper than the private-mirror option the
   original question costed:

   - **No new dependency.** `tools/ci/sentry-report.py` is stdlib-only and
     speaks Sentry's envelope endpoint over plain HTTPS -- no SDK to pin, no
     marketplace action, no `curl | bash`, which is what C-5 and C-7 just
     finished removing.
   - **Grouping is a real gain, not just a privacy tax.** Events fingerprint on
     target + crash type + first non-sanitizer frame, so a standing defect is
     one Sentry issue with a count instead of one GitHub issue per night. The
     seed and run URL are context, never grouping -- putting them in the
     fingerprint would reproduce exactly the noise this replaces.
   - **A finding is never dropped to protect privacy.** An unconfigured or
     unreachable Sentry falls back to the public artifact and says so loudly in
     the summary. Losing a memory-safety finding is worse than publishing one.
     `sentry-report.py` exits 2 for "no DSN" and 1 for "send failed" precisely
     so the workflows can tell a misconfigured repo from a bad night.

   **The cost, recorded because it is a real one:** reproducers for un-triaged
   crashes now live at a third party. That is in the security guide's T4
   section rather than left implicit, with the note that unsetting `SENTRY_DSN`
   opts out -- at the price of the public fallback.

   Needs one repository secret, `SENTRY_DSN`. Without it the workflows behave
   as they did before this change, minus the GitHub issue.
