# An explicit `(load "stdlib/...")` reads the working directory's copy first

**Severity:** low-medium (silent wrong stdlib; mixed-version builds).
**Filed:** 2026-10-16, found measuring `re/replace-all` on the WP0 box (#1238).

`(load "stdlib/re.tur")` opens `stdlib/re.tur` relative to the current
directory, and only if that fails does it try the stdlib the `tur` resolved
(`TUR_STDLIB_DIR`, else the one beside the binary). The prelude always comes
from the resolved stdlib. So a `tur` run from a directory with a `stdlib/`
in it builds against two stdlibs: its own prelude and that directory's
explicitly loaded modules. Nothing says so.

## Repro

```sh
mkdir -p /tmp/r/stdlib
echo '(defn re/compile [p : cstr] : int 41)' > /tmp/r/stdlib/re.tur
printf '(load "stdlib/re.tur")\n(defn main [] : int\n  (do (println (re/compile "a")) 0))\n' > /tmp/r/p.tur
cd /tmp/r && tur run p.tur          # prints 41: the cwd's re.tur, not the stdlib's
tur --interpret p.tur               # 41 too
```

Run from anywhere without a `stdlib/`, the same program loads the real
`re.tur` (and here fails to typecheck, since `re/compile` returns a `Regex`).

Where it bit: `benchmarks/audit/run-corpus.py` ran both sides of a
`remeasure.sh` A/B from the after checkout, so the before `tur` compiled the
after tree's `re.tur` and the fix measured as no change. #1239 runs each side
from its own checkout instead. The same thing happens to anyone running one
checkout's `tur`, or an installed one, from inside another checkout.

## Root cause

`src/compiler/elab_toplevel.c`, the `load` expansion: `elab_read_file(path_buf,
...)` is tried as written (cwd-relative), and the `<stdlib_dir>/<rest>` retry
runs only when that read fails ("off-tree fallback").

The dedup key disagrees with that. `load_path_key` (same file) canonicalises a
`stdlib/<rest>` path through `stdlib_dir` FIRST, and its comment says a
`stdlib/<rest>` load "MEANS the stdlib's <rest>". So the key names the
resolved stdlib's file while the text spliced in is the cwd's.

## Fix directions

- Read a `stdlib/<rest>` load from `stdlib_dir` first, matching the key, and
  fall back to cwd-relative only when `stdlib_dir` is the legacy literal
  `"stdlib"` (no resolved root, as in the wasm embedder). Inside the repo
  with the in-tree build both are the same file, so the suite should not
  notice. Check the web build, which sets `TUR_STDLIB_DIR` itself.
- Or keep cwd-first, and warn once when the cwd copy and the resolved
  stdlib's copy are different files.
