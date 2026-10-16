# A broken `build.tur` prints an error but `tur check` exits 0

**Severity:** medium. Scripts and CI that rely on the exit status pass a
project whose manifest does not parse.

## Repro

```
$ cat build.tur
(defpackage bad
  :name "bad"
  :version "0.1.0"
$ tur check src/main.tur; echo $?
build.tur:1:1: error: unterminated list (missing ')')
0
```

A manifest with no `defpackage` form behaves the same.

## Root cause

`src/compiler/pkg.c:pkg_manifest_read_status` returns early on a read error
and on a missing `defpackage` without marking the manifest malformed. Callers
treat "no manifest" as "not in a project" and carry on.

## Fix directions

- Mark the manifest malformed on both early-return paths so callers fail.
  Add a `run-flags.sh` case for each.
