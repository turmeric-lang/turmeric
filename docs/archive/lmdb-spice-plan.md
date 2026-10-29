# `spices/lmdb`: LMDB bindings, an embedded key-value store

> **Status:** DONE, archived 2026-10-29. L0-L4 implemented 2026-10-25 and
> merged in both repos (turmeric #1376, turmeric-spices #117); section 11
> records what landed and where it departs from the draft. Section 7's
> Windows assessment, which needs a Windows box, has not been run; it is
> tracked in
> [windows-remaining-plan](../upcoming/windows-remaining-plan.md#lmdb-spice----windows-assessment)
> and still uses the probe and results table in section 7 here.
> **Track:** post-v1 -- nothing on the v1 line depends on it.
> **Type:** spice (in `../turmeric-spices/`). The draft expected no compiler
> change; building it found six compiler defects, fixed on turmeric's
> `lmdb-spice` branch (section 11), which the spice needs.

## 0. Summary

`spices/sqlite` gives Turmeric an embedded SQL database. There is no
key-value counterpart: nothing you can open as a file, put bytes into by key,
and read back after a restart, without SQL.

This plan adds `spices/lmdb`: [LMDB](https://github.com/LMDB/lmdb) vendored
as C, with a Turmeric API that **follows LMDB's own shape** -- environments,
transactions, named databases, cursors -- typed the Turmeric way (linear
handles, `Option`, `Result`). It is a binding, not an abstraction: it exposes
what LMDB is good at (MVCC read transactions, sorted keys and range scans,
duplicate-sorted databases, multi-key atomic writes) rather than flattening
it into a dbm.

The distilled API comes later, separately (section 2): a multi-adapter `kv`
spice in the spirit of Ruby's [moneta](https://github.com/moneta-rb/moneta),
with `lmdb` as one of its backends.

## 1. Why LMDB

The survey (2026-10-24) weighed:

| Library | License | Verdict |
| --- | --- | --- |
| **LMDB** | OpenLDAP Public License 2.8 (BSD-style) | **Chosen.** Two C files plus two headers; memory-mapped B+tree; ACID, MVCC (readers never block the writer), no recovery step after a crash; zero-copy reads. 0.9.33 (May 2024) is the current maintenance release. Linux, macOS, Windows. ~30 kB compiled. |
| libmdbx | Apache-2.0 | An LMDB fork with auto-growing files and more API. Rejected for governance, not quality: since December 2025 it ships only as an amalgamation, and most of its test suite is no longer public. |
| TinyCDB | public domain | Constant database: built once, then read-only. Not a general store; a possible later `kv` backend for shipped lookup tables. |
| UnQLite | BSD | Key-value plus a document store; bigger than we need; last release 2022. |
| Tkrzw | Apache-2.0 | Kyoto Cabinet's successor. C++ with a C wrapper; heavier build. |
| GDBM / ndbm | GPL-3 | License incompatible with an MIT spice. (Racket's `file/dbm` wraps the system dbm, which is GDBM on most Linux.) |
| Berkeley DB | AGPL-3 (6.0+) | License. |

What LMDB asks of a binding (each is designed for in section 4):

- **A maximum size, set up front** (`mdb_env_set_mapsize`); a write past it
  fails with `MDB_MAP_FULL`. On Linux and macOS the file only grows as data
  is written; **on Windows 0.9.33 extends the file to the whole map size when
  it opens it** (`mdb.c` 4067-4075: "Windows won't create mappings for zero
  length files and won't map more than the file size").
- **A database is a directory by default**; `MDB_NOSUBDIR` makes it one file
  plus a `-lock` file.
- **Files are not portable** across word size or byte order.
- **One writer at a time** per environment, and a write transaction must end
  on the OS thread that began it.
- **Keys are at most 511 bytes** by default (`mdb_env_get_maxkeysize`).
- **Pointers into the map are valid only until the transaction ends.**

## 2. Where this sits: today a binding, later a `kv` adapter

The eventual goal is a multi-adapter key-value spice, `kv`, after Ruby's
moneta: one small API (get, put, delete, has, maybe increment and expiry)
over interchangeable backends -- in-memory, LMDB, valkey, sqlite, a file
tree. That spice owns the "generic map" question; this one does not.

So `lmdb` is written to be **a good adapter target, not the generic API**:

- It exposes LMDB faithfully, so the `kv` adapter can use whatever it needs
  (one-shot transactions for single operations, a cursor for key listing,
  `MDB_NOOVERWRITE` for put-if-absent) without `lmdb` guessing that API now.
- It does not try to look like a dictionary or implement a map typeclass.
- Section 8 (L4) closes with an adapter-readiness review: write the `kv`
  operations against `lmdb` in a scratch file and fix anything awkward
  here, before the `kv` spice starts.

## 3. Layout and build

```
spices/lmdb/
  build.tur
  README.md               -- so the spice appears on the docs site
  c/lmdb/                 -- LMDB, vendored verbatim (never src/)
    lmdb.h  midl.h  mdb.c  midl.c
    LICENSE  COPYRIGHT    -- the OpenLDAP license notice, which must ship
    VENDORED.md           -- upstream tag, commit, date, and how to refresh
  src/lmdb/
    error.tur             -- LmdbError and the MDB_* / errno mapping
    env.tur               -- Env: create, open, close, map size, sync, stat, copy
    txn.tur               -- Txn: begin, commit, abort, nested, with-*-txn
    dbi.tur               -- Dbi: open named databases, flags, drop, stat
    ops.tur               -- get / put / del on (txn, dbi), put flags
    cursor.tur            -- Cursor and its positioning operations
  tests/                  -- see section 6
  errors/                 -- linear-handle negative fixtures, as sqlite has
```

LMDB has a Makefile and no CMake build, and it is two `.c` files, so it is
**vendored**, not fetched -- the pattern the spices guide describes and
`spices/raygui` and `spices/signal` already use:

```turmeric
:build-opts #map{
  :c-includes ["c/lmdb"]
  :c-sources  ["c/lmdb/mdb.c" "c/lmdb/midl.c"]
  :link-libs  ["pthread"]          ;; robust mutexes on Linux; harmless on macOS
}
```

Windows may need more here; section 7's A1 decides. (0.9.33 references no
`Nt*` functions -- that `ntdll` dependency arrives with LMDB's master branch,
which maps growable files on Windows -- so none is expected.)
`:c-sources` propagate through the `:spices` closure, so a spice depending on
`lmdb` (the future `kv`) links LMDB with no further declaration.

Pin **0.9.33** unless a later tag is confirmed clean on all CI platforms. A
0.9.34 bug report mentions a macOS arm64 compile error in `mdb.c`; check
before taking it. LMDB's command-line tools (`mdb_stat`, `mdb_dump`, ...) are
not vendored; `VENDORED.md` says where to get them for debugging.

## 4. API

House rules throughout: `defopaque` handles, `(Option T)` for "may be
absent", `(Result T LmdbError)` for "may fail", no `:int` stand-ins. Handles
that must be released are `:linear` with `^borrow` accessors, as
`sqlite/db`'s `Db` and `Stmt` are, so a leaked or double-closed handle is a
compile-time error under `-Xsubstructural`.

### 4.1 The model, as LMDB has it

| LMDB | Turmeric | Release |
| --- | --- | --- |
| `MDB_env *` | `Env` (`:linear`) | `env-close` |
| `MDB_txn *` | `Txn` (`:linear`) | `txn-commit` / `txn-abort`, or scoped by `with-*-txn` |
| `MDB_dbi` | `Dbi` (plain) | none needed: LMDB keeps it for the env's life |
| `MDB_cursor *` | `Cursor` (`:linear`) | `cursor-close` |
| `MDB_val` | `cstr` or `Bytes` in, a fresh copy out | -- |

### 4.2 Environments

```turmeric
(env-open path opts)                 ;; -> (Result Env LmdbError); EnvOpts below
(env-close env)                      ;; consumes
(env-set-map-size! ^borrow env n)    ;; grow (or shrink to the data) between txns
(env-info ^borrow env)               ;; -> EnvInfo: map size, last page, readers
(env-stat ^borrow env)               ;; -> DbStat of the main database
(env-sync ^borrow env)               ;; force a flush (for :sync :none)
(env-copy ^borrow env path compact?) ;; hot backup (mdb_env_copy2, MDB_CP_COMPACT)
(env-reader-check ^borrow env)       ;; -> stale reader slots cleared (int)
(env-max-key-size ^borrow env)       ;; -> int
```

`EnvOpts` is a struct (more than five knobs, per the arity guide):
`map-size` (default 1 GiB on Linux/macOS, Windows per section 7),
`max-dbs` (named databases; default 16), `max-readers`, `read-only`,
`subdir` (default false: `MDB_NOSUBDIR`), `sync` (`:full`, `:no-meta-sync`,
`:none`), and `create` (create the file if absent). Every env is opened with
**`MDB_NOTLS`** (4.6).

### 4.3 Transactions

```turmeric
(txn-begin  ^borrow env read-only?)  ;; -> (Result Txn LmdbError)
(txn-begin-nested ^borrow parent)    ;; write txns only: a savepoint
(txn-commit txn)                     ;; consumes; -> (Result nil LmdbError)
(txn-abort  txn)                     ;; consumes

(with-read-txn  ^borrow env (fn [txn] ...))   ;; -> (Result T LmdbError)
(with-write-txn ^borrow env (fn [txn] ...))   ;; commits if the fn returns Ok,
                                              ;; aborts on Err or panic (defer)
```

The scoped forms are the recommended path: the transaction cannot outlive the
closure, and abort-on-failure cannot be forgotten. The raw begin/commit/abort
stay, linear, for callers (and the `kv` adapter) that need them.

LMDB's read-transaction reset/renew (`mdb_txn_reset` / `mdb_txn_renew`, which
let a long-lived reader skip re-allocation) is L3 if a benchmark wants it.

### 4.4 Named databases

```turmeric
(dbi-open ^borrow txn name flags)    ;; name: (Option cstr) -- None is the main db
                                     ;; -> (Result Dbi LmdbError)
(dbi-stat ^borrow txn dbi)           ;; -> DbStat: entries, depth, pages
(dbi-drop ^borrow txn dbi)           ;; empty it
(dbi-delete ^borrow txn dbi)         ;; remove it from the env
```

`flags` is a small set, not a bit mask: `:create`, `:dup-sort`,
`:dup-fixed`, `:integer-key`, `:reverse-key`. `:dup-sort` (several sorted
values per key) is the LMDB feature most worth having that a dbm lacks:
indexes and one-to-many maps without encoding lists into values.

### 4.5 Reading, writing, cursors

```turmeric
(get  ^borrow txn dbi key)                   ;; -> (Option cstr), copied out
(put! ^borrow txn dbi key val)               ;; -> (Result nil LmdbError)
(put-new! ^borrow txn dbi key val)           ;; MDB_NOOVERWRITE: -> (Result bool ...)
(del! ^borrow txn dbi key)                   ;; -> (Result bool ...); false if absent
(del-dup! ^borrow txn dbi key val)           ;; one value of a dup-sort key

(cursor-open ^borrow txn dbi)                ;; -> (Result Cursor LmdbError)
(cursor-close c)
(cursor-first ^borrow c) (cursor-last ^borrow c)
(cursor-next  ^borrow c) (cursor-prev ^borrow c)
(cursor-seek  ^borrow c key)                 ;; MDB_SET_RANGE: first key >= key
(cursor-seek-exact ^borrow c key)            ;; MDB_SET_KEY
(cursor-next-dup ^borrow c) (cursor-next-nodup ^borrow c)
;; each -> (Option (Pair cstr cstr)), the entry it lands on, copied out
(cursor-put! ^borrow c key val) (cursor-del! ^borrow c)
```

Byte-valued twins (`get-bytes`, `put-bytes!`, ...) take and return stdlib
`Bytes` (L3). A `cstr` is stored without its NUL, so a `cstr` key and the
`Bytes` key holding the same bytes are the same key.

`:append` (`MDB_APPEND`, bulk load of pre-sorted keys) and `:reserve`
(write straight into the map) are L3, behind their own functions, since both
can corrupt order or memory if misused.

### 4.6 Errors

```turmeric
(defdata LmdbError
  (MapFull)                ;; MDB_MAP_FULL: grow with env-set-map-size!, retry
  (KeyTooLong int)         ;; the key's length; max is env-max-key-size
  (ReadersFull)            ;; MDB_READERS_FULL
  (DbsFull)                ;; MDB_DBS_FULL: raise EnvOpts max-dbs
  (TxnFull)                ;; MDB_TXN_FULL: too many dirty pages in one txn
  (NotADatabase cstr)      ;; MDB_INVALID / MDB_VERSION_MISMATCH, with the path
  (Incompatible)           ;; MDB_INCOMPATIBLE: dbi flags differ from the file's
  (ReadOnly)               ;; a write in a read-only txn or env
  (Io int cstr))           ;; errno and mdb_strerror text
```

`MDB_NOTFOUND` is never an error: it is `None` from a get or a cursor move
and `false` from a delete. `MDB_KEYEXIST` is `false` from `put-new!`.

## 5. Decisions the binding makes

**Map size.** Default 1 GiB on Linux and macOS, where an unused map costs no
disk (a 1 GiB map holding one record is a 12 KiB file). On Windows the file
is extended to the full map size at open (section 1), so the default there
must be small -- 64 MiB unless section 7's A2 shows the extension is cheap
-- with `with-write-txn-growing` doing the rest. The binding does **not** grow the map behind the
caller's back: a write returns `MapFull`, and the caller calls
`env-set-map-size!` (allowed when the process has no transaction open) and
reruns the transaction. A helper, `(with-write-txn-growing env max fn)`,
does exactly that loop -- doubling up to `max` -- for callers who want it;
the `kv` adapter will.

**One file.** `MDB_NOSUBDIR` by default: `path` is the data file and
`path-lock` sits beside it. `EnvOpts :subdir true` gives LMDB's directory
layout, for files shared with LMDB's own tools.

**Durability.** Full sync on commit by default (LMDB's default).
`:no-meta-sync` (`MDB_NOMETASYNC`) and `:none` (`MDB_NOSYNC`, for caches and
tests) are opt-in. `MDB_WRITEMAP` is never used: a stray pointer write could
then corrupt the file.

**Threads and fibers.** `MDB_NOTLS` always, so a read transaction is not
bound to the OS thread that began it -- Turmeric's async fibers can resume on
another thread. A write transaction is still bound to its thread by LMDB's
writer mutex; `with-write-txn` keeps it inside one synchronous call, and the
docs say not to `await` inside it.

**Copies out.** Every key and value handed back to Turmeric is copied out of
the map, because LMDB's pointers die with the transaction. A zero-copy
borrowed read, typed so it cannot escape `with-read-txn`, is a possible later
addition once the copying API has measured costs (L4).

**Key length.** Checked before the call: a key over `env-max-key-size` is
`KeyTooLong`, not LMDB's generic `MDB_BAD_VALSIZE`.

## 6. Testing

In `spices/lmdb/tests/`, run by `tur test`. The two silent traps in the spices
CLAUDE.md apply: every `describe` inside a `defn`, and `main` returns
`run-all-status`. Each test works in its own temporary directory, closes
every env, then removes the directory (on Windows an open file cannot be
deleted -- section 7's A6).

- **Persistence:** open, put in a write txn, close, reopen, get.
- **Transactions:** a write txn whose fn returns `Err`, or panics, leaves
  nothing behind; a reader begun before a commit still sees the old value
  (MVCC); a nested txn that aborts leaves its parent's writes intact.
- **Named databases and dup-sort:** two `Dbi`s in one env are independent;
  a `:dup-sort` key holds several values, visited in order by
  `cursor-next-dup`; `del-dup!` removes one.
- **Cursors:** `cursor-seek` lands on the first key >= its argument (a prefix
  scan); `first`/`last`/`next`/`prev` walk the sorted order both ways.
- **Limits:** a 511-byte key accepted and a 512-byte one `KeyTooLong`; a
  1 MiB map filled to `MapFull`; `with-write-txn-growing` gets past it; with
  its max at the current size, it returns `MapFull`.
- **Errors:** read-only env refuses writes (`ReadOnly`); a non-LMDB file is
  `NotADatabase`, not a crash; reopening a `:dup-sort` db without the flag
  is `Incompatible`.
- **Backup:** `env-copy` with compaction produces a file that opens and holds
  the same entries.
- **Threads:** readers on two OS threads while a third writes, on the
  threaded scheduler.
- **Linear handles:** `errors/` fixtures for a leaked `Env`, an unfinished
  `Txn` and a double `cursor-close` (TUR-E0100 / E0101 under
  `-Xsubstructural`), plus a positive companion, as `spices/sqlite` has.
- **Leaks:** the suites under leak checking, since every read returns a
  fresh copy.

## 7. Windows assessment (run by hand before L0 closes)

LMDB's Win32 path differs from the POSIX one in the ways most likely to bite
a binding: how the data file is sized, how locks work, and which libraries it
links. This assessment answers those with LMDB alone -- no `tur` needed until
A7 -- so it can run before the spice exists.

### Setup

- The same shell `tur` is built and tested in on Windows: **MSYS2 UCRT64**
  (gcc), as the turmeric repo's Windows CI uses.
- An NTFS volume with a few GiB free.
- LMDB 0.9.33:
  `git clone --branch LMDB_0.9.33 https://github.com/LMDB/lmdb` and work in
  `lmdb/libraries/liblmdb`.
- The probe below saved there as `probe.c`. Usage:
  `probe PATH MAP_MIB RECORDS` -- opens `PATH` as a single-file env with
  `MAP_MIB` of map, writes `RECORDS` 1 KiB values in batches of 1000
  (doubling the map whenever it fills), then reports what it found.

```c
/* probe.c -- LMDB-only checks for the Windows assessment of lmdb-spice-plan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lmdb.h"

#define CHECK(x) do { int rc_ = (x); if (rc_) { \
    fprintf(stderr, "%s failed: %s\n", #x, mdb_strerror(rc_)); exit(1); } } while (0)

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "probe.mdb";
    size_t map_mib   = argc > 2 ? strtoul(argv[2], NULL, 10) : 1024;
    long   n         = argc > 3 ? strtol(argv[3], NULL, 10) : 1;
    MDB_env *env; MDB_txn *txn; MDB_dbi dbi; MDB_envinfo info; MDB_stat st;
    char key[32], val[1024];
    memset(val, 'x', sizeof val);

    CHECK(mdb_env_create(&env));
    CHECK(mdb_env_set_mapsize(env, map_mib << 20));
    CHECK(mdb_env_open(env, path, MDB_NOSUBDIR | MDB_NOTLS, 0664));
    CHECK(mdb_env_info(env, &info));
    printf("opened: map %zu MiB (asked %zu)\n", info.me_mapsize >> 20, map_mib);

    for (long i = 0; i < n; ) {
        long start = i, end = i + 1000 < n ? i + 1000 : n;
        int rc = 0;
        CHECK(mdb_txn_begin(env, NULL, 0, &txn));
        CHECK(mdb_dbi_open(txn, NULL, 0, &dbi));
        for (; i < end; i++) {
            snprintf(key, sizeof key, "k%010ld", i);
            MDB_val k = { strlen(key), key }, v = { sizeof val, val };
            rc = mdb_put(txn, dbi, &k, &v, 0);
            if (rc) break;
        }
        if (rc == MDB_MAP_FULL) {
            mdb_txn_abort(txn);
            CHECK(mdb_env_info(env, &info));
            printf("map full after %ld records: %zu -> %zu MiB\n",
                   i, info.me_mapsize >> 20, (info.me_mapsize >> 20) * 2);
            CHECK(mdb_env_set_mapsize(env, info.me_mapsize * 2));
            i = start;                       /* redo the aborted batch */
            continue;
        }
        CHECK(rc);
        CHECK(mdb_txn_commit(txn));
    }

    CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
    CHECK(mdb_dbi_open(txn, NULL, 0, &dbi));
    CHECK(mdb_stat(txn, dbi, &st));
    mdb_txn_abort(txn);
    CHECK(mdb_env_info(env, &info));
    printf("entries %zu, map %zu MiB, last page %zu (page %u bytes)\n",
           st.ms_entries, info.me_mapsize >> 20, info.me_last_pgno, st.ms_psize);
    mdb_env_close(env);
    return 0;
}
```

### Steps

**A1 -- Build.** `gcc -O2 -Wall -o probe probe.c mdb.c midl.c`
Record every warning and anything extra the link needed. 0.9.33 references
no `Nt*` functions, so `-lntdll` should not be needed; if it is, record it.
Two Windows-only details to watch: `mdb.c` registers a TLS callback in the
`.CRT$XLB` section (GCC branch, around line 4605), so check the probe links
and exits cleanly with MinGW; and retry the build with `-lpthread` added --
L0's `build.tur` lists `pthread` for every platform (manifests have no
per-platform keys), so it must at least not break the Windows link.

**A2 -- What a large map costs (decides the Windows default map size).**
`./probe a.mdb 1024 1`, then in PowerShell:
`(Get-Item a.mdb).Length` and `fsutil sparse queryflag a.mdb`, and note
Explorer's "Size on disk" for `a.mdb`. The source says the length will be the
full 1 GiB (section 1); the question is whether NTFS also *allocates* it --
"Size on disk" 1 GiB, and free space down by 1 GiB -- or reserves it lazily.
Also time the open (`Measure-Command { ./probe a2.mdb 1024 1 }`), since
extending a file can mean zero-filling it. Repeat with `16384` (16 GiB): does
it succeed on a disk with less free space than that, and what does it cost?

**A3 -- Growth.** `./probe b.mdb 1 20000` (1 MiB map, about 20 MiB of data).
Expect several "map full ... -> ... MiB" lines and `entries 20000`. Record the
file's size afterwards, as in A2.

**A4 -- Reopen with a smaller map.** `./probe b.mdb 1 0`. The "opened" line
should report a map at least as large as the file, not 1 MiB (LMDB adopts the
file's size). Record what it says, and that `entries 20000` survived.

**A5 -- Two processes.** In two terminals at once:
`./probe c.mdb 64 200000` in one, and in the other, while the first is still
writing, `./probe c.mdb 64 0` several times. Expect the reader to start
without waiting and report a growing entry count, and the writer to finish
with `entries 200000`. Record any lock or sharing error.

**A6 -- Deleting open files.** While A5's writer runs, try `del c.mdb` (or
`Remove-Item`) from a third shell. Record the error. Then, with nothing
running, confirm `c.mdb` and `c.mdb-lock` delete cleanly. (This is why the
tests close every env before cleaning up.)

**A7 -- Through `tur` (once L0's `build.tur` exists).** `tur test` in
`spices/lmdb` on the same box. Record whether the vendored `:c-sources`
compile through the spice build and whether the smoke test passes. If it
fails where A1 passed, the difference is in how the spice build invokes the
compiler -- record the command (`TUR_SHOW_CC=1`).

### Results (fill in)

The Linux column was measured 2026-10-24 (x86-64, gcc, ext4-style
filesystem in a container) with this exact probe and LMDB 0.9.33, as a
reference for what "working" looks like.

| Item | Linux (reference) | Windows |
| --- | --- | --- |
| OS, filesystem, toolchain (`gcc --version`) | Linux 6.18, gcc | |
| A1 warnings; extra libraries; `-lpthread` OK? | none; none; yes | |
| A2 1 GiB map, 1 record: length / sparse / size on disk / open time | 12 KiB / n.a. / 12 KiB / instant | |
| A2 16 GiB map: succeeds? size on disk | (not run) | |
| A3 growth messages; final entries; file size | 1 -> 2 -> ... -> 64 MiB in six doublings; 20000; 39.3 MiB | |
| A4 reported map on reopen; entries intact? | 39 MiB (the file's size, not the 1 MiB asked); yes | |
| A5 reader blocked or concurrent; errors | (not run) | |
| A6 error deleting an open db; clean delete after close? | n.a. (POSIX unlinks open files) | |
| A7 `tur test` result | 5 files, 41 cases, all pass (2026-10-25) | |

How each answer lands in the plan:

- **A1** sets `:link-libs`. If Windows needs a library that would break the
  POSIX link, that is a manifest-feature gap (no per-platform keys) to file
  in the turmeric repo, not something to work around here.
- **A2** sets the Windows default map size: 64 MiB as planned if the full
  map is allocated up front, larger only if NTFS reserves it lazily and the
  open stays fast. Either way the docs point Windows users at
  `with-write-txn-growing`.
- **A3/A4** confirm that growing the map works there as on POSIX. If not,
  section 5's growth story needs a Windows caveat.
- **A5/A6** decide whether section 6's thread test and temp-directory cleanup
  need Windows handling.

## 8. Phases

| Phase | Delivers | Done when |
| --- | --- | --- |
| **L0** | `c/lmdb` vendored at 0.9.33 with `LICENSE` / `COPYRIGHT` / `VENDORED.md`; `build.tur`; a smoke test (env open, put, get, close); section 7 run on Windows and its results recorded here | builds and passes on Linux x86-64, Linux arm64, macOS arm64; section 7's table filled in |
| **L1** | `lmdb/error`, `lmdb/env`, `lmdb/txn` (raw and scoped), `lmdb/dbi` without flags, `get` / `put!` / `put-new!` / `del!` on `cstr`; `with-write-txn-growing`; `README.md` | section 6's persistence, transaction, limit and error tests green |
| **L2** | cursors; `Dbi` flags (`:dup-sort` and the rest), `del-dup!`; nested transactions; linear-handle `errors/` fixtures | the cursor, dup-sort, nested-txn and linear-handle tests green |
| **L3** | `Bytes` twins; `env-copy`, `env-reader-check`; `:append` and `:reserve` puts; txn reset/renew if wanted | binary round-trip, backup and bulk-load tests green |
| **L4** | a benchmark against `spices/sqlite` on key-value work; the `kv` adapter-readiness review (section 2) | numbers in the README; any awkwardness the review found fixed or filed |

## 9. What it leans on in stdlib

- `stdlib/serial.tur`'s `Bytes` (L3): `:linear` and length-prefixed, which is
  the right shape. If it grows a borrowed slice or view, use that rather than
  copying.
- `defer` running when a closure panics, so `with-write-txn` aborts rather
  than leaving the writer mutex held. L1's panic test checks it.

## 10. Non-goals

- A generic map API, a map typeclass instance, or dbm emulation. That is the
  future `kv` spice's job (section 2).
- SQL, secondary indexes beyond what `:dup-sort` gives, or a query language
  -- that is `spices/sqlite`.
- A network server. LMDB is in-process; several processes on one host can
  share an env, and that is LMDB's business, not ours.
- Cross-architecture file portability.
- Following libmdbx; revisit only if LMDB stops being maintained.

## 11. What landed (2026-10-25)

All five phases' code and tests, on branch `lmdb-spice` in turmeric-spices
(`spices/lmdb`) and turmeric (the compiler fixes). Linux x86-64 measured;
the other CI legs run when the spice PR does.

### Departures from the draft

- **Flags are a `DbiFlags` bit set**, not a vector: `(dbi-flags+
  (dbi-create) (dbi-dup-sort))`. A `[(DbiCreate)]` literal leaked its vector
  on every `dbi-open` (nothing frees a `[...]` temporary), and a typed `&
  rest` of a `defdata` cannot be walked as a list.
- **`Incompatible` is not "reopened without `:dup-sort`".** LMDB 0.9.33
  reopens a named database with the flags stored in the file, whatever is
  passed (`dbi-dup-sort?` reports them). `MDB_INCOMPATIBLE` there means the
  name is an ordinary key of the main database; that is what section 6's
  test checks now.
- **`lmdb/bytes`** holds the `Bytes` twins (and `put-reserve!`), so plain
  users of `lmdb/ops` do not load `stdlib/serial.tur`.
- **Added:** `with-cursor` (closes on panic), `with-nested-txn`,
  `cursor-fold` / `cursor-fold-from` (from the L4 review: a recursive walk
  overflowed the default stack near 1e5 entries), `has?`, `put-append!`,
  `dbi-main`, `env-map-size`, `env-read-only?`, `txn-id`, the `-dup` cursor
  moves, `cursor-del-all-dups!`, `cursor-dup-count`.
- **Txn reset/renew** (L3, "if a benchmark wants it"): not wanted; the
  benchmark's read loop is one transaction.
- **Abort on panic** is a malloc'd C guard the scoped forms' `defer` fires,
  not a `^mut` flag (which leaked its cell per transaction). A `defer`
  cannot test the runtime's `tur_panicking`: it is cleared before defers run.

### Compiler fixes it needed (turmeric `lmdb-spice`, one commit)

| Fix | Symptom in the spice |
| --- | --- |
| `fn-type-borrow-param` | `(fn [^borrow Txn] R)` read `^borrow` as a type parameter; no scoped form could be typed |
| `map-call-elaborates-args-twice` | `(get txn dbi k)`: the user `get` / `has?` spent the borrowed `Txn` |
| `return-value-typed-by-enclosing-argument` | `(put! t (? (dbi-main t)) k v)` -- `?` as an argument -- emitted invalid C |
| `let-ending-in-panic-declares-void-temp`, `nil-instantiated-merge-temp-declared-void` | the panic test; `(? (put! ...))` |
| `cps-capture-named-env` | any CPS-lifted function with a parameter named `env` |
| `nested-byval-app-field-sized-as-a-word` | a `(Result (Option cstr) E)` callback segfaulted (sret through the word shim) |

Filed, not fixed: `borrow-param-consumed-in-own-body`,
`ok-nil-lowercase-miscompiles`, `bare-nullary-ctor-as-value-emits-undeclared-c`
(docs/reported/).

### Tests and leaks

`tur test tests`: smoke, core (20), cursor (14), bytes (6), threads (1) --
every section 6 bullet, the panic and thread cases included.
`errors/run.sh`: five compile-fail fixtures (leaked `Env`, unfinished `Txn`,
double `cursor-close`, use after commit, a consuming callback).

Every suite rebuilt with `-fsanitize=address` and run with leak detection:
no memory errors. The leaks reported are the copies `get` and the cursor
moves hand to the caller (the caller frees them, as with stdlib's "caller
frees" functions) and `Result` payload boxes the compiler never frees
(`(ok (DbStat ...))`, `(err (MapFull))`), not anything the binding keeps.
This was a manual run: `tests/run-leak-check.sh` covers turmeric fixtures,
not spices.

### Benchmark (L4)

`spices/lmdb/bench/` (its own project, so `lmdb` does not depend on
`sqlite`); median of three, Linux x86-64, fsync off on both, 1e5 keys:

| ns/op | lmdb | sqlite |
| --- | ---: | ---: |
| load (one txn) | 705 | 1 519 |
| point get | 818 | 5 312 |
| scan | 66 | 218 |
| one-put commits | 5 689 | 10 913 |

### `kv` adapter review (L4)

`bench/src/bench/kv_adapter.tur` writes get, put, put-if-absent, delete,
has, increment and key listing against the binding and runs them. What it
found is fixed above (folds, flags, the compiler fixes); what remains for the
`kv` spice to decide is ownership of returned strings (document the copy, or
adopt it into `String`) and expiry, which LMDB does not have.

