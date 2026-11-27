# A statement `match` with a `return` arm and a `nil` arm emits invalid C

**Severity:** low (a compile failure in the C compiler, not in `tur`; the
interpreter runs the program; easy to write around with a non-`nil` arm).

A `match` used as a statement, where one arm is `(return ...)` and another is
`nil`, gets an `int64_t` result temporary, and the `nil` arm is assigned to
it as `((void)0)`. `cc` rejects the file.

Found 2026-11-27 while fixing
[early-return-before-later-consume-leaks](../archive/early-return-before-later-consume-leaks.md).
The pre-fix compiler emits the same C.

## Repro

`r/main.tur`:

```turmeric
(defn m [k : int] : int
  (match k
    0 (return 20)
    _ nil)
  5)

(defn main [] : int
  (println (m 0))
  (println (m 1))
  0)
```

```
$ tur run r/main.tur
/tmp/tur-build/main_tur-....c: error: void value not ignored as it ought to be
$ tur --interpret r/main.tur
20
5
```

The emitted C:

```c
int64_t __t204 = 0;
int64_t __t205 = (int64_t)(intptr_t)(k);
if (__t205 == (int64_t)0LL) {
    ...
    return __t207;
}
{
    __t204 = ((void)0);
```

`_ 0` in place of `_ nil` compiles and runs. So does the same shape written
with `if`: `(if (= k 0) (return 20) nil)`.

## Root cause

Not traced. The match's result type is joined from a `!` arm and a `nil`
arm; the temporary is declared from something other than that join (it
comes out `int64_t`), while the `nil` arm is emitted as its `void` value.

## Fix directions

- Find where the match emitter picks the result temporary's C type and have
  it agree with the joined type -- `nil` when the other arms are `!`, in
  which case no temporary is needed and the arm is emitted as a statement.
- Fixture: the repro, compiled and interpreted.
