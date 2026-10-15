#!/usr/bin/env bash
# tests/run-build-project.sh -- manifest-driven `tur build <dir>` smoke test.
#
# Verifies that `tur build <project-dir>` (where the directory carries a
# build.tur manifest) descends into src/ -- including nested src/<pkg>/ --
# compiles every module, resolves cross-module imports, and links a runnable
# binary.  Also asserts the bug it fixes: the manifest itself must not be
# compiled as source.
#
# The fixture is copied into a scratch dir before building so the generated
# .h/.c, _main.c, and .tur-abi-cache/ never land in the repo.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/build-project-smoke"
WORK="$(mktemp -d -t tur-bp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; we are
# exercising the build-driver path, not chasing compiler-internal leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'tur run build' first" >&2
    exit 2
fi

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
BIN="$WORK/app"

# Build the project from a scratch CWD so generated files land in $WORK.
build_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "build-project-compiles" "tur build exit=$build_rc: $build_out"
else
    pass "build-project-compiles"
fi

# The manifest must never be compiled as source.
if grep -q "unbound symbol 'tur-build-project-smoke'" <<< "$build_out"; then
    fail "build-project-skips-manifest" "build.tur was compiled as source"
else
    pass "build-project-skips-manifest"
fi

# The linked binary must exist and run, returning double-it(21) = 42.
if [ -x "$BIN" ]; then
    pass "build-project-binary-exists"
    "$BIN"
    run_rc=$?
    if [ "$run_rc" -eq 42 ]; then
        pass "build-project-cross-module-import"
    else
        fail "build-project-cross-module-import" "exit=$run_rc (expected 42)"
    fi
else
    fail "build-project-binary-exists" "expected $BIN to exist"
fi

# A module declared in :exports with no backing source file must fail loudly
# (Phase 2: parsed :exports map keys are validated against on-disk sources).
GHOST="$WORK/ghost"
mkdir -p "$GHOST/src/app"
cat > "$GHOST/build.tur" <<'EOF'
(defpackage tur-ghost
  :name    "tur-ghost"
  :version "0.1.0"
  :exports #{ "app/util" ["double-it"] "app/missing" ["nope"] })
EOF
cat > "$GHOST/src/app/util.tur" <<'EOF'
(defmodule app/util (export double-it) (defn double-it [x :int] :int (* x 2)))
EOF
ghost_out=$(cd "$WORK" && "$TUR" build "$GHOST" -o "$WORK/ghostbin" 2>&1)
ghost_rc=$?
if [ $ghost_rc -ne 0 ] && grep -q "declares export 'app/missing'" <<< "$ghost_out"; then
    pass "build-project-missing-export-fails"
else
    fail "build-project-missing-export-fails" "rc=$ghost_rc out=$ghost_out"
fi

# Flat layout: build.tur at the root with sources alongside it (no src/ dir).
# Exercises the shallow-scan fallback in collect_project_src_files and the
# regression guard that the stray manifest is never compiled as source.
FLAT="$WORK/flat"
mkdir -p "$FLAT"
cat > "$FLAT/build.tur" <<'EOF'
(defpackage tur-flat
  :name    "tur-flat"
  :version "0.1.0")
EOF
cat > "$FLAT/prog.tur" <<'EOF'
(defmodule prog (defn main [] :int 7))
EOF
flat_out=$(cd "$WORK" && "$TUR" build "$FLAT" -o "$WORK/flatbin" 2>&1)
flat_rc=$?
if [ $flat_rc -eq 0 ] && [ -x "$WORK/flatbin" ] \
   && ! grep -q "unbound symbol 'tur-flat'" <<< "$flat_out"; then
    pass "build-project-flat-skips-manifest"
else
    fail "build-project-flat-skips-manifest" "rc=$flat_rc out=$flat_out"
fi

# T2: project-mode `tur run` from an arbitrary cwd must resolve the turmeric
# runtime sources (e.g. src/runtime/hamt.c, pulled in by the hamt autolink
# marker) absolutely, not relative to cwd.  Before T2 this failed with
# `src/runtime/hamt.c: No such file` whenever cwd was not the turmeric tree.
HAMT="$WORK/hamtproj"
mkdir -p "$HAMT/src"
cat > "$HAMT/build.tur" <<'EOF'
(defpackage tur-hamtproj
  :name    "tur-hamtproj"
  :version "0.1.0")
EOF
# hamt/new + hamt/count force the src/runtime/hamt.c autolink marker; an empty
# map has count 0, so a clean exit code proves the runtime linked and ran.
cat > "$HAMT/src/main.tur" <<'EOF'
(defn main [] :int
  (let [m (hamt/new)]
    (hamt/count m)))
EOF
hamt_out=$(cd "$WORK" && "$TUR" run --offline "$HAMT/src/main.tur" 2>&1)
hamt_rc=$?
# Reproduce the exact T2 entry point too: project mode with no file arg,
# discovered by walking up from cwd inside the project.
hamt_proj_out=$(cd "$HAMT" && "$TUR" run --offline 2>&1)
hamt_proj_rc=$?
if [ $hamt_rc -eq 0 ] && [ $hamt_proj_rc -eq 0 ] \
   && ! grep -q "src/runtime/hamt.c: No such file" <<< "$hamt_out$hamt_proj_out"; then
    pass "run-project-resolves-runtime-from-foreign-cwd"
else
    fail "run-project-resolves-runtime-from-foreign-cwd" \
        "file-mode rc=$hamt_rc proj-mode rc=$hamt_proj_rc out=$hamt_out$hamt_proj_out"
fi

# T3: `tur build <dir>` must compile and link a :spices dependency's modules,
# not just resolve them for type-checking.  A consumer that imports a module
# from a :path-linked spice previously failed at cc with
# `sib__api.h: No such file` because the dep's module was never generated.
DEPROOT="$WORK/cross"
mkdir -p "$DEPROOT/sib/src/sib" "$DEPROOT/consumer/src/consumer"
cat > "$DEPROOT/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat > "$DEPROOT/sib/src/sib/api.tur" <<'EOF'
(defmodule sib/api
  (export answer)
  (defn answer [] :int 42))
EOF
cat > "$DEPROOT/consumer/build.tur" <<'EOF'
(defpackage consumer
  :name "consumer"
  :spices #{ "sib" #{:path "../sib"} })
EOF
cat > "$DEPROOT/consumer/src/consumer/main.tur" <<'EOF'
(defmodule consumer/main
  (import sib/api :refer [answer])
  (defn main [] :int (answer)))
EOF
xspice_out=$(cd "$DEPROOT/consumer" && "$TUR" build "$DEPROOT/consumer" -o "$WORK/xspice" 2>&1)
xspice_rc=$?
if [ $xspice_rc -ne 0 ] || [ ! -x "$WORK/xspice" ]; then
    fail "build-project-links-cross-spice-dep" "rc=$xspice_rc out=$xspice_out"
else
    "$WORK/xspice"
    xrun_rc=$?
    # answer = 42, proving the dep module was compiled, linked, and called.
    if [ "$xrun_rc" -eq 42 ]; then
        pass "build-project-links-cross-spice-dep"
    else
        fail "build-project-links-cross-spice-dep" "exit=$xrun_rc (expected 42)"
    fi
fi

# SYM2 (runtime-symbols-plan): a keyword `:foo` referenced from two different
# module TUs must fold to a single interned record (one pointer) in the final
# binary.  Build a two-module project under -Xsymbols, where each module returns
# `:foo`, and assert pointer identity across the module boundary (exit 0) plus a
# single weak symbol via nm.
SYMP="$WORK/symx"
mkdir -p "$SYMP/src/app"
cat > "$SYMP/build.tur" <<'EOF'
(defpackage tur-sym-cross-tu
  :name    "tur-sym-cross-tu"
  :version "0.1.0"
  :exports #{
    "app/main" ["main"]
    "app/a"    ["a-foo"]
    "app/b"    ["b-foo"]
  })
EOF
cat > "$SYMP/src/app/a.tur" <<'EOF'
(defmodule app/a (export a-foo) (defn a-foo [] :Sym :foo))
EOF
cat > "$SYMP/src/app/b.tur" <<'EOF'
(defmodule app/b (export b-foo) (defn b-foo [] :Sym :foo))
EOF
cat > "$SYMP/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/a :refer [a-foo])
  (import app/b :refer [b-foo])
  (defn same? [x :Sym y :Sym] :int
    ```c
    return (int64_t)((const void *)x == (const void *)y);
    ```)
  (defn main [] :int
    ;; 0 = the two TUs' :foo are the same pointer (cross-TU interning works).
    (if (= (same? (a-foo) (b-foo)) 1) 0 1)))
EOF
sym_out=$(cd "$WORK" && "$TUR" -Xsymbols build "$SYMP" -o "$WORK/symbin" 2>&1)
sym_rc=$?
if [ $sym_rc -ne 0 ] || [ ! -x "$WORK/symbin" ]; then
    fail "build-project-sym-cross-tu" "rc=$sym_rc out=$sym_out"
else
    "$WORK/symbin"
    sym_run_rc=$?
    # Exit 0 proves the two modules' `:foo` are the same interned record -- the
    # single-record guarantee, however it is realized.  project-mode executables
    # now build whole-program (the entry module single-file; see
    # project-mode-rc-runtime-preamble-missing.md), so the two modules land in one
    # TU and `:foo` interns once as a file-local `static` record rather than a
    # weak global folded across TUs -- nm cannot observe a static, so the
    # single-weak-symbol check below is moved to the --shared (.so) build, which
    # keeps separate compilation and is where cross-TU weak-folding still applies.
    if [ "$sym_run_rc" -eq 0 ]; then
        pass "build-project-sym-cross-tu"
    else
        fail "build-project-sym-cross-tu" "exit=$sym_run_rc (expected 0; :foo not folded across TUs)"
    fi
    # Cross-TU weak-folding: build the same modules as a shared library (which
    # retains separate compilation), then assert exactly one weak __tur_sym_foo
    # object survives in the .so -- i.e. the per-TU records folded to one.
    symlib_out=$(cd "$WORK" && "$TUR" -Xsymbols build "$SYMP" --shared -o "$WORK/symlib.so" 2>&1)
    if [ $? -ne 0 ] || [ ! -f "$WORK/symlib.so" ]; then
        fail "build-project-sym-cross-tu-single-symbol" "shared build failed: $symlib_out"
    elif command -v nm >/dev/null 2>&1; then
        sym_count=$(nm "$WORK/symlib.so" 2>/dev/null | grep -c "__tur_sym_foo")
        if [ "$sym_count" -eq 1 ]; then
            pass "build-project-sym-cross-tu-single-symbol"
        else
            fail "build-project-sym-cross-tu-single-symbol" "nm found $sym_count __tur_sym_foo symbols in .so (expected 1)"
        fi
    fi
fi

# F1 regression: prelude macros (when/cond/for/unless) must be visible inside
# a defmodule body compiled by project mode (`tur build <dir>`).  Before F1 the
# project-mode entry points (compile_to_h / compile_to_implementation) skipped
# the stdlib auto-load entirely, so `when` was "unknown function or operator".
WHEN_PROJ="$WORK/prelude-when"
mkdir -p "$WHEN_PROJ/src/app"
cat > "$WHEN_PROJ/build.tur" <<'EOF'
(defpackage prelude-when :name "prelude-when" :version "0.1.0")
EOF
cat > "$WHEN_PROJ/src/app/main.tur" <<'EOF'
(defmodule app/main
  (defn classify [x : int] : int
    (cond
      (> x 0) 3
      (< x 0) 7
      :else   0))
  (defn main [] : int
    ;; when/cond are prelude macros; using cond with an :else branch gives a
    ;; deterministic return value -- 3 if x>0, which proves the prelude loaded.
    (classify 5)))
EOF
when_out=$(cd "$WORK" && "$TUR" build "$WHEN_PROJ" -o "$WORK/whenbin" 2>&1)
when_rc=$?
if [ $when_rc -ne 0 ]; then
    fail "build-project-prelude-when" "tur build exit=$when_rc: $when_out"
else
    "$WORK/whenbin"
    when_run_rc=$?
    if [ "$when_run_rc" -eq 3 ]; then
        pass "build-project-prelude-when"
    else
        fail "build-project-prelude-when" "exit=$when_run_rc (expected 3)"
    fi
fi

# F4 regression: min/max macros added to stdlib/macros.tur must be visible
# inside a defmodule body compiled by project mode.
MINMAX_PROJ="$WORK/prelude-minmax"
mkdir -p "$MINMAX_PROJ/src/app"
cat > "$MINMAX_PROJ/build.tur" <<'EOF'
(defpackage prelude-minmax :name "prelude-minmax" :version "0.1.0")
EOF
cat > "$MINMAX_PROJ/src/app/main.tur" <<'EOF'
(defmodule app/main
  (defn main [] : int
    (+ (min 3 7) (max 2 5))))
EOF
minmax_out=$(cd "$WORK" && "$TUR" build "$MINMAX_PROJ" -o "$WORK/minmaxbin" 2>&1)
minmax_rc=$?
if [ $minmax_rc -ne 0 ]; then
    fail "build-project-prelude-minmax" "tur build exit=$minmax_rc: $minmax_out"
else
    "$WORK/minmaxbin"
    minmax_run_rc=$?
    if [ "$minmax_run_rc" -eq 8 ]; then
        pass "build-project-prelude-minmax"
    else
        fail "build-project-prelude-minmax" "exit=$minmax_run_rc (expected 8)"
    fi
fi

# F3 regression: the user-callable `cons` runtime list constructor must be
# available inside a defmodule body compiled by project mode (`tur build <dir>`).
# `cons` is registered as a builtin lowering to a {head,tail} cons-cell helper
# emitted into each TU's preamble, so it resolves without the stdlib auto-load
# that project mode skips (closes the c-dsl/glsl gap; F6 in the report).
# NOTE (project-mode-rc-runtime-preamble-missing): executable project builds now
# compile whole-program (entry module single-file), which DOES auto-load stdlib
# -- so a local defn named `list-head`/`list-tail` would collide with stdlib's.
# The local cell accessors are named `cell-head`/`cell-tail` to avoid that; the
# assertion (cons builtin + {head,tail} layout -> 33) is unchanged.
CONS_PROJ="$WORK/prelude-cons"
mkdir -p "$CONS_PROJ/src/app"
cat > "$CONS_PROJ/build.tur" <<'EOF'
(defpackage prelude-cons :name "prelude-cons" :version "0.1.0")
EOF
cat > "$CONS_PROJ/src/app/main.tur" <<'EOF'
(defmodule app/main
  (defn cell-head [l : int] : int
    ```c struct __tur_cell_t { int64_t head; int64_t tail; };
    return ((struct __tur_cell_t *)(intptr_t)l)->head; ```)
  (defn cell-tail [l : int] : int
    ```c struct __tur_cell_t { int64_t head; int64_t tail; };
    return ((struct __tur_cell_t *)(intptr_t)l)->tail; ```)
  (defn main [] : int
    ;; Build a cons list with the runtime `cons` builtin and walk two cells.
    ;; head=11, head(tail)=22 -> 33 proves cons cells are allocated and the
    ;; {head,tail} layout matches the list.tur walkers.
    (let [lst (cons 11 (cons 22 0))]
      (+ (cell-head lst) (cell-head (cell-tail lst))))))
EOF
cons_out=$(cd "$WORK" && "$TUR" build "$CONS_PROJ" -o "$WORK/consbin" 2>&1)
cons_rc=$?
if [ $cons_rc -ne 0 ]; then
    fail "build-project-prelude-cons" "tur build exit=$cons_rc: $cons_out"
else
    "$WORK/consbin"
    cons_run_rc=$?
    if [ "$cons_run_rc" -eq 33 ]; then
        pass "build-project-prelude-cons"
    else
        fail "build-project-prelude-cons" "exit=$cons_run_rc (expected 33)"
    fi
fi

# file-scope-c-block-emit-order: a module with a file-scope ```c typedef block
# placed AFTER the defns that use it.  Without the Pass 1a pre-pass fix in
# emit_implementation, the typedef lands after the function bodies in the
# generated .c and cc fails with "use of undeclared identifier".  With the fix
# all EX_INLINE_C blocks are emitted before any defn body regardless of source
# position in the flat item array.
CBLOCK="$WORK/cblock"
mkdir -p "$CBLOCK/src"
cat > "$CBLOCK/build.tur" <<'EOF'
(defpackage cblock-order :name "cblock-order" :version "0.1.0")
EOF
# __mk-cell references tur_cell_t, which is declared in the file-scope C block.
# The C block appears AFTER __mk-cell in source.  Without the fix the typedef
# ends up after the function body in the generated .c and cc rejects it.
cat > "$CBLOCK/src/main.tur" <<'EOF'
(defmodule main
  (defn main [] : int
    (__mk-cell 21))

  (defn __mk-cell [v : int] #{Unsafe} : int
    ```c
    tur_cell_t *p = (tur_cell_t *)malloc(sizeof(tur_cell_t));
    p->val = v * 2;
    return p->val;
    ```)

  ```c
  typedef struct { int64_t val; } tur_cell_t;
  ```)
EOF
cblock_out=$(cd "$WORK" && "$TUR" build "$CBLOCK" -o "$WORK/cblockbin" 2>&1)
cblock_rc=$?
if [ $cblock_rc -ne 0 ]; then
    fail "build-project-cblock-emit-order" "tur build exit=$cblock_rc: $cblock_out"
else
    "$WORK/cblockbin"
    cblock_run_rc=$?
    if [ "$cblock_run_rc" -eq 42 ]; then
        pass "build-project-cblock-emit-order"
    else
        fail "build-project-cblock-emit-order" "exit=$cblock_run_rc (expected 42)"
    fi
fi

# project-mode-defstruct-typedef-missing: a (defstruct ...) defined in a
# project-mode module must emit `typedef struct Name { ... } Name;` into the
# generated header so both the owning .c and any importing modules see the
# type.  Without the fix, the .h was missing the typedef and the .c emitted a
# spurious `Name Name_N;` variable declaration referring to an undeclared
# type, failing cc with "unknown type name".
DSPROJ="$WORK/defstruct-proj"
mkdir -p "$DSPROJ/src/foo"
cat > "$DSPROJ/build.tur" <<'EOF'
(defpackage tur-defstruct-proj
  :name    "tur-defstruct-proj"
  :version "0.1.0"
  :exports #{ "foo/box" ["Box" "make-box" "box-x"] })
EOF
# Box is declared via defstruct; make-box returns a heap pointer cast via the
# Box typedef in inline-C, which only links if `typedef struct Box {...} Box;`
# is visible.  box-x reads the x field as proof the layout matches.
cat > "$DSPROJ/src/foo/box.tur" <<'EOF'
(defmodule foo/box
  (export Box make-box box-x)
  (defstruct Box [x : float y : float])
  (defn make-box [a : float b : float] : int
    ```c
    typedef struct { double x; double y; } Box_;
    Box_ *p = (Box_ *)malloc(sizeof(*p));
    p->x = a; p->y = b;
    return (int64_t)(intptr_t)p;
    ```)
  (defn box-x [b : int] : float
    ```c
    typedef struct { double x; double y; } Box_;
    return ((Box_ *)(intptr_t)b)->x;
    ```)
  (defn fto-i [x : float] : int
    ```c return (int64_t)x; ```)
  (defn main [] : int
    (fto-i (box-x (make-box 21.0 0.0)))))
EOF
ds_out=$(cd "$WORK" && "$TUR" build "$DSPROJ" -o "$WORK/dsbin" 2>&1)
ds_rc=$?
if [ $ds_rc -ne 0 ]; then
    fail "build-project-defstruct-typedef" "tur build exit=$ds_rc: $ds_out"
else
    "$WORK/dsbin"
    ds_run_rc=$?
    if [ "$ds_run_rc" -eq 21 ]; then
        pass "build-project-defstruct-typedef"
    else
        fail "build-project-defstruct-typedef" "exit=$ds_run_rc (expected 21)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules: a module that does a
# top-level (load "stdlib/...") of a bare-defn stdlib file, consumed across
# modules in a `tur build <dir>` project.  The (load ...) must be expanded in
# separate-compilation (project) mode -- exactly as the entry unit does -- and
# the spliced bare defns (sqrt/floor from stdlib/math.tur) must reach codegen so
# the exported defn that calls them links.  Before this fix the load either
# survived to elab_load ("load is only valid at the top level") or its spliced
# defns never reached the per-module .c.
LOADP="$WORK/loadproj"
mkdir -p "$LOADP/src/app"
cat > "$LOADP/build.tur" <<'EOF'
(defpackage tur-loadproj
  :name    "tur-loadproj"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] "app/ops" ["hypot-floor"] })
EOF
# app/ops top-level-loads stdlib/math.tur (bare defns) and exports a defn that
# uses the spliced sqrt/floor.  app/main imports it cross-module.
cat > "$LOADP/src/app/ops.tur" <<'EOF'
(load "stdlib/math.tur")

(defmodule app/ops
  (export hypot-floor)
(defn hypot-floor [a : float b : float] : int
  (float->int (floor (sqrt (+ (* a a) (* b b)))))))
EOF
cat > "$LOADP/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/ops :refer [hypot-floor])
  (defn main [] : int
    ;; floor(sqrt(3*3 + 4*4)) = floor(5.0) = 5 -- the process exit code.
    (hypot-floor 3.0 4.0)))
EOF
loadp_out=$(cd "$WORK" && "$TUR" build "$LOADP" -o "$WORK/loadpbin" 2>&1)
loadp_rc=$?
if [ $loadp_rc -ne 0 ]; then
    fail "build-project-load-bare-defn-module" "tur build exit=$loadp_rc: $loadp_out"
else
    "$WORK/loadpbin"
    loadp_run_rc=$?
    if [ "$loadp_run_rc" -eq 5 ]; then
        pass "build-project-load-bare-defn-module"
    else
        fail "build-project-load-bare-defn-module" "exit=$loadp_run_rc (expected 5)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules (codegen): a module that
# top-level-(load ...)s a *runtime-preamble-dependent* stdlib file -- here
# stdlib/either.tur, which brings in the `Either` ADT and a higher-kinded
# `Functor` instance whose `fmap` dispatches through `tur_poly_fn_t` -- must
# build in separate-compilation (project) mode.  This exercises the per-module
# emission of the base ADT typedef + constructors, the `tur_poly_fn_t` carrier,
# and the on-demand fn-ptr typedefs that the whole-program preamble provides but
# the separate-compilation path historically omitted (unknown type name
# 'tur_poly_fn_t' / 'tur_adt_Either').  fmap (*2) over (Right 21) = 42.
LOADHK="$WORK/loadhk"
mkdir -p "$LOADHK/src/app"
cat > "$LOADHK/build.tur" <<'EOF'
(defpackage tur-loadhk
  :name    "tur-loadhk"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] })
EOF
cat > "$LOADHK/src/app/main.tur" <<'EOF'
(load "stdlib/either.tur")

(defmodule app/main
  (defn main [] : int
    (let [e (Right 21)]
      (match (fmap e (fn [x : int] : int (* x 2)))
        (Left l)  l
        (Right r) r))))
EOF
loadhk_out=$(cd "$WORK" && "$TUR" build "$LOADHK" -o "$WORK/loadhkbin" 2>&1)
loadhk_rc=$?
if [ $loadhk_rc -ne 0 ]; then
    fail "build-project-load-higher-kinded-module" "tur build exit=$loadhk_rc: $loadhk_out"
else
    "$WORK/loadhkbin"
    loadhk_run_rc=$?
    if [ "$loadhk_run_rc" -eq 42 ]; then
        pass "build-project-load-higher-kinded-module"
    else
        fail "build-project-load-higher-kinded-module" "exit=$loadhk_run_rc (expected 42)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules (^fat / closure runtime): a
# module that loads arrow.tur and composes via the bare `>>>` arrow exercises
# the `^fat` auto-shim (__tur_fatshim1) and the closure-application macros that
# the whole-program runtime preamble provides but separate compilation must emit
# per module .c.  arrow.tur is also self-contained re: tuple.tur's `Tuple2` (it
# uses a local layout-compatible pair struct), so no ambient stdlib type leaks.
# (3+1)*2 = 8.
ARROWP="$WORK/arrowproj"
mkdir -p "$ARROWP/src/app"
cat > "$ARROWP/build.tur" <<'EOF'
(defpackage tur-arrowproj
  :name    "tur-arrowproj"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] })
EOF
cat > "$ARROWP/src/app/main.tur" <<'EOF'
(load "stdlib/arrow.tur")

(defmodule app/main
  (defn main [] : int
    (let [inc (fn [x : int] : int (+ x 1))
          dbl (fn [x : int] : int (* x 2))
          c   (>>> inc dbl)]
      (c 3))))
EOF
arrowp_out=$(cd "$WORK" && "$TUR" build "$ARROWP" -o "$WORK/arrowpbin" 2>&1)
arrowp_rc=$?
if [ $arrowp_rc -ne 0 ]; then
    fail "build-project-load-arrow-fatshim" "tur build exit=$arrowp_rc: $arrowp_out"
else
    "$WORK/arrowpbin"
    arrowp_run_rc=$?
    if [ "$arrowp_run_rc" -eq 8 ]; then
        pass "build-project-load-arrow-fatshim"
    else
        fail "build-project-load-arrow-fatshim" "exit=$arrowp_run_rc (expected 8)"
    fi
fi

# load-not-expanded-in-imported-or-project-modules (cross-module instance): a
# module A that (load ...)s a higher-kinded typeclass instance (either.tur's
# Functor [Either]) and is consumed by a module B via `import`.  The instance
# must NOT leak into B's translation unit (where the owner's internal ADT
# typedef is absent); B calls A's exported entry point and A dispatches through
# its own dictionary.  fmap (*2) over (Right 21) = 42.
IMPHK="$WORK/imphk"
mkdir -p "$IMPHK/src/app"
cat > "$IMPHK/build.tur" <<'EOF'
(defpackage tur-imphk
  :name    "tur-imphk"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] "app/lib" ["run"] })
EOF
cat > "$IMPHK/src/app/lib.tur" <<'EOF'
(load "stdlib/either.tur")

(defmodule app/lib
  (export run)
(defn run [] : int
  (let [e (Right 21)]
    (match (fmap e (fn [x : int] : int (* x 2)))
      (Left l)  l
      (Right r) r))))
EOF
cat > "$IMPHK/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/lib :refer [run])
  (defn main [] : int (run)))
EOF
imphk_out=$(cd "$WORK" && "$TUR" build "$IMPHK" -o "$WORK/imphkbin" 2>&1)
imphk_rc=$?
if [ $imphk_rc -ne 0 ]; then
    fail "build-project-import-higher-kinded" "tur build exit=$imphk_rc: $imphk_out"
else
    "$WORK/imphkbin"
    imphk_run_rc=$?
    if [ "$imphk_run_rc" -eq 42 ]; then
        pass "build-project-import-higher-kinded"
    else
        fail "build-project-import-higher-kinded" "exit=$imphk_run_rc (expected 42)"
    fi
fi

# parametric-struct-by-value-carrier-inconsistency: a project module with a
# generic parametric struct (`Box2 [A B]`), a by-value constructor, and a
# by-value accessor.  Whole-program prunes the generic templates and emits the
# monomorphized `Box2__int__int` struct by value; separate compilation must do
# the same -- prune the invalid generic carrier bodies AND emit the
# monomorphized struct-app typedef in the header before the spec-clone decls.
# box-1st(mk-box(21, 99)) = 21.
PSTRUCT="$WORK/pstruct"
mkdir -p "$PSTRUCT/src/app"
cat > "$PSTRUCT/build.tur" <<'EOF'
(defpackage tur-pstruct
  :name    "tur-pstruct"
  :version "0.1.0"
  :exports #{ "app/main" ["main"] })
EOF
cat > "$PSTRUCT/src/app/main.tur" <<'EOF'
(defmodule app/main
(defstruct Box2 [A B] (e1 A) (e2 B))
(defn mk-box [A B] [a :A b :B] : (Box2 A B) (make-struct Box2 a b))
(defn box-1st [A B] [t : (Box2 A B)] :A (.e1 t))
  (defn main [] : int
    (box-1st (mk-box 21 99))))
EOF
pstruct_out=$(cd "$WORK" && "$TUR" build "$PSTRUCT" -o "$WORK/pstructbin" 2>&1)
pstruct_rc=$?
if [ $pstruct_rc -ne 0 ]; then
    fail "build-project-parametric-struct-by-value" "tur build exit=$pstruct_rc: $pstruct_out"
else
    "$WORK/pstructbin"
    pstruct_run_rc=$?
    if [ "$pstruct_run_rc" -eq 21 ]; then
        pass "build-project-parametric-struct-by-value"
    else
        fail "build-project-parametric-struct-by-value" "exit=$pstruct_run_rc (expected 21)"
    fi
fi

# project-mode-rc-runtime-preamble-missing: rc<T> / reference counting in a
# project-mode build.  Separate compilation omitted the inline RC/frame runtime
# (RcControlBlock, rc_cb_alloc, tur_frame, ...), so any `rc/of` failed at cc with
# "unknown type name 'RcControlBlock'".  Executable project builds now compile
# the entry module whole-program (single-file), inlining every imported module
# into one TU that carries the full runtime, so RC works and there is a single
# GC registry by construction.  Run under leak detection to prove the rc is
# allocated, counted, and freed cleanly.
RCPROJ="$WORK/rc-proj"
mkdir -p "$RCPROJ/src/foo"
cat > "$RCPROJ/build.tur" <<'EOF'
(defpackage tur-rc-proj :name "tur-rc-proj" :version "0.1.0"
  :exports #{ "foo/alloc" ["alloc-and-count"] })
EOF
# Module A allocates an rc<int> and an rc<struct-with-rc-field>; module B (main)
# calls into it.  A returns the struct rc's strong count (1) as the exit code.
cat > "$RCPROJ/src/foo/alloc.tur" <<'EOF'
(defmodule foo/alloc
  (export alloc-and-count)
  (defstruct Wrapper :move [val : rc<int>])
  (defn alloc-and-count [] : int
    (let [inner (rc/of 10)]
      (let [w (rc/of (make-struct Wrapper inner))]
        (rc/strong-count w)))))
EOF
cat > "$RCPROJ/src/foo/main.tur" <<'EOF'
(defmodule foo/main
  (import foo/alloc :refer [alloc-and-count])
  (defn main [] : int
    (alloc-and-count)))
EOF
rc_out=$(cd "$WORK" && "$TUR" build "$RCPROJ" -o "$WORK/rcbin" 2>&1)
rc_rc=$?
if [ $rc_rc -ne 0 ] || [ ! -x "$WORK/rcbin" ]; then
    fail "build-project-rc-runtime" "tur build exit=$rc_rc: $rc_out"
else
    ASAN_OPTIONS=detect_leaks=1 "$WORK/rcbin"
    rc_run_rc=$?
    if [ "$rc_run_rc" -eq 1 ]; then
        pass "build-project-rc-runtime"
    else
        fail "build-project-rc-runtime" "exit=$rc_run_rc (expected 1; rc strong-count or a leak/ASan abort)"
    fi
fi

# project-mode-rc-runtime-preamble-missing (owner-TU design): a --shared spice
# that EXPORTS a module using rc<T> compiles via separate compilation, where the
# runtime cannot be inlined per-module (duplicate GC state / duplicate symbols).
# The owner-TU design gives every module .c a static replica of the runtime
# functions plus an extern view of the runtime globals, defined once in the
# generated tur_runtime.c.  Assert the .so links, exports the rc-using function,
# and that the GC registry global has exactly one owning definition.
RCSO="$WORK/rc-so"
mkdir -p "$RCSO/src/widget"
cat > "$RCSO/build.tur" <<'EOF'
(defpackage rc-so :name "rc-so" :version "0.1.0"
  :exports #{ "widget/box" ["alloc-box"] })
EOF
cat > "$RCSO/src/widget/box.tur" <<'EOF'
(defmodule widget/box
  (export alloc-box)
  (defstruct Wrapper :move [val : rc<int>])
  (defn alloc-box [] : int
    (let [inner (rc/of 10)]
      (let [w (rc/of (make-struct Wrapper inner))]
        (rc/strong-count w)))))
EOF
rcso_out=$(cd "$WORK" && "$TUR" build "$RCSO" --shared -o "$WORK/rcbox.so" 2>&1)
rcso_rc=$?
if [ $rcso_rc -ne 0 ] || [ ! -f "$WORK/rcbox.so" ]; then
    fail "build-shared-rc-runtime" "tur build --shared exit=$rcso_rc: $rcso_out"
elif command -v nm >/dev/null 2>&1; then
    # The exported rc-using function must be present, and the GC registry global
    # must resolve to exactly one owning definition (the owner TU), not one per
    # module .c -- proving single shared GC state across the separately-compiled
    # translation units.
    # Portable across GNU nm (Linux) and Apple nm (macOS): macOS has no `nm -D`
    # (Mach-O has no dynamic symbol table -- the flag errors) and prefixes every
    # symbol with an underscore, so read plain `nm` and allow an optional leading
    # `_`.  The exported rc-using function is the external (uppercase T) def.
    has_export=$(nm "$WORK/rcbox.so" 2>/dev/null | grep -cE ' T _?widget__box__alloc_hybox$')
    gc_owners=$(nm "$WORK/rcbox.so" 2>/dev/null | grep -cE ' [A-Za-z] _?gc_all_blocks$')
    if [ "$has_export" -ge 1 ] && [ "$gc_owners" -eq 1 ]; then
        pass "build-shared-rc-runtime"
    else
        fail "build-shared-rc-runtime" "export=$has_export gc_all_blocks_owners=$gc_owners (expected >=1 and 1)"
    fi
else
    pass "build-shared-rc-runtime"
fi

# cfnptr-typedef-emitted-to-c-not-h: a module that EXPORTS a defn whose
# signature carries a `(c-fn [...] R)` parameter (a bare C-ABI function pointer)
# must, under separate compilation (`--shared`), emit the precise fn-ptr typedef
# into its generated `.h` BEFORE the exported declaration that names it -- not
# into the `.c` after it has already included its own `.h`.  Before the fix the
# `.h` referenced `tur_fnptr_..._t` while the typedef was emitted only in the
# `.c`, so cc rejected the header with implicit-int / conflicting-types and any
# importing TU that included the header failed the same way.  A clean `--shared`
# link (the sink module's `.c` includes its own `.h`, and the consumer module's
# `.c` includes the sink `.h`) is the regression assertion; we additionally
# grep the generated header to confirm the typedef precedes its use.
CFNPTR="$WORK/cfnptr"
mkdir -p "$CFNPTR/src/app"
cat > "$CFNPTR/build.tur" <<'EOF'
(defpackage tur-cfnptr
  :name    "tur-cfnptr"
  :version "0.1.0"
  :exports #{ "app/sink" ["run-twice"] "app/main" ["apply-it"] })
EOF
# app/sink exports a defn whose first param is a `(c-fn [float] float)`; app/main
# imports it across the module (.h) boundary and passes a captureless defn as the
# raw C-ABI callback.  Both modules reference the same fn-ptr typedef name, so the
# typedef must live in app/sink's header for the cross-module include to compile.
cat > "$CFNPTR/src/app/sink.tur" <<'EOF'
(defmodule app/sink
  (export run-twice)
  (defn run-twice [cb : (c-fn [float] float) x : float] : float
    ```c
    return cb(cb(x));
    ```))
EOF
cat > "$CFNPTR/src/app/main.tur" <<'EOF'
(defmodule app/main
  (import app/sink :refer [run-twice])
  (export apply-it)
  (defn inc-half [v : float] : float
    (+. v 0.5))
  (defn apply-it [x : float] : float
    (run-twice inc-half x)))
EOF
cfnptr_out=$(cd "$WORK" && "$TUR" build "$CFNPTR" --shared -o "$WORK/cfnptr.so" 2>&1)
cfnptr_rc=$?
if [ $cfnptr_rc -ne 0 ] || [ ! -f "$WORK/cfnptr.so" ]; then
    fail "build-shared-exported-cfn-typedef-in-header" "tur build --shared exit=$cfnptr_rc: $cfnptr_out"
else
    # The fn-ptr typedef must be DEFINED in the sink header before the exported
    # declaration that references it (proves the emit landed in the .h, not the .c).
    sink_h="$CFNPTR/build/obj/app__sink.h"
    if [ -f "$sink_h" ] \
       && grep -q "typedef .*(\*tur_fnptr_double_double_t)(double);" "$sink_h" \
       && td_line=$(grep -n "typedef .*tur_fnptr_double_double_t" "$sink_h" | head -1 | cut -d: -f1) \
       && use_line=$(grep -n "run_hytwice" "$sink_h" | grep "tur_fnptr_double_double_t" | head -1 | cut -d: -f1) \
       && [ -n "$td_line" ] && [ -n "$use_line" ] && [ "$td_line" -lt "$use_line" ]; then
        pass "build-shared-exported-cfn-typedef-in-header"
    else
        fail "build-shared-exported-cfn-typedef-in-header" \
            "shared build linked but typedef not emitted into header before its use (td=$td_line use=$use_line in $sink_h)"
    fi
fi

# dce-inline-c-used-attr: a #[used] defn reached only through its mangled C
# symbol -- a hand-written cross-module inline-C bridge -- must survive
# separate-compilation linkage.  app/a's __helper is unexported and never
# called through the Turmeric graph; app/main reaches it solely via a raw
# `extern int64_t app__a____helper(...)`.  Without #[used] the helper is
# demoted to `static` (separate compilation) or dropped entirely (the
# single-main whole-program shortcut inlines only the entry's Turmeric import
# closure), and the symbol dangles at link time.  #[used] retains it with
# external C linkage and forces every project module to be compiled+linked.
USEDPROJ="$WORK/used-attr"
mkdir -p "$USEDPROJ/src/app"
cat > "$USEDPROJ/build.tur" <<'EOF'
(defpackage tur-used-attr
  :name    "tur-used-attr"
  :version "0.1.0"
  :exports #{
    "app/main" ["main"]
    "app/a"    []
  })
EOF
cat > "$USEDPROJ/src/app/a.tur" <<'EOF'
;; app/a -- a private inline-C helper, NOT exported and never imported.
;; #[used] retains it with external C linkage for the raw-extern bridge below.
(defmodule app/a
  (defn #[used] __helper [x : int] : int
    ```c
    return x + 1;
    ```))
EOF
cat > "$USEDPROJ/src/app/main.tur" <<'EOF'
;; app/main -- reaches app/a's __helper ONLY via its mangled C symbol; there is
;; no `(import app/a)`, so the Turmeric call graph never touches __helper.
(defmodule app/main
  (defn use [x : int] : int
    ```c
    extern int64_t app__a____helper(int64_t);
    return app__a____helper(x);
    ```)
  (defn main [] : int (use 41)))
EOF
used_out=$(cd "$WORK" && "$TUR" build "$USEDPROJ" -o "$WORK/usedbin" 2>&1)
used_rc=$?
if [ $used_rc -ne 0 ]; then
    fail "build-project-used-attr-c-linkage" "tur build exit=$used_rc: $used_out"
else
    "$WORK/usedbin"
    used_run_rc=$?
    if [ "$used_run_rc" -eq 42 ]; then
        pass "build-project-used-attr-c-linkage"
    else
        fail "build-project-used-attr-c-linkage" "exit=$used_run_rc (expected 42)"
    fi
fi

# used-attr-whole-program: the SAME project, built single-file / whole-program
# (the `tur build <file>` / `tur run <file>` / `tur test` path) rather than
# `tur build <project>`.  That path inlines only the entry's Turmeric import
# closure, so app/a's #[used] __helper -- reached only via a raw mangled
# `extern` from app/main, with no `(import app/a)` -- used to be dropped and
# dangle at link.  cmd_build now scans the -I search dirs for #[used] modules
# and force-loads them into the whole-program TU, so the extern resolves.
wp_out=$(cd "$WORK" && "$TUR" build "$USEDPROJ/src/app/main.tur" \
            -I "$USEDPROJ/src" -o "$WORK/usedwpbin" 2>&1)
wp_rc=$?
if [ $wp_rc -ne 0 ]; then
    fail "build-file-used-attr-whole-program" "tur build exit=$wp_rc: $wp_out"
else
    "$WORK/usedwpbin"
    wp_run_rc=$?
    if [ "$wp_run_rc" -eq 42 ]; then
        pass "build-file-used-attr-whole-program"
    else
        fail "build-file-used-attr-whole-program" "exit=$wp_run_rc (expected 42)"
    fi
fi

# exports-map-syntax-tighten-plan: a manifest that uses `:exports #fx{...}`
# (an effect-row literal) instead of a map must be rejected at parse time with
# TUR-E0620, not silently accepted with an empty exports list.  Before the fix
# the reader tagged `#fx{...}` with F_MAP + PROV_FX_EXPLICIT and parse_exports
# only checked the tag, so the category-error manifest sailed through.
FXROW="$WORK/exports-fx-row-rejected"
mkdir -p "$FXROW/src/app"
cat > "$FXROW/build.tur" <<'EOF'
(defpackage tur-exports-fx-row
  :name    "tur-exports-fx-row"
  :version "0.1.0"
  :exports #fx{ "app/main" ["main"] })
EOF
cat > "$FXROW/src/app/main.tur" <<'EOF'
(defmodule app/main (defn main [] : int 0))
EOF
fxrow_out=$(cd "$WORK" && "$TUR" build "$FXROW" -o "$WORK/fxrowbin" 2>&1)
fxrow_rc=$?
if [ $fxrow_rc -ne 0 ] && grep -q "TUR-E0620" <<< "$fxrow_out"; then
    pass "build-project-exports-fx-row-rejected"
else
    fail "build-project-exports-fx-row-rejected" "rc=$fxrow_rc out=$fxrow_out"
fi

# exports-map-syntax-tighten-plan (positive): the canonical `#map{...}` form
# must be accepted at the same slot -- i.e. the tightening did not regress the
# happy path.  Uses the same fixture shape as the fx-row negative above.
MAPFORM="$WORK/exports-map-form"
mkdir -p "$MAPFORM/src/app"
cat > "$MAPFORM/build.tur" <<'EOF'
(defpackage tur-exports-map-form
  :name    "tur-exports-map-form"
  :version "0.1.0"
  :exports #map{ "app/main" ["main"] })
EOF
cat > "$MAPFORM/src/app/main.tur" <<'EOF'
(defmodule app/main (defn main [] : int 0))
EOF
mapform_out=$(cd "$WORK" && "$TUR" build "$MAPFORM" -o "$WORK/mapformbin" 2>&1)
mapform_rc=$?
if [ $mapform_rc -eq 0 ] && [ -x "$WORK/mapformbin" ]; then
    pass "build-project-exports-map-form-accepted"
else
    fail "build-project-exports-map-form-accepted" "rc=$mapform_rc out=$mapform_out"
fi

# exports-map-syntax-tighten-plan follow-up audit: the same effect-row trap
# applies to every OTHER map-shaped manifest slot, not just `:exports`.  Those
# slots checked only the F_MAP tag, so `:cmake-deps #fx{...}` parsed cleanly
# and CMake generation proceeded off an effect-row literal.  parse_* now routes
# through expect_map(), so each slot rejects an effect row with TUR-E0620.
for slot_case in \
    'spices|:spices #fx{ "dep" #fx{:url "https://example.invalid/d" :ref "v1"} }' \
    'spices-entry|:spices #map{ "dep" #fx{:url "https://example.invalid/d" :ref "v1"} }' \
    'cmake-deps|:cmake-deps #fx{ "nng" #map{:url "https://example.invalid/n" :ref "v1"} }' \
    'cmake-deps-entry|:cmake-deps #map{ "nng" #fx{:url "https://example.invalid/n" :ref "v1"} }' \
    'cmake-options|:cmake-deps #map{ "nng" #map{:url "https://example.invalid/n" :ref "v1" :options #fx{:BUILD_SHARED_LIBS "OFF"}} }' \
    'build-opts|:build-opts #fx{ :c-flags ["-DFOO=1"] }' \
    'bin|:bin #fx{ "tur-fxt" "src/app/main.tur" }' \
; do
    slot_name=${slot_case%%|*}
    slot_body=${slot_case#*|}
    SLOTDIR="$WORK/manifest-fx-row-$slot_name"
    mkdir -p "$SLOTDIR/src/app"
    cat > "$SLOTDIR/build.tur" <<EOF
(defpackage tur-fx-row-$slot_name
  :name    "tur-fx-row-$slot_name"
  :version "0.1.0"
  $slot_body
  :exports #map{ "app/main" ["main"] })
EOF
    cat > "$SLOTDIR/src/app/main.tur" <<'EOF'
(defmodule app/main (defn main [] : int 0))
EOF
    slot_out=$(cd "$WORK" && "$TUR" build "$SLOTDIR" -o "$WORK/fxslotbin" 2>&1)
    slot_rc=$?
    # Substring-match rather than `echo | grep -q`: under `set -o pipefail`
    # grep -q exits at the first match and SIGPIPEs echo, so the pipeline
    # reports 141 precisely when the text IS present. The cmake-deps
    # diagnostic is the longest of these (a ~58-char caret run), and it is the
    # one that intermittently failed CI with rc=1 and TUR-E0620 both visibly
    # correct in the failure message.
    if [ $slot_rc -ne 0 ] && [[ "$slot_out" == *"TUR-E0620"* ]]; then
        pass "build-project-manifest-fx-row-rejected-$slot_name"
    else
        fail "build-project-manifest-fx-row-rejected-$slot_name" \
             "rc=$slot_rc out=$slot_out"
    fi
done

# ...and the positive half of the same audit: `#map{...}` is now accepted at
# those slots too (they previously took only the bare `#{...}` spelling, so
# the diagnostic's suggested rewrite would itself have been an error).
MAPSLOT="$WORK/manifest-map-slots"
mkdir -p "$MAPSLOT/src/app"
cat > "$MAPSLOT/build.tur" <<'EOF'
(defpackage tur-map-slots
  :name    "tur-map-slots"
  :version "0.1.0"
  :build-opts #map{ :c-flags ["-DTUR_MAP_SLOT_TEST=1"] }
  :exports #map{ "app/main" ["main"] })
EOF
cat > "$MAPSLOT/src/app/main.tur" <<'EOF'
(defmodule app/main (defn main [] : int 0))
EOF
mapslot_out=$(cd "$WORK" && "$TUR" build "$MAPSLOT" -o "$WORK/mapslotbin" 2>&1)
mapslot_rc=$?
if [ $mapslot_rc -eq 0 ] && [ -x "$WORK/mapslotbin" ]; then
    pass "build-project-manifest-map-slots-accepted"
else
    fail "build-project-manifest-map-slots-accepted" "rc=$mapslot_rc out=$mapslot_out"
fi

# docs/archive/spice-guides-bare-brace-manifest-syntax.md: a bare `{...}` in a
# map-shaped slot is the single most common manifest mistake -- it is what every
# stale copy of the guides spells -- and the hint that would teach the fix had
# gone dead.  It was gated on `got->tag == F_CONTRACT_TYPE`, from when a bare
# `{...}` read as a contract-type annotation; contract types moved to
# `#refine{...}` and bare `{` is now unconditionally curly-infix, so the tag
# never appeared and the user got a bare ":spices must be a map".  The hint is
# unconditional now, with an extra clause naming curly-infix when that is what
# the reader actually saw.
for brace_case in \
    'spices|:spices {"dep" {:url "https://example.invalid/d" :ref "v1"}}' \
    'cmake-deps|:cmake-deps {"nng" {:url "https://example.invalid/n" :ref "v1"}}' \
    'build-opts|:build-opts {:c-flags ["-DFOO=1"]}' \
; do
    brace_name=${brace_case%%|*}
    brace_body=${brace_case#*|}
    BRACEDIR="$WORK/manifest-bare-brace-$brace_name"
    mkdir -p "$BRACEDIR/src/app"
    cat > "$BRACEDIR/build.tur" <<EOF
(defpackage tur-bare-brace-$brace_name
  :name    "tur-bare-brace-$brace_name"
  :version "0.1.0"
  $brace_body
  :exports #map{ "app/main" ["main"] })
EOF
    cat > "$BRACEDIR/src/app/main.tur" <<'EOF'
(defmodule app/main (defn main [] : int 0))
EOF
    brace_out=$(cd "$WORK" && "$TUR" build "$BRACEDIR" -o "$WORK/bracebin" 2>&1)
    brace_rc=$?
    if [ $brace_rc -ne 0 ] &&
       grep -q 'use `#map{\.\.\.}`' <<< "$brace_out" &&
       grep -q 'curly-infix' <<< "$brace_out"; then
        pass "build-project-manifest-bare-brace-hint-$brace_name"
    else
        fail "build-project-manifest-bare-brace-hint-$brace_name" \
             "rc=$brace_rc out=$brace_out"
    fi
done

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
