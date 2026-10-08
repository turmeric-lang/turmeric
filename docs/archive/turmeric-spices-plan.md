# turmeric-spices -- Initial Spice Set Plan

> **Status:** Proposal
> **Prerequisites:** Two build-system features must ship before Phase 1 begins:
>   1. `tur add` `:subdir` key -- monorepo sub-package resolution (see "Repo Structure")
>   2. `tur build --target wasm` -- Emscripten toolchain support for cmake-deps
> **Related:** [package-management-plan.md](archive/package-management-plan.md),
>              [package-management-guide.md](guides/package-management-guide.md)

---

## Overview

`turmeric-spices` is the official monorepo of first-party spices for the
Turmeric ecosystem. It lives at `https://github.com/turmeric-lang/turmeric-spices`
and is organized as a workspace -- each subdirectory under `spices/` is a
self-contained package with its own `build.tur`.

This document proposes an initial set of seven spices in three tiers, ordered
so that later spices can depend on earlier ones and so that the C toolchain
integration is validated incrementally. All cmake-deps spices are expected to
compile under Emscripten via `tur build --target wasm` where the underlying C
library supports it.

---

## Repo Structure

```
turmeric-spices/
  build.tur              -- workspace root (:members lists sub-packages)
  tur.lock               -- workspace-level lock file
  README.md
  spices/
    test/                -- tur-test: testing framework utilities
    math/                -- tur-math: 2D/3D vector/matrix math
    sqlite/              -- tur-sqlite: SQLite3 bindings
    json/                -- tur-json: JSON parsing and serialization
    raylib/              -- tur-raylib: Raylib graphics and input
    http/                -- tur-http: async HTTP/HTTPS client
    regex/               -- tur-regex: PCRE2 regex bindings
```

### Workspace root `build.tur`

```turmeric
(defpackage turmeric-spices
  :name    "turmeric-spices"
  :version "0.1.0"
  :members ["spices/test"
            "spices/math"
            "spices/sqlite"
            "spices/json"
            "spices/raylib"
            "spices/http"
            "spices/regex"])
```

### Referencing a sub-package from outside the monorepo

Consumers pin to the monorepo URL with a `:subdir` key:

```turmeric
:spices {
  "sqlite" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "sqlite-v0.1.0"
            :subdir "spices/sqlite"}
}
```

Both P0 prerequisites must ship before Phase 1 begins:

- **`:subdir` in `tur add`:** the CLI must clone the monorepo and resolve the
  manifest from the given subdirectory rather than the repo root.
- **`tur build --target wasm`:** the build tool must accept an Emscripten
  toolchain target, passing `Emscripten.cmake` to CMake so that cmake-deps
  compile to WASM. Spices whose C deps support Emscripten will be
  Emscripten-compatible out of the box once this lands.

Tags follow the pattern `<spice>-vMAJOR.MINOR.PATCH` so each package is
versioned independently within the same repo. When the Spice registry
launches, packages will be published individually and the `:subdir` key
will no longer be needed.

---

## Tier 1 -- Pure Turmeric (no C deps)

These ship first because they establish API patterns for the rest of the
ecosystem and can be developed without the cmake-deps pipeline in place.

---

### `tur-test` -- Testing framework utilities

**Purpose:** Assertion helpers, `describe`/`it` test blocks, and a result
formatter. Every other spice in this repo depends on it for its own tests.
By building it first, we validate the spice-as-dependency workflow before
any C integration is needed.

**C deps:** none

**Module layout:**

```
spices/test/src/
  assert.tur     -- assert-eq, assert-ne, assert-true, assert-false, assert-err
  suite.tur      -- describe / it / before-each / after-each
  runner.tur     -- collects and runs suites, prints TAP output
```

**`build.tur`:**

```turmeric
(defpackage tur-test
  :name        "tur-test"
  :version     "0.1.0"
  :description "Testing framework utilities for Turmeric"
  :license     "MIT"
  :exports {
    "test/assert" ["assert-eq" "assert-ne" "assert-true" "assert-false"
                   "assert-some" "assert-none" "assert-ok" "assert-err"]
    "test/suite"  ["describe" "it" "before-each" "after-each"]
    "test/runner" ["run-all"]
  })
```

**Usage:**

```turmeric
(import test/assert :refer [assert-eq assert-ok])
(import test/suite  :refer [describe it])
(import test/runner :refer [run-all])

(describe "addition"
  (it "adds two integers"
    (assert-eq (+ 1 2) 3))
  (it "returns ok on success"
    (assert-ok (some-fallible-fn 42))))

(run-all)
```

---

### `tur-math` -- 2D/3D vector and matrix math

**Purpose:** Typed vec2, vec3, vec4, and mat4 values with standard geometric
operations. Written in pure Turmeric with inline-C hot paths where profiling
shows a benefit. `tur-raylib` depends on this to avoid duplicating geometry
types.

**C deps:** none (inline-C only for hot paths if needed)

**Module layout:**

```
spices/math/src/
  vec2.tur     -- Vec2, add, sub, scale, dot, length, normalize, lerp
  vec3.tur     -- Vec3, cross, ...
  vec4.tur     -- Vec4 (also used as RGBA color)
  mat4.tur     -- Mat4, identity, translate, rotate, scale, mul, invert
  quat.tur     -- Quaternion, from-axis-angle, slerp
  math.tur     -- clamp, lerp, remap, deg->rad, rad->deg, approx-eq
```

**`build.tur`:**

```turmeric
(defpackage tur-math
  :name        "tur-math"
  :version     "0.1.0"
  :description "2D/3D vector and matrix math for Turmeric"
  :license     "MIT"
  :spices {
    "test" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
  }
  :exports {
    "math/vec2" ["vec2" "v2-add" "v2-sub" "v2-scale" "v2-dot"
                 "v2-length" "v2-normalize" "v2-lerp"]
    "math/vec3" ["vec3" "v3-add" "v3-sub" "v3-scale" "v3-dot"
                 "v3-cross" "v3-length" "v3-normalize" "v3-lerp"]
    "math/vec4" ["vec4"]
    "math/mat4" ["mat4-identity" "mat4-translate" "mat4-rotate-x"
                 "mat4-rotate-y" "mat4-rotate-z" "mat4-scale" "mat4-mul"
                 "mat4-invert" "mat4-perspective" "mat4-look-at"]
    "math/math" ["clamp" "lerp" "remap" "deg->rad" "rad->deg" "approx-eq"]
  })
```

**Usage:**

```turmeric
(import math/vec3 :refer [vec3 v3-add v3-normalize v3-dot])
(import math/math :refer [deg->rad])

(def forward (v3-normalize (vec3 0.0 0.0 -1.0)))
(def up      (vec3 0.0 1.0 0.0))
(def angle   (deg->rad 45.0))
```

---

## Tier 2 -- C Integration

These validate the `:cmake-deps` pipeline. SQLite ships before Raylib because
its C build is simpler (single amalgamation file, no platform windowing),
making it easier to debug any cmake-deps issues before tackling a heavier
dependency.

---

### `tur-sqlite` -- SQLite3 bindings

**Purpose:** Thin, safe Turmeric bindings over the SQLite3 C library. The
API surface is kept small -- prepare/step/bind/column -- with a higher-level
query helper that returns a `result` of lists of association maps. Errors
surface as `(err <message>)` rather than panics.

**C deps:** SQLite3 (amalgamation via cmake-deps)

**Module layout:**

```
spices/sqlite/src/
  db.tur       -- open, close, exec (fire-and-forget), query, prepare
  stmt.tur     -- step, bind-int, bind-real, bind-text, bind-null, reset, finalize
  row.tur      -- column-count, column-name, column-int, column-real, column-text
  error.tur    -- error codes and message formatting
```

**`build.tur`:**

```turmeric
(defpackage tur-sqlite
  :name        "tur-sqlite"
  :version     "0.1.0"
  :description "SQLite3 bindings for Turmeric"
  :license     "MIT"
  :spices {
    "test" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
  }
  :cmake-deps {
    "sqlite3" {:url     "https://github.com/sqlite/sqlite"
               :ref     "version-3.47.2"
               :options {:BUILD_SHARED_LIBS "OFF"}}
  }
  :exports {
    "sqlite/db"   ["db-open" "db-close" "db-exec" "db-query" "db-prepare"]
    "sqlite/stmt" ["stmt-step" "stmt-bind-int" "stmt-bind-real"
                   "stmt-bind-text" "stmt-bind-null" "stmt-reset" "stmt-finalize"]
  })
```

**Usage:**

```turmeric
(import sqlite/db :refer [db-open db-close db-exec db-query])

(match (db-open "app.db")
  (ok db)
    (do
      (db-exec db "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)")
      (db-exec db "INSERT INTO users (name) VALUES ('Alice')")
      (match (db-query db "SELECT id, name FROM users" [])
        (ok rows) (for [row rows] (println (get row "name")))
        (err msg) (println "query failed:" msg))
      (db-close db))
  (err msg)
    (println "failed to open database:" msg))
```

**Notes:**

- `db-query` returns `(result (list (map string any)))` -- column names as
  string keys, values as their native Turmeric types.
- All statement resources are closed on `db-close`; callers can also finalize
  explicitly via `stmt-finalize`.
- The SQLite amalgamation (`sqlite3.c`) is compiled as a single C translation
  unit -- no system SQLite is used, so the version is always pinned and
  reproducible.

---

### `tur-raylib` -- Raylib graphics and input

**Purpose:** Turmeric bindings for the Raylib game/graphics library. Covers
window management, 2D and 3D drawing, texture loading, text rendering, audio,
and input. Depends on `tur-math` so that `Vec2`, `Vec3`, and `Mat4` are shared
types rather than duplicated.

**C deps:** Raylib (via cmake-deps)

**Module layout:**

```
spices/raylib/src/
  core.tur      -- init-window, close-window, begin-drawing, end-drawing,
                   begin-mode-3d, end-mode-3d, set-target-fps, window-should-close
  camera.tur    -- Camera3D / Camera2D types, update-camera
  shapes.tur    -- draw-circle, draw-rectangle, draw-line, draw-triangle
  textures.tur  -- load-texture, unload-texture, draw-texture
  models.tur    -- load-model, unload-model, draw-model, draw-mesh
  text.tur      -- load-font, draw-text, draw-text-ex, measure-text
  audio.tur     -- init-audio-device, load-sound, play-sound, load-music-stream
  input.tur     -- is-key-down, is-key-pressed, is-mouse-button-down,
                   get-mouse-position, get-mouse-delta
  color.tur     -- color constants (red, green, blue, ...), fade, color-lerp
```

**`build.tur`:**

```turmeric
(defpackage tur-raylib
  :name        "tur-raylib"
  :version     "0.1.0"
  :description "Raylib graphics and input bindings for Turmeric"
  :license     "MIT"
  :spices {
    "math" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "math-v0.1.0"
            :subdir "spices/math"}
    "test" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
  }
  :cmake-deps {
    "raylib" {:url     "https://github.com/raysan5/raylib"
              :ref     "5.5"
              :options {:BUILD_SHARED_LIBS "OFF"
                        :BUILD_EXAMPLES   "OFF"
                        :BUILD_GAMES      "OFF"}}
  }
  :exports {
    "raylib/core"     ["init-window" "close-window" "begin-drawing" "end-drawing"
                       "begin-mode-3d" "end-mode-3d" "clear-background"
                       "set-target-fps" "get-frame-time" "window-should-close"]
    "raylib/camera"   ["camera-3d" "camera-2d" "update-camera"]
    "raylib/shapes"   ["draw-circle" "draw-circle-v" "draw-rectangle"
                       "draw-rectangle-rec" "draw-line" "draw-line-v"
                       "draw-triangle"]
    "raylib/textures" ["load-texture" "unload-texture" "draw-texture"
                       "draw-texture-v" "draw-texture-ex"]
    "raylib/models"   ["load-model" "unload-model" "draw-model" "draw-model-ex"
                       "load-mesh" "draw-mesh"]
    "raylib/text"     ["load-font" "unload-font" "draw-text" "draw-text-ex"
                       "measure-text"]
    "raylib/audio"    ["init-audio-device" "close-audio-device" "load-sound"
                       "unload-sound" "play-sound" "load-music-stream"
                       "unload-music-stream" "play-music-stream" "update-music-stream"]
    "raylib/input"    ["is-key-down" "is-key-pressed" "is-key-released"
                       "is-mouse-button-down" "is-mouse-button-pressed"
                       "get-mouse-position" "get-mouse-delta" "get-mouse-wheel-move"]
    "raylib/color"    ["color" "red" "green" "blue" "white" "black" "gray"
                       "yellow" "orange" "purple" "pink" "skyblue"
                       "fade" "color-lerp"]
  })
```

**Usage:**

```turmeric
(import raylib/core   :refer [init-window close-window begin-drawing end-drawing
                               clear-background set-target-fps window-should-close])
(import raylib/shapes :refer [draw-circle-v])
(import raylib/input  :refer [get-mouse-position])
(import raylib/color  :refer [raywhite red])

(defn main [] :int
  (init-window 800 600 "Hello Raylib")
  (set-target-fps 60)
  (while (not (window-should-close))
    (let [pos (get-mouse-position)]
      (begin-drawing)
      (clear-background raywhite)
      (draw-circle-v pos 40.0 red)
      (end-drawing)))
  (close-window)
  0)
```

**Platform notes:**

- macOS requires linking against `Cocoa`, `IOKit`, and `CoreVideo` --
  Raylib's own CMakeLists.txt handles this automatically.
- Linux requires X11 or Wayland headers; CI should cover both.
- CI must pass on macOS and Linux before `tur-raylib` is tagged stable.
  Windows support is a stretch goal and not required for the initial release.
- Raylib officially supports Emscripten (uses WebGL/OpenAL in place of
  OpenGL/native audio). WASM builds via `--target wasm` are expected to work
  once the P0 prerequisite lands.

---

## Tier 3 -- Networking and Data

These fill out the ecosystem for server-side and data-processing use cases.
They are lower priority than Tier 2 because they do not unblock any other
spices, but they cover the highest-frequency real-world needs after graphics
and databases.

---

### `tur-json` -- JSON parsing and serialization

**Purpose:** Parse JSON into Turmeric values (`map`, `list`, `str`, `int`,
`f64`, `bool`, `nil`) and serialize them back. Uses `yyjson` as the C backend
for performance on large payloads. A pure-Turmeric fallback parser is also
provided for zero-C-deps environments. yyjson itself compiles cleanly under
Emscripten, so the C backend is available on both native and WASM targets once
`--target wasm` is supported.

**C deps:** yyjson (MIT licensed, Emscripten-compatible)

**Module layout:**

```
spices/json/src/
  parse.tur    -- json/parse : string -> result(any) (dispatches to C or fallback)
  parse_tur.tur -- pure-Turmeric fallback parser (zero C deps)
  emit.tur     -- json/emit  : any -> result(string)
  patch.tur    -- json/get-in, json/set-in, json/merge (structural operations)
```

**`build.tur`:**

```turmeric
(defpackage tur-json
  :name        "tur-json"
  :version     "0.1.0"
  :description "JSON parsing and serialization for Turmeric"
  :license     "MIT"
  :cmake-deps {
    "yyjson" {:url     "https://github.com/ibireme/yyjson"
              :ref     "0.10.0"
              :options {:YYJSON_BUILD_TESTS "OFF"
                        :YYJSON_BUILD_DOCS  "OFF"}}
  }
  :exports {
    "json/parse" ["json-parse"]
    "json/emit"  ["json-emit" "json-emit-pretty"]
    "json/patch" ["json-get-in" "json-set-in" "json-merge"]
  })
```

**Usage:**

```turmeric
(import json/parse :refer [json-parse])
(import json/emit  :refer [json-emit])
(import json/patch :refer [json-get-in])

(match (json-parse "{\"name\": \"Alice\", \"age\": 30}")
  (ok obj)
    (do
      (println (json-get-in obj ["name"]))      ; => "Alice"
      (println (json-emit obj)))
  (err msg)
    (println "parse error:" msg))
```

---

### `tur-http` -- Async HTTP/HTTPS client

**Purpose:** Fiber-native async HTTP/1.1 client built directly on
`stdlib/async_socket.tur` and the cooperative scheduler. TLS is handled by
mbedTLS (via cmake-deps), giving HTTPS without pulling in libcurl. Callers
run inside a fiber and park on I/O; the scheduler multiplexes connections
transparently. A minimal response type carries the status code, headers, and
body.

**C deps:** mbedTLS (Apache 2.0, Emscripten-compatible)

**Module layout:**

```
spices/http/src/
  client.tur   -- http-get, http-post, http-put, http-delete, http-request
  request.tur  -- Request type: method, url, headers, body
  response.tur -- Response type: status, headers, body
  tls.tur      -- thin mbedTLS wrapper used by client.tur for HTTPS
  error.tur    -- HttpError and TlsError constructors
```

**`build.tur`:**

```turmeric
(defpackage tur-http
  :name        "tur-http"
  :version     "0.1.0"
  :description "Async HTTP/HTTPS client for Turmeric (async_socket + mbedTLS)"
  :license     "MIT"
  :spices {
    "json" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "json-v0.1.0"
            :subdir "spices/json"
            :optional true}
  }
  :cmake-deps {
    "mbedtls" {:url     "https://github.com/Mbed-TLS/mbedtls"
               :ref     "v3.6.2"
               :options {:ENABLE_TESTING  "OFF"
                         :ENABLE_PROGRAMS "OFF"}}
  }
  :exports {
    "http/client"   ["http-get" "http-post" "http-put" "http-delete" "http-request"]
    "http/request"  ["request"]
    "http/response" ["response-status" "response-header" "response-body"
                     "response-json"]
  })
```

**Usage:**

```turmeric
(import http/client   :refer [http-get])
(import http/response :refer [response-status response-body response-json])

;; Must be called from inside a fiber (the scheduler parks on I/O)
(match (http-get "https://api.example.com/users/1" {})
  (ok resp)
    (do
      (println "status:" (response-status resp))
      (match (response-json resp)
        (ok obj) (println "name:" (json-get-in obj ["name"]))
        (err _)  (println "body:" (response-body resp))))
  (err msg)
    (println "request failed:" msg))
```

**Notes:**

- `http-get` and friends must be called from within a fiber. The simplest
  entry point is `(fiber-new main-fn 0)` at program startup.
- mbedTLS compiles cleanly under Emscripten. For WASM targets the browser's
  own TLS stack sits beneath the WebSocket transport, so mbedTLS is effectively
  a no-op in that context -- this detail is handled in `tls.tur` via a
  compile-time flag.

---

### `tur-regex` -- PCRE2 regex bindings

**Purpose:** Regular expression matching and capture via PCRE2. Exposes
compile-once/match-many semantics so patterns are not recompiled on every
call. Returns `result` types; capture groups come back as a list of
`option string` (named captures are also supported via a map). PCRE2
compiles cleanly under Emscripten.

**C deps:** PCRE2 (BSD licensed, Emscripten-compatible)

**Module layout:**

```
spices/regex/src/
  regex.tur    -- regex-compile, regex-match, regex-match-all, regex-replace
  capture.tur  -- capture group accessors: capture-at, capture-named, capture-count
  error.tur    -- RegexError constructors
```

**`build.tur`:**

```turmeric
(defpackage tur-regex
  :name        "tur-regex"
  :version     "0.1.0"
  :description "PCRE2 regular expression bindings for Turmeric"
  :license     "MIT"
  :spices {
    "test" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
  }
  :cmake-deps {
    "pcre2" {:url     "https://github.com/PCRE2Project/pcre2"
             :ref     "pcre2-10.44"
             :options {:PCRE2_BUILD_TESTS    "OFF"
                       :PCRE2_BUILD_PCRE2_8  "ON"
                       :PCRE2_BUILD_PCRE2_16 "OFF"
                       :PCRE2_BUILD_PCRE2_32 "OFF"}}
  }
  :exports {
    "regex/regex"   ["regex-compile" "regex-match" "regex-match-all" "regex-replace"]
    "regex/capture" ["capture-at" "capture-named" "capture-count"]
  })
```

**Usage:**

```turmeric
(import regex/regex   :refer [regex-compile regex-match])
(import regex/capture :refer [capture-at capture-named])

(match (regex-compile "(?P<year>\\d{4})-(?P<month>\\d{2})-(?P<day>\\d{2})")
  (ok pat)
    (match (regex-match pat "2026-05-21")
      (ok caps)
        (do
          (println (capture-named caps "year"))   ; => "2026"
          (println (capture-named caps "month"))) ; => "05"
      (err _) (println "no match"))
  (err msg) (println "compile error:" msg))
```

---

## Phased Release Plan

| Phase | Spices | Goal |
|-------|--------|------|
| P0 | -- | Ship `:subdir` in `tur add` and `--target wasm` in `tur build` (prerequisites) |
| P1 | `tur-test`, `tur-math` | Validate pure-Turmeric spices and workspace tooling |
| P2 | `tur-sqlite` | Validate cmake-deps + Emscripten with a simple C dep |
| P3 | `tur-raylib` | Validate cmake-deps with a platform-specific C dep; CI on macOS + Linux |
| P4 | `tur-json` | Validate cmake-deps with a small C dep; ship pure-Turmeric fallback |
| P5 | `tur-http` | Async HTTP/HTTPS via async_socket + mbedTLS |
| P6 | `tur-regex` | PCRE2 bindings |

Each phase ships a `CHANGELOG.md` entry and a tagged release per spice
(`<spice>-vMAJOR.MINOR.PATCH`) before the next phase begins.

---

## Decisions Log

These questions were resolved during initial planning.

| Question | Decision |
|----------|----------|
| Async HTTP approach | Fiber-native via `async_socket` + mbedTLS; no libcurl |
| TLS library | mbedTLS (MIT, Emscripten-compatible) |
| JSON WASM strategy | yyjson via cmake-deps on native and WASM; pure-Turmeric fallback for zero-C-deps scenarios |
| Platform CI for tur-raylib | macOS + Linux required; Windows is a stretch goal |
| tur-regex | Added as P6 using PCRE2 |
| Emscripten support | `--target wasm` in `tur build` is a P0 prerequisite; all cmake-deps spices target Emscripten compatibility where the underlying C lib supports it |
