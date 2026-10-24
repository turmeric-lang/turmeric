# `spices/lmdb`: an embedded key-value store, the dbm to sqlite's SQL

> **Status:** draft, not started. Written 2026-10-24 after a survey of
> embeddable key-value stores (section 1) chose LMDB.
> **Track:** post-v1 -- nothing on the v1 line depends on it.
> **Type:** spice (in `../turmeric-spices/`). No compiler or stdlib change is
> needed; section 8 lists the two places it leans on stdlib as it stands.

## 0. Summary

`spices/sqlite` gives Turmeric an embedded SQL database. There is no
key-value counterpart: nothing you can open as a file, put bytes into by key,
and get them back from after a restart, without SQL. Racket ships one as
`file/dbm` (a wrapper of the system's Unix dbm: `dbm-open`, `dbm-set!`,
`dbm-ref`, `dbm-remove!`, `dbm-close!`, and the handle doubles as a
dictionary).

This plan adds `spices/lmdb`: [LMDB](https://github.com/LMDB/lmdb) vendored
as C, under a two-layer Turmeric API --

- **`lmdb/kv`**, the dbm-shaped layer: open a file, `kv-get` / `kv-put!` /
  `kv-delete!` / `kv-each`, each call its own transaction. What most callers
  want, and what Racket's `file/dbm` is.
- **`lmdb/txn`**, the explicit layer: read and write transactions spanning
  many operations, cursors, and named sub-databases, for the callers who need
  atomic multi-key updates or range scans.

LMDB keeps us out of the usual traps of a dbm: it is permissively licensed
(GDBM is GPL-3, Berkeley DB AGPL-3), it is the same library and file format
on every platform (the system "dbm" Racket wraps is GDBM on most Linux and
the libc dbm on macOS), and it is crash-safe with real transactions.

## 1. Why LMDB

The survey (2026-10-24) weighed:

| Library | License | Verdict |
| --- | --- | --- |
| **LMDB** | OpenLDAP Public License 2.8 (BSD-style) | **Chosen.** Two C files plus two headers; memory-mapped B+tree; ACID, MVCC (readers never block the writer), no recovery step after a crash; zero-copy reads. 0.9.33 (May 2024) is the current maintenance release. Linux, macOS, Windows. ~30 kB compiled. |
| libmdbx | Apache-2.0 | An LMDB fork with auto-growing files and more API. Rejected for governance, not quality: since December 2025 it ships only as an amalgamation, and most of its test suite is no longer public. |
| TinyCDB | public domain | Constant database: built once, then read-only. Not a general store. A possible later companion for shipped lookup tables. |
| UnQLite | BSD | Key-value plus a document store; bigger than we need; last release 2022. |
| Tkrzw | Apache-2.0 | Kyoto Cabinet's successor. C++ with a C wrapper; heavier build. |
| GDBM / ndbm | GPL-3 | License incompatible with an MIT spice. |
| Berkeley DB | AGPL-3 (6.0+) | License. |

What LMDB asks of us in return (each is designed for below):

- **A maximum size, set up front** (`mdb_env_set_mapsize`); a write past it
  fails with `MDB_MAP_FULL`. -> section 4.1.
- **A database is a directory by default.** `MDB_NOSUBDIR` makes it one file
  plus a `-lock` file beside it, which is what a dbm user expects. -> 4.2.
- **Files are not portable** across word size or byte order. Documented, not
  worked around: a dbm file never was either.
- **One writer at a time** per database, and a write transaction must finish
  on the OS thread that began it. -> 4.4.
- **Keys are at most 511 bytes** (`mdb_env_get_maxkeysize`) unless rebuilt
  with a larger `MDB_MAXKEYSIZE`. -> 4.5.

## 2. Layout and build

```
spices/lmdb/
  build.tur
  README.md               -- so the spice appears on the docs site
  c/lmdb/                 -- LMDB, vendored verbatim (never src/)
    lmdb.h  midl.h  mdb.c  midl.c
    LICENSE  COPYRIGHT    -- the OpenLDAP license notice, which must ship
    VENDORED.md           -- upstream tag, commit, date, and how to refresh
  src/lmdb/
    error.tur             -- LmdbError and the errno/MDB_* mapping
    env.tur               -- Env: open, close, size, sync, stat
    txn.tur               -- Txn, Dbi, Cursor (the explicit layer)
    kv.tur                -- the dbm-shaped layer over env + txn
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

Notes for L0:

- Pin **0.9.33** unless a later tag is confirmed clean on all three CI
  platforms. A 0.9.34 bug report mentions a macOS arm64 compile error in
  `mdb.c`; check before taking it.
- `:c-sources` propagate through the `:spices` closure, so a spice depending
  on `lmdb` links LMDB with no further declaration.
- LMDB's own command-line tools (`mdb_stat`, `mdb_dump`, ...) are not
  vendored; `VENDORED.md` says where to get them for debugging.
- Windows: LMDB's Win32 path uses `CreateFileMapping`; on Windows the data
  file is extended to the full map size up front (it is sparse on Linux and
  macOS). Verify on CI and document; it decides the Windows default in 4.1.

## 3. API

House rules apply throughout: `defopaque` handles, `(Option T)` for "may be
absent", `(Result T LmdbError)` for "may fail", no `:int` stand-ins. Handles
are `:linear` with `^borrow` accessors, exactly as `sqlite/db`'s `Db` and
`Stmt` are, so a leaked or double-closed handle is a compile-time error under
`-Xsubstructural`.

### 3.1 `lmdb/kv` -- the dbm layer

```turmeric
(defopaque Kv :int :linear)          ;; an Env plus its default Dbi

(kv-open    path)                    ;; -> (Result Kv LmdbError)
(kv-open-with path opts)             ;; KvOpts: map-size, read-only, sync mode
(kv-close   kv)                      ;; consumes

(kv-get     ^borrow kv key)          ;; -> (Option cstr), a fresh copy
(kv-has?    ^borrow kv key)          ;; -> bool
(kv-put!    ^borrow kv key val)      ;; -> (Result nil LmdbError), overwrites
(kv-insert! ^borrow kv key val)      ;; -> (Result bool LmdbError); false if the
                                     ;;    key exists (MDB_NOOVERWRITE)
(kv-delete! ^borrow kv key)          ;; -> (Result bool LmdbError); false if absent
(kv-count   ^borrow kv)              ;; -> int
(kv-each    ^borrow kv (fn [k v] ...))   ;; key order (LMDB sorts keys)
(kv-keys    ^borrow kv)              ;; -> (Vec cstr)
```

Each operation is its own transaction: a read transaction for the readers, a
write transaction committed before return for the writers. Simple and safe;
the cost of a commit per write (an `fsync` unless the sync mode says
otherwise) is what `lmdb/txn` exists to amortise.

`kv-put!` overwrites and `kv-insert!` refuses, so both of Racket's
`dbm-set!` behaviours (`#:replace?` true and false) are a name, not a flag.

### 3.2 Binary keys and values

`cstr` covers the dbm use. Binary data goes through stdlib's `Bytes`
(`stdlib/serial.tur`: linear, length-prefixed, `bytes-len` / `bytes-data`):
`kv-get-bytes` / `kv-put-bytes!` and the same in `lmdb/txn`. Because
`serialize` already produces `Bytes`, a typed value store falls out:
`(kv-put-value! kv key v)` / `(kv-get-value kv key)` over
`serialize`/`deserialize` -- phase L3, once the plain layers have settled.

A cstr key is stored without its NUL, so a `cstr` key and the `Bytes` key
with the same bytes are the same key.

### 3.3 `lmdb/txn` -- the explicit layer

```turmeric
(defopaque Env    :int :linear)
(defopaque Txn    :int :linear)
(defopaque Dbi    :int)              ;; a plain handle: LMDB never frees one
(defopaque Cursor :int :linear)

(env-open path opts) (env-close env)
(env-set-map-size! ^borrow env bytes) (env-sync ^borrow env)
(env-stat ^borrow env)               ;; -> EnvStat struct: entries, depth, pages
(env-max-key-size ^borrow env)       ;; -> int

(with-read-txn  ^borrow env (fn [txn] ...))   ;; -> the fn's value
(with-write-txn ^borrow env (fn [txn] ...))   ;; commits on return; aborts on
                                              ;;    Err or panic, via defer
(txn-dbi ^borrow txn name)           ;; named sub-database; -> (Result Dbi ...)
(txn-get / txn-put! / txn-delete! ^borrow txn dbi key ...)
(cursor-open ^borrow txn dbi) (cursor-close c)
(cursor-seek ^borrow c key)          ;; first key >= key: prefix and range scans
(cursor-next ^borrow c)              ;; -> (Option (Pair cstr cstr))
```

`with-*-txn` taking a closure, rather than exposing begin/commit/abort as the
primary API, is deliberate: the transaction cannot outlive the closure, abort
on failure cannot be forgotten, and a write transaction cannot hop threads
mid-way (4.4). Raw `txn-begin` / `txn-commit` / `txn-abort` stay available
for callers that need them, linear so that an unfinished transaction is a
compile-time error under `-Xsubstructural`.

### 3.4 Errors

```turmeric
(defdata LmdbError
  (MapFull)                ;; MDB_MAP_FULL after auto-grow gave up (4.1)
  (KeyTooLong int)         ;; the key's length; max is env-max-key-size
  (ReadersFull)            ;; MDB_READERS_FULL
  (NotADatabase cstr)      ;; MDB_INVALID / MDB_VERSION_MISMATCH, with the path
  (ReadOnly)               ;; a write on a read-only open
  (Io int cstr))           ;; errno and mdb_strerror text
```

`MDB_NOTFOUND` is never an error: it is `None` from a get and `false` from a
delete. `MDB_KEYEXIST` is `false` from `kv-insert!`.

## 4. Decisions inside the API

### 4.1 Map size, and growing it

Default map size: **1 GiB** on Linux and macOS, where the file is sparse and
only what is written takes space. Windows default to be set in L0 after
measuring (2).

On `MDB_MAP_FULL` the dbm layer **doubles the map size and retries once**,
which `mdb_env_set_mapsize` allows when this process has no transaction open
-- true inside a one-operation `kv-put!`. The explicit layer does not retry
behind the caller's back (its transaction may have done other work); it
returns `MapFull` and the caller calls `env-set-map-size!` and reruns. Cap
the auto-grow at a configurable maximum (default 64 GiB) so a runaway writer
fails rather than filling the disk.

### 4.2 One file, not a directory

`kv-open` and `env-open` pass `MDB_NOSUBDIR` by default: `path` is the data
file and `path-lock` sits beside it. `:subdir true` in the options gives
LMDB's directory layout for anyone sharing files with other LMDB tools.

### 4.3 Durability

Default: full sync on commit (LMDB's default). `KvOpts :sync` also offers
`:no-meta-sync` (`MDB_NOMETASYNC`: durable up to the last commit but one) and
`:none` (`MDB_NOSYNC`, for caches and tests). Never `MDB_WRITEMAP` by default
-- it lets a stray pointer write corrupt the file.

### 4.4 Threads and fibers

Open every environment with **`MDB_NOTLS`**, so a read transaction is not
bound to the OS thread that began it -- Turmeric's async fibers can resume on
another thread. A write transaction is still bound to its thread by LMDB's
writer mutex; `with-write-txn` keeps it inside one synchronous call, and the
docs say not to `await` inside it. Test both on the threaded scheduler.

### 4.5 Keys and copies

- A key longer than `env-max-key-size` (511 by default) is refused with
  `KeyTooLong` before it reaches LMDB, rather than with LMDB's generic
  `MDB_BAD_VALSIZE`.
- Every value handed back to Turmeric is **copied** out of the map: LMDB's
  pointers are valid only until the transaction ends, and the dbm layer ends
  it before returning. A zero-copy borrowed read inside `with-read-txn` is a
  possible later addition, typed so it cannot escape the closure.

## 5. Phases

| Phase | Delivers | Done when |
| --- | --- | --- |
| **L0** | `c/lmdb` vendored at 0.9.33 with `LICENSE`/`COPYRIGHT`/`VENDORED.md`; `build.tur`; one smoke test opening a file, putting and getting a key | builds and passes on Linux x86-64, Linux arm64, macOS arm64; Windows build attempted and its map-size behaviour measured |
| **L1** | `lmdb/error`, `lmdb/kv` with cstr keys and values; auto-grow; `README.md` with a dbm-style example | section 6's kv suites green; the README example runs |
| **L2** | `lmdb/txn`: `Env`, `with-read-txn`/`with-write-txn`, named `Dbi`s, cursors, `env-stat`; linear-handle negative fixtures in `errors/` | atomic multi-key test, range-scan test, and the `errors/` fixtures (E0100/E0101 under `-Xsubstructural`) pass |
| **L3** | `Bytes` keys and values; `kv-put-value!`/`kv-get-value` over `serialize` | round-trip of a struct, a `Vec` and a map through a reopened file |
| **L4** | `lmdb/dict`-style iteration helpers if the dbm layer's users ask for them; a benchmark against `spices/sqlite` doing key-value work | numbers recorded in the README |

## 6. Testing

In `spices/lmdb/tests/`, run by `tur test`. The two silent traps in the spices
CLAUDE.md apply: every `describe` inside a `defn`, and `main` returns
`run-all-status`.

- **Round trip and persistence:** put, close, reopen, get -- the reason the
  spice exists. Each test works in its own temporary directory and removes
  it.
- **dbm semantics:** overwrite vs insert, delete of an absent key, `kv-count`,
  `kv-each` visiting keys in sorted order, a 511-byte key accepted and a
  512-byte one refused with `KeyTooLong`.
- **Growth:** a small map (1 MiB) filled past full grows and the write
  succeeds; with the cap set to the map size, the same write returns
  `MapFull`.
- **Transactions:** a write transaction that returns `Err` or panics leaves
  nothing behind; a reader opened before a commit still sees the old value
  (MVCC).
- **Read-only open** refuses writes with `ReadOnly`.
- **A non-LMDB file** opened as a database returns `NotADatabase`, not a
  crash.
- **Threads:** two OS threads reading while one writes, on the threaded
  scheduler (4.4).
- **Leaks:** the L1 suites under `tests/run-leak-check.sh`-style leak
  checking, since every get returns a fresh copy.

## 7. Non-goals

- SQL, secondary indexes, or a query language -- that is `spices/sqlite`.
- A network server; LMDB is in-process (multi-process on one host works,
  and is LMDB's business, not ours).
- Cross-architecture file portability.
- Following libmdbx; revisit only if LMDB stops being maintained.

## 8. What it leans on in stdlib

- `stdlib/serial.tur`'s `Bytes` (L3). It is `:linear` and length-prefixed,
  which is the right shape; if its API grows a slice or borrowed view, L3
  should use it rather than copying.
- The closure-taking transaction functions rely on `defer` running when a
  closure panics, so a write transaction is aborted rather than left open;
  L2's panic test checks it.

## 9. Open questions

1. **Name.** `lmdb` (the library, as `sqlite`, `postgres` and `valkey` are
   named) or `kv` (the use)? This plan assumes `lmdb`, with `lmdb/kv` as the
   friendly entry point.
2. **Should `Kv` also be usable as a generic map** through whatever map
   typeclass stdlib settles on, as Racket's dbm handle is a dictionary?
   Deferred to L4; it depends on that typeclass existing.
3. **Windows default map size** -- decided by L0's measurement.
