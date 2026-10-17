# Package-manager comments, lockfile docs and a dead helper that disagree with the code

**Severity:** low (comment and docs accuracy). One item is dead code. One item
proposes a small lockfile-format cleanup and is optional. Filed 2026-10-17 from
the "Trust the code over the comments" note in
[package-manager-internals-guide](../guides/package-manager-internals-guide.md).
Checked against `main` 9ac3cd43; line numbers are from that commit.

## Changes

### 1. `PkgLockEntry.sha256` says "SHA-256 of the archive" (`pkg.h:284`)

- **Says:** `char *sha256; /* SHA-256 of the archive */`.
- **Code:** this field holds three different things, depending on the row:
  - **Spice row:** `pkg_fetch_all` stores `pkg_hash_dir`'s `tree1:` content
    hash (`pkg.c:3093`). This is the hash of every regular file's path, size
    and bytes; it is not a hash of an archive.
  - **Spice row whose hashing failed:** it falls back to the git SHA
    (`pkg.c:3094`, `/* git SHA */`), which `pkg_hash_comparable` then refuses
    to compare.
  - **cmake row:** `pkg_cmake_build` copies the git SHA in again, identical to
    `:resolved` (`pkg.c:4619`). Nothing reads it back except
    `lock_pin_signature` and the `tur list` / audit printers.
    `pkg_cmake_verify_lock` compares `:resolved` (`pkg.c:4699`).
- **Change (comment):** spell out all three, e.g. "a spice row: pkg_hash_dir's
  `tree1:` content hash (git SHA if hashing failed -- pkg_hash_comparable
  refuses it); a cmake row: the git SHA again, never compared".
- **Change (structure, optional, touches the lock format):** stop making one
  key hold three kinds of value.
  - (a) When hashing fails, leave `sha256` NULL and do not store the git SHA.
    `pkg_verify_locked_spices` already skips a row with no comparable hash
    (`pkg.c:4662`), so verification behaves the same, and the field means only
    one thing.
  - (b) Stop writing `:sha256` on cmake rows, since it duplicates `:resolved`.
    `pkg_lock_read` already tolerates a missing key. One caution:
    `tur fetch --frozen` hashes the field into `lock_pin_signature`. Make sure
    a frozen run over an old lock does not count the dropped key as a change,
    for example by ignoring `sha256` on cmake rows in the signature.
  - (c) Rename the C field to `content_hash`, keeping `:sha256` on disk for
    compatibility. This is code-only.

  Any of the three makes the struct comment short and true.

### 2. The `pkg.h` file header (`pkg.h:4-17`)

- **Says:** "tur.lock pins all resolved git refs and SHA-256 hashes."
- **Code:** a lock row pins a `:spices` dependency: a missing directory is
  cloned and checked out at `:resolved`, and its tree hash is compared. It
  does **not** pin a `:cmake-deps` dependency. The generated CMake hands
  FetchContent the manifest's `:ref`, and `pkg_cmake_verify_lock` reports a
  mismatch only after the fact. Most rows do not hold a SHA-256 either (see
  item 1). The workflow list beneath also leaves out `tur fetch --frozen`,
  `tur audit` and the global install verbs (`install`, `uninstall`, `list`,
  `upgrade`), which all exist (`main.c:11469`).
- **Change:** "tur.lock pins each URL spice to a commit (`:resolved`) and a
  `tree1:` content hash; for cmake deps it records the commit fetched but does
  not pin it." Either finish the workflow list or replace it with a pointer to
  the guide's "Commands at a glance".

### 3. `pkg_sha256_file` is dead (`pkg.h:386-387`, `pkg.c:1954-1959`)

- **Code:** it is declared and defined but never called anywhere in the tree,
  tests included. `pkg_hash_dir` superseded it when the hash moved off
  `tar | sha256sum`. `tur_sha256_file` keeps its other caller
  (`src/runtime/image.c:215`) and its unit test, so it stays.
- **Change:** delete the declaration and the definition, along with the
  `/* SHA-256 */` banner that heads them.

### 4. Dead `docs/` citations

| Where | Cited | Now at |
|---|---|---|
| `pkg.h:395`, `pkg.c:2372`, `install.c:216` | `docs/reported/windows-spice-fetch-shell-quoting.md` | `docs/archive/` |
| `pkg.c:4534` | `docs/reported/wasm-arm-suppresses-cmake-policy-min.md` | `docs/archive/` |
| `src/runtime/sha256.h:12` (the hash `pkg_hash_dir` uses) | `docs/reported/pkg-hash-shells-out-to-sha256sum.md` | `docs/archive/` (and `pkg.c:1995` already cites it there) |

### 5. User guides show lockfiles `tur` never writes

This is docs, not code, but the guide's note names it, and these are the pages
spice consumers actually read.

- **Where:** `package-management-guide.md` (the samples at about `:385-440`)
  and `consuming-spices-guide.md` (about `:180-225`).
- **Says:** each guide shows two samples. One is a sweet-exp `deflockfile`
  (with no outer parens). The rows have `:sha256 "abc123..."`, a `:subdir`
  key, and cmake rows whose `:sha256` (`"ghi789..."`) differs from
  `:resolved`.
- **Code:** `pkg_lock_write` (`pkg.c:1806`) always writes plain s-expressions,
  never sweet-exp, whatever the manifest's spelling. It never writes
  `:subdir`. A spice's `:sha256` starts with `tree1:`, and a cmake row's
  `:sha256` equals its `:resolved`.
- **Change:** replace both samples with one real lock. The internals guide's
  "Format" block is one, produced from a scratch project. Drop the sweet-exp
  twin and say the file is generated.

### 6. Sibling repo: `turmeric-spices/CLAUDE.md` (not in this repository)

The guide records two claims there that stopped being true for `:spices`:

- that a lock row makes `tur fetch` skip a missing dependency (it clones it);
- that `:resolved` is never checked out (it is, when `:url`/`:ref` still
  match).

Both still hold for `:cmake-deps`. Fixing this needs a change in
turmeric-lang/turmeric-spices, so it is listed here only so it is not lost.

## When this is fixed

Drop the `pkg.h` and user-guide examples from the guide's "Trust the code over
the comments" paragraph. Keep the turmeric-spices sentence until that repo is
fixed.
