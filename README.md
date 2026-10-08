# Turmeric

[![CI](https://github.com/turmeric-lang/turmeric/actions/workflows/ci.yml/badge.svg)](https://github.com/turmeric-lang/turmeric/actions/workflows/ci.yml)

**Latest release:** `v0.63.7` -- `turi` and compiled programs use far less memory (Maps, closures, loops, continuations), and failed contracts now name the function and predicate.

## What

Turmeric is a statically-typed Lisp compiler that targets C99. Write expressive code in one of a few provided dialects, and get fast, portable C out. The compiler (`tur`) elaborates surface syntax into a typed IR, performs borrow checking, reference-count elision, and then emits C source that you can compile with any C99 compiler. There is a JIT interpreter available as well.

## Why

Turmeric exists to explore the intersection of Lisp expressiveness and systems-level control -- closures, algebraic effects, borrow checking, reference counting, and typeclasses -- without a runtime VM or garbage collector as the default execution model. The generated C is readable and can be embedded in any C project.

## Install

**macOS and Linux:**

```sh
curl -sSf https://turmeric-lang.com/install | sh
```

This installs the version manager (`tvm`) and then the latest **release**,
whose tarball is verified against that release's `sha256sums.txt` before it is
unpacked -- and the install **fails** rather than continuing if that check
cannot be made. Release assets also carry
[build provenance](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations),
so you can confirm which workflow run built the bytes you have:

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
name. Using the wrong flag reports a verification failure, not a compromised
download.

On a platform with no prebuilt binary, the installer builds that same release
tag from source instead (needs `cmake` and a C compiler).

**Building `main` instead (unverified, opt-in):**

```sh
# If you tapped this before the move, drop the old tap first -- brew keys its
# tap cache by name and will otherwise keep serving the stale one:
brew untap rjungemann/turmeric 2>/dev/null || true

brew tap turmeric-lang/turmeric https://github.com/turmeric-lang/turmeric
brew install --HEAD turmeric-lang/turmeric/turmeric
```

**The tap URL is not optional.** `brew tap user/repo` resolves to
`github.com/user/homebrew-repo`, and there is no `homebrew-turmeric` repo --
`Formula/turmeric.rb` lives in this one. Spelling the URL out is what points
the tap at the right place; without it the tap fails before `install` is
reached.

The Homebrew formula is **`--HEAD`-only**: it builds whatever is on `main` at
that moment and verifies no checksum. That is useful for tracking development
and is not what you want for an ordinary install. See the
[installation guide](docs/guides/releases-and-installation-guide.md) and
[`SECURITY.md`](SECURITY.md).

**Docker:**

```sh
docker build -t turmeric .
docker run --rm -it turmeric                                         # REPL
docker run --rm -v "$(pwd)":/workspace turmeric tur run /workspace/hello.tur
docker run --rm -v "$(pwd)":/workspace turmeric tur interpret /workspace/hello.tur
```

The image builds `tur` from the local source tree. See
[`Dockerfile`](Dockerfile) for details and the
[installation guide](docs/guides/releases-and-installation-guide.md) for
alternative Linux install paths (prebuilt binaries, build-from-source).

**Version manager (`tvm`):**

To install, switch between, and pin multiple Turmeric releases per-shell
(the `nvm`/`rustup` model), use the bundled Turmeric Version Manager:

```sh
sh tvm/install.sh        # bootstrap into ~/.tvm and wire up your shell rc
tvm install 0.23.1       # download + cache a prebuilt release
tvm use 0.23.1           # activate it for this shell
tvm alias default 0.23.1 # make it the default for new shells
```

See [`tvm/README.md`](tvm/README.md) for the full command set
(`ls`, `ls-remote`, `which`, `run`, `--build`, `.tur-version` auto-switch).

**GitHub Codespaces:**

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/turmeric-lang/turmeric)

**Build from source:**

Prerequisites: a C99 compiler and CMake 3.20+.

```sh
git clone https://github.com/turmeric-lang/turmeric.git
cd turmeric
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5  # debug + sanitizers
cmake --build build -j
```

Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-release -j
```

The compiler binary lands at `build/tur` (or `build-release/tur`).

**Run the tests:**

```sh
bash tests/run.sh
# or: ctest --output-on-failure --test-dir build
```

Once `tur` is on `PATH`, the project's `Justfile` recipes (build, test,
docs, wasm, web-dev, ...) are runnable via `tur run <recipe>` -- no extra
`just` binary required.

## Usage

```sh
./build/tur build path/to/file.tur     # compile to an executable
./build/tur emit-c path/to/file.tur    # print generated C to stdout
./build/tur run path/to/file.tur       # compile and immediately execute
./build/tur repl                       # interactive REPL
./build/tur doc vec-push!              # a symbol's docstring, no network needed
./build/tur docs --open                # the rendered guides and API reference
```

The documentation reads offline everywhere: `tur doc` and `tur docs` in a
terminal, and the **Docs** browser inside
[Try Turmeric](https://turmeric-lang.com/try), which precaches every guide and
API page when you install it -- no toggle, no opt-in. See the
[offline docs guide](docs/guides/offline-docs-guide.md).

## Package Management (Spice)

Turmeric has a built-in package manager called **Spice**. A single `build.tur`
file at the project root declares the package identity and its dependencies
(called *spices*):

```lisp
(defpackage my-app
  :name    "my-app"
  :version "0.1.0"

  ;; Turmeric dependencies -- declared as git URLs
  :spices #map{
    "geom" #map{:url "https://github.com/alice/tur-geom" :ref "v0.2.1"}
    "math" #map{:url "https://github.com/bob/tur-math"   :ref "v1.5.0"}
  }

  ;; C/CMake libraries (CPM-compatible) -- fetched and linked automatically
  :cmake-deps #map{
    "raylib" #map{:url "https://github.com/raysan5/raylib" :ref "5.0"}
  })
```

Common commands:

```sh
tur init --bin my-app        # scaffold a new project
tur add https://github.com/alice/tur-geom --ref v0.2.1
tur fetch                    # fetch all spices, write tur.lock
tur build                    # build the project
tur test                     # run tests
```

See [docs/archive/package-management-plan.md](docs/archive/package-management-plan.md) for the
full design, and [docs/archive/cmake-cpm-integration-plan.md](docs/archive/cmake-cpm-integration-plan.md)
for C/CMake dependency details.

## Examples

**Hello world:**

```lisp
(println "hi")
```

**FizzBuzz:**

```lisp
(let [^mut i 1]
  (while (<= i 100)
    (cond
      (= 0 (mod i 15)) (println "fizzbuzz")
      (= 0 (mod i 3))  (println "fizz")
      (= 0 (mod i 5))  (println "buzz")
      :else            (println i))
    (set! i (+ i 1))))
```

**Closures:**

```lisp
(defn main [] : int
  (let [x 10]
    (let [f (fn [] (+ x 1))]
      (println (f)))))
```

**Algebraic effects:**

```lisp
(defeffect Ask [] :int)

(defn use-ask [] : int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))
; => 42
```

**Typeclasses:**

```lisp
(defclass MyEq [a]
  (eq? [x y] : bool))

(definstance MyEq [int]
  (eq? [x y] (= x y)))

(defn main [] : int
  (if (.eq? 1 1) 0 1))
```

**Contracts:**

```lisp
(defn safe-div [a : int b : int] : int
  (require! (not= b 0))
  (/ a b))
```

**Set literals:**

```lisp
(def primes #s(2 3 5 7 11 13))
```

**Structural equality** -- via the `Eq` typeclass, on any container whose
element type is itself `Eq`:

```lisp
(println (.eq? [1 2 3] [1 2 3]))            ; => true
(println (.eq? #map{:a 1} #map{:a 2}))      ; => false
(println (.eq? #set{1 2} #set{1 2}))        ; => true
```

**Defer:**

```lisp
(defn main [] : int
  (let [f (open-file "log.txt")]
    (defer (close-file f))   ; runs on scope exit
    (write-file f "hello")
    0))
```

**Reference counting:**

```lisp
(defn main [] : int
  (let [r (rc/of 42)]
    (println (rc/strong-count r))
    (let [r2 (rc/clone r)]
      (println (rc/strong-count r)))
    (rc/drop r)
    0))
```

**FFI / inline C:**

```lisp
(extern-c printf [fmt :cstr] :int)

(println (inline-c "(__builtin_popcount(255))"))
```

**Persistent maps (HAMT):**

```lisp
(let [m (hamt-new)]
  (let [m2 (hamt-set m 1 "one")]
    (println (hamt-get m2 1))))
```

**Sized types (size-indexed vectors and buffers):**

```lisp
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(println (sized-vec-len (sized-vec-cons 1 (sized-vec-cons 2 (sized-vec-nil)))))   ; => 2
(println (sized-vec-sum (sized-vec-cons 1 (sized-vec-cons 2 (sized-vec-cons 3 (sized-vec-nil))))))  ; => 6

; Flat buffer -- phantom size index, bounds-checked access
(let [buf (:: (sized-buf-new-zeroed 64) :SizedBuf)]
  (sized-buf-set! buf 0 42)
  (println (sized-buf-get buf 0))       ; => 42
  (sized-buf-free buf))
```

**Literal match patterns:**

```lisp
(defn describe [x : int] : cstr
  (match x
    0 "zero"
    1 "one"
    2 "two"
    _ "many"))

(println (describe 1))   ; => one
```

**Async race:**

```lisp
(let [result (async-race
               (async (do (sleep 100) 1))
               (async (do (sleep  50) 2)))]
  (println result))   ; => 2  (faster task wins; slower is cancelled)
```

**CLI argument parsing:**

```lisp
(load "stdlib/args.tur")
(let [spec (args/spec-new)]
  (args/spec-flag   spec "--verbose")
  (args/spec-option spec "--output" "string" (some "out.txt"))
  (let [result (args/parse spec *args*)]
    (println (args/get-str result "output"))
    (args/result-free result))
  (args/spec-free spec))
```

**Math and bitwise stdlib:**

```lisp
(load "stdlib/math.tur")
(println (sqrt 2.0))       ; => 1.4142135623730951
(println (hypot 3.0 4.0))  ; => 5.0

(load "stdlib/bits.tur")
(println (bit-shr 16 2))   ; => 4
```

**GADTs (generalized algebraic data types):**

```lisp
(defgadt Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int)))

(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))
    (Mul l r) (* (eval-expr l) (eval-expr r))))

(println (eval-expr (Add (Lit 10) (Mul (Lit 4) (Lit 8)))))
; => 42
```

**Structural union types and gradual typing:**

```lisp
; Union type -- exhaustiveness-checked dispatch
(defn describe [x : (int | bool)] : int
  (match x
    (n : int)  (do (println n)   0)
    (b : bool) (do (println b)   0)))

(describe 42)    ; => 42
(describe true)  ; => true

; any top type -- every type is a subtype; tag recoverable at runtime
(defn show-type [x : any] : int
  (println (type-of x))
  0)

(show-type 42)       ; => "int"
(show-type "hello")  ; => "cstr"
```

**Effect row types and effect polymorphism:**

```lisp
(defeffect Write [s :cstr] :nil)
(defeffect Read  []        :cstr)

; #{Write} in the function type is enforced at call sites
(defn greet [name : cstr] #{Write} : nil
  (perform (Write name)))

; Effect-polymorphic: the caller's effect row propagates through
(defn twice [f :(fn [] #{e} :nil)] #{e} : nil
  (do (f) (f)))

; Effect hierarchy: Write ^extends IO; #{Write} satisfies #{IO}
(defeffect IO [] :nil)
(defeffect Log [s :cstr] :nil ^extends IO)

(defn log-message [] #{IO} : nil
  (perform (Log "hello")))
```

**Linear and substructural types:**

```lisp
; ^linear: value must be used exactly once
(defn use-once [^linear x : int] : int x)

; ^affine:   may be dropped, cannot be duplicated
; ^relevant: must be used, may be duplicated
(defn consume-affine [^affine  x : int] : int x)
(defn must-use       [^relevant x : int] : int x)
```

**Multi-shot continuations:**

```lisp
(defeffect Ask [] :int)

; ^multishot k: continuation may be resumed more than once
(defn main [] : int
  (let [r (handle (perform (Ask))
            (Ask [] ^multishot k)
              (+ (resume k 10) (resume k 20)))]
    (println r))   ; => 30
  0)
```

**Session types (binary and multi-party):**

```lisp
; Binary: two-party echo -- one side sends int, other echoes it back
(defn echo-client [^linear ch :(Session (Send int (Recv int Close)))] : int
  (let [ch       (send ch 42)]
    (let [[v ch] (recv ch)]
      (close ch)
      v)))

; Multi-party: global protocol projected onto each role at compile time
(defprotocol Ping [A B]
  (-> A B int)   ; A sends int to B
  (-> B A int))  ; B replies with int to A

(defn role-a [^linear ch :(Role Ping A)] : nil
  (let [ch (send-to ch B 42)]
    (let [[v ch] (recv-from ch B)]
      (println v)   ; => 42
      (close ch))))

(defn main [] : int
  (let [[ra rb] (make-protocol Ping)]
    (let [t (spawn (fn [] (role-b rb)))]
      (role-a ra)
      (join t)))
  0)
```

**Dynamic vars (dynamically-scoped mutable cells):**

```lisp
(load "stdlib/dynvar.tur")   ; provides *log-level*, *locale*, spawn-conveying

(defdynamic *request-id* :cstr "none")

(defn log-msg [msg : cstr] : int
  (println *request-id*)
  (println msg))

(defn handle-req [id : cstr] : int
  ; binding overrides *request-id* for the dynamic extent of its body
  ; -- visible to all code called from here, not just lexically enclosed code
  (binding [*request-id* id]
    (log-msg "processing"))
  0)

; spawn-conveying passes a snapshot of the current binding frame to the child
(defn main [] : int
  (binding [*request-id* "req-1"]
    (let [t (spawn-conveying (fn [] (log-msg "child")))]
      (join t)))  ; child sees "req-1"
  0)
```

## Security

Turmeric promises different things to different inputs -- a byte string handed
to a stdlib reader is not the same as a project tree you build. Those promises,
and the places the implementation does not yet keep them, are written down in
the [Security Guide](docs/guides/security-guide.md).

Two worth knowing before you use it:

- **Building a project runs that project's code** -- inline C, compile-time
  macros, its Justfile, its `:link-flags`. Do not `tur build` a tree you would
  not `make`. `tur check` and the language server aim narrower: they should not
  execute repo-supplied code unless you asked them to, and the guide names the
  three places that is not yet true.
- **`Env/new-sandboxed` is not yet a boundary against hostile code.** The
  capability set is enforced for builtins but not for native functions. Use it
  to catch accidents, not adversaries.

To report a vulnerability, use the
[private advisory form](https://github.com/turmeric-lang/turmeric/security/advisories/new)
rather than a public issue. See [`SECURITY.md`](SECURITY.md).

## Scoped Features for v1

| Feature | Status |
|---|---|
| Reader: vectors, keywords, hex/binary ints, curly-infix, neoteric | ✅ |
| Set literals (`#s(...)`) | ✅ |
| Typed IR, elaborator, operator dispatch | ✅ |
| `def` / `let` / `if` / `do` / `when` / `unless` / `cond` / `set!` / `while` | ✅ |
| `defn`, mutual recursion, multi-file builds | ✅ |
| Closures with capture analysis, env struct synthesis | ✅ |
| `defer` with scope unwind | ✅ |
| Macros (`defmacro`, quasiquote, threading macros) | ✅ |
| `defstruct`, field access, copy/move annotations | ✅ |
| ADTs (`defdata`, `match`, exhaustiveness checking) | ✅ |
| GADTs (`defgadt`, per-constructor type refinement, equality witnesses, `coerce`) | ✅ |
| Structural union types (`A \| B`), `any` top type, gradual typing | ✅ |
| Intersection types (`A & B`) | ✅ |
| Higher-ranked types (Rank-2/N `forall`, existential `pack`/`open`) | ✅ |
| Linear types (`^linear`, `lref<T>`) | ✅ |
| Uniqueness types (`^unique`) | ✅ |
| Substructural types (`^affine`, `^relevant`) | ✅ |
| Effect row types (`#{Effect}` in function types) | ✅ |
| Effect polymorphism (`forall [e]`, implicit row generalisation) | ✅ |
| Effect hierarchy (`^extends`, stdlib `Write ≤ IO` lattice) | ✅ |
| Handler typing (`(handler Effect A B)` first-class handler types) | ✅ |
| Linear continuations (`^linear k` one-shot, affine by default) | ✅ |
| Multi-shot continuations (`^multishot k`, snapshot semantics) | ✅ |
| Session types (binary `Session[P]`, `make-session`, `send`/`recv`/`close`/`offer`/`choose-*`) | ✅ |
| Multi-party session types (`defprotocol`, `Role`, `make-protocol`, `send-to`/`recv-from`) | ✅ |
| Dynamic vars (`defdynamic`, `binding`, `spawn-conveying`, stdlib common vars) | ✅ |
| Sized types (`StaticInt`, `SizedVec`, `SizedBuf`, `SizedMatrix`, `SizedBitVec`) | ✅ |
| Literal match patterns (int, bool, float, str literals as match arms) | ✅ |
| Borrow checker (`&`, `&mut`, reborrow, move semantics) | ✅ |
| Reference counting (`rc/of`, `rc/clone`, `rc/drop`, weak refs) | ✅ |
| RC elision (static analysis to remove redundant retain/release) | ✅ |
| Garbage collector (cycle detection, deterministic mode) | ✅ |
| Typeclasses with dictionary passing (`defclass`, `definstance`) | ✅ |
| Built-in typeclasses: `Eq`, `Ord`, `Show`, `Num` | ✅ |
| Capability passing (v1 effects via typeclasses) | ✅ |
| Panics with an unwind boundary (`panic`, `catch-unwind`, `catch-panic-of`) | ✅ |
| Delimited continuations (`shift` / `reset`) | ✅ |
| Algebraic effects (`defeffect` / `perform` / `handle` / `resume`) | ✅ |
| Serializable continuations | ✅ |
| Backtracking / cloneable continuations | ✅ |
| `extern-c`, inline-C blocks, FFI | ✅ |
| Structural equality (`Eq` typeclass / `.eq?`) | ✅ |
| Runtime contracts (`require!` / `ensure!` / `assert!` / `invariant!`) | ✅ |
| Standard library: `option`, `result`, `vec`, `str`, `slice`, `io`, `log`, `test`, `math`, `bits`, `args` | ✅ |
| Persistent collections (HAMT) | ✅ |
| STM (`TVar`, `atomically`, `retry`) | ✅ |
| Higher-kinded types (Functor, Applicative, Monad, Foldable, Traversable; `^f`/`^^f` syntax) | ✅ |
| Interactive REPL with `:type`, `:doc`, history | ✅ |
| WebAssembly playground | ✅ |
