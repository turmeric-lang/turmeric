---
title: Option niche -- size measurement results
category: Benchmarks
description: SR3 item 4. The per-value claim is exact and confirmed (16 -> 8, half of it alignment padding). The population it applies to is two monomorphs in eight files, not the census the plan published -- which counted `(Option cstr)` and docstring text. And the size trade is not one-directional: the niche costs 184-310 bytes of emitted code per translation unit that uses it.
---

# Option niche -- size measurement

> Measured 2026-08-30, when the niche was `--enable=option-niche` and the
> tagged form the default.  The niche graduated on 2026-09-03; the
> instruments here now key the tagged side on `TUR_OPTION_NICHE=0` and the
> niche side on the default, and "default" in the tables below means the
> tagged form.

Item 4 of [sr3-option-niche-plan.md](../../docs/archive/sr3-option-niche-plan.md):
*"A size measurement worth the name. The gate measured correctness, not bytes.
The claim is 16 -> 8 per value on the eligible population; nobody has run
SR0(a)'s instrument over it since the population changed."*

Measured 2026-08-30, `v0.42.0` Debug build (`./build/tur`), x86-64 Linux,
`cc -O2`. Three instruments in this directory; raw census in `census.json`.

**Three findings, in descending order of how much they change the picture.**

## 1. The per-value claim is exact, and half of it is padding

The typedef extracted verbatim from `tur emit-c` (default path) for
`(Option String)`:

```c
typedef struct tur_adt_Option__String {
    int tag;
    union { struct { } None; struct { void * _0; } Some; } as;
} tur_adt_Option__String;
```

| | bytes |
|---|---:|
| default `sizeof(tur_adt_Option__String)` | **16** |
| niche `sizeof(void *)` | **8** |

So 16 -> 8 holds exactly. The composition is worth recording because the plan
never states it: **4-byte tag + 4 bytes of alignment padding + 8-byte
payload.** Half of what the niche recovers is padding the tag's alignment
forces, not the tag. The win therefore does not shrink if the tag is ever
narrowed to a byte -- `sizeof` would still be 16.

## 2. The population is two monomorphs in eight files, and the plan's census counted the wrong thing

Swept 2352 inputs (every non-`errors/` fixture, plus `stdlib/` and
`examples/`), emitting each twice. **Eligibility is decided by the compiler,
not re-derived**: a monomorph is eligible exactly when its
`tur_adt_Option__*` typedef is present by default and absent under
`--enable=option-niche`.

| | count |
|---|---:|
| inputs censused | 2310 |
| inputs that did not emit by default (pre-existing, not niche-related) | 42 |
| inputs that emit by default and **fail** under the flag | **0** |
| Option monomorph instances seen -- **ineligible** | 4736 |
| Option monomorph instances seen -- **eligible** | **8** |
| **distinct** eligible monomorphs | **2** |

The whole eligible population of the corpus:

| monomorph | files |
|---|---|
| `tur_adt_Option__String` | `stdlib/httpd-string.tur`, and 6 fixtures (`httpd-req-string-opt`, `inline-c-option-byval-param`, and the four `option-niche-*`) |
| `tur_adt_Option__Vec__int` | `option-of-tvec-eq` (1 fixture) |

**The plan's census does not survive contact with the emitter.** It reads:

> the `(Option String)` census the plan named -- `env` (5 spellings),
> `httpd-string` (5), `args` (2), `re` (2), `docstrings` (2) -- is in scope.

Checked one by one against the source, comments and string literals stripped:

- **`env` (5)** are `(Option cstr)`, not `(Option String)`. Verified
  ineligible by the oracle: `tur_adt_Option__cstr`'s typedef is emitted **and
  survives** under the flag. `cstr` is a raw `const char *` -- not an opaque,
  so it cannot carry `:non-null`, and a null `cstr` is a legal value.
- **`args` (2)** are `(Option cstr)` and `(Option ArgResult)`.
- **`re` (2)** are `(Option cstr)`.
- **`docstrings` (2)** are inside **string literals** -- the documentation text
  of `httpd-req-cookie-opt` / `httpd-req-form-opt`. Not code.
- **`httpd-string` (5)** is the only real row -- **and it is 2, not 5.**
  Stripping strings and comments leaves exactly two `(Option String)` API
  functions in the file, `httpd-req-cookie-opt` and `httpd-req-form-opt`; the
  other three hits are their own docstrings. It is the one file the emitted
  census independently found.

**So the eligible stdlib API surface of the whole language is two functions.**
Every one of the five published rows is either the wrong type or inflated by
its own documentation.

Payload spellings across `stdlib/` + `tests/fixtures/`, strings and comments
stripped:

| payload | occurrences | eligible? |
|---|---:|---|
| `int` | 122 | no -- not a pointer |
| **`cstr`** | **34** | **no -- no `:non-null`, and a null cstr is legal** |
| `A` (a tyvar) | 19 | no -- erased |
| **`String`** | **13** | **yes** |
| `float` | 12 | no |
| everything else | 14 | mostly no |

So `cstr` is the largest pointer-payload Option population in the tree at
**2.6x** the eligible one (34 against 13), and **4x** counting only real
stdlib API surface (8 sites in `env`/`re`/`args` against 2 in
`httpd-string`) -- and it is the population the niche cannot reach.

**`cstr` is not un-annotated, it is structurally unreachable.**
`sr3_payload_is_nonnull_pointer` (types.c:1483) begins by extracting an
`AdtDef` from the payload and returns false when there is none. `cstr` is
`TY_CSTR`, a builtin TypeKind c-named `const char *` -- not an ADT, not a
`defopaque`, and so not a thing `:non-null` can be written on. No allowlist
row or annotation reaches it; the eligibility machinery only speaks ADTs.

And the irony is total: `env/get` **already implements the niche by hand.**

```turmeric
(defn env/get [name : cstr] #fx{Proc} : (Option cstr)
  (let [v (env/get-raw name)] (if (= v 0) (none) (some (:: v :cstr)))))
```

It tests the raw pointer against 0 and maps null to `(none)`, so the invariant
the niche needs -- a payload inside a `Some` is never 0 -- is established
there by construction. It is the ideal candidate semantically and out of
reach representationally.

Nor is switching those sites to `String` a migration path. `getenv` returns a
borrowed pointer into the environment block; `String` is an owned handle whose
constructor mallocs unconditionally -- which is precisely the warrant for its
`:non-null`. Retyping `env/get` to `(Option String)` would add an allocation
and a copy per lookup to save 8 bytes of stack. The `cstr` population is not
un-migrated, it is genuinely a different thing.
That is the finding with a future in it: the plan unshelved slice B because
giving `defopaque` a pointer C spelling "makes `String` eligible and is the
whole census." `String` was made eligible and it is **not** the whole census;
`(Option cstr)` is two and a half times larger and needs a different key --
non-nullness on a builtin pointer type, which nothing can currently declare.

## 3. The size trade is not one-directional: the niche costs code

Measured as the emitted translation unit compiled to an **object** (`-c`), so
the number is exactly the emitted code -- no libturi to cancel out, and no
sanitizer runtime to inflate it.

| fixture | default | niche | delta |
|---|---:|---:|---:|
| `option-niche-string` | 11323 | 11633 | **+310** |
| `inline-c-option-byval-param` | 11390 | 11638 | **+248** |
| `option-of-tvec-eq` | 12371 | 12555 | **+184** |
| *control:* `adt-basic` | 10954 | 10954 | **+0** |
| *control:* `option-basic` | 11921 | 11921 | **+0** |
| *control:* `adt-recursive` | 11180 | 11180 | **+0** |

(bytes of `.text`)

**The control is the load-bearing row.** Three fixtures with no eligible
monomorph are byte-identical under the flag -- which is an object-code proof
of the inertness the corpus result ("2712/0 both ways") asserts at the level
of test outcomes, and it is what licenses reading the other three deltas as
the niche's own cost rather than compiler noise.

Where the extra code comes from: the niche has to **enforce** what the default
representation can simply represent. A null payload is a legal value in a
16-byte tagged Option and an impossibility in an 8-byte niche one, so the
niche emits the `:non-null` check at the `Some` constructor and
`tur_opt_value_checked` at the carrier crossing (2 abort sites in the niche
emission of `option-niche-string` against 1 by default). That is the same
declaration-is-not-a-proof gap that keeps the experiment a prototype, showing
up as bytes.

**Do not over-read this.** 200-300 bytes is immaterial in absolute terms, as
is 8 bytes per value. The finding is the *direction*: the niche is not a
size win at the program level, it is a data-size win paid for in code size,
and on this corpus both effects round to nothing. Break-even is roughly 25-40
simultaneously-live eligible Options in one translation unit, which the
population above says does not occur.

## What this measurement does NOT produce, and why

**No aggregate "bytes saved" figure.** Two reasons, both of which would make
such a number a fabrication:

- **Per-value bytes need live values, not sites.** The census counts declared
  storage slots in emitted C, and it counts EMISSION sites -- stdlib's Option
  machinery re-emits per compilation unit, so a corpus-wide sum is one stdlib
  body times the file count. That is the trap the
  [CE0 census](../../docs/artifacts/ce0-container-element-census.md) recorded
  from the other side, where its 4321 class-3 reads turned out to be one
  `vec-eq-loop` body times the fixture count. Per-file counts run 7-32 for
  files using a handful of Options, which is that inflation visible.
- **Container elements are already known to be at exact parity.** Both
  representations materialize the same carrier box at the erased boundary
  ([filed](../../docs/archive/option-niche-container-elements-box-at-parity.md)),
  so the 8 bytes apply to direct positions only -- locals, params, returns,
  match, struct fields.

## Coverage notes -- read before quoting these numbers

- **42 inputs did not emit by default** and are outside the census. They fail
  on the default path, so they are pre-existing and not the flag's doing; the
  count is reported rather than swallowed, per SR0(a)'s discipline.
- **`stdlib/httpd-string.tur` is in the census but not the code-size table.**
  It does not compile standalone (`httpd_tls_ops undeclared`) -- a module
  needing its siblings, not a niche defect.
- **`option-niche-crossings`'s DEFAULT emission does not compile**
  (`invalid initializer: tur_adt_Option__String __scrut_v = (__ps_266)`).
  That is a second instance of
  [inline-c-carrier-producer-byval-container-element](../../docs/archive/inline-c-carrier-producer-byval-container-element.md)
  on a different crossing -- a match scrutinee rather than a `vec-of` element
  -- and it is invisible to CI because the fixture carries
  `flags: --enable=option-niche` and so is never built the other way.
- **One machine, one ABI.** `sizeof` is x86-64 SysV. The 16 is
  alignment-driven, so a 32-bit target would show 8 -> 4 and the same ratio.
