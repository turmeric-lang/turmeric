# `tur run`: shebang recipes fail when `TMPDIR` contains a space or shell metacharacter

**Severity:** low. A recipe whose body starts with `#!` exits 127 without
running when `$TMPDIR` contains a space. The same path is handed to `/bin/sh`
as shell text, so a `TMPDIR` containing `;`, `$(...)` or a backtick runs that
text. An attacker who controls your environment already controls your shell,
so this is not a privilege boundary, but a valid directory name should not
break the recipe or be interpreted as shell. Found while triaging CodeQL alert
#55 (`cpp/command-line-injection`, `justrun.c:2653`). The alert itself points
at the per-line `system(cmd)`, which runs Justfile text and does so on purpose
(WP2 D-2, documented in place). This shebang path is the one where the
command string carries text the Justfile author did not write.

## Repro

```just
bang:
    #!/bin/sh
    echo shebang-ran
```

```sh
$ mkdir -p "/tmp/my tmp"
$ TMPDIR="/tmp/my tmp" tur run bang      # v0.61.0
sh: /tmp/my: No such file or directory
$ echo $?
127
```

## Root cause

`src/compiler/justrun.c:2590` builds the script path from `tur_temp_dir()`
(that is, `$TMPDIR`), then `:2623` runs it with `system(tmpl)`. `system` passes
its argument to `sh -c`, so the path is word-split and expanded. WP2 (D-2)
moved this path from a hard-coded `/tmp` to `tur_temp_dir()` so that `TMPDIR`
would be honored. Before that change, the path never contained arbitrary
characters.

## Fix

Run the script without a shell: `fork` plus `execv(tmpl, (char *[]){tmpl, NULL})`
(the kernel still honors the shebang), with `_spawnv` or the existing Windows
branch on Windows. As a stopgap, single-quote the path before `system`. Add a
`tests/run-security-driver.sh` case that sets `TMPDIR` to a directory with a
space.
