# Archive

This folder contains planning and design documents for Turmeric features under
active development or consideration. For user-facing guides and tutorials, see
[../guides/](../guides/).

## Active Planning Documents

### Phase 19+ (Active Development)

- **[thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md)** -- Thread API design; see [../guides/threading-guide.md](../guides/threading-guide.md)
- **[fiber-asm-ctx-plan.md](fiber-asm-ctx-plan.md)** -- Fiber context-switching fallback strategy for macOS
- **[thread-cancellation-plan.md](thread-cancellation-plan.md)** -- Cooperative thread cancellation (TC0--TC2); prerequisite for WT3 (WASM threads)
- **[wasm-threads-plan.md](wasm-threads-plan.md)** -- WASM pthread support; WT0--WT2 and WT4 complete; WT3 deferred pending TC0--TC2
- **[release-tagging-plan.md](release-tagging-plan.md)** -- Semi-automatic version bumping and GitHub release workflow (not started)

### Ongoing Implementation

- **[contracts-plan.md](contracts-plan.md)** -- Runtime contracts; C0-C1 complete (see [../guides/error-handling-guide.md](../guides/error-handling-guide.md)); C2+ planned
- **[effect-rows-plan.md](effect-rows-plan.md)** -- Effect row enforcement; ER0-ER5 complete, ER6 core done (`try-with`, `--dump-effects`, `--lint-effects`); ER6 advanced items planned (see [../guides/effects-system-guide.md](../guides/effects-system-guide.md))
- **[gadts-plan.md](gadts-plan.md)** -- GADTs; G0-G4 substantially complete (see [../guides/gadts-guide.md](../guides/gadts-guide.md)); `equal-cong` deferred pending HKT
- **[gadts-followup-tasks.md](gadts-followup-tasks.md)** -- Open polish items from the GADT and intersection/union phases
- **[recursive-types-free-monad-plan.md](recursive-types-free-monad-plan.md)** -- Recursive types and Free monad; RF0-RF4 planned
- **[package-management-plan.md](package-management-plan.md)** -- Spice package manager v2; see [../guides/package-management-guide.md](../guides/package-management-guide.md)
- **[cmake-cpm-integration-plan.md](cmake-cpm-integration-plan.md)** -- CMake/CPM integration for C dependencies (v2.x target)
- **[build-and-test-ux-plan.md](build-and-test-ux-plan.md)** -- Build and test UX improvements (dev loop quality-of-life)
- **[vscode-c-inlining-plan.md](vscode-c-inlining-plan.md)** -- C-inlining syntax highlighting for VS Code extension
- **[hkt-deferred-tasks.md](hkt-deferred-tasks.md)** -- Higher-kinded types implementation tracking
- **[hkt-opaque-dispatch-plan.md](hkt-opaque-dispatch-plan.md)** -- HKT opaque-container dispatch; D0 planned (see [../guides/hkt-guide.md](../guides/hkt-guide.md) SS Known Limitations)
- **[interpreter-features-plan.md](interpreter-features-plan.md)** -- Gaps in `src/turi/eval.c`; pattern matching, Phase S4, async/S7 items
- **[effects-continuations-tasks.md](effects-continuations-tasks.md)** -- Consolidated task list for effect rows, linear-continuations, multishot-continuations

### CLI and Developer Experience

- **[developer-ergonomics-plan.md](developer-ergonomics-plan.md)** -- Day-one UX baseline checklist (help flags, eval command, etc.); several items outstanding
- **[autodoc-plan.md](autodoc-plan.md)** -- Docstring standard and doc generator (see also CLAUDE.md); doctest integration pending
- **[doctest-plan.md](doctest-plan.md)** -- Executable doctests from `;;;` Example blocks; not yet started
- **[guide-syntax-toggle-plan.md](guide-syntax-toggle-plan.md)** -- Toggle widget for S-expression/sweet-exp paired examples in guides
- **[lsp-hover-definition-completion-plan.md](lsp-hover-definition-completion-plan.md)** -- LD0-LD4: hover, go-to-definition, completion for the LSP server; not started
- **[datum-comment-plan.md](datum-comment-plan.md)** -- `#;` datum comments (DC0-DC3); no prerequisites; not yet started

### Try Turmeric / Web REPL

- **[try-turmeric-and-tutorial-plan.md](try-turmeric-and-tutorial-plan.md)** -- Web REPL and tutorial site planning
- **[try-turmeric-smoke-tests-plan.md](try-turmeric-smoke-tests-plan.md)** -- Smoke test strategy for the web REPL
- **[try-turmeric-wasm-effects-plan.md](try-turmeric-wasm-effects-plan.md)** -- WASM effects integration for Try Turmeric

### Performance and Testing

- **[performance-improvement-plan.md](performance-improvement-plan.md)** -- Compiler optimization roadmap
- **[performance-comparison-plan.md](performance-comparison-plan.md)** -- Five-language performance comparison framework
- **[perf-comparison-improvements-plan.md](perf-comparison-improvements-plan.md)** -- Idiomatic Turmeric refactor + turi/Rust targets
- **[test-suite-cleanup-plan.md](test-suite-cleanup-plan.md)** -- Documents root causes of intentionally-failing fixtures (956 pass, 5 tracked)
- **[skipped-spices-cleanup-plan.md](skipped-spices-cleanup-plan.md)** -- Phase 1 complete (Category A fixed); six spices still have `requires.typecheck-skip`; companion to `spice-aware-check-plan.md` (history)
- **[spice-test-runner-fix-plan.md](spice-test-runner-fix-plan.md)** -- Five-bug investigation blocking `tur test` / `tur fetch` in all spices; root cause: `{...}` parsed as `F_CONTRACT_TYPE`
- **[interpret-perf-tests-plan.md](interpret-perf-tests-plan.md)** -- Benchmarks runnable via `tur --interpret`

### Design Explorations (Not Yet Scoped)

- **[remove-exceptions-plan.md](remove-exceptions-plan.md)** -- Plan to remove remaining exception machinery
- **[set-literal-plan.md](set-literal-plan.md)** -- `#s(...)` set literal syntax
- **[scscm-tidal-spices-plan.md](scscm-tidal-spices-plan.md)** -- SuperCollider/Tidal live coding spice pair (Phase 20+ target)
- **[effect-types-row-polymorphism-plan.md](effect-types-row-polymorphism-plan.md)** -- Full first-class effect polymorphism (ET0-ET4); not started; v3 target
- **[multishot-continuations-plan.md](../upcoming/multishot-continuations-plan.md)** -- Multi-shot continuations (MS0-MS4); deferred until linear-continuations stable
- **[linear-continuations-plan.md](linear-continuations-plan.md)** -- Linear continuations; deferred to v5+
- **[self-hosted-interpreter-plan.md](self-hosted-interpreter-plan.md)** -- `libturi` importable eval API + self-hosted REPL (speculative; future project)
- **[stubs-and-workarounds.md](stubs-and-workarounds.md)** -- Catalog of known placeholders and test workarounds
- **[c-dsl-plan.md](c-dsl-plan.md)** -- Lisp-to-C99 DSL embedded in Turmeric; type-safe FFI code generation; speculative
- **[glsl-dsl-plan.md](glsl-dsl-plan.md)** -- GLSL shader DSL compiling to GLSL source; pairs with `c-dsl-plan.md`; speculative
- **[opengl-spice-plan.md](opengl-spice-plan.md)** -- `tur-opengl` wrapping OpenGL 3.3 + GLFW + GLAD; draft spice design
- **[new-spices-plan.md](new-spices-plan.md)** -- Seven new Tier-2 spices: postgres, valkey, osc, rtaudio, rtmidi, wav, png; none started
- **[design-mf4-struct-gadt-namespaces.md](design-mf4-struct-gadt-namespaces.md)** -- Separate struct/GADT namespaces to resolve `Vec` name collision (MF4)
- **[reader-macros-plan.md](reader-macros-plan.md)** -- User-defined `#foo[...]` reader macros (RM0-RM4); not started
- **[reader-macros-transitive-plan.md](reader-macros-transitive-plan.md)** -- Thread reader-macro registry through module loading; depends on RM0-RM4
- **[per-spice-docs-plan.md](per-spice-docs-plan.md)** -- Per-spice HTML pages from `README.md` + `;;;` docstrings; tooling plan
- **[web-cleanup-plan.md](web-cleanup-plan.md)** -- CSS consolidation, Prism.js syntax highlighting, web component unification

### Current Plans (Recently Added)

- **[sandboxed-eval-plan.md](sandboxed-eval-plan.md)** -- SB0--SB4 sandboxed eval implementation; SB1--SB4 close remaining gaps
- **[turmeric-spices-plan.md](turmeric-spices-plan.md)** -- Official first-party spice monorepo; all seven spices shipped; see [github.com/turmeric-lang/turmeric-spices](https://github.com/turmeric-lang/turmeric-spices)
- **[cross-plan-followups-plan.md](cross-plan-followups-plan.md)** -- Cross-plan follow-up tasks (F3-2..F3-7 dictionary passing, F5 MutableMap, F6 turi fixture gaps, F8 defstruct compound annotations)
- **[defstruct-field-types-plan.md](defstruct-field-types-plan.md)** -- Residual hardening after F8 landed; `exg5-exists-cycle` and `exg4-pack-into-struct` still blocked
- **[existential-gc-followup-plan.md](existential-gc-followup-plan.md)** -- GC integration for packed existentials; cycle-construction fixtures blocked on defstruct compound annotations
- **[existential-types-plan.md](existential-types-plan.md)** -- `pack`/`open` with typeclass constraints; phases partially in progress
- **[history/httpd-compression-zlib-spice-plan.md](history/httpd-compression-zlib-spice-plan.md)** -- New `tur/zlib` spice wrapping system zlib + `mw-compress` gzip middleware (M6) in `stdlib/httpd-compress.tur`; draft, not started; see [httpd-guide.md](../guides/httpd-guide.md) for planned destination section

### Language Ergonomics (Planned)

- **[fn-type-bare-identifier-plan.md](history/fn-type-bare-identifier-plan.md)** -- Drop leading colons inside `(fn [...] ...)` type expressions; all four phases shipped (parser, codemod, TUR-D0001 deprecation warning, then hard reject)
- **[internal-define-plan.md](history/internal-define-plan.md)** *(history)* -- body-level `define` (let\* semantics); implemented; see [binding-forms-guide.md](../guides/binding-forms-guide.md)
- **[letrec-and-named-let-plan.md](history/letrec-and-named-let-plan.md)** *(history)* -- `letrec` + named let; implemented; see [binding-forms-guide.md](../guides/binding-forms-guide.md)

### Compiler and Codegen (Active)

- **[arbitrary-arity-kinds-plan.md](arbitrary-arity-kinds-plan.md)** -- Lift the `Tuple5` / `KIND_ARROW5` cap; replace closed `Kind` enum with an integer-backed representation; not started
- **[closure-typed-invocation-abi-plan.md](closure-typed-invocation-abi-plan.md)** -- Thread declared `fn` arg/return types through to C invocation sites; retire `TUR_APPLYn` / fat-shim int64 erasure; not started
- **[codegen-clang-int-pointer-cleanup-plan.md](codegen-clang-int-pointer-cleanup-plan.md)** -- Clean `int<->pointer` casts in generated C to fix modern Clang warnings; not started
- **[cross-module-specialization-cache-plan.md](cross-module-specialization-cache-plan.md)** -- Cross-module ABI specialization cache (Phase J of unboxing plan); prerequisites A--I landed; not started
- **[defmodule-per-file-scoping-plan.md](defmodule-per-file-scoping-plan.md)** -- Fix "defmodule must be first form" diagnostic to scope per-file instead of per-compilation-unit; unblocks `(defmodule tur/zlib ...)` in the zlib spice; not started
- **[unboxing-and-monomorphization-plan.md](unboxing-and-monomorphization-plan.md)** -- Unboxed structs, monomorphization, and sized primitives; partial (sized types landed); remaining work in phases B/C

### Tooling (Active)

- **[tur-run-plan.md](tur-run-plan.md)** -- `tur run` Justfile task runner + `tur new` scaffold; basic `tur run` shipped (see [tur-run-guide.md](../guides/tur-run-guide.md)); `tur new`, extended phases (RN6--RN9) outstanding

## Extracted Guides

The following planning documents have guide counterparts in [../guides/](../guides/):

| Guide | Origin |
|---|---|
| [async-await-guide.md](../guides/async-await-guide.md) | `async-await-plan.md` (history) |
| [backtracking-guide.md](../guides/backtracking-guide.md) | `backtracking-cloneable-continuations-plan.md` (history) |
| [cli-args-guide.md](../guides/cli-args-guide.md) | `cli-arguments-plan.md` (history), `arg-parser-plan.md` (history) |
| [effects-system-guide.md](../guides/effects-system-guide.md) | `effects-plan.md` (history); `effect-rows-plan.md` (ER0-ER6 core) |
| [effects-vs-monads.md](../guides/effects-vs-monads.md) | `effects-vs-monads.md` (archive copy deleted; guides version is canonical) |
| [error-handling-guide.md](../guides/error-handling-guide.md) | `contracts-plan.md` (C0-C1 section) |
| [formatter-guide.md](../guides/formatter-guide.md) | `auto-formatter-plan.md` (history) |
| [gadts-guide.md](../guides/gadts-guide.md) | `gadts-plan.md` (G0-G4) |
| [gadts-cookbook.md](../guides/gadts-cookbook.md) | `gadts-plan.md` (examples) |
| [generators-guide.md](../guides/generators-guide.md) | `generators-and-sequences-plan.md` (history) |
| [hamt-guide.md](../guides/hamt-guide.md) | `hamt-plan.md` (history) |
| [hkt-guide.md](../guides/hkt-guide.md) | `closure-typeclass-dispatch-plan.md` (history; CCL complete) |
| [hrt-guide.md](../guides/hrt-guide.md) | `higher-ranked-types-plan.md` (history) |
| [minikanren-tutorial.md](../guides/minikanren-tutorial.md) | `minikanren-plan.md` (history) |
| [module-system-guide.md](../guides/module-system-guide.md) | `module-system-plan.md` (history) |
| [package-management-guide.md](../guides/package-management-guide.md) | `package-management-plan.md`, `tur-cli-plan.md` |
| [serializable-continuations-guide.md](../guides/serializable-continuations-guide.md) | `serializable-continuations-plan.md` (history) |
| [session-types-guide.md](../guides/session-types-guide.md) | `session-types-plan.md` (history; SS0--SS8 complete); `stdlib-session-typed-channels-plan.md` (history; S1--S3 complete) |
| [opaques-guide.md](../guides/opaques-guide.md) | augmented by `stdlib-opaque-handle-types-plan.md` (history; Tier 1--3 complete) |
| [type-annotations-guide.md](../guides/type-annotations-guide.md) | augmented by `spaced-type-annotation-migration-plan.md` (history; Phases 1--5 complete) |
| [sized-types-guide.md](../guides/sized-types-guide.md) | `sized-types-plan.md` (history; SZ3 complete) |
| [stm-guide.md](../guides/stm-guide.md) | `stm-plan-2.md` (history) |
| [substructural-types-guide.md](../guides/substructural-types-guide.md) | `substructural-types-plan.md` (complete); `linear-types-plan.md` (history) |
| [threading-guide.md](../guides/threading-guide.md) | `threading-tasks.md` (complete); `select-fair-blocking-plan.md` (history) |
| [type-annotations-guide.md](../guides/type-annotations-guide.md) | `compound-type-annotations-plan.md` (complete) |
| [union-intersection-types-guide.md](../guides/union-intersection-types-guide.md) | `intersection-union-types-plan.md` (history; IT0--IT4 complete) |
| [uniqueness-types-guide.md](../guides/uniqueness-types-guide.md) | `uniqueness-types-plan.md` (complete) |
| [vscode-guide.md](../guides/vscode-guide.md) | `vscode-syntax-highlighting-plan.md` (history) |
| [contract-types-guide.md](../guides/contract-types-guide.md) | `contract-types-plan.md` (history; CT0--CT4 substantially complete) |
| [dynamic-vars-guide.md](../guides/dynamic-vars-guide.md) | `dynamic-vars-plan.md` (history; DV0--DV4 complete) |
| [web-continuations-tutorial.md](../guides/web-continuations-tutorial.md) | `web-continuations-tutorial-plan.md` (complete) |
| [web-emscripten-tutorial.md](../guides/web-emscripten-tutorial.md) | `web-emscripten-tutorial-plan.md` (history) |
| [frame-guide.md](../guides/frame-guide.md) | `frame-spice-plan.md` (history; FR0-FR10 complete) |
| [binding-forms-guide.md](../guides/binding-forms-guide.md) | `internal-define-plan.md` (history); `letrec-and-named-let-plan.md` (history) |
| [tur-watch-guide.md](../guides/tur-watch-guide.md) | `tur-watch-spice-plan.md` (history; v0.2.0); `tur-watch-tree-per-file-naming-plan.md` (history) |
| [web-stack-guide.md](../guides/web-stack-guide.md) | `tur-httpd-plan.md` (history); `tur-template-plan.md` (history); `tur-tourist-plan.md` (history) |
| [httpd-guide.md](../guides/httpd-guide.md) | `tur-httpd-stdlib-plan.md` (history; H1-H7 shipped) |
| [data-literals-guide.md](../guides/data-literals-guide.md) | `data-literals-plan.md` (history; DL0-DL6 complete) |
| [reactor-guide.md](../guides/reactor-guide.md) | `tur-reactor-plan.md` (history; R1-R8 shipped); `reactor-run-fibers-plan.md` (history; F1-F8 shipped) |
| [httpd-tls-guide.md](../guides/httpd-tls-guide.md) | `tur-tls-plan.md` (history; T1-T6 complete) |
| [developing-spices-guide.md](../guides/developing-spices-guide.md) | augmented by `tur-fetch-system-first-plan.md` (history; SF0-SF4 complete) |
| [httpd-middleware-guide.md](../guides/httpd-middleware-guide.md) | `httpd-middleware-async-plan.md` (history; Track M, M0-M8) |
| [httpd-async-guide.md](../guides/httpd-async-guide.md) | `httpd-middleware-async-plan.md` (history; Track A, A0-A4) |
| [syntax-guide.md](../guides/syntax-guide.md) | `syntax-guide-plan.md` (history); `parse-check-subcommand-plan.md` (history); `sweet-exp-followups-plan.md` (history) |
| [parser-combinators-tutorial.md](../guides/parser-combinators-tutorial.md) | `parser-combinators-tutorial-plan.md` (history) |
| [tourist-routing-guide.md](../guides/tourist-routing-guide.md) | `tourist-routing-composition-plan.md` (history; TR0-TR4 complete) |
| [lsp-guide.md](../guides/lsp-guide.md) | augmented by `ai-assistant-lsp-integration-plan.md` (history; LSP gap-fill + `tur mcp`) |
| [ai-assistant-integration-guide.md](../guides/ai-assistant-integration-guide.md) | `ai-assistant-lsp-integration-plan.md` (history; MCP surface) |

## Historical Documents

Completed implementation plans and superseded design explorations are in
[history/](history/). Recent additions (post-v0.12.0 sweep):

- **[notebook-spice-plan.md](history/notebook-spice-plan.md)** -- `tur-notebook` spice; NB0-NB12 complete; see [notebook-guide.md](../guides/notebook-guide.md)
- **[stats-spice-plan.md](history/stats-spice-plan.md)** -- `tur-stats` spice shipped; see [stats-guide.md](../guides/stats-guide.md)
- **[plot-spice-plan.md](history/plot-spice-plan.md)** -- `tur-plot` spice shipped; follow-ups in [plot-spice-followups-plan.md](plot-spice-followups-plan.md)
- **[adt-type-params-plan.md](history/adt-type-params-plan.md)** -- Typed type parameters for `defdata` / `defgadt`; complete
- **[csv-optional-delimiter-plan.md](history/csv-optional-delimiter-plan.md)** -- Optional delimiter in `stdlib/csv.tur`; complete
- **[test-recovery-plan.md](history/test-recovery-plan.md)** -- `turi_fixture_tests` / `tur_spice_resolver_tests` fully green
- **[tuple-type-plan.md](history/tuple-type-plan.md)** -- N-ary tuple types (`Tuple2`..`Tuple5`, TP1) shipped
- **[direct-anonymous-lambda-application-plan.md](history/direct-anonymous-lambda-application-plan.md)** -- `((fn [...] ...) args)` direct application; landed in `7660a5b8`
- **[param-type-annotation-plan.md](history/param-type-annotation-plan.md)** -- Spaced `: T` parameter type annotations; landed in `fccb1621`
- **[typed-slots-generic-substrate-plan.md](history/typed-slots-generic-substrate-plan.md)** -- GS1-GS5 substrate; subsumed by completed TS1-TS6
- **[typed-slots-gs5-compiler-support-plan.md](history/typed-slots-gs5-compiler-support-plan.md)** -- GS5 compiler support; subsumed by completed TS1-TS6

Post-v0.14.6 sweep:

- **[variadic-typing-plan.md](history/variadic-typing-plan.md)** -- Typed variadic rest parameters (V0--V7); complete; documented in CLAUDE.md
- **[control-flow-completeness-plan.md](history/control-flow-completeness-plan.md)** -- Pre-v1.0 control-flow gap closure (CF0--CF7); complete; see [effects-system-guide.md](../guides/effects-system-guide.md) and [backtracking-guide.md](../guides/backtracking-guide.md)
- **[control-flow-completeness-audit.md](history/control-flow-completeness-audit.md)** -- Pre-/post-v1.0 control-flow audit; companion to above
- **[typing-gap-plan.md](history/typing-gap-plan.md)** -- Pre-v1.0 advanced-typing gap closure (TY0--TY6); complete
- **[typing-gap-audit.md](history/typing-gap-audit.md)** -- Pre-/post-v1.0 advanced-typing audit; companion to above
- **[first-class-handlers-plan.md](history/first-class-handlers-plan.md)** -- First-class effect handler values (FH0--FH7); complete; `effects-system-guide.md` updated
- **[first-class-handlers-semantics.md](history/first-class-handlers-semantics.md)** -- FH0 operational semantics spec; companion to above
- **[handler-typecheck-and-typename-followups-plan.md](history/handler-typecheck-and-typename-followups-plan.md)** -- Handler arg-checking + `type_name` ownership follow-ups (PH0--PH3); complete
- **[lifetime-syntax-plan.md](history/lifetime-syntax-plan.md)** -- `'a` lifetime-annotation surface syntax (LS0--LS5); complete; `substructural-types-guide.md` updated
- **[sized-types-completion-plan.md](history/sized-types-completion-plan.md)** -- Sized types SZ4--SZ9 (real `-Xsized-types` flag, type-level index, static checking, inference); complete; see [sized-types-guide.md](../guides/sized-types-guide.md)
- **[sized-types-index-spec.md](history/sized-types-index-spec.md)** -- SZ6 type-level size-index spec; companion to above
- **[manifest-driven-build-descent-plan.md](history/manifest-driven-build-descent-plan.md)** -- Manifest-aware `tur build <dir>` + recursive src/ collection + T1--T3 follow-ups; complete; documented in CLAUDE.md
- **[local-spice-dev-workflow-plan.md](history/local-spice-dev-workflow-plan.md)** -- `:path`/`:members` local cross-spice dependency resolution (LS1--LS8); complete; documented in CLAUDE.md
- **[scscm-spice-import-refactor-plan.md](history/scscm-spice-import-refactor-plan.md)** -- scscm spice import refactor + `scscm-compile` fixture repair; complete
- **[repl-spice-watch-flake-plan.md](history/repl-spice-watch-flake-plan.md)** -- De-flake `tur_repl_spice_watch` CI test; complete
- **[reader-float-parsing-plan.md](history/reader-float-parsing-plan.md)** -- Float-literal exponent + precision fixes (Option B); complete
- **[asan-debug-leaks-plan.md](history/asan-debug-leaks-plan.md)** -- ASan/LSan leak policy + ABI-spec arena fix (Phase 1+2); complete; documented in CLAUDE.md
- **[known-bugs-followups-plan.md](history/known-bugs-followups-plan.md)** -- Remaining open bugs from `known-bugs.md`; all resolved
- **[internal-define-plan.md](history/internal-define-plan.md)** -- Body-level `define` (let\* semantics); complete; see [binding-forms-guide.md](../guides/binding-forms-guide.md)
- **[letrec-and-named-let-plan.md](history/letrec-and-named-let-plan.md)** -- `letrec` + named let; complete; see [binding-forms-guide.md](../guides/binding-forms-guide.md)
- **[raygui-spice-plan.md](history/raygui-spice-plan.md)** -- `tur-raygui` immediate-mode GUI spice; complete (v0.1.0 in turmeric-spices)
- **[tur-watch-spice-plan.md](history/tur-watch-spice-plan.md)** -- `tur-watch` cross-platform watcher spice; complete (v0.2.0); see [tur-watch-guide.md](../guides/tur-watch-guide.md)
- **[tur-watch-tree-per-file-naming-plan.md](history/tur-watch-tree-per-file-naming-plan.md)** -- `tur-watch` v0.2.0 per-file naming in tree mode; complete
- **[tur-httpd-plan.md](history/tur-httpd-plan.md)** -- `tur-httpd` threaded HTTP/1.1 server spice; complete (v0.1.0); see [web-stack-guide.md](../guides/web-stack-guide.md)
- **[tur-template-plan.md](history/tur-template-plan.md)** -- `tur-template` ERB/EJS-style templating engine spice; complete (v0.1.0); see [web-stack-guide.md](../guides/web-stack-guide.md)
- **[tur-tourist-plan.md](history/tur-tourist-plan.md)** -- `tur-tourist` Scotty-style micro-framework spice; complete (v0.1.0); see [web-stack-guide.md](../guides/web-stack-guide.md)
- **[tur-fmt-plan.md](history/tur-fmt-plan.md)** -- `tur fmt` in-place formatter with directory walking; complete (see `tur fmt --help`); see also [formatter-guide.md](../guides/formatter-guide.md)
- **[solid-modeling-sdf-raylib-plan.md](history/solid-modeling-sdf-raylib-plan.md)** -- `tur-sdf-raylib` SDF + raylib solid modeling spice; Phase 1 complete (v0.1.0 in turmeric-spices)

Post-v0.16.0 sweep:

- **[tur-reactor-plan.md](history/tur-reactor-plan.md)** -- `tur/reactor` lightweight evented reactor; R1-R8 shipped; see [reactor-guide.md](../guides/reactor-guide.md)
- **[reactor-run-fibers-plan.md](history/reactor-run-fibers-plan.md)** -- Local fiber driver on top of `tur/reactor`; F1-F8 shipped; F9 (global-scheduler rewrite) split out to [scheduler-on-local-fiber-group-plan.md](scheduler-on-local-fiber-group-plan.md)
- **[tur-httpd-stdlib-plan.md](history/tur-httpd-stdlib-plan.md)** -- `stdlib/httpd` lightweight HTTP/1.1 server (was `tur-httpd-plan.md`); H1-H7 shipped; see [httpd-guide.md](../guides/httpd-guide.md)
- **[tur-tls-plan.md](history/tur-tls-plan.md)** -- `tur-tls` spice + httpd H5 integration; T1-T6 complete; see [httpd-tls-guide.md](../guides/httpd-tls-guide.md)
- **[data-literals-plan.md](history/data-literals-plan.md)** -- `#map{...}` / `#set{...}` / `[...]` data literals (`-Xdata-literals`); DL0-DL6 complete; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[tur-fetch-system-first-plan.md](history/tur-fetch-system-first-plan.md)** -- `:prefer-system` resolution for `tur fetch :cmake-deps`; SF0-SF4 complete; documented in [developing-spices-guide.md](../guides/developing-spices-guide.md)
- **[list-quasiquote-plan.md](history/list-quasiquote-plan.md)** -- Runtime list quasiquote (`` #` ``); rejected (Option D / "not now")
- **[runtime-symbols-plan.md](history/runtime-symbols-plan.md)** -- First-class `:Sym` type (`-Xsymbols`); SYM0--SYM6 complete; see [symbols-guide.md](../guides/symbols-guide.md)
- **[schema-plan.md](history/schema-plan.md)** -- `tur/schema` runtime validation; SC0--SC4/SC6 shipped here; SC5/SC7 completed in `return-type-dispatch-and-schema-sc5-sc7-plan.md`; see [schema-guide.md](../guides/schema-guide.md)
- **[return-type-dispatch-and-schema-sc5-sc7-plan.md](history/return-type-dispatch-and-schema-sc5-sc7-plan.md)** -- Return-type-directed dispatch + `HasSchema` typeclass (SC5) + `Functor`/`Applicative`/`Alternative` (SC7); complete; see [schema-guide.md](../guides/schema-guide.md)
- **[sc7-carrier-duality-plan.md](history/sc7-carrier-duality-plan.md)** -- SC7 final blocker: transparent int-newtype approach for `(Schema a)` wrapper; resolved
- **[json-reader-macro-plan.md](history/json-reader-macro-plan.md)** -- `#json(...)` reader macro; JR0--JR5 complete; see [json-guide.md](../guides/json-guide.md)
- **[typed-collection-elements-plan.md](history/typed-collection-elements-plan.md)** -- Polymorphic `Vec[A]`/`Map[K V]` element carrier (TCE0--TCE6); complete; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[typed-map-surface-plan.md](history/typed-map-surface-plan.md)** -- Typed `Map[K V]` API surface (TMS0--TMS5); `smap-*` retired; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[generic-map-key-dispatch-plan.md](history/generic-map-key-dispatch-plan.md)** -- Uniform `#map{...}`/`hamt-of` content-keyed dispatch for typed keys (GMK0--GMK4); complete via Approach B; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[generic-hash-eq-dispatch-plan.md](history/generic-hash-eq-dispatch-plan.md)** -- `Hash`/`Eq` typeclass dispatch for typed map keys (GHE0--GHE5); effectively complete; generic-dict path extracted to GDE plan
- **[generic-dict-eq-map-dispatch-plan.md](history/generic-dict-eq-map-dispatch-plan.md)** -- Content equality through polymorphic `^Eq A` constraint (GDE0--GDE5); complete
- **[diagnose-unbound-call-heads-plan.md](history/diagnose-unbound-call-heads-plan.md)** -- `tur check` diagnoses unbound call heads (UCH0--UCH2); complete
- **[codegen-parentheses-warnings-plan.md](history/codegen-parentheses-warnings-plan.md)** -- BIN_INFIX/VARIADIC_FOLD paren trimming to silence `-Wparentheses-equality` (PW0--PW3); complete
- **[codegen-cross-module-private-defn-collision-plan.md](history/codegen-cross-module-private-defn-collision-plan.md)** -- Fix for private same-named defns across modules collapsing to one C symbol (CC0--CC2); complete
- **[fat-closure-return-position-plan.md](history/fat-closure-return-position-plan.md)** -- `^fat` return-type marker auto-shimming non-capturing lambdas at return sites; complete; documented in test-suite-idioms-plan.md

Post-v0.18.0 sweep:

- **[httpd-middleware-async-plan.md](history/httpd-middleware-async-plan.md)** -- `tur/httpd` standard middleware library and async server; M0-M8 + A0-A4 shipped; see [httpd-middleware-guide.md](../guides/httpd-middleware-guide.md) and [httpd-async-guide.md](../guides/httpd-async-guide.md). Remaining items tracked in [httpd-middleware-plan.md](httpd-middleware-plan.md)
- **[error-handling-deferred-plan.md](history/error-handling-deferred-plan.md)** -- `?` query operator, contracts, panic lints, `catch-unwind` (R1, C2, R6a-d, R2 + R6c); complete; see [error-handling-guide.md](../guides/error-handling-guide.md)
- **[cps-transform-plan.md](history/cps-transform-plan.md)** -- Whole-program CPS transform (CPS0-CPS11); complete; backs `call/cc`, `escape`, and the delimited-control substrate; see [delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)
- **[call-cc-completion-plan.md](history/call-cc-completion-plan.md)** -- Undelimited `call/cc` / `escape` on the CPS substrate (CC0-CC6); complete; enabled by default; see [delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)
- **[ai-assistant-lsp-integration-plan.md](history/ai-assistant-lsp-integration-plan.md)** -- LSP capability gap-fill + MCP server (`tur mcp`); shipped in #173; see [lsp-guide.md](../guides/lsp-guide.md) and [ai-assistant-integration-guide.md](../guides/ai-assistant-integration-guide.md)
- **[parse-check-subcommand-plan.md](history/parse-check-subcommand-plan.md)** -- `tur parse-check` subcommand backing the guide toggle-pair AST checker; shipped in #182; see [syntax-guide.md](../guides/syntax-guide.md)
- **[syntax-guide-plan.md](history/syntax-guide-plan.md)** -- Unified syntax guide covering s-expressions and sweet-expression mode; shipped in #182; see [syntax-guide.md](../guides/syntax-guide.md)
- **[sweet-exp-followups-plan.md](history/sweet-exp-followups-plan.md)** -- SRFI-110 preprocessor follow-ups (curly-infix operator detection, neoteric bracket chaining); all landed (#188); see [syntax-guide.md](../guides/syntax-guide.md)
- **[parser-combinators-tutorial-plan.md](history/parser-combinators-tutorial-plan.md)** -- Parser-combinators tutorial outline; shipped as [parser-combinators-tutorial.md](../guides/parser-combinators-tutorial.md)
- **[tourist-routing-composition-plan.md](history/tourist-routing-composition-plan.md)** -- `tourist/routing` (`url-map!`, `cascade!`, `cascade-with!`, `req-full-path`) in `turmeric-spices/spices/tourist`; TR0-TR4 complete; see [tourist-routing-guide.md](../guides/tourist-routing-guide.md)
- **[tursweet-extension-rename-plan.md](history/tursweet-extension-rename-plan.md)** -- Global rename of `.tursweet` -> `.tur.sweet` and `--lang tursweet` -> `--lang sweet-exp`; complete
- **[drop-just-dependency-plan.md](history/drop-just-dependency-plan.md)** -- Replace `just` invocations in docs/bootstrap with plain CMake + `tur run`; complete; see [tur-run-guide.md](../guides/tur-run-guide.md)
- **[defstruct-inline-c-byvalue-callsite-plan.md](history/defstruct-inline-c-byvalue-callsite-plan.md)** -- Compiler: sync call-site to inline-C by-value struct params (DS0-DS2); complete
- **[variadic-rest-closure-cast-plan.md](history/variadic-rest-closure-cast-plan.md)** -- Compiler: cast variadic-rest function-pointer args at call sites (V0-V2); complete
- **[curried-call-cast-rough-edges-plan.md](history/curried-call-cast-rough-edges-plan.md)** -- Compiler: partial-application + closure-call `(intptr_t)` cast fixes surfaced by the httpd middleware work; complete
- **[bare-fat-lambda-param-plan.md](history/bare-fat-lambda-param-plan.md)** -- Allow bare `^fat g` param on `(fn ...)` lambdas; implemented
- **[noncapturing-closure-inline-c-dispatch-plan.md](history/noncapturing-closure-inline-c-dispatch-plan.md)** -- Auto-shim non-capturing closures at fat-dispatched inline-C sinks; implemented (2026-06-03)
- **[httpd-conn-struct-consolidation-plan.md](history/httpd-conn-struct-consolidation-plan.md)** -- Consolidate `HttpdConn` struct definitions in `stdlib/httpd.tur`; implemented (2026-06-03)
- **[io-real-filesystem-and-list-dir-uncompilable.md](history/io-real-filesystem-and-list-dir-uncompilable.md)** -- `stdlib/io.tur` nested-`static` + missing `<dirent.h>` defects; resolved
- **[log-capability-vtable-uncompilable.md](history/log-capability-vtable-uncompilable.md)** -- `stdlib/log.tur` and `test/capability.tur` vtable defects (same root cause as io); resolved
- **[sourcefile-uninit-xform-map-fix.md](history/sourcefile-uninit-xform-map-fix.md)** -- `SourceFile` uninitialised `xform_map` crash; done
- **[stale-fn-gensym-snapshots-on-main.md](history/stale-fn-gensym-snapshots-on-main.md)** -- Off-by-one `__fn_*` gensym in two committed snapshots; regenerated
- **[signal-spice-broken-build.md](history/signal-spice-broken-build.md)** -- `tur-signal` spice failed `tur check` due to removed `__arrow_call1` and missing imports; resolved as part of closure-ABI Phase 0a/0b work
- **[stdlib-future-linearity-aliasing.md](history/stdlib-future-linearity-aliasing.md)** -- Refcount the shared `FutureCell` so `Future` can be `:affine`; resolved
- **[fn-typed-return-lowered-to-result-type.md](history/fn-typed-return-lowered-to-result-type.md)** -- `defn` returning `(fn [...] T)` mis-lowered to `T`; producer side resolved
- **[nested-closure-transitive-capture.md](history/nested-closure-transitive-capture.md)** -- Two-level nested closures failing to thread grandparent captures; resolved
- **[borrow-param-forwarding-drop.md](history/borrow-param-forwarding-drop.md)** -- LT1 linear-drop check spuriously firing on `^borrow` param forwarding; resolved
- **[defgadt-malformed-pattern-segfault.md](history/defgadt-malformed-pattern-segfault.md)** -- NULL-deref SEGV on malformed `defgadt` constructor; regression fixture in place
- **[bare-fat-sink-poly-box-slot0-int64-mismatch.md](history/bare-fat-sink-poly-box-slot0-int64-mismatch.md)** -- Poly box / bare `^fat` sink slot-0 int64 shim vs `double` invoke cast; fixed (2026-06-04)
- **[separator-fold-collides-emitted-c-names.md](separator-fold-collides-emitted-c-names.md)** -- every separator folded to `_` so distinct ADT / constructor names shared one emitted C symbol; ADT and constructor names now use the injective mangler and the joiners are structural only; fixed (2026-09-02)
- **[option-niche-graduation-breaks-carrier-some-null.md](option-niche-graduation-breaks-carrier-some-null.md)** -- graduating the Option niche makes a legal carrier `Some(NULL)` a diagnostic and nothing in the release notes said so; the entry is now in `CHANGELOG.md` `[Unreleased]`, announced ahead of the flip; fixed (2026-09-02)
- **[value-struct-payload-sum-monomorph-box-has-no-owner.md](value-struct-payload-sum-monomorph-box-has-no-owner.md)** -- a stdlib Result/Option monomorph over a value-struct payload boxed it and nothing freed it; four rounds of scope-exit / after-consumer drops, the last reaching class-method call sites and return-dispatched producers; the glue route assessed and declined; fixed (2026-09-03)
- **[return-dispatched-sum-mint-in-constrained-instance-miscompiles.md](return-dispatched-sum-mint-in-constrained-instance-miscompiles.md)** -- a class method returning the bare class var whose Option instance mints its element by re-dispatch failed at `cc` three ways (ctor-argument carrier deref of a by-value re-dispatched result, a base clone header disagreeing with its spilled tail, an applied instance head reading a lowered defstruct as a tyvar and going move-only); fixed (2026-09-03)
- **[option-niche-container-elements-box-at-parity.md](option-niche-container-elements-box-at-parity.md)** -- a niche `(Option String)` Vec element boxed at the erased push boundary either way, so the niche's 16->8 never reached containers; CE1/CE2 put the payload word in the slot and the row reads 17.8 MB against 79.7 MB; fixed (2026-09-03)
- **[json-str-result-and-file-readers-missing.md](json-str-result-and-file-readers-missing.md)** -- `#json-str?<T>` landed 2026-08-21 and `#json-file<T>` / `#json-file?<T>` on 2026-09-02, over a `json/decode-file!` that owns its buffer and panics catchably on an unreadable path; fixed (2026-09-02)
- **[minikanren-example-implements-no-minikanren.md](minikanren-example-implements-no-minikanren.md)** -- the example named after miniKanren was a fact table with nested loops and imported nothing from `stdlib/logic.tur`; rewritten as a real relational program (parento, grandparento, appendo in every direction) and the guide section updated; fixed (2026-09-02)
- **[try-turmeric-browser-suites-green-while-failing.md](try-turmeric-browser-suites-green-while-failing.md)** -- the browser job was green with five failing Playwright tests, uploaded its report only when nothing failed, and had no `/ci` rows; the report is always uploaded, the suites feed honest timings rows and a job-summary warning, and the three test-side failures are fixed; fixed (2026-09-02)
- **[turi-suite-accounting-and-reporting-gaps.md](turi-suite-accounting-and-reporting-gaps.md)** -- `run-turi.sh` counted 2615 of 2714 discovered fixtures (marker skips and error-pass skips wrote no result, the two passes honoured different markers, a denylist entry named no fixture, the async scripts ran twice) and none of it reached CI; now a census with an accounting check, `TUR_SKIP_PARTIAL`, and parsed counts in the timings rows; fixed (2026-09-02)
- **[guestbook-example-has-no-import-graph.md](guestbook-example-has-no-import-graph.md)** -- the guestbook example had no import graph, stale syntax in every file and a dependency on an unimplemented continuation surface; rewritten as a spice with one continuation per page, driven end to end by `tests/run-guestbook.sh`; fixed (2026-09-02)
- **[serializable-continuations-aspirational-surface.md](serializable-continuations-aspirational-surface.md)** -- the guides documented `serial-cont->bytes` / `bytes->serial-cont` / `serial-resume`, a `serial-continuation<T>` struct, `ResourceSerializable` and a `(serial-shift [k] ...)` binder that did not exist; the three functions are now real (`bytes->serial-cont` validates and returns a Result) and the guides describe the shipped form; fixed (2026-09-02)
- **[image-dumps-globals-registry-missing.md](image-dumps-globals-registry-missing.md)** -- plan AI3 was unbuilt, so a `def ^mut` written during init silently fell out of the image; `defimage-global` + `image/track-globals!` now serialise declared globals as a second image section restored before resume, and `TUR-W0706` warns when `init` writes an undeclared global; fixed (2026-09-02)
- **[debugger-and-tracer-only-instrument-main.md](debugger-and-tracer-only-instrument-main.md)** -- `tur dap` / `tur trace` armed the debugger only around `(main)`, so a top-level program never stopped and recorded 0 steps; the launch now arms around the load when there is no `main`; fixed (2026-09-02)
- **[fmt-drops-comments-in-handle-and-binding-modifier-gaps.md](fmt-drops-comments-in-handle-and-binding-modifier-gaps.md)** -- `tur fmt` dropped comments in header/arm gaps and split `^mut` binding pairs; gap re-emission in every printer, modifier-aware bindings, trailing-comment and `&mut` sugar fixes; fixed (2026-09-02)
- **[c-sources-propagate-only-one-level.md](c-sources-propagate-only-one-level.md)** -- `:c-sources` propagated one `:spices` level only; now the whole closure with per-path dedup, sharing the `:cmake-deps` resolver; fixed (2026-09-02)
- **[examples-have-no-suite-coverage.md](examples-have-no-suite-coverage.md)** -- examples sweep already checks and runs every example (snake unbaselined); `TUR-W0624` warns on a near-miss `main` with no entry point; resolved (2026-09-02)
- **[perform-inside-loop-has-no-lowering.md](perform-inside-loop-has-no-lowering.md)** -- `perform` reachable from a `while` body evicted the function (tail-position loop, conditional perform join, conditional/repeated loop-carried `set!`, loop followed by statements, extern-c callees coloring callers); `examples/snake` builds; fixed (2026-09-02)
- **[typeclass-constrained-param-erases-adt-to-int64.md](typeclass-constrained-param-erases-adt-to-int64.md)** -- bare `x` in `[^Show a x]` defaulted to `int` instead of the binder's type, so the constrained body dispatched on the carrier fallback (cc failure at a by-value ADT); bare params after a binder now take the binder's tyvar; fixed (2026-09-02)
- **[erased-fn-sink-float-wrapper-carrier-mismatch.md](history/erased-fn-sink-float-wrapper-carrier-mismatch.md)** -- native-float `__poly_N` wrapper / capturing-lambda thunk invoked through the int64 carrier cast by an erased `(fn [a] b)` typeclass-method sink; bridged through bits, fixed (2026-09-02)
- **[poly-wrapper-forces-int64-args-non-int-fat-sink.md](history/poly-wrapper-forces-int64-args-non-int-fat-sink.md)** -- `make_poly_wrapper` forcing int64 arg params on float-class methods; fixed (2026-06-04)
- **[poly-to-fat-drops-args-beyond-first-multiarg-method.md](history/poly-to-fat-drops-args-beyond-first-multiarg-method.md)** -- N-ary `__tur_poly_to_fat*` carriers / shims for multi-arg typeclass methods; fixed
- **[polymorphic-return-type-instantiation-collapses-to-first-tyvar.md](history/polymorphic-return-type-instantiation-collapses-to-first-tyvar.md)** -- Polymorphic accessor return type wrongly collapsed to first tyvar; fixed (2026-06-04)
- **[tuplen-struct-param-passed-by-pointer-codegen-mismatch.md](history/tuplen-struct-param-passed-by-pointer-codegen-mismatch.md)** -- TupleN (N>=3) struct param mis-passed by pointer to by-value callee; fixed (2026-06-04)
- **[typeclass-method-int-carrier-return-truncates-non-int.md](history/typeclass-method-int-carrier-return-truncates-non-int.md)** -- int64-carrier truncating non-int instance results (explicit annotations); fixed (2026-06-04)
- **[closure-first-class-type-plan.md](history/closure-first-class-type-plan.md)** -- First-class closure type (Closure Repr Unification Phase 3, Option B); B-0..B-4 shipped
- **[closure-returning-instance-method-codegen-plan.md](history/closure-returning-instance-method-codegen-plan.md)** -- `definstance` dict-field codegen for closure-returning methods; T1-T6 complete
- **[poly-to-fat-typed-shim-plan.md](history/poly-to-fat-typed-shim-plan.md)** -- Typed `__tur_poly_to_fat1` shim generalisation; capturing-closure path implemented
- **[ptr-generic-parameterised-type-plan.md](history/ptr-generic-parameterised-type-plan.md)** -- Parameterised `:ptr<T>` first-class type; P1-P4 core implemented
- **[sum-types-either-plan.md](history/sum-types-either-plan.md)** -- `Either L R` sum type end-to-end; landed in `d993ba3d`
- **[stdlib-effect-rows-plan.md](history/stdlib-effect-rows-plan.md)** -- Capability effect-row tags on I/O-touching stdlib modules; completed in `cdcf646f`; see [effects-system-guide.md](../guides/effects-system-guide.md)
- **[positional-nominal-type-identity-fix-plan.md](history/positional-nominal-type-identity-fix-plan.md)** -- Type-checker fix to enforce nominal identity on positional struct/opaque/ADT params; landed alongside the partial-application fix
- **[positional-nominal-type-identity-not-checked.md](history/positional-nominal-type-identity-not-checked.md)** -- Bug report behind the above plan; resolved
- **[partial-application-skips-captured-arg-type-check.md](history/partial-application-skips-captured-arg-type-check.md)** -- `elab_partial_apply` capture loop now checks provided arg types; both nominal and kind slices fixed
- **[curried-fn-typed-param-second-application-not-callable.md](history/curried-fn-typed-param-second-application-not-callable.md)** -- Second application of higher-order-returning param now type-checks; fixed (2026-06-04)
- **[arrow-thin-call-segfaults-capturing-closures.md](history/arrow-thin-call-segfaults-capturing-closures.md)** -- `__arrow_call*` thin-call helpers crashing on capturing closures; RESOLVED 2026-06-03 in closure-representation-unification
- **[eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md](history/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md)** -- Constrained-Eq dispatcher passing bare comparator into `^fat` sink; RESOLVED 2026-06-03
- **[fat-fn-param-capturing-closure-gap.md](history/fat-fn-param-capturing-closure-gap.md)** -- Capturing closure unable to reach an `fn`-typed `^fat` parameter; RESOLVED 2026-06-03
- **[ptr-void-direct-call-representation-split.md](history/ptr-void-direct-call-representation-split.md)** -- Direct call of a `:ptr<void>` callback crashed half the time; RESOLVED 2026-06-03
- **[intra-instance-method-dispatch-unsupported.md](history/intra-instance-method-dispatch-unsupported.md)** -- `(.other self ...)` intra-instance dispatch; FIXED 2026-06-04
- **[poly-defn-shares-inner-closure-body-across-monomorphizations.md](history/poly-defn-shares-inner-closure-body-across-monomorphizations.md)** -- Polymorphic defn shared one inner closure body across specialisations; RESOLVED 2026-06-04 (diagnose direction)
- **[instance-method-closure-return-lowered-to-result-type.md](history/instance-method-closure-return-lowered-to-result-type.md)** -- `definstance` closure-returning method mis-lowered to its result type; RESOLVED 2026-06-04
- **[stdlib-linear-handle-borrows.md](history/stdlib-linear-handle-borrows.md)** -- `^borrow` parameter attribute for non-consuming accessors on linear handles; RESOLVED 2026-06-04
- **[defgadt-copy-and-shared-bounds.md](history/defgadt-copy-and-shared-bounds.md)** -- `defgadt :copy` opt-out for shared GADT values; resolved 2026-06-04
- **[range-gadt-typeclass-migration-plan.md](history/range-gadt-typeclass-migration-plan.md)** -- Fold the `Bound` GADT into `range.tur`'s endpoint representation + graduate `-Xgadt` to default-on (A1/B1/B2); complete; `Show`/`Eq`/`Ord [Bound]` instances; see [gadts-guide.md](../guides/gadts-guide.md)

Post-v0.19.0 sweep:

- **[stdlib-arrow-typeclass-reintroduction-plan.md](history/stdlib-arrow-typeclass-reintroduction-plan.md)** -- Re-introduce the Arrow typeclass; delivered 2026-06-05; supersedes `stdlib-arrow-scaleback-plan.md`
- **[stdlib-arrow-scaleback-plan.md](history/stdlib-arrow-scaleback-plan.md)** -- Superseded by the typeclass reintroduction (2026-06-05)
- **[stdlib-inline-c-deworkaround-plan.md](history/stdlib-inline-c-deworkaround-plan.md)** -- Stdlib inline-C de-workaround Phases 1--4; complete 2026-06-04
- **[stdlib-inline-c-tier3-triage.md](history/stdlib-inline-c-tier3-triage.md)** -- Tier-3 inline-C triage classification artifact; complete 2026-06-04
- **[stdlib-linearity-affinity-plan.md](history/stdlib-linearity-affinity-plan.md)** -- Promote remaining stdlib handles to `:affine`; complete 2026-06-04 (Future was the last item)
- **[stdlib-refinement-collections-plan.md](history/stdlib-refinement-collections-plan.md)** -- Refinement collections shipped 2026-06-04
- **[one-off-script-print-and-annotation-ergonomics.md](history/one-off-script-print-and-annotation-ergonomics.md)** -- Script print/annotation ergonomics findings; all FIXED
- **[arrow-compose-float-closure-int64-thunk-mismatch.md](history/arrow-compose-float-closure-int64-thunk-mismatch.md)** -- Arrow-compose float closure int64 thunk mismatch; Direction B Stage A landed 2026-06-05
- **[boxed-fn-typed-closure-return-miscompiles.md](history/boxed-fn-typed-closure-return-miscompiles.md)** -- Boxed fn-typed closure return miscompile; RESOLVED 2026-06-05 (`src/compiler/elab_fns.c`)
- **[poly-closure-inner-dispatch-result-erased.md](history/poly-closure-inner-dispatch-result-erased.md)** -- Poly-closure inner-dispatch result erasure; RESOLVED 2026-06-06 via `poly-closure-result-specialization-plan` Stage E
- **[use-after-move-on-local-let-bound-float-vs-captured.md](history/use-after-move-on-local-let-bound-float-vs-captured.md)** -- Use-after-move on let-bound float; FIXED in `elab_call.c` with regression fixture
- **[dsp-pair-bit-cast-helpers-obsolete.md](history/dsp-pair-bit-cast-helpers-obsolete.md)** -- DSP pair bit-cast helpers obsolete; resolved by the rebuild (`dsp.tur` deleted)
- **[svf-low-pass-removed-no-consumers.md](history/svf-low-pass-removed-no-consumers.md)** -- `svf-low-pass` removed; resolved by the rebuild (no consumers)
- **[linalg-decomp-qr-parser-unterminated-list.md](history/linalg-decomp-qr-parser-unterminated-list.md)** -- Resolved as a source bug in turmeric-spices, not a compiler bug

Post-v0.18.0 followup sweep:

- **[category-arrowzero-implementation-plan.md](history/category-arrowzero-implementation-plan.md)** -- `Category` typeclass + honest Kleisli `ArrowZero`; shipped in #290 under maintainer override (audit recommendation was resolved-by-audit); see [arrows-guide.md](../guides/arrows-guide.md)
- **[vec-typed-fat-closure-readback-fixture-regressed-codegen.md](history/vec-typed-fat-closure-readback-fixture-regressed-codegen.md)** -- `vec-typed-fat-closure-readback` fixture + `^fat`-arg call-slot + ascribed-aggregate-return regressions; RESOLVED (fixed in `9588cda7`, #288)
- **[fn-type-first-class-application-plan.md](history/fn-type-first-class-application-plan.md)** -- First-class `:fn` closure values; F1--F6 landed; suite green
- **[fn-first-class-stdlib-deworkaround-plan.md](history/fn-first-class-stdlib-deworkaround-plan.md)** -- Retire `^fat`-sink shims in parser/backtrack/logic combinators on top of first-class `:fn`; complete
- **[stdlib-session-typed-channels-plan.md](history/stdlib-session-typed-channels-plan.md)** -- `stdlib/schan.tur` session-typed channel wrappers (S1--S3); complete; see [session-types-guide.md](../guides/session-types-guide.md)
- **[stdlib-opaque-handle-types-plan.md](history/stdlib-opaque-handle-types-plan.md)** -- `defopaque` handle types across stdlib (threadpool, future, chan, timer, reactor, taskgroup, mutex/condvar/rwlock, atomic, stm, thread, fiber, process, fs, io, ref); Tier 1--3 complete; see [opaques-guide.md](../guides/opaques-guide.md)
- **[spaced-type-annotation-migration-plan.md](history/spaced-type-annotation-migration-plan.md)** -- Spaced `name : type` annotation migration; Phases 1--5 (codemod + repo + spices + docs) complete; Phase 6 CI enforcement and Phase 7 reader deprecation are deferred follow-ups; see [type-annotations-guide.md](../guides/type-annotations-guide.md)
- **[let-bound-sf-loses-outer-arg-type-when-inner-captures.md](history/let-bound-sf-loses-outer-arg-type-when-inner-captures.md)** -- `let`-bound SF outer-arg type lost when inner closure captures it; type-check half resolved (`tests/check-sf-let-bind-inner-call.sh`); native-codegen half tracked separately in `sf-two-level-closure-return-miscompiles-out-binding`

Post-v0.19.1 sweep:

- **[captureless-closure-lost-through-untyped-vec.md](history/captureless-closure-lost-through-untyped-vec.md)** -- Captureless closures losing fn-pointer identity through untyped `Vec`; FIXED in #307 (box at `TY_TYVAR`-param boundary in `elab_call.c`)
- **[defmodule-export-scoping-track.md](history/defmodule-export-scoping-track.md)** -- Consolidated defmodule export-scoping + project-mode `cons` resolution track; Defects A+B FIXED 2026-06-09/10; F2/F5 (selective `:refer` of math/bits) deferred as non-blocking enhancements
- **[load-inside-defmodule-silently-loses-names.md](history/load-inside-defmodule-silently-loses-names.md)** -- `(load "...")` inside `defmodule`/`defn` bodies silently dropped; RESOLVED 2026-06-10 (Option A: load expansion)
- **[load-not-expanded-in-imported-or-project-modules.md](history/load-not-expanded-in-imported-or-project-modules.md)** -- Top-level `(load ...)` not expanded in imported/project-mode modules; RESOLVED (per-module fixed-runtime emission + import path); covered by `tests/run-build-project.sh`
- **[parametric-struct-by-value-carrier-inconsistency.md](history/parametric-struct-by-value-carrier-inconsistency.md)** -- Generic parametric-struct-by-value carrier inconsistency between `make-struct` and `(.field t)`; RESOLVED 2026-06-10 in `emit_implementation`/`emit_header`
- **[project-mode-defstruct-typedef-missing.md](history/project-mode-defstruct-typedef-missing.md)** -- Project-mode codegen missing `defstruct` typedef in per-module headers; FIXED
- **[project-mode-file-scope-c-block-emit-order.md](history/project-mode-file-scope-c-block-emit-order.md)** -- Project-mode emitted file-scope `` ```c `` blocks after dependent defn bodies; FIXED
- **[project-mode-rc-runtime-preamble-missing.md](history/project-mode-rc-runtime-preamble-missing.md)** -- Project-mode RC/frame runtime preamble + struct drop/walk glue missing; FIXED (T1--T11 landed); per-module fixed-runtime emission is now idempotent
- **[tur-signal-rebuild-plan.md](history/tur-signal-rebuild-plan.md)** -- `tur-signal` spice rebuild on modern typed + fat-closure infra; functionally complete; source-side rebuild landed in `../turmeric-spices/spices/signal/` (2026-06-06)

Post-v0.19.1 followup sweep (2026-06-11):

- **[application-image-dumps-plan.md](history/application-image-dumps-plan.md)** -- Serializable continuations + application image dumps; AI1--AI8 shipped; see [image-dumps-guide.md](../guides/image-dumps-guide.md)
- **[build-tur-sweet-manifest-plan.md](history/build-tur-sweet-manifest-plan.md)** -- `build.tur.sweet` manifest support (SW0--SW8); shipped; documented in CLAUDE.md
- **[fn-type-colons-sweet-exp-plan.md](history/fn-type-colons-sweet-exp-plan.md)** -- `fn`-type colon codemod sweet-exp coverage (S1--S5); LANDED in PR #322
- **[row-type-in-value-position-loses-elements.md](history/row-type-in-value-position-loses-elements.md)** -- `#row{...}` in value-type position now rejected with TUR-E0012 in Layer 4; RESOLVED
- **[sized-types-phantom-index.md](history/sized-types-phantom-index.md)** -- SizedVec size index now real (SZ6--SZ8 + cross-parameter unification); RESOLVED 2026-06-10; see [sized-types-guide.md](../guides/sized-types-guide.md)
- **[spices-v0.18-typing-migration-plan.md](history/spices-v0.18-typing-migration-plan.md)** -- Spices migration to post-v0.17 advanced typing + mangling; substantially complete (P1/P2 classes resolved across the spice tree)
- **[typeclass-associated-types-missing.md](history/typeclass-associated-types-missing.md)** -- Associated type members on typeclasses; minimal milestone landed (single associated type, dictionary-free type-level projection); RESOLVED
- **[variadic-hkt-rows-missing.md](history/variadic-hkt-rows-missing.md)** -- Variadic HKT rows via `^&` row-kinded parameters; IMPLEMENTED (all six layers landed)

Post-v0.19.1 churn sweep (2026-06-11):

- **[bare-fat-result-monomorphization-plan.md](history/bare-fat-result-monomorphization-plan.md)** -- Intra-module non-recursive float register class via per-call-site monomorphization; landed 2026-06-10
- **[fn-type-colons-sweet-exp-instances-plan.md](history/fn-type-colons-sweet-exp-instances-plan.md)** -- `fn`-type colon codemod sweet-exp coverage extended to `definstance`/`defclass`/`defprotocol`; landed in `f8c12dba`
- **[minikanren-2-core-operators-plan.md](history/minikanren-2-core-operators-plan.md)** -- miniKanren-style example project and tutorial guide; shipped in `07380d06`
- **[cross-module-wrapper-macro-vec-arg-elaborated-as-expression.md](history/cross-module-wrapper-macro-vec-arg-elaborated-as-expression.md)** -- Cross-module wrapper macro vec-arg elaborated as expression; RESOLVED via macro-expansion stack fix (`0564e578`)
- **[macro-cannot-emit-inline-c-block.md](history/macro-cannot-emit-inline-c-block.md)** -- Macro emitting CBLOCK-headed list now auto-wrapped with `do`; RESOLVED in `0564e578`
- **[macro-cannot-emit-multiple-top-level-forms.md](history/macro-cannot-emit-multiple-top-level-forms.md)** -- Top-level `(do ...)` splices into program-items; RESOLVED in `5d512dfe`
- **[macro-unquote-in-type-position-rejected.md](history/macro-unquote-in-type-position-rejected.md)** -- `substitute_params` recurses into `F_TYPE_ANN` payload; RESOLVED in `3166316d`
- **[row-polymorphic-defn-call-from-row-polymorphic-context-missing-codegen.md](history/row-polymorphic-defn-call-from-row-polymorphic-context-missing-codegen.md)** -- Row-polymorphic defn call codegen; RESOLVED via `emit_abi_call_is_generic_relay` ignoring rows
- **[serial-shift-unsupported-context-miscompile.md](history/serial-shift-unsupported-context-miscompile.md)** -- TUR-E0706 hard error + prelude-gate broadened; RESOLVED in `0b141834`
- **[stm-or-else-compiled-branches-are-noop-stubs.md](history/stm-or-else-compiled-branches-are-noop-stubs.md)** -- `EX_STM` now emits body inline (TI3+TI4); RESOLVED in `7b8fd8e9`
- **[stm-tvar-cas-swap-modify-compiled-path-broken.md](history/stm-tvar-cas-swap-modify-compiled-path-broken.md)** -- STM functions emitted in `emit_module.c` + `modify` lowered in elaborator; RESOLVED in `7b8fd8e9`
- **[top-level-def-init-dropped.md](history/top-level-def-init-dropped.md)** -- Top-level `def` inits emitted to `__tur_module_def_init` constructor; RESOLVED in `0a904851`
- **[turi-inline-c-ignores-comparison-operator.md](history/turi-inline-c-ignores-comparison-operator.md)** -- `ic_eval_binexpr` precedence climbing for trailing binops; RESOLVED in `7b8fd8e9`
- **[typeclass-constrained-defn-rejected.md](history/typeclass-constrained-defn-rejected.md)** -- Constraint-syntax parsing + null-def check + struct-receiver carrier skip; RESOLVED in `4188ff27`/`1f2101f6`/`a7659144`
- **[cascade-struct-redef-non-identical-blocks.md](history/cascade-struct-redef-non-identical-blocks.md)** -- Per-declaration dedup for cascade struct redefs; RESOLVED in `2cff84ac`
- **[cons-builtin-rejects-cstr-head.md](history/cons-builtin-rejects-cstr-head.md)** -- `cons_wildcard` in `elab_call.c` accepts `:cstr` heads; RESOLVED in `900f9481`
- **[fat-shim-void-ptr-calls-bare-not-fat.md](history/fat-shim-void-ptr-calls-bare-not-fat.md)** -- Fat-shim `:ptr<void>` call dispatch; RESOLVED in `599706b1` (PR #311)
- **[tur-run-alias-breaks-snapshot-ci-guard.md](history/tur-run-alias-breaks-snapshot-ci-guard.md)** -- `tur run` alias support in `justrun.c`; RESOLVED in `54c14891`
- **[unterminated-list-caret-anchors-outermost.md](history/unterminated-list-caret-anchors-outermost.md)** -- Unterminated-list caret anchored at single char; RESOLVED in `fa960cd2`

Post-v0.20.0 sweep (2026-06-12):

- **[ci-nondeterministic-macro-elaboration.md](history/ci-nondeterministic-macro-elaboration.md)** -- Release-only macro-elaboration nondeterminism + ASan leak surfaced on PR #336 CI; three root causes (RC-leak on `__symbol_register`, `LookupKey` UB-init, snapshot drift); all fixed
- **[turi-capturing-shift-unimplemented.md](history/turi-capturing-shift-unimplemented.md)** -- Context-capturing `serial-shift` / `cloneable-shift` implemented via runtime context reification in `src/turi/eval.c`; landed in commit `3eadda5b`
- **[turi-error-fixture-diag-divergences.md](history/turi-error-fixture-diag-divergences.md)** -- All 9 `errors/` fixtures whose `--interpret` diagnostic diverged are now reconciled; `TURI_ERRORS_DENY` is empty
- **[turi-harness-compiles-instead-of-interpreting.md](history/turi-harness-compiles-instead-of-interpreting.md)** -- `tests/run-turi.sh` now runs fixtures with `tur --interpret` (TI8); harness green at 122 passed
- **[turi-inline-c-accessor-miscompiles-boolean-returns.md](history/turi-inline-c-accessor-miscompiles-boolean-returns.md)** -- `ic_exec_accessor` now refuses negated/disjunctive boolean returns rather than silently inverting them (TI8.b/W4)
- **[turi-native-set-count-layout-overflow.md](history/turi-native-set-count-layout-overflow.md)** -- `native_set_*` shims rewritten over `{void* hamt}`; `set.tur` joined the prelude; HAMT-backed `#set{}` literals now safe under `--interpret` (TI8.b/W1b)
- **[turi-select-needs-channel-primitives.md](history/turi-select-needs-channel-primitives.md)** -- `(select ...)` carve-out documented in [eval-api.md](../guides/eval-api.md); real native-channel `EX_SELECT` interpretation tracked in `docs/archive/turi-parity-post-v1-plan.md`

Post-v0.24.x / v0.25.x sweep (2026-06-26):

- **[drop-x-flags-plan.md](history/drop-x-flags-plan.md)** -- All 16 `-X` flags now accept-and-warn no-ops (v0.24.0); see [compiler-flags-guide.md](../guides/compiler-flags-guide.md)
- **[experimental-flag-mechanism-plan.md](history/experimental-flag-mechanism-plan.md)** -- `--enable=<name>` registry (XF0-XF4, XF6); shipped v0.24.3 (#524); see [experimental-flags-guide.md](../guides/experimental-flags-guide.md)
- **[experimental-flag-xf5-stale-premise.md](history/experimental-flag-xf5-stale-premise.md)** -- XF5 deferred with explicit rationale; resolved
- **[stm-fine-grained-locking-plan.md](history/stm-fine-grained-locking-plan.md)** -- TL2 fine-grained locking complete (v0.24.3 #528); see [stm-guide.md](../guides/stm-guide.md)
- **[spices-c-sources-plan.md](history/spices-c-sources-plan.md)** -- `:c-sources` / `:c-includes` manifest keys shipped (v0.24.1 #516); see [developing-spices-guide.md](../guides/developing-spices-guide.md)
- **[throw-deprecation-plan.md](history/throw-deprecation-plan.md)** -- `throw`/`try`/`catch` removed end-to-end (v0.25.0); see [error-handling-guide.md](../guides/error-handling-guide.md)
- **[catch-unwind-drops-captures-segv.md](history/catch-unwind-drops-captures-segv.md)** -- `catch-unwind` capture drop SEGV resolved (v0.24.x)
- **[callcc-escape-through-cont-param-misslowers.md](history/callcc-escape-through-cont-param-misslowers.md)** -- CC4 flavored continuations (v0.25.0 #527)
- **[carrier-crossing-recovery-routing-plan.md](history/carrier-crossing-recovery-routing-plan.md)** -- Carrier-crossing recovery routing; resolved
- **[always-on-linear-session-fixture-failures.md](history/always-on-linear-session-fixture-failures.md)** -- Linear/session fixture failures from always-on graduation (v0.24.1 #518)
- **[b4-fat-closure-byvalue-adt-abi-plan.md](history/b4-fat-closure-byvalue-adt-abi-plan.md)** -- B4 fat-closure by-value ADT ABI graduated (v0.25.2)
- **[byvalue-result-field-access-casts-aggregate-to-pointer.md](history/byvalue-result-field-access-casts-aggregate-to-pointer.md)** -- By-value `Result` field access codegen fix
- **[struct-adt-convergence-s1-bridging-findings.md](history/struct-adt-convergence-s1-bridging-findings.md)** -- CONV-S1 struct/ADT convergence (v0.25.2 #540/#546)
- **[struct-return-through-closure-loses-type-report.md](history/struct-return-through-closure-loses-type-report.md)** -- By-value struct/ADT results through closure ABI (#538); paper trail at `struct-return-through-closure-loses-type.md`
- **[struct-ergonomics-plan.md](history/struct-ergonomics-plan.md)** -- Auto-bound constructor + keyword args + `with` form (v0.25.1 #535)
- **[struct-constructor-currying-plan.md](history/struct-constructor-currying-plan.md)** -- Constructor currying (CURRY-V0/V1/V2 + DOC); shipped
- **[parametric-struct-fn-field-call-passes-concrete-arg-to-carrier-ptr.md](history/parametric-struct-fn-field-call-passes-concrete-arg-to-carrier-ptr.md)** -- Parametric struct fn-field carrier-ptr ABI fix (v0.25.1 #534)
- **[parametric-defstruct-fn-field-gaps.md](history/parametric-defstruct-fn-field-gaps.md)** -- Remaining defstruct fn-field gaps (v0.24.3 #523/#525)
- **[make-struct-parametric-fn-field-inference.md](history/make-struct-parametric-fn-field-inference.md)** -- Type-arg inference from fn-typed fields; resolved
- **[typeclass-associated-types-followups-plan.md](history/typeclass-associated-types-followups-plan.md)** -- Multi-param projection + fundeps (v0.24.3 #522)
- **[dot-method-call-misroutes-to-typeclass.md](history/dot-method-call-misroutes-to-typeclass.md)** -- `(. obj field args)` receiver-first routing (v0.25.1 #533)
- **[used-attr-not-honored-in-single-file-whole-program.md](history/used-attr-not-honored-in-single-file-whole-program.md)** -- `#[used]` honored on single-file builds (v0.25.1 #532)
- **[macro-typed-inline-lambda-arg.md](history/macro-typed-inline-lambda-arg.md)** -- Typed inline lambdas as macro arguments (v0.25.0 #529)
- **[macro-args-elaborated-before-expansion.md](history/macro-args-elaborated-before-expansion.md)** -- Per-parameter `^syntax` marker landed
- **[macro-backquote-dot-sym-drops-siblings.md](history/macro-backquote-dot-sym-drops-siblings.md)** -- No longer reproduces; resolved
- **[list-macro-quote-vs-syntactic-symbol.md](history/list-macro-quote-vs-syntactic-symbol.md)** -- F_QUOTE-wrapped F_SYM in call-head position; resolved
- **[multi-param-struct-annotation-degenerate-tyapp.md](history/multi-param-struct-annotation-degenerate-tyapp.md)** -- Multi-param struct annotation fixed end-to-end
- **[eq-map-typed-consumer-blocked-on-transparent-newtype.md](history/eq-map-typed-consumer-blocked-on-transparent-newtype.md)** -- `Map` now a non-transparent `:heap` struct
- **[eval-mode-unknown-call-deferred-to-runtime.md](history/eval-mode-unknown-call-deferred-to-runtime.md)** -- TUR-W0040 deferred-call diagnostic
- **[ct-primitives-cannot-walk-type-ann-nodes.md](history/ct-primitives-cannot-walk-type-ann-nodes.md)** -- `type-ann-inner` and `type-ann?` shipped
- **[nested-vec-literals-collapse-to-runtime-vec.md](history/nested-vec-literals-collapse-to-runtime-vec.md)** -- Not reproducible; resolved
- **[defgodot-script-macro-vec-quote-semantics.md](history/defgodot-script-macro-vec-quote-semantics.md)** -- Closed / archived
- **[godot-binding-api-surface-expansion-plan.md](history/godot-binding-api-surface-expansion-plan.md)** -- T3.A-T3.E shipped; see [godot-binding-guide.md](../guides/godot-binding-guide.md)
- **[godot-binding-game-ergonomics-plan.md](history/godot-binding-game-ergonomics-plan.md)** -- T4.A-T4.D shipped; see [godot-binding-guide.md](../guides/godot-binding-guide.md)
- **[libturi-embed-include-paths.md](history/libturi-embed-include-paths.md)** -- libturi embed include paths fix
- **[libturi-embed-interpret-mode-flag.md](history/libturi-embed-interpret-mode-flag.md)** -- libturi interpret-mode flag
- **[libturi-per-embed-env-and-peripherals.md](history/libturi-per-embed-env-and-peripherals.md)** -- Per-embed env + peripherals (Gaps 1-8 all landed)
- **[let-bind-passbyptr-struct-param-invalid-initializer.md](history/let-bind-passbyptr-struct-param-invalid-initializer.md)** -- Invalid C initializer for by-pointer struct param (v0.25.2 #542)
- **[turi-value-pool-residual-sites.md](history/turi-value-pool-residual-sites.md)** -- Residual pooling sites tracked
- **[turi-env-owned-value-arena-pool-plan.md](history/turi-env-owned-value-arena-pool-plan.md)** -- Env-owned value arena pool Phase 1 (v0.25.2 #537)
- **[c-num-spelling-uninitialized-on-some-type-paths.md](history/c-num-spelling-uninitialized-on-some-type-paths.md)** -- Bare `Type t;` zero-init fix
- **[untyped-native-registration-blocks-curated-facades.md](history/untyped-native-registration-blocks-curated-facades.md)** -- Typed native registration API landed
- **[tur-build-cmake-deps-workspace-overreach.md](history/tur-build-cmake-deps-workspace-overreach.md)** -- Workspace cmake-dep overreach resolved
- **[project-mode-no-stdlib-autoload.md](history/project-mode-no-stdlib-autoload.md)** -- Project-mode stdlib autoload fix
- **[tourist-session-followups-plan.md](history/tourist-session-followups-plan.md)** -- S0/R0/V0/C0 all landed
- **[thread-pool-followups-plan.md](history/thread-pool-followups-plan.md)** -- TY-V0/WORK-V0/FUT-V0/SCOPE-V0/TRY-V0 all landed
- **[valkey-typed-variadic-cmd-builder-plan.md](history/valkey-typed-variadic-cmd-builder-plan.md)** -- P0+P1+P2 all landed
- **[spices-pr47-watch-notebook-triage.md](history/spices-pr47-watch-notebook-triage.md)** -- Triage concluded
- **[mise-plugin-plan.md](history/mise-plugin-plan.md)** -- Mise plugin shipped (v0.25.2); see [mise-asdf-guide.md](../guides/mise-asdf-guide.md)
- **[end-to-end-monomorphization-plan.md](history/end-to-end-monomorphization-plan.md)** -- Track A complete (archived 2026-06-19); successor track at `end-to-end-monomorphization-plan-2.md`

Post-v0.32.6 sweep (2026-07-31):

Sixty-five reports and plans resolved between 2026-07-26 and 2026-07-31. Nine
same-vintage reports stayed in [../archive/](.) because each still carries a
named open finding -- see "Held back from the v0.32.6 sweep" below.

*Representation consolidation (fn values, carriers, container elements).* The
campaign's live scoreboard is the missing-cells table in
[value-representations-guide.md](../guides/value-representations-guide.md):

- **[fn-typed-value-return-ascribe-miscompiles.md](history/fn-typed-value-return-ascribe-miscompiles.md)** -- fn-value-as-data matrix; fat-normalization stage 2
- **[fn-value-carrier-fat-seam-residuals.md](history/fn-value-carrier-fat-seam-residuals.md)** -- let-alias and `if`-join seams after stage 2
- **[fn-payload-in-container-undeclared-temp.md](history/fn-payload-in-container-undeclared-temp.md)** -- fn read out of a container payload, then called
- **[fn-element-tyvars-not-substituted-in-spec-types.md](history/fn-element-tyvars-not-substituted-in-spec-types.md)** -- `TY_FN` arm missing from the spec-type instantiator
- **[vec-byvalue-struct-element-invalid-c.md](history/vec-byvalue-struct-element-invalid-c.md)** -- width-independent container element protocol (increment 3)
- **[class-method-result-into-generic-invalid-c.md](history/class-method-result-into-generic-invalid-c.md)** -- method result into a generic call argument (increment 2)
- **[result-monad-bind-typed-boundary-miscompiles.md](history/result-monad-bind-typed-boundary-miscompiles.md)** -- `do-m` over `Result` across a typed boundary (increment 2)
- **[concrete-codegen-layout-kind-enumerations-drift.md](history/concrete-codegen-layout-kind-enumerations-drift.md)** -- three `TypeKind` switches must agree; now `default`-less + two CI guards
- **[typed-slots-nested-specialization-float-garbage.md](history/typed-slots-nested-specialization-float-garbage.md)**, **[typed-result-map-cps-clone-struct-assign.md](history/typed-result-map-cps-clone-struct-assign.md)** -- two nested-fixture miscompiles the compiling harnesses never saw

*Higher-kinded and constrained-polymorphic dispatch:*

- **[constrained-hkt-lifted-lambda-keeps-representative-instance.md](history/constrained-hkt-lifted-lambda-keeps-representative-instance.md)** -- Route B dict-passing; a lifted continuation no longer keeps the representative instance
- **[constrained-hkt-abstract-var-requires-last-param-free.md](history/constrained-hkt-abstract-var-requires-last-param-free.md)** -- the partial-application hole moved into the `Type`; `(Result _ cstr)` now abstracts
- **[constrained-hkt-byvalue-carriers.md](history/constrained-hkt-byvalue-carriers.md)** -- by-value carrier ABI for constrained kind-polymorphic fns
- **[hkt-inline-c-instance-body-loses-result-type.md](history/hkt-inline-c-instance-body-loses-result-type.md)** -- one authority for "inline-C returns by value?"; the hkt-guide limitation is gone
- **[hkt-rc-construct-body-boxes-handle.md](history/hkt-rc-construct-body-boxes-handle.md)** -- stop heap-boxing a carrier-width instance result

*Refinement types and the solver:*

- **[macro-generated-refined-crossings-do-not-discharge.md](history/macro-generated-refined-crossings-do-not-discharge.md)** -- crossing collection walks *into* macro expansions
- **[frozen-macro-breaks-refinement-guard-discharge.md](history/frozen-macro-breaks-refinement-guard-discharge.md)** -- Span+head+arity crossing identity survives macro copying
- **[refine-callsite-path-conds-lost-multi-form-body.md](history/refine-callsite-path-conds-lost-multi-form-body.md)** -- call-site path conditions span the caller's whole body
- **[refine-float-measure-missort.md](history/refine-float-measure-missort.md)** -- float measures were Int-sorted, producing a false proof (soundness)
- **[reads-nonstrict-silent-trust.md](history/reads-nonstrict-silent-trust.md)** -- an unproven `#reads` crossing no longer passes silently; TUR-W0372 text corrected
- **[refined-multi-compile-memory-corruption.md](history/refined-multi-compile-memory-corruption.md)** -- uninitialized arena-allocated memo field, not a use-after-free
- **[refined-obligations-silently-pass-in-release.md](history/refined-obligations-silently-pass-in-release.md)** -- **retracted**; NDEBUG contract stripping is deliberate, `--keep-contracts` opts back in
- **[refine-fuzzer-subprocess-stdlib-double-load.md](history/refine-fuzzer-subprocess-stdlib-double-load.md)** -- `load_path_key` canonicalized before resolving `stdlib_dir`
- **[arena-debug-poisoning-plan.md](history/arena-debug-poisoning-plan.md)** -- `TUR_DEBUG_ARENA_POISON` / `TUR_DEBUG_ARENA_GUARD` (AP1/AP2/AP4)
- **[corpus-reader-tail-plan.md](history/corpus-reader-tail-plan.md)**, **[corpus-child-crashes-silent-under-asan.md](history/corpus-child-crashes-silent-under-asan.md)** -- last 7 SMT-LIB corpus skips closed; ASan-killed children no longer tally as passes

*Reference counting, GC, and ownership:*

- **[gc-strong-cycles-not-collected.md](history/gc-strong-cycles-not-collected.md)** -- CG0-CG2 + CG4; live strong `rc<T>` cycles are reclaimed
- **[collections-cannot-hold-rc-values.md](history/collections-cannot-hold-rc-values.md)** -- `stdlib/rcvec.tur`, the flat-buffer-but-traced container; see [gc-guide.md](../guides/gc-guide.md)
- **[stdlib-weak-ref-audit-plan.md](history/stdlib-weak-ref-audit-plan.md)** -- WR0-WR4; `stdlib/weak.tur` + [ownership-guide.md](../guides/ownership-guide.md)
- **[two-collectors-dlopen-boundary.md](history/two-collectors-dlopen-boundary.md)** -- two GC copies in one process are safe, for a better reason than assumed
- **[set-bang-does-not-release-old-rc-value.md](history/set-bang-does-not-release-old-rc-value.md)** -- `set!` releases the rc value it overwrites
- **[rc-scalar-default-glue-invalid-free.md](history/rc-scalar-default-glue-invalid-free.md)** -- scalar payloads default to no-op glue; use `rc_set_value`
- **[rc-free-queue-drain-is-quadratic.md](history/rc-free-queue-drain-is-quadratic.md)** -- linear drain; a free must not re-enter the freer
- **[rc-tur-legacy-instances-do-not-compile.md](history/rc-tur-legacy-instances-do-not-compile.md)** -- `stdlib/rc.tur` compiles again
- **[closure-capture-escapes-linearity.md](history/closure-capture-escapes-linearity.md)** -- a closure that *consumes* a captured linear/unique value is itself linear/unique
- **[gc-heap-struct-rc-nonzero-on-darwin.md](history/gc-heap-struct-rc-nonzero-on-darwin.md)**, **[gc-leak-gate-darwin-sanitized-probe-drift.md](history/gc-leak-gate-darwin-sanitized-probe-drift.md)** -- Darwin zone-probe noise; a malloc probe means nothing under ASan

*JIT / MIR (`tur jit`)* -- permanent constraints extracted to [jit-guide.md](../guides/jit-guide.md):

- **[jit-macos-apple-sdk-headers-force-cc-fallback.md](history/jit-macos-apple-sdk-headers-force-cc-fallback.md)** -- seven Apple SDK blockers; corpus fallbacks 31 -> 17
- **[jit-macos-full-corpus-extension-and-atexit.md](history/jit-macos-full-corpus-extension-and-atexit.md)** -- `__extension__`, `atexit`, and the packed-struct split-out
- **[jit-arm64-uint128-align-struct-layout-skew.md](history/jit-arm64-uint128-align-struct-layout-skew.md)** -- `__uint128_t` alignment skewed `ucontext_t`; fixed in the MIR fork
- **[mir-two-word-struct-return-goto-loop-miscompile.md](history/mir-two-word-struct-return-goto-loop-miscompile.md)** -- `make_one_ret` aliasing both slots across a goto backedge
- **[jit-tur-apply-casts-to-aggregate-param-type.md](history/jit-tur-apply-casts-to-aggregate-param-type.md)** -- `TUR_APPLY<N>_T` casting to an aggregate type is not legal C
- **[hoisted-inline-c-precedes-includes.md](history/hoisted-inline-c-precedes-includes.md)** -- hoisted `#include`s now precede all hoisted code
- **[httpd-new-pool-fail-drops-handler-fails-under-jit.md](history/httpd-new-pool-fail-drops-handler-fails-under-jit.md)** -- harness env drift (`TUR_BIND_LOOPBACK`), not a JIT defect
- **[libturi-symbols-basename-collision.md](history/libturi-symbols-basename-collision.md)** -- `runtime/symbols.c` joins `TUR_CORE_SOURCES`; title disavowed
- **[named-let-self-tail-not-tco.md](history/named-let-self-tail-not-tco.md)**, **[cps-colored-noncapture-named-let-recurses-through-entry.md](history/cps-colored-noncapture-named-let-recurses-through-entry.md)** -- named-let self-TCO, both the capturing and non-capturing forms

*Interpreter and REPL:*

- **[turi-repl-quadratic-reparse.md](history/turi-repl-quadratic-reparse.md)** -- O(N^2) reparse on a long-lived env; fixed by TR2 incremental elaboration
- **[tvar-cell-dangled-across-promotion-rewind.md](history/tvar-cell-dangled-across-promotion-rewind.md)** -- `EX_TVAR_NEW` cell dangling across a scratch-promotion rewind
- **[web-repl-lang-switch-drops-stdlib.md](history/web-repl-lang-switch-drops-stdlib.md)** -- a `#lang` switch resets the session but the pinned prelude survives
- **[wasm32-promo-hash-shift-ub.md](history/wasm32-promo-hash-shift-ub.md)** -- 64-bit finalizer on a 32-bit `uintptr_t` in the shipped web bundle

*Language surface and tooling:*

- **[composite-type-alias-gap.md](history/composite-type-alias-gap.md)** -- `defalias` accepts any type expression (Phase TA2); see [syntax-guide.md](../guides/syntax-guide.md)
- **[row-ops-drop-field-names.md](history/row-ops-drop-field-names.md)** -- labels survive the row algebra; see [row-types-guide.md](../guides/row-types-guide.md)
- **[c-keyword-function-names-not-mangled.md](history/c-keyword-function-names-not-mangled.md)** -- the `tur_u_` guard prefix; see [name-mangling-guide.md](../guides/name-mangling-guide.md)
- **[defn-shadows-return-special-form.md](history/defn-shadows-return-special-form.md)** -- TUR-W0042 at the definition; reserved-names rule in [syntax-guide.md](../guides/syntax-guide.md)
- **[named-effectful-defn-as-fat-fn-value-ices.md](history/named-effectful-defn-as-fat-fn-value-ices.md)** -- taking an effectful fn's address is not performing its effect
- **[effectful-fn-typed-param-call-segfaults.md](history/effectful-fn-typed-param-call-segfaults.md)** -- `^fat` param with a non-empty effect row
- **[persistent-map-cstr-keys-identity-compared.md](history/persistent-map-cstr-keys-identity-compared.md)** -- `^persistent` maps compare `:cstr` keys by content
- **[no-compiler-version-constraint-in-manifest.md](history/no-compiler-version-constraint-in-manifest.md)** -- `:tur-version`; see [developing-spices-guide.md](../guides/developing-spices-guide.md)
- **[spice-cycle-include-path-blowup.md](history/spice-cycle-include-path-blowup.md)** -- a mutual `:spices` cycle is supported, terminated by a visited set
- **[lsp-symbol-retention-never-primes.md](history/lsp-symbol-retention-never-primes.md)** -- stdlib-only completion fallback for a never-parsed buffer
- **[fmt-bootstrap-stdlib-rcvec-not-self-formatted.md](history/fmt-bootstrap-stdlib-rcvec-not-self-formatted.md)** -- `tur fmt` learned type-parameter vectors on `defn`
- **[fmt-idempotence-head-z-silently-skips-on-bsd.md](history/fmt-idempotence-head-z-silently-skips-on-bsd.md)**, **[ctest-parallel-contention-false-failures.md](history/ctest-parallel-contention-false-failures.md)** -- two harness checks that passed without testing anything
- **[wasm-artifact-requires-second-deploy-commit.md](history/wasm-artifact-requires-second-deploy-commit.md)** -- `web/public/turmeric.{js,wasm}` untracked; `sw.js` `CACHE_VERSION` auto-bumped

### Resolved since the v0.32.6 sweep

Closed reports that arrived after the sweep, still in [../archive/](.) rather
than `history/` because they have not been swept yet:

- **[sweet-dollar-double-applies-single-call.md](sweet-dollar-double-applies-single-call.md)** -- sweet-exp `$` no longer wraps a rest-of-line that is already one complete expression, so it composes with neoteric / curly-infix / data literals; see [syntax-guide.md](../guides/syntax-guide.md)
- **[ci-cps-tramp-turi-timeouts-under-load.md](ci-cps-tramp-turi-timeouts-under-load.md)** -- the turi flake was ~3.5 GiB RSS per fixture, not CPU contention; full depth moved to compiled-only siblings, and both harnesses now report a timeout as a timeout instead of a "stdout mismatch"

### Held back from the v0.32.6 sweep

Same vintage, still in [../archive/](.) because each carries a named open
finding rather than a closed one:

- **[defalias-plan.md](defalias-plan.md)** -- TA1/TA2 shipped, but export semantics (an alias does not cross `(import ...)`) and gendocs rendering are unanswered
- **[emitted-taskgroupblock-layout-mismatch.md](emitted-taskgroupblock-layout-mismatch.md)** -- narrow fix landed; the structural one is plan item S2, and the macOS repro was never re-confirmed
- **[jit-c2mir-ignores-pragma-pack.md](jit-c2mir-ignores-pragma-pack.md)** -- `#pragma pack` fixed; `__attribute__((packed))` is still silently ignored
- **[jit-xxh64-missing-prototype.md](jit-xxh64-missing-prototype.md)** -- crash fixed; dropping `-Wno-error=implicit-function-declaration` is a scheduled breaking change
- **[jit-reactor-fixtures-abort-under-mir.md](jit-reactor-fixtures-abort-under-mir.md)** -- six weak `tur_scheduler_*_st` *functions* carry the same hazard and cannot be value-copied
- **[map-show-keyword-key-raw-int.md](map-show-keyword-key-raw-int.md)** -- `Show [Sym]` shipped; six `TypeKind` arms remain unaudited
- **[hkt-fmap-result-is-not-droppable.md](hkt-fmap-result-is-not-droppable.md)** -- "Still open" list needs reconciling against its two resolved siblings
- **[spice-guides-bare-brace-manifest-syntax.md](spice-guides-bare-brace-manifest-syntax.md)** -- guides corrected; spice-README coverage and `tur add-cmake` comment-dropping are not
- **[turi-interp-incremental-reclamation-plan.md](turi-interp-incremental-reclamation-plan.md)** -- TR1 carrier relocation shelved as demand-driven

### Archived 2026-09-16

- **[playground-session-hygiene-plan.md](playground-session-hygiene-plan.md)** -- Try Turmeric's eval session: PS1 rebuilds a lost elaboration session by replaying committed turns, PS2/PS3 read the docstring table from C (doc panel, `tur doc`, `(doc ...)`), PS4 lets a later turn redefine any def* form, PS5 makes Run rewind to the stdlib and replay the other tabs. Executed 2026-09-16; the record at its end covers the PS3 route change, the measured costs, and a `defmacro*` abort fixed on the way
- **[doc-lookup-poisons-the-playground-eval-session.md](doc-lookup-poisons-the-playground-eval-session.md)** -- one doc-panel lookup broke Run for the rest of the session and blamed the user's `main` on an auto-loaded stdlib module; resolved by the plan above, which also corrects two of its claims

### Archived 2026-09-09

Each declares every phase closed, so it is a record rather than open work.
Where a plan leaves a named thread, the banner at its top says where that
thread now lives.

- **[mutable-globals-plan.md](mutable-globals-plan.md)** -- `^mut` global state made visible to write frames, effect rows and module boundaries; G1-G5b landed, `global-state` graduated in 0.35.0 and `write-frames` in 0.37.0. The read-side follow-up is [trusted-refinement-claims-plan.md](trusted-refinement-claims-plan.md) (R1-R4 landed; archived 2026-09-28)
- **[offline-docs-plan.md](offline-docs-plan.md)** -- one docs pack serving the site, the in-app Try Turmeric viewer and offline mode; OD1-OD5 landed 2026-08-26. See [offline-docs-guide.md](../guides/offline-docs-guide.md)
- **[tur-run-feature-audit-plan.md](tur-run-feature-audit-plan.md)** -- `tur run` parity with just 1.54.0 (attributes, `--set`, groups, platform attributes, modules, backticks); phases 0-3 executed 2026-08-30. The Windows shebang path stays with the Windows port
- **[sum-representation-plan.md](sum-representation-plan.md)** -- multi-variant ADTs by value, Option/Result as real sums, the Option niche; SR0-SR4 all closed (SR2c graduated 2026-08-27, the niche 2026-09-03, SR4 flipped 2026-09-02). Gate records: [sr1-gate-results.md](sr1-gate-results.md), [sr2-gate-results.md](sr2-gate-results.md)
- **[sr1-gate-results.md](sr1-gate-results.md)** -- the SR1 prototype gate (2026-08-26): eight crossings, one latent bug, one scope decision; SR1 shipped from this worklist
- **[sr2-gate-results.md](sr2-gate-results.md)** -- the SR2 prototype gate (2026-08-27): the plan's prerequisite line corrected, an eleven-fixture worklist worked to zero, SR2a paid
- **[reclamation-plan.md](reclamation-plan.md)** -- freeing what the emitted program allocates; RM0 ran, RM1 built and narrowed, RM2/RM3 recorded as not starting (regions shipped separately), RM4 decided the SR4 default. The spine residue is attributed under `docs/artifacts/`
- **[container-element-form-plan.md](container-element-form-plan.md)** -- per-monomorph element words in Vec for niche Options; CE0-CE3 built and default since the niche graduated, CE5 re-measured, CE4 (Map/Set/HAMT) deferred by evidence
- **[macro-system-direction-plan.md](macro-system-direction-plan.md)** -- procedural macros on turi, not phases; Stages 0-3 and Stage 4's RM5 and R3 reflection landed, the JIT fast path shelved pending demand. The CT-language freeze policy lives here
- **[ecs-component-set-bounds-plan.md](ecs-component-set-bounds-plan.md)** -- the structural `(has ...)` world bound; resolved negatively 2026-08-20 when the ECS profile it gated on measured dictionary dispatch inside noise. `defworld-classes` took the ergonomics half spice-side
- **[suite-timing-trends-plan.md](suite-timing-trends-plan.md)** -- per-suite CI wall-clock trends on the `ci-metrics` branch and the `/ci` dashboard; Phases 0-5 landed (dashboard 2026-08-28), Phase 6 declined by the two-week readout (`tur_tests` flat at -0.5%). The readout also found ctest truncating the census line of the two largest suites (fixed in `ci.yml`) and four suites that never run in full in CI ([../reported/ci-suites-that-never-run-on-hosted-runners.md](../reported/ci-suites-that-never-run-on-hosted-runners.md))

### Archived 2026-09-05

- **[regions-plan.md](regions-plan.md)** -- RM3 declared regions over the arena; R1-R5 landed, `with-region` added, GRADUATED on by default 2026-09-05 (`TUR_REGIONS=0` is the hatch). The static escape walk's admitted result shapes are recorded under graduation item 2; the spine residue stays with [reclamation-plan.md](reclamation-plan.md)
- **[solver-extension-plan.md](solver-extension-plan.md)** -- the refinement solver as an incremental, backtracking decision procedure; SX0/SX1/SX2/SX3/SX8/SX9 landed (trail, DFS driver, incremental EUF, `tur smt`, `--dump-refine=json` at schema 1), SX4/SX6 parked on measured-shut gates. Its integer-tail follow-up is [solver-integer-tail-plan.md](solver-integer-tail-plan.md) (Phases 1, 3(b), 4 landed; 2 and 3(a) parked; archived 2026-09-28)

### Completed plans swept out of `docs/upcoming/` (2026-08-15)

Each of these declares itself executed/landed with nothing outstanding, so it
is a record rather than open work. `docs/upcoming/` holds only unfinished
plans; a plan whose experiment has not graduated yet stays there, because the
graduation is still ahead of it.

- **[def-define-consolidation-plan.md](def-define-consolidation-plan.md)** -- one `def` form for top-level and body positions; EXECUTED 2026-08-05 (D1-D4)
- **[lsp-client-gaps-plan.md](lsp-client-gaps-plan.md)** -- `tur lsp` gaps found by writing a real client (Trowel); everything scheduled in its SS6 has landed
- **[try-turmeric-lsp-plan.md](try-turmeric-lsp-plan.md)** -- LSP-backed editor intelligence in the web playground; L0-L4 landed, and SS7 closed the `emcc` gap SS6.4 could not
- **[refined-graduation-plan.md](refined-graduation-plan.md)** -- graduating the `refined` experiment; EXECUTED 2026-08-01, shipped in v0.33.0
- **[refined-dogfood-ecs-report.md](refined-dogfood-ecs-report.md)** -- the tur-ecs cost measurement (1.004x) behind that graduation's precondition 2
- **[refine-predicate-measures-plan.md](refine-predicate-measures-plan.md)** -- boolean-sorted measures in refinement predicates; RM-B0..RM-B3 landed
- **[refine-stateful-measures-plan.md](refine-stateful-measures-plan.md)** -- refinements over mutable state; LANDED as Candidate B (`#reads` + the borrow-based `frozen` region); RM-S1 not pursued
- **[turi-incremental-elaboration-design.md](turi-incremental-elaboration-design.md)** -- incremental parse + elaboration for the turi REPL; SHIPPED and on by default 2026-07-25

Two more found in the same sweep, both of which read as open only because a
banner was never updated after the work landed. Each banner is corrected here:

- **[mir-interp-tier-plan.md](mir-interp-tier-plan.md)** -- a tier-0 `MIR_interp` engine. CLOSED: I0 retracted the premise and measured I1-I3 as not worth building; I4 (engine reachable from a plain `libturi` embedder) LANDED 2026-07-31. The header still said "I4 still recommended" after I4 shipped
- **[refinement-types-plan.md](refinement-types-plan.md)** -- RT0-RT7 plus the in-house solver S0-S4, all landed; `refined` graduated in v0.33.0. Still the **status source** for the feature and the `tests/fixtures/refine-*` corpus, so it is archived as a record rather than retired. Its sibling-plan block also claimed the RM-S A-vs-B decision was open; it was decided (Candidate B) on 2026-07-26

Earlier additions:

- **[frame-spice-plan.md](history/frame-spice-plan.md)** -- `tur-frame` dataframe spice; FR0-FR10 complete; see [frame-guide.md](../guides/frame-guide.md)
- **[module-docstrings-plan.md](history/module-docstrings-plan.md)** -- Module-level `;;;` docstring rendering; implemented and documented in CLAUDE.md
- **[signal-processing-arrows-plan.md](history/signal-processing-arrows-plan.md)** -- Arrow-based DSP tutorial; extracted to `tur-signal` v0.1.0; see [arrows-guide.md](../guides/arrows-guide.md)
- **[spice-aware-check-plan.md](history/spice-aware-check-plan.md)** -- Per-file `tur check` auto-discovers `build.tur`; implemented and documented in CLAUDE.md
- **[test-perf-plan.md](history/test-perf-plan.md)** -- Stamp caching + ccache; T1-A/B/C/D and T2-A/C complete

## Design rationale docs

Design-decision rationale and FAQs live in [../design/](../design/):

- **[typed-slots-gs5-representation-rationale.md](../design/typed-slots-gs5-representation-rationale.md)** -- Why GS5 does not require tagged unions
