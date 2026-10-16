# Concurrent `tur build`s of same-named files in different directories swap programs

**Severity:** medium-high. Two builds running at once can silently produce each
other's binary.

## Repro

```sh
mkdir -p a b
printf '(defn main [] : int\n  (println "A")\n  0)\n' > a/m.tur
printf '(defn main [] : int\n  (println "B")\n  0)\n' > b/m.tur
for i in 1 2 3; do
  (cd a && tur build m.tur -o a$i) & (cd b && tur build m.tur -o b$i) & wait
done
for i in 1 2 3; do echo "a$i=$(a/a$i) b$i=$(b/b$i)"; done
```

One run printed `a3=B`: directory `a`'s binary runs directory `b`'s program.
The driver's own investigation saw 3 of 6 binaries swapped.

## Root cause

A single-file build writes its intermediate C to `/tmp/tur-build/`, at a name
from `src/main.c:stable_c_path`. That name is derived from the input path **as
typed** (`m.tur` -> `.../m_tur.c`), not its real path. Two concurrent builds
of `m.tur` from different directories write, compile and link the same
intermediate file.

## Fix directions

- Key the intermediate name on the input's `realpath` (or a hash of it), and
  include the process id or use `mkstemp` for the file actually written, then
  rename it.
- The same reasoning applies to any other `/tmp/tur-build` artifact named from
  the input.
