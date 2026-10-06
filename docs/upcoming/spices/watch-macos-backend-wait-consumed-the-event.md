# `watch`: Darwin `backend-wait` dequeued the event it was only meant to observe

> **RESOLVED 2026-09-09 -- `backend-wait` now polls the kqueue fd.**
>
> Both macOS failures below are fixed and `spices/watch` is 9/9 on macOS
> (three consecutive clean runs). Neither was the documented kqueue-on-a-
> directory limitation, which the investigation started out assuming.
>
> - `spices/watch/src/watch/backend.tur` -- Darwin `backend-wait` uses
>   `poll(2)` on `w->kq` instead of `kevent()`.
> - `spices/watch/src/watch/platform.h` -- `<poll.h>` is no longer inside
>   the Linux-only arm.
> - `spices/watch/tests/tree_test.tur` -- seeds `leaf.txt` before opening
>   the watcher so the "atomic-save" case is actually a save over an
>   existing file.

- **Severity:** high (the documented `backend-wait` + `backend-drain-into`
  sequence, the one the module docstring tells callers to use, could not
  return an event on Darwin at all).
- **Platforms:** macOS only. Linux was and is unaffected.
- **Found:** 2026-09-09, macOS 27 (Darwin 27.0.0), `tur` v0.46.0.

## Symptom

`bash`-equivalent: `tur test tests` in `spices/watch`, 2 of 9 suites red,
reproducibly (observed on three runs before the fix and three after):

```
# 3/8 backend-drain-into tests failed.
ok 1 - backend-supported?
ok 2 - backend-open returned non-zero
ok 3 - backend-wait saw an event
ok 4 - backend-drain-into returned >= 0
not ok 5 - at least one event in evbuf
not ok 6 - event name or count matches target
not ok 7 - first event has non-zero mask
ok 8 - backend-drain-into on null returns -1

# 1/7 watch/tree tests failed.
...
not ok 5 - event kind is write or rename (atomic-save)
ok 6 - event path is a/b/leaf.txt (per-file naming)
```

## Why the obvious explanation was wrong

`docs/notebook-watch-semantics.md` section 10 notes that kqueue on a *directory*
does not fire for in-place content modification of a child file, and that
looks like it predicts both failures. It does not, and the passing
assertions are what give it away:

- `backend_drain_into_test` asserts `backend-wait saw an event` (#3) and
  that **passed**. The OS fired. The write in that test is `fopen("w")` on
  a path that the test's own cleanup step has just `unlink`ed, so it is a
  fresh *entry-level* create -- precisely the case section 10 says does work.
- `tree_test` asserts `watch-next returned an event` (#4) and
  `event path is a/b/leaf.txt` (#6), and both **passed**. Only the *kind*
  was wrong.

A limitation that stops events from firing cannot produce a failure whose
"an event fired" assertion is green.

## Root cause 1: `backend-wait` consumed the kevent (the drain failure)

`backend-wait` is specified as observational -- "the OS queue has events
ready (call backend-drain next)". On Linux it was `poll(2)` on the inotify
fd, which is level-triggered and reads nothing, so the following
`read(2)` in `backend-drain-append` gets the events.

The Darwin arm instead called

```c
struct kevent ev;
n = kevent(w->kq, NULL, 0, &ev, 1, &ts);
```

`kevent()` with an output buffer **dequeues**. The retrieved event's
`fflags` were written into a local `struct kevent ev` and then dropped on
return. `backend-drain-append` then polled the same kqueue with a zero
timeout, found it empty, computed `combined == 0`, and left `ev_n` at 0 --
so on Darwin, `backend-wait` returning 1 was reliably followed by
`backend-event-count` returning 0.

Minimal C repro (no turmeric involved):

```c
/* dir kqueue, EVFILT_VNODE, EV_ADD|EV_ENABLE|EV_CLEAR, NOTE_WRITE|... */
fopen(dir "/target.txt", "w"); /* ... */ fclose(fp);
usleep(200*1000);

struct kevent out;  struct timespec ts = {1,0};
int n  = kevent(kq, NULL, 0, &out, 1, &ts);   /* n=1, fflags=0x2 (NOTE_WRITE) */
struct kevent outs[16]; struct timespec z = {0,0};
int n2 = kevent(kq, NULL, 0, outs, 16, &z);   /* n2=0  <-- the wait ate it */
```

The same program shows `poll(kqfd, POLLIN)` returning readable twice in a
row before any `kevent()` call, and not readable after the queue is
drained -- i.e. level-triggered and non-consuming.

Why it went unnoticed for so long: the two in-tree callers of
`backend-wait` both mask it. `__watch-next-single` re-`stat`s the target
instead of reading `evbuf`, and the tree path never calls `backend-wait`
at all -- `__watcher-tree-poll-fired` already `poll(2)`s the backend fds it
gets from `backend-fd` (which is `w->kq` on Darwin). Only a direct backend
caller, i.e. the test, could see it.

### Fix

Make the Darwin arm of `backend-wait` do what the Linux arm and the tree
path already do -- `poll(2)` the fd:

```c
struct pollfd pfd;
pfd.fd     = w->kq;
pfd.events = POLLIN;
int rc = poll(&pfd, 1, (int)timeout_ms);
```

`poll(2)` also takes `-1` for "block forever" natively, so the special
case for a negative timeout goes away. `<poll.h>` moves out of the
Linux-only arm of `platform.h`.

## Root cause 2: `tree_test`'s "atomic-save" was not one (the kind failure)

`tree_test` deletes `a/b/leaf.txt` in its cleanup step, builds the
directories, opens the tree watcher, and only then does a temp-write +
`rename` into `a/b/leaf.txt`. So the target **did not exist** when the
watcher took its baseline snapshot.

Linux reports `kind=rename` because inotify sees the `rename(2)` itself
(`IN_MOVED_TO`) whether or not the destination existed. Darwin's tree
layer recovers names by diffing directory snapshots; a name absent from
the previous snapshot is honestly a *create*, and the diff has no way to
know it arrived via rename. `kind=2` is the correct answer to the question
the fixture actually asked.

### Fix

Seed the file before opening the watcher, so the save is an overwrite:

```turmeric
(write-file leaf "initial")
```

The snapshot diff then sees the same name with a different inode, which is
unambiguously an atomic save, and both backends report `kind=rename`. The
assertion itself is unchanged -- what changed is that the fixture now
establishes the precondition its own name claims. It also strengthens the
Darwin coverage: the diff now exercises the inode-change branch, which is
the branch every real editor hits, instead of the new-entry branch.

The seed write happens strictly *before* `watch-open-tree`, so it produces
no events on either backend and cannot alter what Linux observes.

The residual create-vs-rename divergence for genuinely new names is left
in place and documented separately in
`docs/watch-darwin-fresh-name-is-create.md`.

## Verification

- **macOS 27 / Darwin 27.0.0, `tur` v0.46.0, local: 9/9**, three
  consecutive clean runs. Was 7/9, reproducible.
- **Linux, CI `watch (ubuntu-latest)`: 9/9** -- `# All 8 backend-drain-into
  tests passed.`, `# All 7 watch/tree tests passed.`, and the tree fixture
  reports `# event path: [a/b/leaf.txt] kind: 4`, the same `rename` the
  Darwin snapshot diff now produces.

Linux could not be re-run locally (this box is macOS with no container
runtime), so the ubuntu CI leg is what measured it. The structural argument
matched: the `backend-wait` change is entirely inside
`#elif defined(__APPLE__)`, the `<poll.h>` move is inert on Linux (it was
already included there), and the fixture's added write happens strictly
before the watcher exists, so it emits no events on either backend.
