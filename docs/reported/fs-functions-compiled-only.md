# Most of `stdlib/fs.tur` cannot run under the interpreter

**Severity: medium (expressiveness / engine parity).** 25 of the 29
inline-C functions in `stdlib/fs.tur` have no interpreter native, so every one
of them fails under `tur --interpret` and in `tur repl`, and so does
`fs/read-string` (`stdlib/fs-string.tur`). Among them are the basics a script
or a REPL session reaches for first: `fs/read-text`, `fs/read-string`,
`fs/read-lines`, `fs/write-text`, `fs/exists?` and even `fs/path-join`, which
does no I/O at all. The compiled path (`tur run`,
`tur build`, `tur jit`) runs all of them.

Found 2026-10-15 while adding `fs/read-string` (markdown-library-plan S3),
which is compiled-only for the same reason.

## Repro

```sh
cat > /tmp/fs-probe.tur <<'EOF'
(load "stdlib/fs.tur")
(defn main [] : int (do (println (fs/path-join "a" "b")) 0))
EOF
./build/tur run /tmp/fs-probe.tur          # a/b
./build/tur --interpret /tmp/fs-probe.tur  # error below
```

```
tur: eval: inline-C not supported in interpreter mode (function uses a native
C implementation; run it with `tur build`/`tur run` instead of `--interpret`)
```

The same error, measured for `fs/exists?`, `fs/read-text`, `fs/write-text`
and `fs/path-join`, and in `tur repl` after `(load "stdlib/fs.tur")`.

## Root cause

Every `fs.tur` function except `fs/walk`, `fs/walk-fn` and `fs/walk-names`
has an inline-C body. The
interpreter runs an inline-C body only through a native registered under the
function's name. Otherwise it stops with the error above
(`src/turi/eval.c:13266`). Only the four `fs/tmpfile*` functions are
registered (`src/turi/interpreter_natives.c:6122`, with `TURI_CAP_FS` rows
in `src/turi/native_caps.c:167`).

Without natives:
- `fs/stat`, `-free`, `-size`, `-mtime`, `-mode`
- `fs/exists?`, `fs/file?`, `fs/dir?`
- `fs/mkdir`, `fs/mkdirp`, `fs/rmdir`, `fs/rm`, `fs/rename`, `fs/copy`
- `fs/glob`, `fs/paths-free`
- `fs/read-text`, `fs/write-text`, `fs/append-text`, `fs/read-lines`
  (and `fs/read-string`, in `stdlib/fs-string.tur`)
- `fs/read-dir`, `fs/walk-dir?`
- `fs/path-join`, `fs/free-cstr`, `fs/dup-cstr`

Knock-on effects:
- The generated doctests skip all 9 `fs.tur` examples as "interpreter
  incompatible" (`tools/run-doctests.sh`).
- `tests/run-turi.sh` never runs a fixture that uses these functions.

## Fix directions

1. **Pure helpers become Turmeric.** `fs/path-join` and `fs/dup-cstr` only
   touch bytes, so a Turmeric body works on every engine with no native.
   `fs/free-cstr` can call `cstr-free` (`stdlib/cstr.tur`).
2. **Natives for the I/O, in the order a script needs them:**
   - first `fs/read-text`, `fs/read-string`, `fs/read-lines`,
     `fs/write-text`, `fs/append-text`, `fs/exists?`;
   - then `fs/stat` and its accessors, `fs/mkdirp`, `fs/rm`, `fs/read-dir`.

   Each one is a `turi_env_register_native` call beside `fs/tmpfile`'s plus a
   `TURI_CAP_FS` row in `native_caps.c`, so the capability gate (and the web
   build, which denies FS) keeps working. The natives must return the same
   `(Result _ IoError)` shapes as the compiled bodies, errno included.
3. **Guard the parity.** Once a function has a native, a fixture that uses it
   with no inline C of its own runs under `run-turi.sh` as well, and its
   doctest example stops being skipped. Either one keeps the two engines
   agreeing.
