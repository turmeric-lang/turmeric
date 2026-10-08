# `buf_puts` breaks the incidental-NUL invariant `buf_printf` establishes, and the driver depends on it

**Severity:** medium (latent; one character away from a heap overread, and the
existing overread was a live ASan abort until WP2 backed it out)

**Found:** 2026-09-29, executing WP2 of
[security-audit-plan](../upcoming/security-audit-plan.md).

**Status: RESOLVED 2026-10-01** by fix direction 1. `buf_putc` and
`buf_write` (so `buf_puts`) now reserve one byte past what they append and
store a NUL there without counting it, which is what `buf_vprintf` did by
accident; the invariant is written down on `Buf` in `src/runtime/buf.h`. A new
`buf_truncate` shortens a Buf without breaking it, and the two direct
`src_acc.len` writes in `src/turi/env.c` use it. `buf_put_quoted`
(`src/main.c`) -- the helper WP2 had to keep on `buf_printf` with a warning
comment -- is a plain `buf_puts` again, which is precisely the swap the repro
below describes. Verified: `tests/spice-c-sources-tests.sh` passes 10/10 with
it, and with the old `buf.c` restored the same run fails with the original
`heap-buffer-overflow ... in strlen`.

## One line

`buf_printf` reserves `n + 1` bytes and lets `vsnprintf` write its NUL, so a
`Buf` built only out of `buf_printf` is *incidentally* readable as a C string
before anyone appends a terminator -- and several `src/main.c` call sites read
one that way. `buf_puts` and `buf_write` reserve exactly `n`. Swapping one for
the other is a silent heap-buffer-overflow.

## The mechanism

`src/runtime/buf.c`:

```c
void buf_write(Buf *b, const char *s, size_t n) {
    if (!n) return;
    if (b->len + n > b->cap) grow(b, b->len + n);   /* exactly n */
    memcpy(b->data + b->len, s, n);
    b->len += n;
}
void buf_puts(Buf *b, const char *s) { buf_write(b, s, strlen(s)); }

void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    int n = vsnprintf(NULL, 0, fmt, copy);
    size_t need = (size_t)n + 1;                    /* n + 1 */
    if (b->len + need > b->cap) grow(b, b->len + need);
    int written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    b->len += (size_t)written;                      /* len excludes the NUL */
}
```

`vsnprintf` always NUL-terminates, and `buf_vprintf` reserved room for that
byte but does not count it in `len`. So after any `buf_printf`, `data[len]` is
`'\0'` -- not by contract, by arithmetic.

`link_command_run` (`src/main.c`) relies on it:

```c
if (aux_includes && aux_includes->len > 0) buf_puts(&cmd, aux_includes->data);
if (aux_sources  && aux_sources->len  > 0) buf_puts(&cmd, aux_sources->data);
```

Neither buffer is explicitly terminated by its producer (`collect_spice_aux_c`,
which filled both with `buf_printf`). The `buf_puts` here does `strlen` on
them.

## Repro

Change one line in `collect_spice_aux_c` from `buf_printf(sources, " %s", path)`
to the `buf_puts` equivalent, then, with a Debug (ASan) build:

```sh
./build/tur build tests/fixtures/spices/spice-with-aux-c
```

```
ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 140
    #0 strlen
    #1 buf_puts buf.c:44
    #2 link_command_run main.c
```

## Why it is worth a report rather than a comment

Three reasons this is more than the local bug WP2 already fixed:

1. **The invariant is undocumented.** Nothing in `buf.h` or `buf.c` says a
   `buf_printf`-built Buf is readable as a C string, and nothing says a
   `buf_puts`-built one is not. Every explicit `buf_putc(&b, '\0')` in the tree
   reads as belt-and-braces rather than as the load-bearing line it is at the
   sites that have one.
2. **The fixture suite does not cover it.** 3405 fixtures passed with the
   overread in place. It needs a spice with vendored `:c-sources`, which only
   `tests/spice-c-sources-tests.sh` builds -- a dedicated-runner suite outside
   `tests/run.sh`.
3. **`buf_puts` is the obvious call.** It is the one a reader reaches for when
   appending a string, and it is wrong specifically at the sites where the
   result is later read as a C string.

## Fix directions

1. **Make the invariant real and free.** Have `buf_write` reserve `n + 1` and
   write `data[len] = '\0'` without counting it, matching what `buf_vprintf`
   already does by accident. One `grow` argument and one store; every existing
   caller keeps working, the explicit `buf_putc(&b, '\0')` sites become
   redundant rather than wrong, and the whole class disappears. This is the
   preferred direction.
2. Add `buf_cstr(Buf *)` that terminates on demand and returns `data`, and use
   it at every site that reads a Buf as a string. More churn, and it leaves the
   trap in place for the next direct `->data` read.
3. Document the asymmetry in `buf.h` and leave the code alone. Cheapest, and it
   relies on the next author reading the header.

Direction 1 costs a byte of headroom per Buf and removes the sharp edge
entirely.
