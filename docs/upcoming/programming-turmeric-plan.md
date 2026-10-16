# Programming Turmeric -- Book Outline Plan

NOTE: This plan is up-to-date with turmeric v0.16.0. If the version is later than this, then revisit the features and adjust the outline accordingly.

Inspired by *Programming Ruby* (the Pickaxe), adapted for Turmeric's character:
functional-first, Lisp syntax, typeclasses, linear types, effects, inline C,
and a WASM-ready runtime. The arc is the same -- tour, ecosystem, deep-dive,
reference -- but the chapters diverge wherever Turmeric's design demands it.

---

## Preface

- Why Turmeric?
  - Functional, embeddable, WASM-ready
  - Escape hatch to C when you need it
  - Capability-based I/O model
- Notation conventions
  - S-expression syntax
  - `;;; docstring` format
  - `; => result` for expected output in examples
- Road Map
  - How the five parts build on one another
  - Which chapters are safe to skip on a first read
  - Suggested paths for application authors vs. language explorers
- Resources
  - The generated API reference (`docs/api/`)
  - The guides under `docs/guides/`
  - The web REPL and example notebooks
  - Where to ask questions and report issues

---

## Part I -- Facets of Turmeric *(tutorial)*

### Chapter 1 -- Getting Started

- Building from source (`just build`)
  - Prerequisites: a C compiler, CMake, and (optionally) `just`
  - The CMake fallback for fresh containers: `cmake -S . -B build` then
    `cmake --build build -j`
  - Where the compiler lands: `./build/tur`
  - Debug vs. release builds and the ASan/UBSan-instrumented debug binary
- Running a script: `turi myscript.tur`
  - The `tur run <file>` convenience and spice auto-discovery
  - `tur emit-c` / `tur build` for ahead-of-time compilation to C
  - Passing arguments through to the script via `*args*`
- The interactive REPL (command line)
  - Starting `tur repl` and evaluating forms at the prompt
  - Inspecting definitions with `(doc 'name)`
  - Multi-line input and editing conveniences
- The web REPL (WASM, Monaco editor)
  - Running entirely in the browser with no local install
  - The Monaco editor and the live doc panel
  - Sharing snippets and the limits of the sandboxed environment
- Hello, World
  - The minimal program: a `main` returning `:int`
  - Printing with `println`
  - Running it three ways: script, REPL, and web

### Chapter 2 -- Turmeric.new

A fast lap around the language to give readers enough footing for the rest of
the book.

- Everything is an expression; the value of a form is always defined
  - No statements: `if`, `do`, and `let` all evaluate to a value
  - The last form in a body is the result
  - Consequences for composability and refactoring
- Atoms: integers, booleans, `nil-value`, `cstr`, `ptr`
  - Integer and boolean literals
  - String literals and the `:cstr` view
  - `nil-value` as the empty/absent marker and raw `ptr`
- Calling functions: `(f a b)` prefix syntax
  - Operators are ordinary functions in prefix position
  - Nesting calls and reading them inside-out
  - A note on sweet-expression alternatives (covered later)
- Defining functions: `defn`, return types, arity
  - The `[param :type ...]` parameter vector
  - Mandatory return-type annotation
  - Arity, currying, and partial application at a glance
- `let` for local bindings, `do` for sequential evaluation
  - Binding vectors and lexical scope
  - Shadowing and nested `let`
  - `do` for grouping side-effecting steps
- Printing: `println`, `print`
  - Newline vs. no-newline variants
  - Formatting values for output
  - Where output goes (stdout) and flushing
- Commenting: `;` line comments and `;;;` doc-comments
  - `;` for inline and line comments
  - `;;;` doc-comments attached to definitions
  - `; => result` convention in examples
- Reading command-line arguments via `*args*`
  - `*args*` as a pre-declared cons list
  - Walking it with `head` / `tail`
  - Forward reference to `stdlib/args.tur` for structured parsing

### Chapter 3 -- Structs, Types, and Values

- Primitive types: `:int`, `:bool`, `:cstr`, `:ptr`, `:void`
  - What each primitive represents and its C mapping
  - `:void` as the absence of a return value
  - When to reach for sized primitives (forward reference to Ch. 18d)
- Defining structs with `defstruct`
  - The field list with per-field type annotations
  - Generated constructor and accessor functions
  - Documenting structs with `;;;`
- Field access and construction
  - Constructing a struct value positionally
  - Reading fields by name
  - Updating: producing a new value vs. mutation
- Type annotations on function parameters and return types
  - Why annotations are required, not inferred at boundaries
  - Type errors and the diagnostics you will see
  - Annotations as living documentation
- Value vs. pointer semantics
  - When values are copied vs. shared
  - `:ptr` and raw pointer handling
  - Implications for equality and identity
- Introducing linear structs (`:linear`) -- full treatment in Part III
  - The one-sentence intuition: consumed exactly once
  - A teaser example: a handle that cannot be used twice
  - Pointer to Chapter 18 for the full story

### Chapter 4 -- Lists, Options, and Results

- Singly-linked lists: `cons`, `nil-value`, `head`, `tail`
  - Building and deconstructing cons cells
  - Recursion and accumulator patterns over lists
  - Cost model: O(1) prepend, O(n) traversal
- `option`: `some` / `none` for nullable values
  - Representing "maybe absent" without null
  - Pattern matching on `some` / `none`
  - Mapping and chaining over optional values
- `result`: `ok` / `err` for fallible operations
  - Carrying an error payload alongside failure
  - Distinguishing recoverable errors from absence
  - Forward reference to error-handling patterns (Ch. 7)
- `vec`: growable arrays
  - Indexed access and in-place growth
  - Push/pop and capacity behavior
  - When a vec beats a list
- `map` / `hamt`: persistent hash maps
  - Key/value lookup and insertion
  - Structural sharing in the HAMT backing
  - Persistence: old versions stay valid
- Choosing the right container
  - A decision table by access pattern
  - Persistent vs. mutable trade-offs
  - Memory and allocation considerations

### Chapter 5 -- Functions, Macros, and Pattern Matching

- First-class functions and lambdas (`fn`)
  - Passing functions as arguments and returning them
  - Closures capturing lexical environment
  - Currying and partial application
- Higher-order functions: `map`, `filter`, `fold`
  - Transforming with `map`
  - Selecting with `filter`
  - Reducing with `fold` and choosing left vs. right
- `defmacro` and quasiquoting: writing code that writes code
  - Macros run at compile time on syntax
  - Quasiquote, unquote, and splicing basics
  - When a macro is the right tool vs. a function
- Core macros from `stdlib/macros.tur`: `cond`, `when`, `for`, `do-m`, `doc`
  - Branching with `cond` and `when`
  - Iteration with `for`
  - Monadic sequencing with `do-m` and lookups with `doc`
- Pattern matching with `match` and destructuring
  - Matching literals, constructors, and wildcards
  - Binding sub-values during a match
  - Destructuring structs and lists
- Guards in match arms
  - Adding boolean predicates to an arm
  - Ordering arms and exhaustiveness
  - Combining guards with destructuring

### Chapter 6 -- Typeclasses and Polymorphism

- What a typeclass is and why it is not a class
  - Behavior keyed by type, not data bundled with methods
  - Constraints on `defn` signatures vs. inheritance hierarchies
  - The dictionary-passing model under the hood
- `definstance` and dispatch
  - Declaring an instance of a typeclass for a concrete type
  - How the compiler resolves which instance to call
  - Coherence: one instance per type, and why overlap is rejected
- Built-in typeclasses: `Equal`, `Comparable`, `Functor`, `Monad`
  - `Equal` and `Comparable` for `=`, ordering, and sorting
  - `Functor` and `fmap` over containers
  - `Monad` and `bind`/`do-m` for sequencing effects
- Writing your own typeclass
  - Declaring the class signature and its method set
  - Providing instances for your own and stdlib types
  - Higher-kinded type variables for container-like classes
- How this differs from Ruby mixins and Java interfaces
  - No open classes or monkey-patching
  - Static resolution vs. runtime method lookup
  - Constraints travel with the type, not the object

### Chapter 7 -- Error Handling and Contracts

- Chaining `result` values without nesting
  - Flattening nested `ok`/`err` with bind
  - Short-circuiting on the first error
  - Transforming error payloads along the way
- When to use `option` vs. `result`
  - Absence vs. failure-with-reason
  - Converting between the two
  - API design: what to return to callers
- `contract.tur`: `assert!`, `require!`, `ensure!`
  - `require!` for preconditions on inputs
  - `ensure!` for postconditions on outputs
  - `assert!` for internal invariants
- Runtime contracts vs. type-level guarantees
  - What a contract checks at run time
  - What the type system rules out at compile time
  - Forward reference to refinement types (Ch. 18a)
- Propagation patterns: early-exit with `do-m`
  - Threading `result` through a computation
  - Readable happy-path code with errors handled implicitly
  - Combining `do-m` with contracts

### Chapter 8 -- I/O and Side Effects

- Printing and reading stdin
  - Writing to stdout and stderr
  - Reading lines and raw input
  - Buffering and flushing behavior
- File I/O with `io.tur`: open, read, write, close
  - Opening files and the returned handle
  - Reading and writing bytes/text
  - Closing deterministically and avoiding leaks
- The capability pattern: passing I/O handles rather than using globals
  - Why globals undermine testability and reasoning
  - Threading capabilities explicitly through call sites
  - Forward reference to effects and capabilities (Ch. 23)
- Async I/O overview: `async_file.tur`, `async_pipe.tur`
  - Non-blocking file operations
  - Non-blocking pipes
  - How async I/O composes with the concurrency models (Ch. 9)

### Chapter 9 -- Concurrency

One of Turmeric's strongest suits; gets a full tutorial chapter.

- Fibers and cooperative multitasking (`fiber.tur`)
  - Yielding and resuming explicitly
  - Lightweight stacks vs. OS threads
  - Cooperative scheduling pitfalls
- OS threads (`thread.tur`, `threadpool.tur`)
  - Spawning and joining threads
  - Sharing the work across a thread pool
  - Data races and what the type system can and cannot prevent
- Channels and CSP-style message passing (`chan.tur`)
  - Typed channels and send/receive
  - Buffered vs. unbuffered semantics
  - Building pipelines of communicating tasks
- Software Transactional Memory (`stm.tur`)
  - Atomic transactions over shared refs
  - Optimistic concurrency and retries
  - Composing transactions
- Async sockets (`async_socket.tur`)
  - Non-blocking accept/read/write
  - Integrating with the reactor/event loop
  - Pairing with channels for connection handling
- When to use each model
  - A decision guide by workload shape
  - Mixing models in one program
  - Performance and complexity trade-offs

### Chapter 10 -- Testing

- `test.tur`: `assert`, `assert-true`, `assert-false`, `assert-eq`
  - The core assertion forms and their messages
  - Comparing values with `assert-eq`
  - Structuring a test function
- Writing fixture files (ASCII-only rule; the reason why)
  - The `tests/fixtures/` layout and `expected.c` snapshots
  - The ASCII-only rule and the parser-hang reason behind it
  - `requires.*` skip markers for conditional fixtures
- Running tests with `just test`
  - Building and running the compiled-fixture suite
  - Reading FAIL lines and codegen mismatches
  - Leak detection (ASan/LSan) policy
- Test organization: files, suites, naming
  - Grouping related tests
  - Naming conventions for discoverability
  - Keeping fixtures and snapshots in sync

---

## Part II -- Turmeric in Its Setting

### Chapter 11 -- From the Command Line

- Invoking `turi`: options and flags
  - The subcommand surface (`run`, `build`, `emit-c`, `repl`, ...)
  - Global flags that precede the subcommand
  - Reading help and version output
- `*args*` and the `stdlib/args.tur` structured parser
  - Flags (`--verbose`), options (`--output file`), positional args, subcommands
  - Walking `*args*` directly with `head`/`tail`
  - The strict rule: no `parse-arg`/raw `g_tur_args` access
- Environment variables with `env.tur`
  - Reading and defaulting variables
  - Presence checks vs. value reads
  - Using env vars for configuration cleanly
- Writing shebang scripts
  - The shebang line and making a script executable
  - Argument handling in script mode
  - Distribution and portability notes

### Chapter 12 -- Processes and the OS

- Spawning subprocesses: `process/spawn`, `process/run`, `process/capture`
  - `spawn` for fire-and-forget child processes
  - `run` for synchronous wait-for-exit
  - `capture` for collecting output into a value
- Pipes and capturing stdout/stderr
  - Wiring child stdout/stderr to pipes
  - Reading streamed output incrementally
  - Combining vs. separating the two streams
- `process/exec` for replacing the current process
  - Semantics: no return on success
  - Passing argv and environment
  - When exec beats spawn
- Signal handling with `signal/`
  - Installing handlers for common signals
  - Safe actions inside a handler
  - Graceful shutdown patterns
- Exit codes and `process/exit`
  - Conventional exit-code meanings
  - Propagating a child's exit code
  - Flushing before exit

### Chapter 13 -- File System and Paths

- Pure path operations with `path.tur`: join, basename, dirname, extension, normalize
  - Joining and splitting path segments
  - Extracting basename, dirname, and extension
  - Normalizing `.`/`..` without touching the filesystem
- Directory operations: `fs/mkdir`, `fs/mkdirp`, `fs/rmdir`, `fs/glob`
  - Creating directories, recursively with `mkdirp`
  - Removing directories safely
  - Listing and matching entries with `glob`
- File metadata: `fs/stat`, `fs/exists?`, `fs/file?`, `fs/dir?`
  - Stat results and what they expose
  - Existence and type predicates
  - Handling missing-file cases
- Convenience: `fs/read-text`, `fs/write-text`, `fs/tmpfile`
  - One-call whole-file read/write
  - Temporary files and cleanup
  - When to prefer the low-level `io.tur` API

### Chapter 14 -- Networking

- TCP sockets with `net.tur` and `async_socket.tur`
  - The `Socket` linear type and connection lifecycle
  - Blocking vs. non-blocking sockets
  - Reading and writing framed data
- A simple HTTP client from scratch
  - Building a request line and headers
  - Sending the request and parsing the response
  - Handling chunked bodies and status codes
- The capability model applied to network handles
  - Passing sockets as capabilities, not globals
  - Scoping a connection's lifetime
  - Forward reference to TLS and `tur-httpd` (Ch. 36)

### Chapter 15 -- Building and Tools

- The `just` build system: `build`, `test`, `release`, `docs`, `wasm`
  - The recipe names and what each produces
  - `tur run <recipe>` vs. upstream `just`
  - The CMake fallback for fresh containers
- The `gendocs.py` pipeline: `;;;` -> HTML -> `docstrings.tur`
  - Parsing `;;;` docstrings out of stdlib sources
  - Emitting the HTML API reference into `docs/api/`
  - `--emit-tur` to rebuild the runtime lookup table
- `turi_doc_lookup` and the runtime doc table
  - How `(doc 'name)` resolves at run time
  - The generated table's role
  - Sharing the table with the web doc panel
- Keeping `stdlib/docstrings.tur` up to date (it is auto-generated; do not edit)
  - Why hand-edits get clobbered
  - Regenerating after docstring changes
  - Committing the regenerated file alongside the change

### Chapter 16 -- The Web REPL and WebAssembly

- The Monaco-based REPL in `web/`
  - Editor integration and live evaluation
  - Wiring user input to the WASM evaluator
  - Output rendering and error display
- How `turi_doc_lookup` powers the doc panel
  - Looking up docs without printing to the console
  - Exported from `src/wasm_glue.c`
  - Keeping parity with the CLI `(doc ...)`
- Building the WASM module with `just wasm`
  - The Emscripten build and its prerequisites
  - Running `just docs` first
  - Output size and optimization notes
- The `web/public/turmeric.wasm` artifact and `turmeric.js` glue
  - What the `.wasm` binary contains
  - The JS glue's responsibilities
  - Loading and instantiating in the browser
- Embedding Turmeric in a web page
  - Minimal host page setup
  - Calling into the runtime from JS
  - Sandboxing and capability limits in the browser

---

## Part III -- Turmeric Crystallized *(advanced)*

### Chapter 17 -- The Inline-C Escape Hatch

- Syntax: the `` ```c ... ``` `` block inside a `defn`
  - Where the block may appear in a body
  - What C sees: the generated function signature
  - Restrictions (e.g. no inline-C in variadic bodies)
- Declaring external functions with `extern-c`
  - Binding a C symbol to a Turmeric name
  - Header and link requirements
  - Type mapping for the declared signature
- The closing-paren style rule: `` ```) `` on the same line (and why)
  - The Markdown-rendering hazard it avoids
  - The exact required form
  - Applying it consistently in docs and fixtures
- Marshaling values across the boundary: `:int` for pointers, `:cstr`, `:ptr`
  - Passing integers, strings, and raw pointers
  - Casting handles in `#{Unsafe}` code
  - Lifetime and ownership at the boundary
- When to reach for inline C vs. keeping things pure Turmeric
  - Performance-critical inner loops
  - Wrapping existing C libraries
  - The cost: lost safety guarantees

### Chapter 18 -- Linear and Substructural Types

- What "linear" means: a value must be consumed exactly once
  - The "exactly once" rule stated precisely
  - Why it rules out use-after-free and double-free
  - Threading a linear value through a computation
- `lref`, move semantics, and the `TUR-E0100` diagnostic
  - Borrowing with `lref`
  - Moves that consume the original binding
  - Reading and fixing `TUR-E0100`
- Linear structs in practice: `Socket`, `FileHandle`, `MutexGuard`
  - Resources that must be released exactly once
  - Encoding open/close discipline in the type
  - RAII-like guards without destructors
- Affine vs. linear: the substructural lattice
  - Affine: at most once
  - Relevant: at least once
  - Where linear sits and what each enables
- Uniqueness types (`:unique`) and how they differ from linearity
  - Uniqueness as a guarantee about the past, not the future
  - Safe in-place mutation of unique values
  - Choosing `:unique` vs. `:linear`
- Patterns for guaranteed cleanup without `try/finally`
  - Consuming a guard to release a resource
  - Scoped helpers that take a closure
  - Comparison with exception-based cleanup
- *See also: `substructural-types-guide.md`, `uniqueness-types-guide.md`*

### Chapter 18a -- Contract and Refinement Types

- Contract types: `{ x : T | p }` syntax and the `-Xcontracts` flag
  - Reading the refinement syntax
  - Enabling the feature with `-Xcontracts`
  - Where the predicate `p` applies
- Predicate-as-`Form` storage; runtime checks at boundaries
  - How predicates are stored as syntax
  - Inserting checks at function boundaries
  - Cost of runtime checking
- Refinement types (`-Xrefinements`): the compiler proves predicates statically
  - Moving checks from run time to compile time
  - What "proven" buys you
  - Enabling `-Xrefinements`
- Worked example: ridding a codebase of defensive `require!` guards
  - Identifying redundant runtime guards
  - Replacing them with refined types
  - Verifying the guards are now unnecessary
- Limits: what the prover can and cannot discharge; SMT fallback
  - Predicates within the decidable fragment
  - When the SMT solver is invoked
  - Failure modes and how to help the prover
- *See also: `contract-types-guide.md`, `upcoming/refinement-types-plan.md`*

### Chapter 18b -- Union, Intersection, and Existential Types

- Union types (`(or T1 T2)`): typed sums without a wrapping ADT
  - Representing "one of these" without a constructor
  - Construction and consumption
  - Contrast with tagged ADTs
- Intersection types (`(and T1 T2)`): combining capabilities
  - A value that satisfies several types at once
  - Capability composition use cases
  - Subtyping interactions
- Narrowing in `match` and conditional contexts
  - Refining a union to a branch's type
  - Flow-sensitive narrowing in conditionals
  - Exhaustiveness over union members
- Existential types (`exists T. ...`): hiding implementation type
  - Packing a value with its operations
  - Clients that cannot name the hidden type
  - Abstraction boundaries
- Existential vectors and heterogeneous collections (`vec-existential.tur`)
  - Storing differently-typed values uniformly
  - Recovering behavior via packed interfaces
  - When heterogeneity is worth it
- Type erasure: when to forget structure on purpose
  - Dropping static detail deliberately
  - Trade-offs vs. keeping full types
  - Interop and serialization motivations
- *See also: `union-intersection-types-guide.md`, `existential-types-guide.md`, `type-erasure-guide.md`*

### Chapter 18c -- GADTs and Higher-Rank Types

- Generalized Algebraic Data Types: indexing constructors by their result type
  - Constructors that refine the type index
  - Pattern matching that recovers the index
  - Contrast with ordinary ADTs
- Worked example: a tag-safe expression evaluator
  - Encoding expression types in the index
  - An `eval` that cannot return the wrong type
  - Eliminating runtime tag checks
- `gadt-vec.tur`: length-indexed vectors as a GADT in stdlib
  - Tracking length in the type
  - Safe head/tail on non-empty vectors
  - Compile-time length arithmetic
- Higher-rank types (HRT): `forall a. ...` inside a parameter position
  - Quantifiers nested under arrows
  - Passing polymorphic functions as arguments
  - Why prenex form is not enough
- When HRT lets you write APIs you cannot write with prenex polymorphism
  - Callbacks that must stay polymorphic
  - ST-style encapsulation patterns
  - Inference limitations and annotations
- *See also: `gadts-guide.md`, `gadts-cookbook.md`, `hrt-guide.md`*

### Chapter 18d -- Sized Types

- The motivation: tracking dimensions in the type system
  - Bugs that dimension-tracking eliminates
  - Where unsized code goes wrong
  - The cost/benefit of sized types
- Sized primitives: `:i8`, `:i16`, `:i32`, `:i64`, `:u8`-`:u64`, `:f32`, `:f64`
  - Exact-width integers and their ranges
  - Floating-point widths
  - Conversions and overflow behavior
- Sized containers: `sized.tur`, `sized-buf.tur`, `sized-matrix.tur`, `sized-bits.tur`
  - Fixed-capacity buffers
  - Dimension-checked matrices
  - Bit-width-checked bitsets
- Length-indexed APIs: catch off-by-one at compile time
  - Encoding length in signatures
  - Safe indexing and slicing
  - Concatenation and length arithmetic
- Interop with linalg, frames, and FFI buffers
  - Sharing sizes with `tur-linalg`
  - Frame column lengths
  - Matching C buffer sizes across FFI
- *See also: `sized-types-guide.md`, `sized-primitives-guide.md`*

### Chapter 19 -- Metaprogramming with Macros

- Deep dive into `defmacro`: quasiquote, unquote, splicing
  - Building syntax with quasiquote
  - Splicing lists into templates
  - Multi-form expansions
- Hygiene and gensym
  - Avoiding variable capture
  - Generating fresh names with gensym
  - When to break hygiene deliberately
- Writing a DSL inside Turmeric: a worked example
  - Designing the surface syntax
  - Expanding it to core forms
  - Error reporting from a macro
- Limits of macro metaprogramming vs. runtime reflection
  - What macros cannot see (runtime values)
  - Compile-time vs. run-time trade-offs
  - Alternatives when macros fall short

### Chapter 20 -- Fixed Points, Free Monads, and Higher-Kinded Types

- The `fix.tur` fixed-point type: recursive types without recursion in the type
  - Factoring recursion out of a functor
  - The `Fix` wrapper and `unfix`
  - Why this enables generic recursion schemes
- Catamorphisms (`cata`) and anamorphisms (`ana`)
  - Folding a structure to a value with `cata`
  - Unfolding a seed into a structure with `ana`
  - Composing them (hylomorphisms)
- The Free Monad (`free.tur`): `free-pure`, `lift`, `bind`, `fmap`, `run`
  - Building programs as data
  - Lifting effects and sequencing with `bind`
  - Interpreting with `run`
- Higher-kinded types via `typeclass.tur`
  - Type constructors as type-class parameters
  - Encoding `Functor`/`Monad` generically
  - Limitations of the encoding
- When these abstractions pay off
  - Recurring patterns they capture
  - Readability vs. indirection
  - Knowing when simpler code wins

### Chapter 21 -- Session Types

- Protocol-safe communication with `session.tur`
  - Describing a protocol as a session type
  - Type-checked send/receive sequencing
  - Detecting protocol violations at compile time
- Dual endpoints: `send!` on one side requires `recv!` on the other
  - The duality relationship
  - Generating the dual automatically
  - Mismatched-endpoint errors
- Encoding a client/server handshake as a type
  - Modeling request/response rounds
  - Branching protocols (choice/offer)
  - Termination and channel closing
- Static guarantees vs. runtime overhead
  - What is checked statically
  - Residual runtime costs
  - When session types are worth the ceremony

### Chapter 22 -- Dynamic Variables

- `dynvar.tur`: thread-local dynamic binding without global state
  - Dynamic scope vs. lexical scope
  - Per-thread isolation
  - Why this is safer than mutable globals
- `dynvar/bind`, `dynvar/get`, `dynvar/set`
  - Establishing a binding for a dynamic extent
  - Reading the current value
  - Mutating within a scope
- Practical patterns: logging context, request-scoped config, capability threading
  - Carrying a logger or trace ID implicitly
  - Request-scoped configuration
  - Reducing capability-passing boilerplate

### Chapter 23 -- Effects and Capabilities

- The effects model in `effects.tur`
  - Effects as declared, handled operations
  - Separating effect description from interpretation
  - Comparison with monad transformers
- Capability-passing style: explicit I/O capabilities vs. global side effects
  - Passing the authority to perform an effect
  - Auditing what a function can do from its signature
  - Restricting capabilities in subcomputations
- Writing effect handlers
  - Interpreting an effect operation
  - Resuming a computation with a result
  - Layering handlers
- Composing effects
  - Combining multiple effect kinds
  - Ordering and interaction of handlers
  - Performance considerations

---

## Part III.5 -- Data, Plots, and Notebooks *(applied)*

The book pivots here from language internals to the spice ecosystem that makes
Turmeric usable for day-to-day data work. Each chapter focuses on one spice
from `../turmeric-spices/` and how it composes with the others.

### Chapter 23a -- Dataframes with `tur-frame`

- Columnar in-memory dataframes modeled on R's `data.frame` and pandas
  - Column-oriented layout and why it suits analytics
  - The mental model for R/pandas users
  - Immutability and structural sharing
- Backed by Apache Arrow's C Data Interface for zero-copy interop with
  Python, R, DuckDB, and Polars
  - What the Arrow C Data Interface is
  - Zero-copy hand-off across language boundaries
  - Lifetime/ownership across the FFI
- Building frames: `frame-from-cols`, `frame-from-rows`
  - Column-major construction with `frame-from-cols`
  - Row-major construction with `frame-from-rows`
  - Validating column lengths and names
- Columns and types: `column-int64`, `column-float64`, `column-utf8`,
  `type-int64`, `type-float64`, `type-utf8`
  - Typed column constructors
  - The type descriptors and null handling
  - Mapping to Arrow logical types
- Selecting and projecting: `select-cols`, `drop-cols`, `rename`,
  `with-col`, `map-col`, `mutate`
  - Choosing and dropping columns
  - Renaming and adding derived columns
  - Element-wise transforms with `map-col`/`mutate`
- Filtering, sorting, sampling: `filter`, `drop-nulls`, `distinct`,
  `sample`, `arrange`
  - Predicate filtering and null removal
  - Deduplication with `distinct`
  - Ordering with `arrange` and random `sample`
- Grouping and aggregation: `group-by`, `agg-sum`, `agg-mean`, `agg-count`,
  `agg-min`, `agg-max`, `summarize`
  - Forming groups with `group-by`
  - Per-group aggregates
  - Collapsing to a summary frame
- Joins: `inner-join`, `left-join`, `join`
  - Inner vs. left join semantics
  - Join keys and column collisions
  - The generic `join` entry point
- Reshape: `melt`
  - Wide-to-long reshaping
  - Identifier vs. value columns
  - Pairing with group/aggregate
- *See also: `frame-guide.md`*

### Chapter 23b -- Statistics and Formulas

- `tur-stats`: descriptive statistics, OLS, t-tests, on `tur-frame` inputs
  - Summary statistics over columns
  - Ordinary least squares regression
  - Hypothesis tests (t-tests and friends)
- Wilkinson-style formula DSL (`y ~ x1 * x2 + I(x3^2)`) via `tur-stats-formula`
  - The `~` formula syntax and interactions
  - Transformations with `I(...)`
  - Expanding a formula into a model matrix
- Pairing formulas with `frame-from-cols` outputs
  - Feeding frame columns into a model
  - Aligning column names with formula terms
  - Reading back fitted results
- *See also: `stats-guide.md`, `upcoming/stats-formula-plan.md`*

### Chapter 23c -- Plotting with `tur-plot`

- Tier-1 spice for 2D data visualization, rendered via `tur-plutovg`
  - The rendering pipeline on top of plutovg
  - The plot/series/axis object model
  - Default styling and theming
- Function plots, scatter points, intervals, histograms, contours
  - Continuous curves from a function
  - Discrete scatter and interval marks
  - Distribution views: histograms and contours
- Legends, axes, titles
  - Labeling axes and adding titles
  - Auto vs. manual tick placement
  - Legend entries and placement
- Output paths: PNG file or a plutovg surface for further composition
  - Writing a standalone PNG
  - Returning a surface for further drawing
  - Resolution and sizing controls
- Pairing plots with frames: `plot-frame-col`, sampled curves from typed `defn`s
  - Plotting a column directly
  - Sampling a typed function into a series
  - Combining frame data with computed curves
- Composing several plots into one image with `plot-into-canvas`
  - Laying out subplots on a canvas
  - Shared vs. independent axes
  - Exporting the composed image
- *See also: `tur-plot` spice README*

### Chapter 23d -- Generating Images

The image-generation stack is a small, composable set of spices: draw with
plutovg, read/write pixels with png, and (optionally) generate procedural
shapes with SDFs.

- **`tur-plutovg`** -- 2D vector graphics engine (the same that backs LunaSVG
  and PlutoSVG): canvases, paths, fills, strokes, gradients, patterns, fonts,
  PNG export. Good for SVG-style rendering, icons, plotting backends, and
  print-quality output.
  - Canvas and path-building API
  - Fills, strokes, gradients, and patterns
  - Text/fonts and PNG export
- **`tur-png`** -- libpng wrapper for `png-read` / `png-write`, plus per-pixel
  RGBA accessors. The pixel-level I/O layer that pairs naturally with
  `tur-plutovg` for generative art.
  - Reading and writing PNG files
  - Per-pixel RGBA get/set
  - Bridging to plutovg surfaces
- **`tur-sdf-raylib`** -- signed-distance-field primitives plus marching-cubes
  meshing, rendered through raylib. Phase 1 ships CPU SDF + MC; Phase 2 adds
  colored SDFs.
  - SDF primitives and combinators
  - Marching-cubes meshing
  - Phasing: CPU SDF/MC now, colored SDFs later
- **`tur-raylib`** / **`tur-raygui`** -- real-time windowed graphics and an
  immediate-mode GUI on top.
  - Window/render loop basics
  - Immediate-mode GUI widgets
  - Input handling
- **`tur-opengl`** / **`tur-glsl`** -- low-level GPU access for shader-driven
  generation.
  - Buffers, shaders, and the GL pipeline
  - Authoring GLSL from Turmeric
  - When to drop to the GPU
- Worked example: rendering a parameterized poster with plutovg, exporting
  it via png, and re-using the same renderer headlessly in a notebook.
  - Parameterizing the renderer
  - Exporting to PNG
  - Reusing the renderer headlessly in a notebook

### Chapter 23e -- Notebooks with `tur-notebook`

- Literate `.tur.md` notebooks -- Markdown prose with `turmeric` code fences
  - The `.tur.md` file format
  - Mixing prose and executable code fences
  - Version-control friendliness
- Session-backed evaluation: each cell runs in a persistent evaluator
  - Shared state across cells
  - Re-running and ordering effects
  - Session lifecycle and reset
- The TUI (`tur nb tui`): cell editing, insert/delete/paste, dirty-save
  quit handling, search (`/`, `n`, `N`), help overlay (`?`),
  focused-output toggling (`o`), user keybinding overrides
  - Cell editing and insert/delete/paste
  - Search, help overlay, and output focus
  - Dirty-save quit handling and keybinding overrides
- Standalone HTML export with the vendored docs-site stylesheet
  - Producing a self-contained HTML file
  - The bundled stylesheet
  - Embedding rendered images
- The `exec` subcommand for scripted, headless notebook execution
  - Running a notebook non-interactively
  - Capturing outputs/errors
  - CI and batch use
- The image-hook display system: returning a plutovg/png artifact from a
  cell renders it inline
  - The display-hook protocol
  - Returning a plutovg surface or PNG
  - Inline rendering in TUI and HTML
- Pairing notebooks with frames + plot + plutovg for end-to-end data
  notebooks
  - Loading and shaping data with frames
  - Visualizing with plot/plutovg
  - A complete analysis-to-report flow
- *See also: `notebook-guide.md`*

---

## Part IV -- Language Reference

### Chapter 24 -- Syntax Reference

- Source layout and the reader
  - File encoding and the ASCII rule
  - How the reader tokenizes forms
  - Reader dispatch (`#lang`, data literals)
- Atom types and their literals
  - Integer, boolean, and string literals
  - `nil-value` and pointer atoms
  - Keyword and symbol literals
- Special forms, A-Z:
  - `defn`, `defmacro`, `defstruct`, `definstance`
  - `let`, `do`, `if`, `match`, `fn`
  - `extern-c`, `import`
  - Inline-C block syntax
- The `;;;` docstring format in full
  - Required fields by definition kind
  - Module docstrings and separators
  - Conventions: ASCII, `; =>` examples, docstring levels

### Chapter 25 -- Type System Reference

- Primitive types: `:int`, `:bool`, `:cstr`, `:ptr`, `:void`
  - Representation and C mapping
  - Literals for each
  - `:void` in return position
- Sized primitives: `:i8`/`:i16`/`:i32`/`:i64`, `:u8`-`:u64`, `:f32`, `:f64`
  - Widths and ranges
  - Signed vs. unsigned
  - Conversion rules
- Struct field syntax and field access notation
  - Declaring fields with types
  - Constructor and accessor naming
  - Nested struct access
- Substructural qualifiers: `:linear`, `:unique`, lref, move semantics, `TUR-E0100`
  - The qualifier keywords
  - Borrowing and moving
  - The `TUR-E0100` diagnostic
- Contract types `{ x : T | p }` (`-Xcontracts`)
  - Syntax and enabling flag
  - Predicate semantics
  - Boundary check insertion
- Refinement types and static predicate proof (`-Xrefinements`)
  - What is proven statically
  - The SMT fallback
  - Annotations that aid proof
- Union (`(or T1 T2)`) and intersection (`(and T1 T2)`) types
  - Construction and elimination
  - Narrowing rules
  - Subtyping behavior
- Existential types and type erasure
  - Packing and unpacking
  - Hidden-type scope
  - Erasure semantics
- GADT constructors and HRT (`forall`) parameter positions
  - Index-refining constructors
  - `forall` under arrows
  - Inference and required annotations
- Sized type constructors and length indexing
  - Length-indexed signatures
  - Index arithmetic
  - Container constructors
- Type annotations on `defn` parameters and return types
  - Required positions
  - Type variable scope
  - Diagnostics for mismatches
- Typeclass constraints and higher-kinded type variables
  - Constraint syntax on signatures
  - Higher-kinded variables
  - Resolution and coherence

### Chapter 26 -- Macro Reference

- All macros in `stdlib/macros.tur`, with signatures and examples
  - One entry per macro with signature
  - Expansion shown alongside usage
  - Cross-references to tutorial chapters
- `cond`, `when`, `unless`, `for`, `do-m`, `doc`, and others
  - Conditionals: `cond`, `when`, `unless`
  - Iteration: `for`
  - Sequencing and lookup: `do-m`, `doc`

---

## Part V -- Standard Library Reference

### Chapter 27 -- Core Data Types

- `list.tur` -- singly-linked lists
  - `cons`/`head`/`tail` and traversal helpers
  - Map/filter/fold over lists
  - Cost model and when to prefer `vec`
- `option.tur` -- optional values
  - `some`/`none` constructors
  - Mapping and chaining
  - Conversion to/from `result`
- `result.tur` -- error handling
  - `ok`/`err` constructors
  - Bind and map over results
  - Extracting and defaulting
- `pair.tur` -- generic two-element pairs
  - Construction and `fst`/`snd`
  - Mapping over components
  - Use as lightweight tuples
- `tuple.tur` -- fixed-arity tuples
  - Constructing fixed-arity tuples
  - Positional access
  - Destructuring in `match`
- `str.tur` -- UTF-8 string views
  - View semantics over bytes
  - Slicing and searching
  - Encoding/decoding helpers
- `vec.tur` -- growable arrays
  - Push/pop and indexed access
  - Capacity and growth
  - Iteration helpers
- `set.tur` -- persistent sets
  - Membership and insertion
  - Union/intersection/difference
  - Structural sharing
- `map.tur` / `hamt.tur` -- persistent hash maps
  - Lookup, insert, remove
  - The HAMT backing structure
  - Iteration and merging
- `mutmap.tur` -- mutable hash map
  - In-place insert/remove
  - When mutation beats persistence
  - Iteration caveats
- `ref.tur` / `rc.tur` -- references and reference-counted boxes
  - Mutable cells with `ref`
  - Shared ownership with `rc`
  - Cycle and lifetime concerns
- `hash.tur` -- general-purpose hashing
  - Hashing primitives and structs
  - Combining hashes
  - Custom hash instances
- `equal.tur` -- structural equality
  - Deep equality semantics
  - The `Equal` typeclass
  - Deriving equality
- `range.tur` / `float-range.tur` -- integer and float ranges
  - Bounded and stepped ranges
  - Iterating a range
  - Float-range precision notes

### Chapter 28 -- Collections and Algorithms

- `slice.tur` -- array slices
  - Borrowing a window into an array
  - Bounds and sub-slicing
  - Avoiding copies
- `zipper.tur` -- list zippers for cursor-style traversal
  - Focus plus left/right context
  - Moving the cursor
  - Local edits in O(1)
- `gadt-vec.tur` -- length-indexed vectors
  - Length tracked in the type
  - Safe head/tail
  - Length arithmetic on concat
- `vec-existential.tur` -- existentially-typed heterogeneous vectors
  - Storing mixed types uniformly
  - Recovering behavior via interfaces
  - Use cases and trade-offs
- `bits.tur` / `sized-bits.tur` -- bitset operations
  - Set/clear/test bits
  - Bitwise combinators
  - Width-checked variant
- `sized.tur` / `sized-buf.tur` / `sized-matrix.tur` -- size-typed containers
  - Fixed-capacity buffers
  - Dimension-checked matrices
  - Compile-time size checks
- `grid.tur` -- 2D grids
  - Row/column indexing
  - Neighborhood access
  - Iteration patterns
- `parsec.tur` -- parser combinators
  - Primitive parsers and sequencing
  - Choice and repetition
  - Error reporting
- `re.tur` -- regular expressions
  - Compiling and matching
  - Capture groups
  - Replace/split helpers
- `backtrack.tur` -- backtracking search
  - Choice points and failure
  - Pruning the search
  - Collecting solutions
- `logic.tur` -- miniKanren-style relational programming
  - Logic variables and unification
  - Conjunction/disjunction goals
  - Running queries
- `gen.tur` -- generators
  - Lazily yielding values
  - Driving a generator
  - Composing generators
- `csv.tur` -- CSV reading and writing
  - Parsing with configurable options
  - Type inference on columns
  - Writing rows back out
- `json.tur` -- JSON parsing and serialization
  - Parsing into a value tree
  - Serializing back to text
  - Mapping to/from structs
- `schema.tur` -- structural data validation
  - Declaring a schema
  - Validating values against it
  - Collecting validation errors
- `digest.tur` -- cryptographic digests
  - Hash algorithms offered
  - Streaming vs. one-shot
  - Hex/base encoding of output

### Chapter 29 -- I/O, Files, and Networking

- `io.tur` -- low-level file I/O and directory listing
  - Open/read/write/close
  - Directory enumeration
  - Error handling on I/O
- `async_file.tur` -- non-blocking file operations
  - Submitting async reads/writes
  - Awaiting completion
  - Integration with the reactor
- `async_pipe.tur` -- non-blocking pipes
  - Creating async pipes
  - Streaming data through them
  - Backpressure handling
- `async_socket.tur` -- non-blocking sockets
  - Async accept/connect
  - Non-blocking read/write
  - Event-loop integration
- `net.tur` -- Socket linear type and helpers
  - The `Socket` linear type
  - Connection lifecycle
  - Send/receive helpers
- `env.tur` -- environment variables *(proposed)*
  - Reading variables
  - Defaults and presence checks
  - Setting for child processes
- `process.tur` -- process management *(proposed)*
  - Spawn/run/capture
  - Pipes and exit codes
  - `exec` semantics
- `path.tur` -- path string operations *(proposed)*
  - Join/split/normalize
  - Basename/dirname/extension
  - Pure (no filesystem) operations
- `fs.tur` -- high-level file system operations *(proposed)*
  - Directory create/remove/glob
  - Metadata and existence checks
  - Whole-file read/write and tmpfiles

### Chapter 30 -- Concurrency and Synchronization

- `thread.tur`, `threadpool.tur` -- OS threads and pools
  - Spawn and join
  - Submitting work to a pool
  - Sizing and lifecycle
- `fiber.tur` -- cooperative coroutines
  - Yield and resume
  - Lightweight stacks
  - Scheduling caveats
- `chan.tur` -- typed channels
  - Send/receive semantics
  - Buffered vs. unbuffered
  - Closing channels
- `stm.tur` -- software transactional memory
  - Transactional refs
  - Atomic commit/retry
  - Composing transactions
- `mutex.tur`, `condvar.tur`, `rwlock.tur` -- classical synchronization
  - Mutual exclusion with guards
  - Condition-variable waiting
  - Read/write locking
- `atomic.tur` -- atomic integers
  - Load/store and CAS
  - Fetch-and-op operations
  - Memory-ordering notes
- `sync.tur` -- barrier and once primitives
  - One-time initialization
  - Barrier synchronization
  - Use cases
- `future.tur` -- deferred values
  - Creating and completing futures
  - Awaiting results
  - Combining futures
- `scheduler.tur`, `scheduler_mt.tur` -- work-stealing scheduler
  - Task queues and stealing
  - Single- vs. multi-threaded
  - Tuning parallelism
- `taskgroup.tur` -- structured concurrency groups
  - Scoped task spawning
  - Joining and cancellation
  - Error propagation
- `select.tur` -- multiplexed channel select
  - Waiting on multiple channels
  - Default/timeout cases
  - Fairness

### Chapter 31 -- System and Utilities

- `args.tur` -- CLI argument parser
  - Flags, options, and positionals
  - Subcommands
  - Structured parse results
- `env.tur` -- environment variables
  - Reading and defaulting
  - Presence checks
  - Child-process environments
- `process.tur` -- process management
  - Spawn/run/capture
  - Pipes and exit codes
  - `exec` and signals
- `path.tur` / `fs.tur` -- paths and high-level filesystem
  - Pure path manipulation
  - Directory and metadata operations
  - Whole-file convenience helpers
- `signal/` -- POSIX signal handling
  - Installing handlers
  - Safe handler actions
  - Graceful shutdown
- `time.tur` / `timer.tur` -- wall clock and timers
  - Reading the clock
  - Durations and arithmetic
  - One-shot and repeating timers
- `math.tur` -- numeric functions
  - Elementary functions
  - Rounding and clamping
  - Constants
- `random.tur` -- random number generation
  - Seeding and generators
  - Ranges and distributions
  - Reproducibility
- `log.tur` -- structured logging
  - Levels and fields
  - Output sinks
  - Contextual logging
- `term.tur` -- terminal control and TUI primitives
  - Cursor and screen control
  - Colors and styles
  - Raw-mode input
- `serial.tur` -- serializable continuations
  - Capturing a continuation
  - Serializing and resuming
  - Use in durable workflows
- `workflow.tur` -- durable workflows
  - Defining steps
  - Persistence and resumption
  - Failure recovery
- `reactor.tur` -- event-loop reactor
  - Registering interest in events
  - The dispatch loop
  - Integrating async I/O
- `httpd.tur` -- embedded HTTP server
  - Routing requests
  - Handlers and responses
  - Capability-passed sockets

### Chapter 32 -- Advanced Type Machinery

- `fix.tur` -- fixed-point of a functor
  - The `Fix`/`unfix` wrapper
  - `cata`/`ana` recursion schemes
  - Generic recursion
- `free.tur` -- Free Monad
  - Programs as data
  - `lift`/`bind`/`fmap`
  - Interpreting with `run`
- `arrow.tur` -- Arrow abstraction
  - Arrow composition
  - First/second/`***`
  - When arrows beat monads
- `comonad.tur` -- Comonad abstraction
  - `extract`/`extend`/`duplicate`
  - Context-carrying computations
  - Example use cases
- `nat.tur` -- type-level naturals
  - Encoding naturals in types
  - Arithmetic at the type level
  - Backing length-indexed types
- `typeclass.tur` -- typeclass dispatch infrastructure
  - Dictionary representation
  - Instance resolution
  - Higher-kinded support
- `typeclass-eq.tur` / `typeclass-clone.tur` / `typeclass-functor.tur` -- canonical instances
  - `Equal` instances
  - `Clone` instances
  - `Functor` instances
- `existential.tur` -- existential type machinery
  - Packing values with operations
  - Unpacking in scope
  - Heterogeneous storage
- `safe.tur` -- safe-cast helpers for substructural types
  - Checked conversions
  - Borrowing helpers
  - Avoiding unsafe casts
- `session.tur` -- session types
  - Protocol encoding
  - Dual endpoints
  - Compile-time checking
- `dynvar.tur` -- dynamic variables
  - Bind/get/set
  - Per-thread scope
  - Context threading
- `effects.tur` -- algebraic effects
  - Declaring effects
  - Writing handlers
  - Composing effects
- `capability.tur` -- capability structs
  - Bundling authority
  - Passing capabilities explicitly
  - Restricting in subcomputations
- `contract.tur` -- runtime assertions
  - `assert!`/`require!`/`ensure!`
  - Failure behavior
  - Relation to refinement types

---

## Part VI -- Spice Ecosystem Reference

A short reference index of the first-party spices in `../turmeric-spices/`,
cross-referenced from the applied chapters in Part III.5.

### Chapter 33 -- Data and Analytics Spices

- `tur-frame` -- columnar dataframes, Arrow C Data Interface
  - Frame construction and column types
  - Select/filter/group/join verbs
  - Zero-copy Arrow interop
- `tur-stats` -- statistics on frames
  - Descriptive statistics
  - OLS regression
  - Hypothesis tests
- `tur-stats-formula` -- Wilkinson-style formula DSL *(planned)*
  - The `~` formula syntax
  - Term expansion to model matrices
  - Pairing with frames
- `tur-linalg` -- linear algebra
  - Vectors and matrices
  - Decompositions and solves
  - Sized-type integration
- `tur-math` -- extended math routines
  - Special functions
  - Numeric utilities
  - Precision considerations

### Chapter 34 -- Graphics and Image Spices

- `tur-plutovg` -- 2D vector graphics (paths, fills, gradients, text, PNG)
  - Canvas and path API
  - Fills, strokes, gradients
  - Text and PNG export
- `tur-png` -- libpng read/write with per-pixel access
  - `png-read`/`png-write`
  - Per-pixel RGBA accessors
  - Bridging to plutovg surfaces
- `tur-plot` -- 2D plotting on top of plutovg
  - Plot types and series
  - Axes, legends, titles
  - PNG/surface output
- `tur-sdf-raylib` -- signed-distance fields with raylib rendering
  - SDF primitives and combinators
  - Marching-cubes meshing
  - raylib rendering
- `tur-raylib` / `tur-raygui` -- real-time windowed graphics and immediate-mode GUI
  - Window and render loop
  - Immediate-mode widgets
  - Input handling
- `tur-opengl` / `tur-glsl` -- low-level GPU and shader access
  - GL buffers and pipeline
  - Authoring GLSL
  - Shader-driven generation

### Chapter 35 -- Notebooks and Tooling Spices

- `tur-notebook` -- `.tur.md` literate notebooks (TUI, HTML export, exec mode)
  - Session-backed cell evaluation
  - TUI editing and HTML export
  - Headless `exec` mode
- `tur-template` -- text templating
  - Template syntax and placeholders
  - Rendering with a context
  - Escaping rules
- `tur-watch` -- file-watcher driven workflows
  - Watching paths for changes
  - Triggering actions
  - Debouncing
- `tur-test` -- extended test runner
  - Test discovery
  - Richer assertions and reporting
  - CI integration

### Chapter 36 -- I/O and Integration Spices

- `tur-httpd` / `tur-http` / `tur-tls` -- HTTP server, client, TLS
  - Embedded server routing
  - HTTP client requests
  - TLS termination and certificates
- `tur-postgres` / `tur-sqlite` / `tur-valkey` -- databases
  - Connecting and querying
  - Parameterized statements
  - Result mapping
- `tur-json` / `tur-regex` / `tur-ansi` -- text and protocol helpers
  - JSON parse/serialize
  - Regex matching
  - ANSI styling
- `tur-osc` / `tur-rtmidi` / `tur-rtaudio` / `tur-wav` / `tur-tidal` / `tur-signal` -- audio and music
  - OSC and MIDI I/O
  - Real-time audio and WAV
  - Tidal-style patterns and DSP
- `tur-c-dsl` -- declarative inline-C DSL
  - Declaring C bindings concisely
  - Type-mapped wrappers
  - Reducing inline-C boilerplate
- `tur-tourist` -- guided-tour packaging for spices
  - Authoring guided tours
  - Packaging examples
  - Running a tour

---

## Appendices

- **A: Building from Source** -- prerequisites, `just build`, release vs. debug
  - Toolchain prerequisites (C compiler, CMake, `just`)
  - The CMake fallback for fresh containers
  - Debug vs. release and the sanitizer build
- **B: Inline-C Quick Reference** -- type mapping table, common patterns
  - Turmeric-to-C type mapping table
  - Common marshaling patterns
  - The closing-paren style rule
- **C: `just` Target Reference** -- all targets and what they do
  - Build/test/release/docs/wasm targets
  - `tur run` vs. upstream `just`
  - Configure and bootstrap recipes
- **D: Docstring Format Cheat Sheet** -- `;;;` required fields, conventions, ASCII rule
  - Required fields by definition kind
  - Module docstrings and separators
  - ASCII-only and example conventions
- **E: Language Changes by Phase** -- B1 through present, feature timeline
  - Phase-by-phase feature additions
  - Notable breaking changes
  - Mapping features to chapters
