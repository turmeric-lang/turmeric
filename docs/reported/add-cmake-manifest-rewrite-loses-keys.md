# `tur add-cmake` rewrites `build.tur` and drops what it does not model

**Severity:** high. Running one command silently loses parts of a project's
manifest, and some of the losses change behavior.

## Repro

```turmeric
;; my project manifest -- keep this comment
(defpackage a
  :name "a"
  :version "0.1.0"
  :experiments [])
```

```
$ tur add-cmake https://example.invalid/fmt.git --ref 10.0.0
Added cmake dep 'fmt' -> https://example.invalid/fmt.git @ 10.0.0
$ cat build.tur
;;; build.tur -- project manifest for "a"
(defpackage a
  :name        "a"
  :version     "0.1.0"

  :cmake-deps #{
    "fmt" #{:url "https://example.invalid/fmt.git" :ref "10.0.0"}
  }
)
```

The comment and `:experiments []` are gone. An empty `:experiments` list is
meaningful: it suppresses the user's `~/.config/turmeric/experiments.tur`
(CLAUDE.md "Experimental Compiler Features"). So after the rewrite the
project silently picks up the user's experiments.

According to the investigation (not re-run here), the rewrite also drops
`:members`, `:subdir` and `:exports` symbol lists, and turns `:global true`
into `#{:url ""}`, which the next manifest read rejects.

## Root cause

`tur add-cmake` regenerates the whole file from the parsed `PkgManifest`
(`src/compiler/pkg.c:pkg_manifest_write`). That struct does not model every
key. `tur add` does not have this problem: it splices one form into the
existing text (`pkg_build_tur_add_spice`).

## Fix directions

- Make `add-cmake` splice its entry into the existing text, as `tur add`
  does.
- Until then, refuse to rewrite a manifest that contains keys the writer
  does not round-trip.
