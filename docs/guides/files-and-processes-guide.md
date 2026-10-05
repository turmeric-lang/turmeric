---
title: Files and Processes
category: Data Structures and Libraries
description: Read and write files, walk directories, read stdin line by line, and run child processes with the typed fs / io / process stdlib -- every failure is a (Result T IoError)
---

# Files and Processes

Three on-demand stdlib modules cover the operating system:

| Module | Namespace | Load with |
|---|---|---|
| `stdlib/fs.tur` | paths: `fs/read-text`, `fs/write-text`, `fs/stat`, `fs/mkdirp`, `fs/walk`, ... | `(load "stdlib/fs.tur")` |
| `stdlib/io.tur` | streams and handles: `file-open`, `file-read-line`, `io/read-line`, ... | `(load "stdlib/io.tur")` |
| `stdlib/process.tur` | children: `process/run`, `process/output`, `process/spawn`, ... | `(load "stdlib/process.tur")` |

To print diagnostics without touching stdout, use the `eprintln` / `eprint`
builtins: they take the same argument types as `println` and write to stderr.

## Errors are values: `IoError`

Every operation that can fail returns `(Result T IoError)`. `IoError`
(`stdlib/io-error.tur`, loaded by all three modules) carries the errno:

```turmeric
(load "stdlib/fs.tur")

(defn main [] : int
  (match (fs/read-text "config.txt")
    (Ok text) (do (println text) 0)
    (Err e)   (do (if (io-error/not-found? e)
                    (eprintln "no config.txt -- using defaults")
                    (eprintln (io-error/message e)))
                  1)))
```

`io-error/message` is the `strerror(3)` text; `io-error/not-found?`,
`io-error/exists?` and `io-error/permission?` replace comparing against errno
constants, and `io-error/code` gives the raw number for interop. Operations
with no useful success value -- `fs/mkdir`, `fs/rm`, `fs/write-text`,
`process/chdir`, ... -- return `(Result nil IoError)`.

End of input is not an error: `io/read-line` answers `(Option cstr)`, `None` at
the end, and `file-read-line` answers `(Result (Option cstr) IoError)`.

## Ownership

Every returned string or vector is fresh and owned by the caller. Strings are
released with `free`; a `(Vec cstr)` from `fs/glob`, `fs/walk`, `fs/read-dir`
or `fs/read-lines` with `fs/paths-free` (strings and vector together), and
`env/all`'s with `env/all-free`. `FileHandle` and `ChildHandle` are linear:
close a file exactly once with `file-close`, reap a child exactly once with
`process/wait`.

## Files

```turmeric
(load "stdlib/fs.tur")
(load "stdlib/io.tur")

;; whole-file convenience
(fs/write-text "/tmp/notes.txt" "one\ntwo\n")
(fs/append-text "/tmp/notes.txt" "three\n")

;; bounded memory: one line per call
(defn count-lines [fh : FileHandle n : int] : int
  (match (file-read-line fh)
    (Ok o)  (match o
              (Some _) (count-lines fh (+ n 1))
              (None)   (do (file-close fh) n))
    (Err e) (do (file-close fh) -1)))

(defn main [] : int
  (match (file-open "/tmp/notes.txt" "rb")
    (Ok fh) (println (count-lines fh 0))   ; 3
    (Err e) (eprintln (io-error/message e)))
  0)
```

`file-write`, `file-write-str`, `file-seek` (with a `SeekFrom`:
`SeekStart` / `SeekCurrent` / `SeekEnd`) and `file-tell` round out the
handle API. `file-close` returns a Result too: a buffered write that fails
surfaces there.

## Directories

`fs/read-dir` lists one directory, sorted. `fs/walk` returns every path under
a root, depth first, each directory before its contents; `fs/walk-fn` visits
the same paths through a callback without building the vector. Neither follows
symlinks on POSIX.

```turmeric
(fs/walk-fn "src" (fn [path : cstr dir : bool] : nil
                    (when (not dir) (println path))))
```

## Stdin

```turmeric
(load "stdlib/io.tur")

(defn echo-lines [] : int
  (match (io/read-line)
    (Some line) (do (println line) (echo-lines))
    (None)      0))
```

`io/read-stdin` reads all of it at once.

## Processes

Arguments follow the program as separate strings; the program is also
`argv[0]`, and `PATH` is searched when it has no slash.

```turmeric
(load "stdlib/process.tur")

(defn main [] : int
  ;; run and wait; the child shares this process's stdout/stderr
  (match (process/run "make" "-j4")
    (Ok st) (println (exit-status/success? st))
    (Err e) (eprintln (io-error/message e)))   ; e.g. not-found: no `make`
  ;; collect everything
  (match (process/output (process/default-opts) "git" "status" "--short")
    (Ok o)  (do (println (.out o))
                (match (.status o)
                  (Exited c)   (println c)
                  (Signaled s) (println (signal->number s))))
    (Err e) (eprintln (io-error/message e)))
  0)
```

`ExitStatus` is `(Exited code)` or `(Signaled sig)`, so a killed child is never
read as an exit code. `process/output` reads stdout and stderr together (a
child that fills one pipe cannot deadlock it); its `ProcOpts` sets the child's
stdin text (`input`), working directory (`cwd`) and environment (`env`).
`process/capture` is the stdout-only shortcut. `process/spawn` +
`process/wait` give you the child in between -- `process/kill` it with a
`Signal` via `process/child-pid`.

## Windows

Each function has a Windows branch or reports an `IoError` there.
`process/output` is not implemented on Windows yet and answers `ENOSYS`;
`process/kill` maps only `SigKill` / `SigTerm` (both to `TerminateProcess`);
`fs/walk` follows symlinked directories there (no `lstat`).
