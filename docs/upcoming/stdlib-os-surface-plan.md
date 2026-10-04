# stdlib OS surface -- the fs / io / process / time gaps a typical user hits

> **Status: PROPOSED 2026-10-04.** Its three open questions were answered by
> the author the same day; see section 7. Written in response to "is the stdlib
> missing fs, os, or posix-related functions that a typical user would expect
> to see?" The answer was yes; this is the plan, ordered quick wins and pre-v1
> work first.
> **Type:** stdlib (`stdlib/fs.tur`, `io.tur`, `process.tur`, `time.tur`,
> `net.tur`, `env.tur`)
> **Related:** [`stdlib-int-stand-in-audit.md`](../reported/stdlib-int-stand-in-audit.md)
> (callbacks and container payloads typed `:int` -- a different slice of the
> same CLAUDE.md rule; this plan owns the fs/process return types and list
> arguments that audit does not cover),
> [`inline-c-results-guide.md`](../guides/inline-c-results-guide.md) (the
> `tur_ok_*` / `tur_err_*` / `tur_some_*` / `tur_none` builders every retype
> below uses).

## 0. Summary

The basic file and process operations exist. What is missing is the next layer
a user reaches for in their first hour -- reading a line from stdin, writing
through a file handle, streaming a file's lines -- and several of the functions
that *do* exist report failure through status integers and NULL sentinels
instead of `Result` / `Option`, which CLAUDE.md's "No Lazy `:int` Stand-Ins"
rule forbids for new surface.

**v1 justification, stated honestly.** tur-signal, the v1 gate, calls none of
this: its imports are entirely `signal/*`, and it touches no fs, process,
stdin, or socket API. So nothing here is on tur-signal's call surface. The case
for the pre-v1 phases is the first-hour experience of a user who is *not*
writing tur-signal, plus one timing argument: the P1 retypes are breaking
changes, and they are cheapest before a v1 compatibility promise. Evidence that
users already work around the gaps: spice test fixtures hand-roll their own
`(defn read-file [path : cstr] : cstr ...)` (e.g. the `template` spice's
`tests/fixtures/*/main.tur`) rather than use stdlib's.

## 1. Phase overview

| Phase | Theme | Breaking? | v1? | Size |
|---|---|---|---|---|
| **P0** | Quick wins: stdin lines, handle writes, time resolution, kill | No (additive; one bug fix) | Pre-v1 | ~1-2 days |
| **P1** | Typed errors: `IoError`, `Result`/`Option` returns, typed lists | **Yes** | Pre-v1 | ~3-4 days |
| **P2** | Streaming and traversal: file lines, recursive walk, richer child I/O | No | Pre-v1 (should) | ~3 days |
| **P3** | POSIX breadth: symlinks/permissions, dates, blocking sockets, signals, misc | No | Post-v1 | open-ended |

Each phase lands as its own PR (P1 may split by module). Every new function
gets a full `;;;` docstring, a fixture under `tests/fixtures/`, and a
`requires.posix-apis` marker where the body needs an API MinGW lacks.

---

## 2. P0 -- quick wins (pre-v1, additive)

### P0.1 Read a line from stdin

The Turmeric dialect has no plain line reader. Today a user's options are r7rs
`read-line` (only via the Scheme layer -- `scheme_lower.c` maps it to
`r7rs-read-line`), `async-read-stdin` (non-blocking, buffer-based), or
`read-int-console` in `effects.tur` (`scanf("%d")`).

```turmeric
(defn io/read-line [] #fx{IO} : (Option cstr))   ; none at EOF; newline stripped
(defn io/read-stdin [] #fx{IO} : (Option cstr))  ; all of stdin; none on read error
```

- `getline(3)` on POSIX; MinGW has no `getline`, so the body carries an
  `fgets`-loop fallback under `#ifdef _WIN32` (the pattern `process.tur`
  already uses) rather than a `requires.posix-apis` skip.
- The returned buffer is a fresh heap allocation owned by the caller; say so in
  the docstring (`tur_some_ptr` over a fresh `malloc`, per the results guide).
- Name check: `read-line` unqualified stays the r7rs mapping; the Turmeric
  dialect gets the `io/` prefix, matching `fs/`, `env/`, `process/`.
- Fixture: pipe a three-line stdin (one line without a trailing newline) and
  print each line plus the EOF `none`.

### P0.2 Write, seek, and append on `FileHandle`

`io.tur` has `file-open` / `file-read` / `file-close` but no write.

```turmeric
(defn file-write [^borrow fh : FileHandle buf : ptr<void> len : int] #fx{FS} : int)  ; bytes written
(defn file-write-str [^borrow fh : FileHandle s : cstr] #fx{FS} : int)
(defn file-seek [^borrow fh : FileHandle offset : int whence : SeekFrom] #fx{FS} : int)
(defn file-tell [^borrow fh : FileHandle] #fx{FS} : int)
(defn fs/append-text [path : cstr content : cstr] #fx{FS} : int)
```

- `SeekFrom` is a three-variant ADT (`SeekStart` / `SeekCurrent` / `SeekEnd`),
  not an `int` whence -- a tagged union gets a real type.
- These return `int` (byte count / offset) to match the existing `file-read`.
  P1 converts the whole family to `Result` at once; P0 does not pre-empt that
  decision piecemeal.

### P0.3 Fix `get-time-ms` resolution (bug)

`time.tur:237` implements `get-time-ms` as `time(NULL)` -- one-second
resolution behind a millisecond name. Fix to `clock_gettime(CLOCK_REALTIME)`
(`GetSystemTimePreciseAsFileTime` on Windows) and add the monotonic clock users
actually want for measuring intervals:

```turmeric
(defn time/now-ms [] #fx{IO} : int)        ; wall clock, ms since epoch
(defn time/monotonic-ns [] #fx{IO} : int)  ; for durations; never goes backwards
```

Keep `get-time-ms` as the wall-clock spelling, with its resolution fixed.
Fixture: two reads around a `sleep-ms 15` differ by >= 15 and < 1000.

### P0.4 `process/kill`

```turmeric
(defn process/kill [pid : Pid sig : Signal] #fx{Proc} : int)
```

`Signal` is a small ADT (`SigTerm` / `SigKill` / `SigInt` / `SigHup`, plus
`SigOther int` as an escape hatch) -- not a raw signal number. On Windows only
`SigKill`/`SigTerm` map (`TerminateProcess`); the rest return an error code.
Fixture: spawn `sleep 10`, kill it, and assert `process/wait` reports a
signaled exit.

---

## 3. P1 -- typed errors (pre-v1, breaking)

### P1.1 One error type

```turmeric
(defopaque IoError :int)                         ; carries errno
(defn io-error/code [e : IoError] : int)         ; the raw errno, for interop
(defn io-error/message [e : IoError] : cstr)     ; strerror(3); static storage
(defn io-error/not-found? [e : IoError] : bool)  ; ENOENT
(defn io-error/exists? [e : IoError] : bool)     ; EEXIST
(defn io-error/permission? [e : IoError] : bool) ; EACCES / EPERM
```

errno is a genuine machine integer, but it is wrapped so a signature reading
`(Result nil IoError)` says what it means, and so the predicates replace the
"compare against a magic constant" habit. Built in inline-C with
`tur_err_int(errno)`.

### P1.2 Retype the existing surface

| Function | Today | After |
|---|---|---|
| `fs/mkdir`, `mkdirp`, `rmdir`, `rm`, `rename`, `copy`, `write-text`, `append-text` | `: int` status | `(Result nil IoError)` |
| `fs/stat` | `StatInfo`, `0` on failure | `(Result StatInfo IoError)` |
| `fs/read-text` | `cstr`, `NULL` on failure | `(Result cstr IoError)` |
| `file-open` | `FileHandle` + `file-handle-ok?` | `(Result FileHandle IoError)`; `file-handle-ok?` removed |
| `file-read`, `file-write`, `file-seek`, `file-tell` | `int`, `-1` on error | `(Result int IoError)` |
| `process/run`, `process/wait` | `int` | `(Result ExitStatus IoError)` |
| `process/capture` | `: int` (**docstring already says it returns a result**) | `(Result cstr IoError)` |
| `process/cwd` | `cstr`, `NULL` on failure | `(Result cstr IoError)` |
| `io/read-line` (P0.1) | -- | unchanged: `(Option cstr)`; EOF is not an error |

`ExitStatus` is a two-variant ADT -- `Exited int` / `Signaled Signal` -- so a
caller cannot mistake a killed child for exit code 9.

### P1.3 Typed list arguments and results

`fs/glob`, `env/all`, and every `argv : int` in `process/*` pass untyped cons
lists. Move them to a typed container using the parametric-signature-over-
carrier pattern the int-stand-in audit chose (`Vec`'s shape: typed signature,
int64 carrier underneath, inline-C body unchanged).

**Decided 2026-10-04: `(Vec cstr)`** for list results (`glob`, `env/all`,
P2's `walk`) -- auto-loaded, already typed, indexable. `(List cstr)` from
`list-typed.tur` was the alternative (a zero-cost view over the existing cons
carrier) but is not auto-loaded. `argv` moves to the variadic `& args : cstr`
form, which already type-checks each element:

```turmeric
(process/run "/bin/ls" "-l" "/tmp")   ; instead of (cons "/bin/ls" (cons "-l" ...))
```

### P1.4 Merge the duplicate surfaces

`io.tur` and `fs.tur` overlap (`file-exists?` returns `int`, `fs/exists?`
returns `bool`; `read-file` vs. `fs/read-text`). Make `fs/` the filesystem
namespace and `io/` the stream/handle namespace. The duplicates go through
the deprecate-then-remove cycle in P1.5.

### P1.5 Migration

Call sites are few in-tree: ~20 fixtures reference `fs/*` / `process/*`
(mostly `fs/tmpfile`, already typed) and 3 stdlib modules. turmeric-spices is
the larger consumer -- ~85 `write-file`, ~37 `file-exists?`, ~24 `read-file`
call sites, many of them local redefinitions rather than stdlib calls. Land P1
in turmeric first, then one spices PR that migrates the callers (spices CI
re-pins turmeric `main` per run, so the spices PR must follow promptly).
**Decided 2026-10-04: deprecate, then decommission.** The release that lands
P1 keeps each old name as a thin wrapper over its replacement, marked with the
existing `^deprecated "message"` attribute (`(defn ^deprecated "use fs/exists?"
file-exists? ...)`), which warns at every use site and names the replacement.
The following minor release deletes the wrappers. The CHANGELOG entry for each
release says which of the two steps it is.

The cycle only covers names that are *going away* (the `io.tur` duplicates
from P1.4, and `file-handle-ok?`). A function that keeps its name but changes
its return type (`fs/mkdir`, `fs/stat`, `process/capture`, ...) cannot also
keep its old signature, so those are a hard break in the P1 release. That is
what forces the spices migration PR to land right behind P1: the renamed
callers can wait out the deprecation window, the retyped ones cannot.

---

## 4. P2 -- streaming and traversal (pre-v1, should)

### P2.1 Line iteration over a file

```turmeric
(defn fs/read-lines [path : cstr] #fx{FS} : (Result (Vec cstr) IoError))
(defn file-read-line [^borrow fh : FileHandle] #fx{FS} : (Result (Option cstr) IoError))
```

`read-lines` is the convenience; `file-read-line` is the bounded-memory form
for large files -- one line per call, `none` at EOF, an `IoError` on a failed
read, so errors are never swallowed. It shares its `getline` / `fgets` body
with P0.1's `io/read-line`. A lazy `Seq` was considered and not chosen:
`stdlib/seq`'s `Seq` is not parametric today (`(defstruct Seq [mk : int])`),
so `(Seq cstr)` would be exactly the untyped stand-in this plan removes. Revisit
once `Seq` takes a type parameter.

### P2.2 Recursive directory walk

```turmeric
(defn fs/walk [root : cstr] #fx{FS} : (Result (Vec cstr) IoError))
(defn fs/walk-fn [root : cstr f : (fn [cstr bool] nil)] #fx{FS} : (Result nil IoError))
```

The callback form receives `(path is-dir?)` and avoids materializing a large
tree. Does not follow symlinks (no cycles); P3 adds an option once `lstat`
exists. Windows: `FindFirstFile` / `FindNextFile` branch.

### P2.3 Richer child-process I/O

`process/capture` returns stdout only. Add:

```turmeric
(defstruct ProcOutput [stdout : cstr stderr : cstr status : ExitStatus])
(defstruct ProcOpts [stdin : (Option cstr) cwd : (Option cstr) env : (Option (Vec cstr))])

(defn process/output [opts : ProcOpts path : cstr & args : cstr]
  #fx{Proc} : (Result ProcOutput IoError))
```

`fork`/`pipe`/`execve` on POSIX (`requires.posix-apis` for the fixture);
`CreateProcess` with redirected handles on Windows, or explicitly unsupported
there with an `IoError` until `windows-remaining-plan.md` reaches it. Uses a
`defstruct` options value per the arity style guide. **Must not deadlock:**
read stdout and stderr concurrently (`poll`), not one then the other.

---

## 5. P3 -- POSIX breadth (post-v1)

Ordered by expected demand. None is started before P0-P2 land.

1. **Symlinks and permissions.** `fs/lstat`, `fs/symlink?`, `fs/readlink`,
   `fs/symlink`, `fs/chmod` (taking a `Mode` value, not a raw octal int),
   `fs/realpath`, and `stat` accessors for permission bits and `atime`.
   Windows: symlinks need developer mode or admin; return an `IoError` rather
   than pretending.
2. **Dates.** `time/utc-parts` / `time/local-parts` returning a `DateTime`
   struct, `time/format-iso8601`, `time/parse-iso8601`. No time-zone database;
   local time via `localtime_r`.
3. **Blocking sockets.** `net/tcp-connect`, `net/tcp-listen`, `net/accept`,
   `net/send`, `net/recv` on the existing linear `Socket`. Today these exist
   only in `async_socket.tur`, so a simple client means adopting the async
   module.
4. **Signals outside the reactor.** `signal/on-interrupt` (and a general
   `signal/handle`) for synchronous programs; the handler sets a flag the
   program polls, because running arbitrary Turmeric code in a signal handler
   is not async-signal-safe.
5. **Misc.** `os/hostname`, `os/uid` / `os/gid`, `term/is-tty-fd?` for an
   arbitrary `Fd`, `file-sync` (`fsync`), `fs/touch` (`utimensat`).

---

## 6. Cross-cutting rules

- **Effects.** Filesystem functions carry `#fx{FS}`, stdin/stdout `#fx{IO}`,
  processes and environment `#fx{Proc}`, sockets `#fx{Net}` -- the tags the
  existing modules already use.
- **Region stores.** None of the planned bodies writes a caller's word into
  memory that outlives a bracket: the containers they build hold fresh
  `malloc`'d strings. If any new body *does* store a caller-supplied word
  (e.g. a `ProcOpts` field retained past the call), it gets
  `TUR_REGION_NOTE` and a case in `tests/fixtures/region-escape-via-store`, per
  CLAUDE.md.
- **Ownership.** Every returned `cstr` / container is caller-owned and fresh;
  the docstring says how to free it. No `option`/`result` is declared over a
  box something else owns.
- **Windows.** Every function either has a `_WIN32` branch or returns an
  `IoError` on Windows, documented in its docstring. No silent no-ops.
- **Docs.** Each phase updates the stdlib API docs (`tur run docs`) and, once
  P1 lands, gains a short "Files and processes" section in a guide.

## 7. Decisions log

1. **2026-10-04 -- list results are `(Vec cstr)`** (section 3.3).
2. **2026-10-04 -- old `io.tur` names are deprecated for one release, then
   removed** (section 3.5).
3. **2026-10-04 -- this plan stays in `docs/upcoming/`**, not
   `docs/upcoming/v1/`. P0 and P1 are still the intended pre-v1 work; P2 may
   slip past v1 if the track needs the time.
