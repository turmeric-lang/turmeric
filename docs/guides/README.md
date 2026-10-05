---
title: Guides Index
category: Getting Started
description: Index and overview of Turmeric user-facing documentation, guides, and tutorials
---

# Turmeric Guides

User-facing documentation for Turmeric features, tutorials, and best practices.

Code examples are available in both standard S-expression syntax (`turmeric`) and
sweet-expression syntax (`sweet-exp`). Use the toggle above each paired example to
switch between them.

## Authoring paired examples

Write two consecutive fenced blocks -- a `turmeric` block immediately followed (no
prose between) by a `sweet-exp` block. `genguides.py` detects the pair and renders
a toggle widget automatically. A lone `turmeric` block with no `sweet-exp` sibling
renders as a plain code block; all existing blocks remain valid.

The second (sweet-exp) variant always opens with `#lang sweet-exp` when the snippet
is a complete runnable program; inline snippets omit the directive. Do not use the
invalid `#lang turmeric/sweet-exp` or mix `#lang turmeric/neoteric` into sweet-exp
labelled examples -- the correct directive is `#lang sweet-exp` in every case.

Example:

````markdown
```turmeric
(defn use-ask [] : int
  (+ 1 (perform (Ask))))
```
```sweet-exp
defn use-ask [] :int
  {1 + perform(Ask())}
```
````

Note: all guide content must be ASCII-only. Use `--` (double hyphen), never em dashes.

## Coverage

Every `turmeric` + `sweet-exp` toggle pair is **machine-verified**: the two sides
must read to the same AST. `tools/check-guide-pairs.py` extracts each pair and
runs `tur parse-check <turmeric> <sweet-exp>` (exit 0 = same AST), so a drifted
sweet-exp block fails the check.

Run `just check-guides` (or `python3 tools/check-guide-pairs.py docs/guides/`) to
verify all pairs and see current coverage. A pair that is genuinely illustrative
pseudo-code can opt out by marking its opening fence ```` ```turmeric no-check ````.

## Getting Started

- **[introducing-saffron.md](introducing-saffron.md)** -- A first tour of Saffron, the dynamically typed dialect: Try Turmeric in the browser, printing, flow control, functions, ADTs, typeclasses, and effects, every example in both syntaxes
- **[quickstart.md](quickstart.md)** -- Prose introduction: expressions, functions, control flow, Option, Result, collections, closures, structs, and algebraic effects
- **[r7rs-guide.md](r7rs-guide.md)** -- R7RS-small Scheme as `#lang r7rs`: getting started, the libraries, ports and `read`, importing Turmeric modules, the tooling, and where it differs from the standard
- **[sicp-guide.md](sicp-guide.md)** -- Working through SICP in `#lang r7rs`: installing, the interpreter and REPL, the definitions the book assumes, chapter-by-chapter notes, and the known rough edges
- **[saffron-guide.md](saffron-guide.md)** -- The Saffron dialect reference: what `any` as the default type changes, the dynamic operations, the checked boundary with typed Turmeric, and the one feature it gives up
- **[syntax-guide.md](syntax-guide.md)** -- How to read and write Turmeric -- s-expression and sweet-expression syntax
- **[releases-and-installation-guide.md](releases-and-installation-guide.md)** -- Install Turmeric from a prebuilt release or Homebrew, what's in the tarball, and how a maintainer cuts a new release
- **[repl.md](repl.md)** -- Reference for the `tur repl` interactive read-eval-print loop -- startup, evaluation, meta-commands, configuration
- **[repl-tutorial.md](repl-tutorial.md)** -- 22-step interactive tutorial to follow at `tur repl` or the web REPL

## Language Basics

- **[binding-forms-guide.md](binding-forms-guide.md)** -- Body-level `def`, `letrec`, and named-let -- the three local binding idioms that complement `let` and `defn`
- **[data-literals-guide.md](data-literals-guide.md)** -- Compact literal syntax for maps, vecs, and sets using `#map{...}`, `#set{...}`, and `[...]`
- **[macros-guide.md](macros-guide.md)** -- `defmacro`: quasiquote/unquote/splice, manual hygiene with `gensym`, `^syntax` parameters, recursion over arguments, type interpolation in templates, and the compile-time evaluator's deliberate limits
- **[mutable-globals-guide.md](mutable-globals-guide.md)** -- `def ^mut`, what the compiler checks about a global write, and the concurrency story -- including what it deliberately does not cover
- **[numeric-tower-guide.md](numeric-tower-guide.md)** -- Exact `Rational` and hand-written `Complex` arithmetic, the `#rat{...}` / `#cx{...}` literals, and `Num`-typeclass operator overloading
- **[strings-guide.md](strings-guide.md)** -- The `cstr` vs `str` vs `String` tiering -- which string type to reach for, and when an owned `String` must replace a borrowed `cstr`
- **[module-system-guide.md](module-system-guide.md)** -- Module system, namespacing, and exports
- **[structs-guide.md](structs-guide.md)** -- Defining and using struct types with `defstruct`
- **[sum-types-guide.md](sum-types-guide.md)** -- Declaring sum types with `defdata`, pattern matching, exhaustiveness, the `^non-exhaustive` opt-out, the FFI layout, and the stdlib `Either` module
- **[function-arity-guide.md](function-arity-guide.md)** -- When to use positional parameters, defstruct options, and variadic rest parameters
- **[currying-guide.md](currying-guide.md)** -- Haskell-style partial application and over-application of Turmeric functions
- **[symbols-guide.md](symbols-guide.md)** -- First-class runtime symbols (`:Sym` type) with compile-time interning and optional dynamic interning
- **[reader-forms-guide.md](reader-forms-guide.md)** -- Complete reference for every syntactic form the Turmeric reader recognises
- **[fat-closure-annotation-guide.md](fat-closure-annotation-guide.md)** -- When and why to mark function-typed parameters and return positions `^fat`, and what breaks without it
- **[cli-args-guide.md](cli-args-guide.md)** -- Passing arguments to scripts with `*args*` and parsing them with `stdlib/args.tur`
- **[range-reader-plan.md](range-reader-plan.md)** -- Plan for `#r{...}` reader-level range shorthand (RR0-RR4) -- desugars to Range constructor calls at read time
- **[typing-handles-callbacks-results-guide.md](typing-handles-callbacks-results-guide.md)** -- How to type opaque handles, callbacks, and option/result values properly instead of using `:int` stand-ins

## Type System

- **[type-annotations-guide.md](type-annotations-guide.md)** -- Compound type annotation syntax: `(-> a b)`, `(vec T)`, `forall`, and more
- **[polymorphism-guide.md](polymorphism-guide.md)** -- A tour of the polymorphism mechanisms Turmeric provides -- parametric, ad-hoc, structural, row, kind, rank, and substructural
- **[typeclass-guide.md](typeclass-guide.md)** -- Defining typeclasses with `defclass`, implementing instances with `definstance`, constraints, associated types, functional dependencies, and default implementations
- **[hkt-guide.md](hkt-guide.md)** -- Higher-kinded types (functor, monad, applicative abstractions, performance/dispatch model)
- **[hrt-guide.md](hrt-guide.md)** -- Higher-ranked types: rank-2/3 polymorphic function parameters
- **[row-types-guide.md](row-types-guide.md)** -- Type-level rows: the `#row{...}` reader form, row-kinded (`^&`) parameters, the row algebra, erasure, and the limits
- **[union-intersection-types-guide.md](union-intersection-types-guide.md)** -- Union (`A | B`) and intersection (`A & B`) types, `any`, gradual typing
- **[sized-types-guide.md](sized-types-guide.md)** -- Tracking data-structure sizes in the type system for memory layout, stack allocation, and type-safe array operations
- **[sized-primitives-guide.md](sized-primitives-guide.md)** -- Fixed-width numeric types (`int8/i8` through `uint64/u64`, `float32/f32`) -- literal syntax, coercion rules, and casting

## Advanced Types

- **[gadts-guide.md](gadts-guide.md)** -- GADTs: `defgadt`, type refinement, equality witnesses, union types, gradual typing
- **[gadts-cookbook.md](gadts-cookbook.md)** -- GADTs cookbook: practical patterns and recipes
- **[existential-types-guide.md](existential-types-guide.md)** -- Existential types: pack/open, typeclass constraints, hiding concrete types behind opaque boundaries
- **[opaques-guide.md](opaques-guide.md)** -- Named nominal newtypes over a representation type -- what they are, what they're for, and how to construct, unwrap, and combine them
- **[advanced-type-system-rationale.md](advanced-type-system-rationale.md)** -- Why Turmeric chose the type system features it did, why dependent types remain deferred, and how refinement types went from deferred to shipped

## Type Safety

- **[substructural-types-guide.md](substructural-types-guide.md)** -- `^linear`, `^affine`, `^relevant` type disciplines
- **[uniqueness-types-guide.md](uniqueness-types-guide.md)** -- `^unique`: at-most-one-reference ownership

## Error Handling

- **[error-handling-guide.md](error-handling-guide.md)** -- `Result`, `Option`, `panic`, contract macros (`assert!`, `require!`, `ensure!`)
- **[contract-types-guide.md](contract-types-guide.md)** -- Contract types: `{ x : T | p }`, `:pre`/`:post` annotations, FFI contracts

## Refinement Types

- **[refinement-types-guide.md](refinement-types-guide.md)** -- Static discharge of `#refine{ x : T | p }`: what gets proved, call-site crossings, `--strict-refine`, and the documented limits
- **[stateful-refinements-guide.md](stateful-refinements-guide.md)** -- Refinements over mutable state: `#reads` measures and the `frozen` region that makes them congruent
- **[refinement-solver-internals-guide.md](refinement-solver-internals-guide.md)** -- How the in-house solver chain works (normalization, EUF, Fourier-Motzkin, Nelson-Oppen, cube expansion), and how to debug a specific obligation

## Functional Patterns

- **[arrows-guide.md](arrows-guide.md)** -- Bare-function arrow combinators and building DSP signal graphs with `stdlib/arrow.tur` and `stdlib/signal/`
- **[generators-guide.md](generators-guide.md)** -- Zero-overhead generators with `gen`/`yield`, lazy `Seq` combinators, and the `Range` type
- **[effects-vs-monads.md](effects-vs-monads.md)** -- When to reach for an effect handler and when to reach for a monad value

## Concurrency and Async

- **[threading-guide.md](threading-guide.md)** -- OS threads, `Arc<T>`, `Mutex<T>`, `Atomic<T>`, channels
- **[async-await-guide.md](async-await-guide.md)** -- Async/await with fibers and delimited continuations
- **[stm-guide.md](stm-guide.md)** -- Software transactional memory -- API reference and mechanics
- **[stm-tutorial.md](stm-tutorial.md)** -- STM tutorial: concepts, patterns, and worked examples
- **[session-types-guide.md](session-types-guide.md)** -- Model protocols as types, whether the protocol has two participants, or more
- **[dynamic-vars-guide.md](dynamic-vars-guide.md)** -- Thread-local, dynamically-scoped mutable cells with `defdynamic` and `binding`
- **[thread-pool-guide.md](thread-pool-guide.md)** -- Bounded POSIX worker-thread pool with back-pressure for fan-out work over `ptr<void>` items
- **[reactor-guide.md](reactor-guide.md)** -- Lightweight evented I/O with `tur/reactor`

## Advanced Control Flow

- **[delimited-control-operators-guide.md](delimited-control-operators-guide.md)** -- How `shift`/`reset`, `shift0`/`reset0`, `call/cc`, and `escape` differ
- **[effects-system-guide.md](effects-system-guide.md)** -- Algebraic effects, dependency injection, custom control flow
- **[logic-programming-guide.md](logic-programming-guide.md)** -- Backtracking, logic programming, constraint solving with cloneable continuations
- **[backtracking-guide.md](backtracking-guide.md)** -- Nondeterministic backtracking using the list monad in Turmeric's `stdlib/backtrack.tur`
- **[checkpointing-guide.md](checkpointing-guide.md)** -- Cloneable continuations for persistent workflows and checkpointing
- **[serializable-continuations-guide.md](serializable-continuations-guide.md)** -- Serializable continuations for persistent workflows and cross-process computation
- **[web-continuations-guide.md](web-continuations-guide.md)** -- Compact reference: `send-form-and-wait`, continuation store, routing model
- **[tur-logic-guide.md](tur-logic-guide.md)** -- How to use and extend `tur/logic` for miniKanren-style relational programming in Turmeric
- **[state-machines-guide.md](state-machines-guide.md)** -- Five different approaches to modeling state machines in Turmeric

## Data Structures and Libraries

- **[hamt-guide.md](hamt-guide.md)** -- Persistent hash maps with structural sharing (HAMT)
- **[frame-guide.md](frame-guide.md)** -- In-memory columnar dataframes with `tur-frame` -- building, querying, joining, and exporting via the Arrow C Data Interface
- **[stats-guide.md](stats-guide.md)** -- Statistics spice for Turmeric -- descriptive stats, distributions, hypothesis tests, OLS regression, and resampling
- **[json-guide.md](json-guide.md)** -- Compile-time `#json(...)` reader macro and runtime `tur/json` library for parsing and serializing JSON
- **[schema-guide.md](schema-guide.md)** -- Validate untyped boundary data with composable schema values using `stdlib/schema.tur`, with accumulating, path-tagged errors
- **[ecs-guide.md](ecs-guide.md)** -- Building games and simulations with `tur-ecs` and the `tur-ecs-raylib` companion
- **[ecs-storage-guide.md](ecs-storage-guide.md)** -- How `tur-ecs`'s three component-storage backends (`Dense`, `Sparse`, `Tag`) differ in layout, cost, and ergonomics
- **[ecs-vs-haskell-ecs.md](ecs-vs-haskell-ecs.md)** -- Side-by-side walk through a small game in apecs (Haskell), aztecs (Haskell), and `tur-ecs`

## Networking and Web

- **[httpd-guide.md](httpd-guide.md)** -- Build HTTP/1.1 servers with `stdlib/httpd` -- handlers, routing, middleware, and keep-alive
- **[httpd-middleware-guide.md](httpd-middleware-guide.md)** -- Reference for the shipped httpd middleware -- logging, CORS, basic auth, JSON, cookies, multipart, body-size, rate-limit, static files
- **[httpd-async-guide.md](httpd-async-guide.md)** -- Run handlers as fibers on a reactor thread -- non-blocking I/O, await primitives, in-flight cap, and middleware interop
- **[httpd-tls-guide.md](httpd-tls-guide.md)** -- How to terminate TLS on an httpd-new server using the `tur-tls` spice
- **[tourist-routing-guide.md](tourist-routing-guide.md)** -- Compose `tur-tourist` HTTP sub-apps with mount and prefix combinators for larger applications
- **[tourist-session-guide.md](tourist-session-guide.md)** -- Cookie-backed sessions for `tur-tourist` with swappable storage backends (memory, file)
- **[websocket-guide.md](websocket-guide.md)** -- Build RFC 6455 WebSocket clients and servers in Turmeric with the `tur-ws-client` and `tur-ws-server` spices
- **[web-stack-guide.md](web-stack-guide.md)** -- Building HTTP servers with `tur-httpd`, `tur-template`, and `tur-tourist` -- the composable web stack for Turmeric
- **[cloudflare-deployment-guide.md](cloudflare-deployment-guide.md)** -- Three approaches for deploying Turmeric-written services to Cloudflare -- Containers, Interpreter-in-WASM, and AOT WASM via emit-c

## Tutorials

- **[cellular-automata-comonad-tutorial.md](cellular-automata-comonad-tutorial.md)** -- Cellular automata with comonads
- **[custom-effects-tutorial.md](custom-effects-tutorial.md)** -- Writing custom effects
- **[minikanren-1-relations-and-queries.md](minikanren-1-relations-and-queries.md)** -- Logic programming with miniKanren -- relations, composition, and bidirectional queries
- **[snake-game-tutorial.md](snake-game-tutorial.md)** -- Building the snake game example
- **[web-continuations-tutorial.md](web-continuations-tutorial.md)** -- Multi-page web forms using serializable continuations (guestbook example)
- **[web-emscripten-tutorial.md](web-emscripten-tutorial.md)** -- Compile a Turmeric project to WebAssembly and run it in a browser
- **[parser-combinators-tutorial.md](parser-combinators-tutorial.md)** -- Build parser combinators from scratch on top of the backtracking (list) monad
- **[datalog-01-concepts.md](datalog-01-concepts.md)** -- Datalog Pt. 1: concepts -- create a database and a query system to go with it
- **[datalog-02-minimal-impl.md](datalog-02-minimal-impl.md)** -- Datalog Pt. 2: minimal implementation
- **[datalog-03-query-api.md](datalog-03-query-api.md)** -- Datalog Pt. 3: query API
- **[datalog-04-indexing.md](datalog-04-indexing.md)** -- Datalog Pt. 4: indexing

## Package Management

- **[consuming-spices-guide.md](consuming-spices-guide.md)** -- Adding, fetching, and using spice packages in a Turmeric project
- **[developing-spices-guide.md](developing-spices-guide.md)** -- Creating, testing, and publishing Turmeric spice packages
- **[package-management-guide.md](package-management-guide.md)** -- Creating projects, adding spices, `build.tur`, `tur.lock`, CLI reference
- **[using-turmeric-from-cmake.md](using-turmeric-from-cmake.md)** -- How to publish a Turmeric library for consumption by C and C++ projects via CMake or CPM using `tur emit-cmake`
- **[mise-asdf-guide.md](mise-asdf-guide.md)** -- Install and switch Turmeric compiler versions with the `asdf-turmeric` plugin (works for both `mise` and `asdf`)

## Editor and IDE

- **[vim-guide.md](vim-guide.md)** -- Vim / Neovim syntax highlighting installation and configuration
- **[vscode-guide.md](vscode-guide.md)** -- VS Code extension installation and configuration
- **[lsp-guide.md](lsp-guide.md)** -- Configuring editors to use the Turmeric language server for diagnostics
- **[time-travel-tracing-guide.md](time-travel-tracing-guide.md)** -- Recording an interpreted run with `tur trace` and replaying it over DAP so a debugger can step backwards
- **[ai-assistant-integration-guide.md](ai-assistant-integration-guide.md)** -- Using the Turmeric MCP server and LSP with Copilot CLI, Claude CLI, OpenCode, and VS Code Copilot
- **[devcontainer-guide.md](devcontainer-guide.md)** -- Running Turmeric development in a devcontainer from VS Code or the CLI
- **[formatter-guide.md](formatter-guide.md)** -- `tur format` CLI and web REPL Format button
- **[notebook-guide.md](notebook-guide.md)** -- Literate-programming `.tur.md` notebooks with `tur-notebook` -- TUI interface, HTML export, and scripted exec mode

## CLI Tools

- **[tur-new-guide.md](tur-new-guide.md)** -- Scaffold a new Turmeric spice (library or binary) with standard layout, build manifest, tests, and optional CI
- **[tur-run-guide.md](tur-run-guide.md)** -- Built-in Justfile-compatible task runner for building, testing, and managing Turmeric projects
- **[tur-watch-guide.md](tur-watch-guide.md)** -- Cross-platform filesystem watching with debounce and coalescing for CLI tools
- **[tvm-guide.md](tvm-guide.md)** -- Install, switch between, and pin Turmeric compiler releases the same way nvm does for Node
- **[compiler-flags-guide.md](compiler-flags-guide.md)** -- Diagnostic and debug flags accepted by `tur`; list of removed `-X` feature flags
- **[test-runner-contract.md](test-runner-contract.md)** -- Test framework API and contract
- **[test-suite-portability-guide.md](test-suite-portability-guide.md)** -- Fixture-suite pitfalls: portability, parallel ctest, sanitizers and heap probes, and which harness actually leak-checks what
- **[autodoc-guide.md](autodoc-guide.md)** -- Writing `;;;` docstrings and generating API docs with `tools/gendocs.py`
- **[image-dumps-guide.md](image-dumps-guide.md)** -- Warm-start a Turmeric program by saving and restoring a post-init continuation, Lisp/Smalltalk/pdumper style

## Performance

- **[memory-usage-guide.md](memory-usage-guide.md)** -- What each language feature costs in heap memory, measured, what frees it, and strategies for using less
- **[performance-guide.md](performance-guide.md)** -- Writing fast Turmeric programs -- numerical computation, data structures, string processing, concurrency, memory, recursion, I/O, and benchmarking methodology
- **[monomorphization-abi-guide.md](monomorphization-abi-guide.md)** -- How Turmeric's end-to-end monomorphization ABI works, why the by-value path replaced the int64 carrier, and how to read `__spec_*` symbols
- **[jit-guide.md](jit-guide.md)** -- The in-process MIR JIT (`tur jit`) end to end -- what MIR is, how the engine is wired into the build, and what it does differently from the `cc` path (the fallback contract, the permanent constraints, and the inline-C rules that only bite under the JIT)

## Compiler Internals

- **[compiler-internals.md](compiler-internals.md)** -- End-to-end walkthrough of the `tur` compiler pipeline and source layout in `src/`, aimed at contributors
- **[value-representations-guide.md](value-representations-guide.md)** -- The representation inventory and the producer/boundary matrix -- the live scoreboard for the representation-consolidation campaign
- **[gc-guide.md](gc-guide.md)** -- How memory is managed -- reference counting, the Bacon-Rajan cycle collector, arenas, and what is (and isn't) GC-managed
- **[ownership-guide.md](ownership-guide.md)** -- Which ownership strategy to reach for -- persistent-immutable, single-owner mutable, linear/affine handles, `rc<T>` for genuine sharing, and `weak<T>` to break the resulting cycles
- **[name-mangling-guide.md](name-mangling-guide.md)** -- How Turmeric turns source names into valid C identifiers -- the injective scheme, the legacy fold, and when each applies
- **[type-erasure-guide.md](type-erasure-guide.md)** -- Snapshot of where the `tur` compiler collapses higher-level types down to `int64_t` at the C boundary, and the three mechanisms it uses
- **[typeclass-internals-guide.md](typeclass-internals-guide.md)** -- How `definstance` lowers to a C dictionary struct + singleton, how method-field C types are resolved, and the closure-handle convention
- **[turi-parity-guide.md](turi-parity-guide.md)** -- Feature-by-feature parity matrix between the compiled Turmeric path (`tur`) and the tree-walking interpreter (`turi`)

## Interoperability

- **[c-integration-guide.md](c-integration-guide.md)** -- Foreign function interface (FFI) and C interop
- **[ffi-guide.md](ffi-guide.md)** -- Dynamic FFI: dlopen/dlsym, the experimental `call-ptr` form, extern-c under the interpreter, and the JIT thunk engine (with a worked libzmq example)
- **[eval-api.md](eval-api.md)** -- C embedding API for evaluating Turmeric expressions and calling Turmeric functions from within a C program using `libturi.a`
- **[inline-c-results-guide.md](inline-c-results-guide.md)** -- Build typed Result/Option values inside inline-C bodies with the preamble helpers instead of hand-rolling structs or returning sentinel ints
- **[sandboxing-guide.md](sandboxing-guide.md)** -- Running untrusted Turmeric code safely inside a C host using `turi_env_new_sandboxed`, capability flags, and resource limits

## Reference

- **[bibliography.md](bibliography.md)** -- Academic papers, theses, and technical references cited across Turmeric design documents and guides
- **[style-guide.md](style-guide.md)** -- Canonical idioms and formatting conventions for Turmeric code -- function arity, indentation, naming, and inline-C style

---

## Finding Guides

**By topic:**
- Getting Started → [introducing-saffron.md](introducing-saffron.md), [quickstart.md](quickstart.md), [syntax-guide.md](syntax-guide.md), [repl.md](repl.md), [repl-tutorial.md](repl-tutorial.md), [releases-and-installation-guide.md](releases-and-installation-guide.md)
- Language Basics → [structs-guide.md](structs-guide.md), [sum-types-guide.md](sum-types-guide.md), [strings-guide.md](strings-guide.md), [module-system-guide.md](module-system-guide.md), [binding-forms-guide.md](binding-forms-guide.md), [mutable-globals-guide.md](mutable-globals-guide.md), [function-arity-guide.md](function-arity-guide.md), [currying-guide.md](currying-guide.md), [cli-args-guide.md](cli-args-guide.md)
- Type System → [polymorphism-guide.md](polymorphism-guide.md), [type-annotations-guide.md](type-annotations-guide.md), [hkt-guide.md](hkt-guide.md), [hrt-guide.md](hrt-guide.md), [row-types-guide.md](row-types-guide.md), [union-intersection-types-guide.md](union-intersection-types-guide.md), [sized-types-guide.md](sized-types-guide.md), [sized-primitives-guide.md](sized-primitives-guide.md)
- Advanced Types → [gadts-guide.md](gadts-guide.md), [existential-types-guide.md](existential-types-guide.md), [opaques-guide.md](opaques-guide.md), [advanced-type-system-rationale.md](advanced-type-system-rationale.md)
- Type Safety → [substructural-types-guide.md](substructural-types-guide.md), [uniqueness-types-guide.md](uniqueness-types-guide.md)
- Error Handling → [error-handling-guide.md](error-handling-guide.md), [contract-types-guide.md](contract-types-guide.md)
- Functional Patterns → [arrows-guide.md](arrows-guide.md), [generators-guide.md](generators-guide.md), [effects-vs-monads.md](effects-vs-monads.md)
- Concurrency and Async → [threading-guide.md](threading-guide.md), [async-await-guide.md](async-await-guide.md), [stm-guide.md](stm-guide.md), [session-types-guide.md](session-types-guide.md), [dynamic-vars-guide.md](dynamic-vars-guide.md), [thread-pool-guide.md](thread-pool-guide.md), [reactor-guide.md](reactor-guide.md)
- Advanced Control Flow → [delimited-control-operators-guide.md](delimited-control-operators-guide.md), [effects-system-guide.md](effects-system-guide.md), [logic-programming-guide.md](logic-programming-guide.md), [serializable-continuations-guide.md](serializable-continuations-guide.md), [web-continuations-guide.md](web-continuations-guide.md), [state-machines-guide.md](state-machines-guide.md)
- Data Structures and Libraries → [hamt-guide.md](hamt-guide.md), [frame-guide.md](frame-guide.md), [stats-guide.md](stats-guide.md), [json-guide.md](json-guide.md), [schema-guide.md](schema-guide.md), [ecs-guide.md](ecs-guide.md)
- Networking and Web → [httpd-guide.md](httpd-guide.md), [httpd-middleware-guide.md](httpd-middleware-guide.md), [tourist-routing-guide.md](tourist-routing-guide.md), [websocket-guide.md](websocket-guide.md), [web-stack-guide.md](web-stack-guide.md), [cloudflare-deployment-guide.md](cloudflare-deployment-guide.md)
- Tutorials → [snake-game-tutorial.md](snake-game-tutorial.md), [minikanren-1-relations-and-queries.md](minikanren-1-relations-and-queries.md), [parser-combinators-tutorial.md](parser-combinators-tutorial.md), [datalog-01-concepts.md](datalog-01-concepts.md)
- Package Management → [package-management-guide.md](package-management-guide.md), [consuming-spices-guide.md](consuming-spices-guide.md), [developing-spices-guide.md](developing-spices-guide.md), [using-turmeric-from-cmake.md](using-turmeric-from-cmake.md), [mise-asdf-guide.md](mise-asdf-guide.md), [turmeric-spices](https://github.com/turmeric-lang/turmeric-spices)
- Editor and IDE → [vim-guide.md](vim-guide.md), [vscode-guide.md](vscode-guide.md), [lsp-guide.md](lsp-guide.md), [time-travel-tracing-guide.md](time-travel-tracing-guide.md), [ai-assistant-integration-guide.md](ai-assistant-integration-guide.md), [devcontainer-guide.md](devcontainer-guide.md), [formatter-guide.md](formatter-guide.md), [notebook-guide.md](notebook-guide.md)
- CLI Tools → [tur-new-guide.md](tur-new-guide.md), [tur-run-guide.md](tur-run-guide.md), [tvm-guide.md](tvm-guide.md), [compiler-flags-guide.md](compiler-flags-guide.md), [autodoc-guide.md](autodoc-guide.md)
- Performance → [performance-guide.md](performance-guide.md), [memory-usage-guide.md](memory-usage-guide.md), [monomorphization-abi-guide.md](monomorphization-abi-guide.md), [jit-guide.md](jit-guide.md)
- Compiler Internals → [compiler-internals.md](compiler-internals.md), [value-representations-guide.md](value-representations-guide.md), [name-mangling-guide.md](name-mangling-guide.md), [type-erasure-guide.md](type-erasure-guide.md), [typeclass-internals-guide.md](typeclass-internals-guide.md), [turi-parity-guide.md](turi-parity-guide.md)
- Interoperability → [c-integration-guide.md](c-integration-guide.md), [ffi-guide.md](ffi-guide.md), [eval-api.md](eval-api.md), [inline-c-results-guide.md](inline-c-results-guide.md), [sandboxing-guide.md](sandboxing-guide.md)
- Reference → [bibliography.md](bibliography.md), [style-guide.md](style-guide.md)

**By level:**
- Beginner → [introducing-saffron.md](introducing-saffron.md), [quickstart.md](quickstart.md), [repl-tutorial.md](repl-tutorial.md), then core feature guides
- Intermediate → Advanced control flow, type system
- Advanced → Effects, logic programming, serializable continuations, checkpointing

---

## Planning and Design

For design documents, architecture, and phase planning, see:
- **[docs/](https://github.com/turmeric-lang/turmeric/tree/main/docs)** -- Main docs folder
- **[../archive/](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/README.md)** -- Active planning documents
- **[../archive/history/](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/README.md)** -- Historical completed work
