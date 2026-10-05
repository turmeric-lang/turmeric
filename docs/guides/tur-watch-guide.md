---
title: tur-watch Guide
category: CLI Tools
description: Cross-platform filesystem watching with debounce and coalescing for CLI tools
---

# tur-watch Guide

`tur-watch` provides cross-platform filesystem watching (inotify on Linux,
kqueue on Darwin) with debounce and coalescing helpers for CLI tools that
need a "file changed, react" loop.

---

## Installation

```turmeric no-check
:spices #map{
  "watch" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "watch-v0.2.0"
               :subdir "spices/watch"}
}
```
```sweet-exp
:spices
#map{
  "watch" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "watch-v0.2.0"
               :subdir "spices/watch"}
}
```

---

## Modules

| Module | Contents |
|--------|----------|
| `watch/event` | `watch-event` record, kind constants, accessors |
| `watch/opts` | `watch-opts` record, `default-watch-opts` |
| `watch/watch` | `watch-open-one`, `watch-open-tree`, `watch-close`, `watch-next`, `watch-drain` |
| `watch/debounce` | `debounce-batch-*` primitives for path coalescing |

`watch/backend` is internal and is not exported.

---

## Quick start -- Single file

```turmeric
(import watch/event :refer [watch-event-kind watch-event-path watch-event-free
                             watch-kind-write watch-kind-rename])
(import watch/opts  :refer [default-watch-opts])
(import watch/watch :refer [watch-open-one watch-close watch-next])

(defn main [] : int
  (let [opts (default-watch-opts)
        w    (watch-open-one "notes.md" opts)]
    (loop []
      (let [ev (watch-next w -1)]   ;; block indefinitely
        (if (= ev 0)
          0
          (do
            (println (str-concat "changed: " (watch-event-path ev)))
            (watch-event-free ev)
            (recur)))))
    (watch-close w)
    0))
```
```sweet-exp
import watch/event :refer [watch-event-kind watch-event-path watch-event-free
                            watch-kind-write watch-kind-rename]
import watch/opts :refer [default-watch-opts]
import watch/watch :refer [watch-open-one watch-close watch-next]
defn main [] :int
  let [opts default-watch-opts()
       w    watch-open-one("notes.md" opts)]
    loop []
      let [ev watch-next(w -1)]
        if {ev = 0}
          0
          do
            println(str-concat("changed: " watch-event-path(ev)))
            watch-event-free(ev)
            recur()
    watch-close(w)
    0
```

---

## Quick start -- Recursive directory tree

```turmeric
(import watch/event :refer [watch-event-path watch-event-kind
                             watch-kind->cstr watch-event-free])
(import watch/opts  :refer [watch-opts-make watch-opts-free])
(import watch/watch :refer [watch-open-tree watch-close watch-next])

(defn main [] : int
  (let [opts (watch-opts-make 1 150 1 0 1)  ;; recursive, 150 ms debounce
        w    (watch-open-tree "src/" opts)]
    (watch-opts-free opts)
    (loop []
      (let [ev (watch-next w -1)]
        (if (= ev 0)
          0
          (do
            (println (str-concat (watch-kind->cstr (watch-event-kind ev))
                                 " "
                                 (watch-event-path ev)))
            (watch-event-free ev)
            (recur)))))
    (watch-close w)
    0))
```
```sweet-exp
import watch/event :refer [watch-event-path watch-event-kind
                             watch-kind->cstr watch-event-free]
import watch/opts :refer [watch-opts-make watch-opts-free]
import watch/watch :refer [watch-open-tree watch-close watch-next]
defn main [] :int
  let [opts (watch-opts-make 1 150 1 0 1)  ;; recursive, 150 ms debounce
        w    (watch-open-tree "src/" opts)]
    watch-opts-free(opts)
    loop([] let([ev (watch-next w -1)] if({ev = 0} 0 do(println(str-concat(watch-kind->cstr(watch-event-kind(ev)) " " watch-event-path(ev))) watch-event-free(ev) recur()))))
    watch-close(w)
    0
```

---

## API reference

### Opening a watcher

```turmeric
;; Watch a single file or directory
(watch-open-one path opts)   ;; returns watcher handle, or 0 on failure

;; Watch a directory tree recursively
(watch-open-tree dir opts)   ;; returns watcher handle

;; Release the watcher
(watch-close w)
```
```sweet-exp
;; Watch a single file or directory
watch-open-one(path opts)
;; returns watcher handle, or 0 on failure
;; Watch a directory tree recursively
watch-open-tree(dir opts)
;; returns watcher handle
;; Release the watcher
watch-close(w)
```

### Options

```turmeric
;; Use the defaults (150 ms debounce, non-recursive)
(default-watch-opts)

;; Construct custom options
;; watch-opts-make recursive debounce-ms coalesce? reserved periodic?
(watch-opts-make 1 200 1 0 0)

(watch-opts-free opts)   ;; release options handle
```
```sweet-exp
;; Use the defaults (150 ms debounce, non-recursive)
default-watch-opts()

;; Construct custom options
;; watch-opts-make recursive debounce-ms coalesce? reserved periodic?
watch-opts-make(1 200 1 0 0)

watch-opts-free(opts)   ;; release options handle
```

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `recursive` | int | 0 | 1 to recurse into subdirectories (tree mode only) |
| `debounce-ms` | int | 150 | Coalescing window in milliseconds |
| `coalesce?` | int | 1 | 1 to coalesce burst events into one |
| `reserved` | int | 0 | Must be 0 |
| `periodic?` | int | 0 | Reserved for future periodic polling |

### Reading events

```turmeric
;; Block until the next event (timeout-ms = -1 blocks forever)
(watch-next w timeout-ms)    ;; returns event handle, or 0 on timeout

;; Drain all pending events without blocking
(watch-drain w)              ;; returns a cons list of event handles (may be 0)
```
```sweet-exp
;; Block until the next event (timeout-ms = -1 blocks forever)
watch-next(w timeout-ms)
;; returns event handle, or 0 on timeout
;; Drain all pending events without blocking
watch-drain(w)
;; returns a cons list of event handles (may be 0)
```

### Event accessors

```turmeric
(watch-event-kind  ev)        ;; event kind constant (see below)
(watch-event-path  ev)        ;; file path that changed
(watch-event-free  ev)        ;; release the event handle
```
```sweet-exp
watch-event-kind(ev)
;; event kind constant (see below)
watch-event-path(ev)
;; file path that changed
watch-event-free(ev)
;; release the event handle
```

### Event kinds

```turmeric
(watch-kind-write)    ;; file content changed
(watch-kind-create)   ;; file created
(watch-kind-delete)   ;; file deleted
(watch-kind-rename)   ;; file renamed / moved
(watch-kind-none)     ;; no change (returned only on timeout/drain miss)

(watch-kind->cstr kind)  ;; human-readable string for a kind constant
```
```sweet-exp
watch-kind-write()    ;; file content changed
watch-kind-create()   ;; file created
watch-kind-delete()   ;; file deleted
watch-kind-rename()   ;; file renamed / moved
watch-kind-none()     ;; no change (returned only on timeout/drain miss)

watch-kind->cstr(kind)  ;; human-readable string for a kind constant
```

---

## Debounce helper

For tools that re-run a command on any change, `watch/debounce` coalesces
a burst of events into one batched notification:

```turmeric
(import watch/debounce :refer [debounce-batch-new debounce-batch-add
                               debounce-batch-ready? debounce-batch-drain
                               debounce-batch-free])

(let [batch (debounce-batch-new 300)]  ;; 300 ms window
  (loop []
    (let [ev (watch-next w 50)]       ;; 50 ms poll step
      (if (not= ev 0)
        (do
          (debounce-batch-add batch ev)
          (recur))
        ;; No event -- check if debounce window elapsed
        (if (debounce-batch-ready? batch)
          (let [paths (debounce-batch-drain batch)]
            ;; paths is a cons list of changed cstr paths
            (rebuild! paths)
            (recur))
          (recur)))))
  (debounce-batch-free batch))
```
```sweet-exp
import watch/debounce :refer [debounce-batch-new debounce-batch-add
                               debounce-batch-ready? debounce-batch-drain
                               debounce-batch-free]

let [batch debounce-batch-new(300)]  ;; 300 ms window
  loop []
    let [ev watch-next(w 50)]       ;; 50 ms poll step
      if {ev not= 0}
        do
          debounce-batch-add(batch ev)
          recur()
        ;; No event -- check if debounce window elapsed
        if debounce-batch-ready?(batch)
          let [paths debounce-batch-drain(batch)]
            ;; paths is a cons list of changed cstr paths
            rebuild!(paths)
            recur()
          recur()
  debounce-batch-free(batch)
```

---

## Notes

- On Darwin, tree-mode events carry per-file names via directory-snapshot
  diffing (v0.2.0). On Linux, `inotify` names are used directly.
- `watch-open-one` on a directory watches the directory itself, not its
  contents. Use `watch-open-tree` for recursive subtree watching.
- Events must be freed with `watch-event-free` after use to avoid leaks.
- `watch-next` with `timeout-ms = 0` is a non-blocking poll.

---

## See also

- [Threading Guide](threading-guide.md) -- integrating file-watching with OS threads
- [tur-notebook Guide](notebook-guide.md) -- `tur nb render --watch` uses tur-watch internally
