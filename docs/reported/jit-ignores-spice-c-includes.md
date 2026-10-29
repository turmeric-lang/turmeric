# `tur jit` does not pass a spice's `:c-includes` to c2mir

**Severity:** low (the program still runs, on the cc fallback). A spice that
declares `:build-opts #map{:c-includes ["c/foo"]}` and whose inline C includes
a header from there (`/* __tur_include__: #include "foo.h" */`) builds and
tests under cc -- `tur build`, `tur test` and `tur run` add `-I<spice>/c/foo`
(`drv_collect_build_aux`) -- but `tur jit` gives c2mir only the SDK include
directories (`jit_sdk_include_dirs`, `src/driver/jit.c`):

```
<tur-jit>:51:10: error in opening file tur_simd.h
tur: warning: TUR-W0070: jit engine could not compile this program (see diagnostics above); falling back to the cc path
```

Found with tur-simd (turmeric-spices): `tur jit tests/simd.tur` from
`spices/simd`.

## Cause

`cmd_jit` builds `jit_incs` from `jit_sdk_include_dirs` alone.  The REPL's
JIT path does call `drv_collect_build_aux` (`src/driver/jit.c`, the spice
reload hook) but only reads its link flags and `:c-sources`; the include
flags it collects are freed unused.  The collected flags are a shell-quoted
` -I'<path>'` string, so they cannot be handed to c2mir as they are.

## Fix directions

Have `collect_spice_aux_c_roots` (src/driver/spice.c) also return the
`:c-includes` directories as plain paths, and append them to `jit_incs` in
`cmd_jit` and the REPL hook (the array is three wide today).
