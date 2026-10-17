# The float-conversion checker passes silently on a relative path

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). A tooling change, checked by hand.

**Severity:** low (tooling). CLAUDE.md tells contributors to run
`python3 tests/check-emitted-float-conversions.py FILE.tur` as the mechanism
check for float<->int conversions. Run from outside the repo root with a
relative path, it checks nothing and reports success.

## Repro

```
$ cd /some/dir && python3 /path/to/turmeric/tests/check-emitted-float-conversions.py prog.tur
check-emitted-float-conversions: 0 program(s) checked, 0 finding(s)
$ echo $?
0
$ python3 /path/to/turmeric/tests/check-emitted-float-conversions.py "$PWD/prog.tur"
check-emitted-float-conversions: 1 program(s) checked, 0 finding(s)
```

## Root cause

`lint_tur` runs `tur emit-c` with the repo root as its working directory, so a
relative input path no longer names the file. The failure is swallowed and
the file is counted as nothing to check.

## Fix directions

- Resolve every input path to an absolute path before changing directory.
- Exit non-zero when an explicit file list results in zero programs checked.

## Resolution (2026-10-17)

`tests/check-emitted-float-conversions.py` resolves every named file (and
`--tur`) to an absolute path before `tur emit-c` runs from the repo root. A
named file that does not exist exits 2, and so does a file list in which
nothing compiled to a program. `--corpus` is unchanged.
