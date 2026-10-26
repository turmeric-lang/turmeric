---
title: Files and Processes
category: Data Structures and Libraries
description: Read and write files, walk directories, manage symlinks and permissions, read stdin line by line, run child processes, handle signals, speak TCP and work with calendar dates through the typed fs / io / process / net / time / os stdlib -- every failure is a (Result T IoError)
tags:
  - files
  - io
  - processes
  - filesystem
  - tcp
---

# Files and Processes

These on-demand stdlib modules cover the operating system:

| Module | Namespace | Load with |
|---|---|---|
| `stdlib/fs.tur` | paths: `fs/read-text`, `fs/write-text`, `fs/stat`, `fs/mkdirp`, `fs/walk`, `fs/symlink`, `fs/chmod`, ... | `(load "stdlib/fs.tur")` |
| `stdlib/io.tur` | streams and handles: `file-open`, `file-read-line`, `file-sync`, `io/read-line`, ... | `(load "stdlib/io.tur")` |
| `stdlib/process.tur` | children and signals: `process/run`, `process/output`, `process/spawn`, `signal/handle`, ... | `(load "stdlib/process.tur")` |
| `stdlib/net.tur` | blocking TCP: `net/tcp-connect`, `net/tcp-listen`, `net/accept`, `net/send`, `net/recv` | `(load "stdlib/net.tur")` |
| `stdlib/os.tur` | the machine and user: `os/hostname`, `os/uid`, `os/gid` | `(load "stdlib/os.tur")` |
| `stdlib/time.tur` | clocks and dates: `time/now-ms`, `time/utc-parts`, `time/format-iso8601`, ... | `(import time)` |
| `stdlib/fs-string.tur` | a whole file as an owned `String`: `fs/read-string` | `(load "stdlib/fs-string.tur")` |

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
`io-error/exists?`, `io-error/permission?`, `io-error/connection-refused?` and
`io-error/interrupted?` replace comparing against errno constants, and
`io-error/code` gives the raw number for interop. Operations
with no useful success value -- `fs/mkdir`, `fs/rm`, `fs/write-text`,
`process/chdir`, ... -- return `(Result nil IoError)`.

End of input is not an error: `io/read-line` answers `(Option cstr)`, `None` at
the end, and `file-read-line` answers `(Result (Option cstr) IoError)`.

## Ownership

Every returned string or vector is fresh and owned by the caller. Strings are
released with `free`, except the owned `String` from `fs/read-string`
(`(load "stdlib/fs-string.tur")`), which is released with `string/release`;
a `(Vec cstr)` from `fs/glob`, `fs/walk`, `fs/read-dir`
or `fs/read-lines` with `fs/paths-free` (strings and vector together), and
`env/all`'s with `env/all-free`. `FileHandle`, `ChildHandle` and `Socket` are
linear: close a file exactly once with `file-close`, reap a child exactly once
with `process/wait`, close a socket exactly once with `net/close`.

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
surfaces there. `file-sync` goes further and asks the OS to put the data on
disk before it returns. `fs/touch` creates an empty file, or bumps an
existing one's times without truncating it.

## Directories

`fs/read-dir` lists one directory, sorted. `fs/walk` returns every path under
a root, depth first, each directory before its contents; `fs/walk-fn` visits
the same paths through a callback without building the vector. Neither follows
symlinks on POSIX.

```turmeric
(fs/walk-fn "src" (fn [path : cstr dir : bool] : nil
                    (when (not dir) (println path))))
```

`fs/walk-follow` and `fs/walk-fn-follow` do descend into symlinked
directories. A link back to a directory already on the path from the root is
reported but not entered, so a cycle cannot loop the walk.

## Symlinks and permissions

```turmeric
(load "stdlib/fs.tur")

(fs/symlink "config.prod.toml" "config.toml")   ; target, then the link
(fs/symlink? "config.toml")                     ; => true
(fs/readlink "config.toml")                     ; => (Ok "config.prod.toml")
(fs/realpath "config.toml")                     ; => (Ok "/srv/app/config.prod.toml")
```

`fs/stat` follows a link and `fs/lstat` describes the link itself. Read either
`StatInfo` with `fs/stat-kind` (`KindFile` / `KindDir` / `KindSymlink` /
`KindOther`), `fs/stat-size`, `fs/stat-mtime`, `fs/stat-atime` and
`fs/stat-perms`.

Permissions are a `Mode` value, not an octal integer. Build one from its `ls`
spelling, or adjust the bits you read:

```turmeric
;; chmod u+x
(match (fs/stat "run.sh")
  (Ok s)  (let [r (fs/chmod "run.sh" (mode/with (fs/stat-perms s) (PermUser) (PermExec)))]
            (fs/stat-free s)
            r)
  (Err e) (Err e))

(match (mode/parse "rw-r-----")
  (Some m) (fs/chmod "secret.txt" m)
  (None)   (Err (io-error/of-code 22)))
```

`mode/has?`, `mode/without` and `mode/format` (back to `"rwxr-xr-x"`) take
the same `PermUser` / `PermGroup` / `PermOther` and `PermRead` / `PermWrite`
/ `PermExec` arguments.

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

## Signals

A synchronous program can catch a signal instead of dying of it. The handler
only records that the signal arrived, because running arbitrary code inside a
signal handler is not safe. The program polls:

```turmeric
(load "stdlib/process.tur")

(defn work-loop [n : int] : int
  (if (signal/interrupted?)
    (do (eprintln "stopping") n)
    (do (step) (work-loop (+ n 1)))))

(defn main [] : int
  (signal/on-interrupt)          ; Ctrl+C no longer kills the program
  (work-loop 0)
  0)
```

`signal/handle` catches any `Signal`. `signal/pending?` peeks at it, and
`signal/take!` reports it and clears it. `signal/ignore` and `signal/reset`
set the other two dispositions. A caught signal also wakes a blocking
`net/accept` or `net/recv` with an `io-error/interrupted?` error, so a server
waiting for its next client can check the flag and shut down. Do not combine
`signal/handle` with `reactor-add-signal` for the same signal.

## TCP sockets

`stdlib/net.tur` is blocking, one call at a time. Programs on the cooperative
scheduler use `stdlib/async_socket.tur` instead.

```turmeric
(load "stdlib/net.tur")

(defn fetch [host : cstr] : int
  (match (net/tcp-connect host 80)
    (Ok s)  (do (net/send s "GET / HTTP/1.0\r\n\r\n")
                (match (net/recv s 4096)
                  (Ok o)  (match o (Some text) (println text) (None) (println "<closed>"))
                  (Err e) (eprintln (io-error/message e)))
                (net/close s)
                0)
    (Err e) (do (eprintln (io-error/message e)) 1)))
```

A server is `net/tcp-listen` (port 0 picks a free port, which
`net/local-port` reports), then `net/accept` in a loop. `net/recv` answers
`(Ok (None))` once the peer closes, and `net/shutdown-write` is the half-close
that tells the peer you are done sending. Writing to a closed peer is an
`EPIPE` error, never a SIGPIPE.

## Dates and times

`(import time)` brings the clocks (`time/now-ms`, `time/monotonic-ns`) and
calendar dates. A `DateTime` is a plain struct, with fields `year`, `month`,
`day`, `hour`, `minute`, `second`, `millis` and `offset` (minutes east of
UTC):

```turmeric
(defmodule app
  (import time)
  (defn main [] : int
    (let [now (time/now-ms)
          utc (time/utc-parts now)
          loc (time/local-parts now)]       ; the process's time zone (TZ)
      (println (time/format-iso8601 utc))   ; 2026-10-21T09:30:00.123Z
      (println (time/format-iso8601 loc))   ; 2026-10-21T11:30:00.123+02:00
      (println (= (time/datetime->ms loc) now)))  ; true
    (match (time/parse-iso8601 "2026-10-21T11:30:00+02:00")
      (Some dt) (println (.hour dt))        ; 11
      (None)    (println "not a timestamp"))
    0))
```

There is no time-zone database: UTC is computed directly, and local time comes
from the C library (`localtime_r`).

## The machine and the user

`stdlib/os.tur`: `os/hostname` answers `(Result cstr IoError)`, and `os/uid` /
`os/gid` answer typed `Uid` / `Gid` values (`uid->int` / `gid->int` for the
number). `term/is-tty-fd?` (`stdlib/term.tur`) asks whether any `Fd` is a
terminal.

## Windows

Each function has a Windows branch or reports an `IoError` there.
`process/output` is not implemented on Windows yet and answers `ENOSYS`;
`process/kill` maps only `SigKill` / `SigTerm` (both to `TerminateProcess`);
`fs/walk` follows symlinked directories there (no `lstat`). `fs/lstat` is
`fs/stat`, `fs/readlink` answers `ENOSYS`, `fs/realpath` does not resolve
links, and `fs/symlink` needs Developer Mode or administrator rights (`EPERM`
otherwise). `fs/chmod` keeps only the owner-write bit (read-only or not).
`signal/handle` catches only `SigInt` and `SigTerm`. `os/uid` / `os/gid` answer
`ENOSYS`, because Windows has no numeric user ids.
