# A `ptr<Foo>` parameter accepts a `ptr<Bar>`: the pointee is not checked

**Severity: medium (silent loss of type checking).** `type_eq` treats two
typed pointers as equal only when their pointee types are equal
(`src/compiler/types.c:278-284`). Only the untyped `ptr<void>` is meant to
match any pointer. At a call, though, a `ptr<Foo>` parameter accepts a
`ptr<Bar>` argument with no diagnostic. CLAUDE.md recommends `:ptr<TheStruct>`
as a typed handle, and today that gives no more protection than `ptr<void>`.
Filed 2026-10-17 while correcting CLAUDE.md's type spellings.

## Repro

Measured on the v0.65.5 release binary with `tur check`:

```turmeric
(defstruct Foo [a : int])
(defstruct Bar [b : int])
(defn mk-bar [] : ptr<Bar>
  ```c
  return 0;
  ```)
(defn f [x : ptr<Foo>] : int 0)
(defn main [] : int
  (println (f (mk-bar)))   ; accepted, exit 0
  0)
```

The same result in every position tried:

| Shape | Result |
| --- | --- |
| `[x : ptr<Foo>]` given a `ptr<Bar>` | accepted |
| `[x :ptr<Foo>]` (unspaced) given a `ptr<Bar>` | accepted |
| `: ptr<Foo>` return type whose body returns a `ptr<Bar>` | accepted |
| `[x : ptr<int>]` given a `cstr` | refused, but the message reads `expected ptr<void>, got cstr` |

That last row is a lead. By the time the argument check runs, the parameter's
pointee seems to be gone, and the message prints the untyped spelling. The
pointer-ness of the argument is still checked.

## Root cause

Not located. `ptr_type_from_keyword_name` (`src/compiler/elab_types.c:468-527`)
does build a typed pointer with its pointee, and `type_eq` compares it. So the
loss is somewhere between the signature and the call-site comparison. The
argument checker's pointer handling in `src/compiler/elab_call.c` (for example
around `:1660-1690` and `:1912`) is the first place to look, along with
whatever records a defn parameter's type for later calls. There is no negative
fixture for the rule: `tests/fixtures/errors/` has none for a pointee
mismatch.

## Fix directions

1. Find where the pointee is dropped and keep it. Then add
   `tests/fixtures/errors/typed-ptr-pointee-mismatch/` with the repro above.
2. Run the corpus. Code that passes a typed pointer where a different one is
   declared has been compiling, so expect some real mismatches to surface.
3. Until then, CLAUDE.md steers new handles to `defopaque`, which is checked.

## Note (2026-10-21): `rc<T>` has the same hole

An `rc<U>` argument is accepted for an `rc<S>` parameter, with the callee
defined before or after the call (`(defn b [^borrow st : rc<S> k : int] ...)`
called as `(b (rc/of (U 7)) 3)` passes `tur check`). The call check
(`src/compiler/elab_call.c`, `arg_ok = (args[i]->type.kind == expected_arg_kind)`)
compares kinds, and only re-checks the full type for a struct, ADT, session or
role parameter. Found closing
[forward-call-with-rc-argument-rejected](../archive/forward-call-with-rc-argument-rejected.md).
