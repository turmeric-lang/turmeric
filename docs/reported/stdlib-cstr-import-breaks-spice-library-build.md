# Importing stdlib's `cstr` module breaks `tur build <spice-dir>` (`cstr.h` not found)

**Severity: medium (a spice that imports `cstr` type-checks, runs and tests
fine, but its library cannot be built).** Found 2026-10-29 executing
stats-formula-plan.

## Repro

```sh
mkdir -p cs/src/cs && cd cs
cat > build.tur <<'X'
(defpackage cs-repro :name "cs-repro" :version "0.1.0"
  :exports #map{"cs/a" ["a-len"]})
X
cat > src/cs/a.tur <<'X'
(defmodule cs/a
  (export a-len)
  (import cstr :refer [cstr-len])
  (defn a-len [s : cstr] : int (cstr-len s)))
X
tur check src/cs/a.tur   # ok
tur build .              # fails
```

```
build/obj/cs__a.h:26:10: fatal error: cstr.h: No such file or directory
```

`build/obj/` holds `cs__a.{c,h}` and `tur_runtime.{c,h}` but no `cstr.h`:
the header names the imported stdlib module's header, and the project build
never emits one for a stdlib module. `tur run`, `tur test` and `tur check`
are unaffected, which is why a spice CI that runs only those stays green.
Reproduces on `main` (1ecdacc9) and v0.72.0. No spice on turmeric-spices
`main` imports `cstr`, so nothing is broken there today.

## Fix directions

- Emit (or skip `#include`-ing) the headers of imported stdlib modules in the
  project build, the way `tur run` compiles them into one unit.
- A fixture: a manifest-rooted project importing `cstr`, built with
  `tur build <dir>`.

## Workaround

Local byte helpers (spices/stats-formula/src/formula/text.tur).
