# New Spices Plan: Postgres, Valkey, OSC, RtAudio, RtMidi

> **Status:** Draft Plan
> **Last Updated:** 2026-05-22
> **Type:** Spice Design

---

## Overview

Five new Tier-2 spices (cmake C/C++ dependencies) for the `turmeric-spices` monorepo:

| Spice | Tag | C/C++ dep | Purpose |
|-------|-----|-----------|---------|
| `tur-postgres` | `postgres-v0.1.0` | libpq (PostgreSQL 16) | SQL database client |
| `tur-valkey` | `valkey-v0.1.0` | hiredis 1.2 | Key-value store (Valkey/Redis) |
| `tur-osc` | `osc-v0.1.0` | liblo 0.32 | Open Sound Control messaging |
| `tur-rtaudio` | `rtaudio-v0.1.0` | RtAudio 6.0 | Cross-platform audio I/O |
| `tur-rtmidi` | `rtmidi-v0.1.0` | RtMidi 6.0 | Cross-platform MIDI I/O |
| `tur-wav` | `wav-v0.1.0` | libsndfile 1.2 | WAV (and PCM audio format) read/write |
| `tur-png` | `png-v0.1.0` | libpng 1.6 | PNG image read/write |

All five are Tier 2: pure Turmeric source wrapping a C (or C-wrapped C++) library fetched via CMake CPM. No additional tooling beyond what `tur-sqlite`, `tur-http`, etc. already require.

---

## Conventions

All five spices follow the same layout used by `tur-sqlite` and `tur-http`:

```
spices/<name>/
  build.tur           -- defpackage manifest
  src/<name>/
    *.tur             -- one file per exported module
  tests/<name>/
    *_test.tur        -- one test file per module
```

`build.tur` lists `:cmake-deps` with a pinned `:ref`, sets `:exports`, and marks the `test` spice as optional. Inline-C blocks in the source files call directly into the C (or C wrapper) API.

---

## tur-postgres

### C dependency

| Field | Value |
|-------|-------|
| Library | libpq |
| Source | Ships with PostgreSQL; use the standalone mirror `https://github.com/postgres/postgres` or a system install via `find_package(PostgreSQL)` |
| Preferred cmake strategy | `find_package(PostgreSQL REQUIRED)` first; fall back to a static build of the libpq sources via CPM if not found |
| Header | `#include <libpq-fe.h>` |

### Modules and exports

```
postgres/db     -- connection lifecycle
postgres/stmt   -- prepared statements and parameter binding
postgres/row    -- result iteration and column access
postgres/notify -- asynchronous LISTEN/NOTIFY
```

### API sketch

```turmeric
;; postgres/db
(db-connect "host=localhost dbname=myapp user=alice password=secret")
;; => result<conn :int>
(db-close conn)
(db-exec conn "CREATE TABLE ...")          ;; => result<:void>
(db-query conn "SELECT * FROM users")      ;; => result<rows :int>
(db-query-params conn "SELECT * FROM users WHERE id = $1"
                 (cons "42" 0))            ;; => result<rows :int>

;; postgres/stmt
(stmt-prepare conn "get-user" "SELECT * FROM users WHERE id = $1" 1)
(stmt-exec-prepared conn "get-user" (cons "42" 0))   ;; => result<rows :int>
(stmt-deallocate conn "get-user")

;; postgres/row
(rows-count rows)                          ;; => :int
(rows-free rows)
(row-get rows i "name")                    ;; => :cstr (text)
(row-get-int rows i "age")                 ;; => :int
(col-count rows)                           ;; => :int
(col-name rows j)                          ;; => :cstr

;; postgres/notify
(notify-listen conn "channel")             ;; => result<:void>
(notify-unlisten conn "channel")
(notify-poll conn)                         ;; => option<notification :int>
(notification-channel n)                   ;; => :cstr
(notification-payload n)                   ;; => :cstr
```

### Implementation phases

- [ ] **PG0** -- `build.tur` manifest; cmake `find_package(PostgreSQL)` integration; `db-connect` and `db-close`
- [ ] **PG1** -- `db-exec`, `db-query`; `rows-count`, `rows-free`, `row-get`, `row-get-int`, `col-count`, `col-name`
- [ ] **PG2** -- `db-query-params` with `$N` placeholders; `stmt-prepare`, `stmt-exec-prepared`, `stmt-deallocate`
- [ ] **PG3** -- transactions (`db-begin`, `db-commit`, `db-rollback`); `notify-listen`, `notify-poll`, `notification-channel/payload`
- [ ] **PG4** -- tests; README section; `postgres-v0.1.0` tag

---

## tur-valkey

Valkey is the open-source fork of Redis maintained by the Linux Foundation. The wire protocol is identical to Redis 7.x, so hiredis works without modification.

### C dependency

| Field | Value |
|-------|-------|
| Library | hiredis |
| URL | `https://github.com/redis/hiredis` |
| Pinned ref | `v1.2.0` |
| Header | `#include <hiredis/hiredis.h>` |
| cmake option | `DISABLE_TESTS ON`, `BUILD_SHARED_LIBS OFF` |

### Modules and exports

```
valkey/client   -- connection lifecycle
valkey/cmd      -- command dispatch (string + typed helpers)
valkey/reply    -- reply type inspection and extraction
valkey/pubsub   -- publish/subscribe
```

### API sketch

```turmeric
;; valkey/client
(client-connect "127.0.0.1" 6379)    ;; => result<client :int>
(client-connect-unix "/tmp/valkey.sock")
(client-close c)
(client-ping c)                       ;; => result<:void>

;; valkey/cmd
(cmd c "SET" (cons "key" (cons "value" 0)))   ;; => result<reply :int>
(cmd c "GET" (cons "key" 0))
(cmd c "DEL" (cons "key" 0))
(cmd c "EXPIRE" (cons "key" (cons "60" 0)))
(cmd c "INCR" (cons "counter" 0))
(cmd c "LPUSH" (cons "list" (cons "a" (cons "b" 0))))
(cmd c "HSET" (cons "hash" (cons "field" (cons "val" 0))))

;; valkey/reply
(reply-type r)       ;; => :cstr -- "string" "integer" "array" "nil" "error"
(reply-string r)     ;; => :cstr
(reply-int r)        ;; => :int
(reply-array-len r)  ;; => :int
(reply-array-get r i);; => reply :int
(reply-free r)

;; valkey/pubsub
(pubsub-subscribe c "channel")           ;; => result<:void>
(pubsub-unsubscribe c "channel")
(pubsub-publish c "channel" "message")   ;; => result<:int>  (receiver count)
(pubsub-recv c)                          ;; => option<message :int>
(message-channel m)                      ;; => :cstr
(message-payload m)                      ;; => :cstr
```

### Implementation phases

- [ ] **VK0** -- `build.tur`; CPM hiredis; `client-connect`, `client-connect-unix`, `client-close`, `client-ping`
- [ ] **VK1** -- `cmd`; `reply-type`, `reply-string`, `reply-int`, `reply-free`
- [ ] **VK2** -- `reply-array-len`, `reply-array-get`; typed convenience wrappers (`cmd-get`, `cmd-set`, `cmd-del`, `cmd-incr`, `cmd-expire`)
- [ ] **VK3** -- hash/list/set typed helpers (`cmd-hset`, `cmd-hget`, `cmd-lpush`, etc.); `pubsub-subscribe`, `pubsub-recv`, `message-channel/payload`
- [ ] **VK4** -- pipelining (`pipeline-append`, `pipeline-flush`); tests; README section; `valkey-v0.1.0` tag

---

## tur-osc

### C dependency

| Field | Value |
|-------|-------|
| Library | liblo |
| URL | `https://github.com/radarsat1/liblo` |
| Pinned ref | `0.32` |
| Header | `#include <lo/lo.h>` |
| cmake option | `BUILD_SHARED_LIBS OFF`, `WITH_TESTS OFF`, `WITH_EXAMPLES OFF` |

liblo provides a portable, callback-based OSC implementation. The server runs on a background thread managed by liblo; callbacks are registered per address pattern.

### Modules and exports

```
osc/server   -- UDP/TCP receive, pattern dispatch
osc/client   -- UDP/TCP send
osc/msg      -- message construction and inspection
osc/bundle   -- timetag-stamped bundles
```

### API sketch

```turmeric
;; osc/server
(server-new "7770" ":udp")              ;; => result<server :int>
(server-new-tcp "7770")
(server-free s)
(server-add-method s "/freq" ",f" handler-fn)  ;; register typed handler
(server-recv s 100)                     ;; => :int (messages handled; 100ms timeout)
(server-recv-noblock s)

;; osc/client
(client-new "localhost" "7770" ":udp")  ;; => result<client :int>
(client-free c)
(client-send c msg)                     ;; => result<:void>
(client-send-bundle c bundle)

;; osc/msg
(msg-new "/freq")                       ;; => msg :int
(msg-add-float m 440.0)
(msg-add-int m 1)
(msg-add-string m "hello")
(msg-free m)
(msg-path m)                            ;; => :cstr
(msg-arg-count m)                       ;; => :int
(msg-arg-type m i)                      ;; => :cstr  ("f" "i" "s" "b" ...)
(msg-arg-float m i)                     ;; => :float
(msg-arg-int m i)                       ;; => :int
(msg-arg-string m i)                    ;; => :cstr

;; osc/bundle
(bundle-new timetag)                    ;; => bundle :int  (timetag = :float seconds)
(bundle-add-msg b msg)
(bundle-free b)
```

### Implementation phases

- [ ] **OS0** -- `build.tur`; CPM liblo; `server-new`, `server-free`, `client-new`, `client-free`, `client-send`
- [ ] **OS1** -- `msg-new`, `msg-add-{float,int,string}`, `msg-free`; basic send-receive round-trip test
- [ ] **OS2** -- `server-add-method`, `server-recv`, `server-recv-noblock`; `msg-arg-*` inspection
- [ ] **OS3** -- `bundle-new`, `bundle-add-msg`, `client-send-bundle`; timetag helpers
- [ ] **OS4** -- TCP transport; pattern-matching wildcards (`/foo/*`); tests; README section; `osc-v0.1.0` tag

---

## tur-rtaudio

RtAudio is a C++ library with an official C wrapper (`rtaudio_c.h`). Turmeric's inline-C calls the C wrapper API exclusively; no C++ in the spice source.

### C/C++ dependency

| Field | Value |
|-------|-------|
| Library | RtAudio |
| URL | `https://github.com/thestk/rtaudio` |
| Pinned ref | `6.0.1` |
| Header | `#include <rtaudio/rtaudio_c.h>` |
| cmake option | `RTAUDIO_BUILD_TESTING OFF`, `BUILD_SHARED_LIBS OFF` |
| Link | `rtaudio` (the cmake target exports a C-compatible static library) |

### Modules and exports

```
rtaudio/core     -- library init/teardown, API enumeration
rtaudio/devices  -- device enumeration and capability query
rtaudio/stream   -- stream open/start/stop/close, format constants
```

### API sketch

```turmeric
;; rtaudio/core
(audio-new ":alsa")          ;; => result<audio :int>  -- API: :alsa :core-audio :wasapi :jack :dummy
(audio-free a)
(audio-api a)                ;; => :cstr

;; rtaudio/devices
(device-count a)             ;; => :int
(device-info a id)           ;; => device-info :int
(device-info-name di)        ;; => :cstr
(device-info-output-chans di);; => :int
(device-info-input-chans di) ;; => :int
(device-info-sample-rates di);; => list<:int>  (e.g. 44100, 48000, 96000)
(device-default-output a)    ;; => :int
(device-default-input a)     ;; => :int

;; rtaudio/stream
(stream-open a
  :output {:device (device-default-output a)
           :channels 2
           :format ":float32"}
  :input  {:device (device-default-input a)
           :channels 1
           :format ":float32"}
  :sample-rate 48000
  :buffer-frames 512
  :callback callback-fn)     ;; => result<:void>
(stream-start a)             ;; => result<:void>
(stream-stop a)
(stream-close a)
(stream-latency a)           ;; => :int (frames)
(stream-sample-rate a)       ;; => :int
```

Audio callback signature (called from RtAudio's internal thread):

```turmeric
;;; callback -- audio processing callback.
;;;
;;; Parameters:
;;;   out    -- output buffer (:cstr / float*)
;;;   in     -- input buffer  (:cstr / float*)
;;;   frames -- number of sample frames
;;;   time   -- stream time in seconds (:float)
;;;   status -- underflow/overflow flags (:int)
;;;
;;; Returns:
;;;   0 to continue, 1 to drain and stop, 2 to abort.
(defn my-callback [out : cstr in : cstr frames : int time : float status : int] : int
  ...)
```

### Implementation phases

- [ ] **RA0** -- `build.tur`; CPM RtAudio; `audio-new`, `audio-free`
- [ ] **RA1** -- `device-count`, `device-info`, `device-info-*`, `device-default-output/input`
- [ ] **RA2** -- `stream-open` with output-only path; `stream-start`, `stream-stop`, `stream-close`
- [ ] **RA3** -- full-duplex (input + output); `stream-latency`, `stream-sample-rate`; format constants (`:int16`, `:int32`, `:float32`, `:float64`)
- [ ] **RA4** -- tests (sine wave smoke test with dummy API); README section; `rtaudio-v0.1.0` tag

---

## tur-rtmidi

RtMidi ships a C wrapper (`rtmidi_c.h`) alongside the C++ core. Same approach as tur-rtaudio: inline-C calls the C wrapper only.

### C/C++ dependency

| Field | Value |
|-------|-------|
| Library | RtMidi |
| URL | `https://github.com/thestk/rtmidi` |
| Pinned ref | `6.0.0` |
| Header | `#include <rtmidi/rtmidi_c.h>` |
| cmake option | `RTMIDI_BUILD_TESTING OFF`, `BUILD_SHARED_LIBS OFF` |
| Link | `rtmidi` |

### Modules and exports

```
rtmidi/core   -- library init/teardown, API enumeration
rtmidi/in     -- MIDI input port, callback-based receive
rtmidi/out    -- MIDI output port, synchronous send
rtmidi/msg    -- MIDI message constructors and decoders
```

### API sketch

```turmeric
;; rtmidi/core
(midi-in-new ":alsa")         ;; => result<midi-in :int>  -- API: :alsa :core-midi :winmm :jack :dummy
(midi-out-new ":alsa")        ;; => result<midi-out :int>
(midi-in-free mi)
(midi-out-free mo)
(midi-in-port-count mi)       ;; => :int
(midi-out-port-count mo)      ;; => :int
(midi-in-port-name mi i)      ;; => :cstr
(midi-out-port-name mo i)     ;; => :cstr

;; rtmidi/in
(midi-in-open mi i "Turmeric In")   ;; => result<:void>
(midi-in-open-virtual mi "Turmeric Virtual In")
(midi-in-close mi)
(midi-in-set-callback mi callback-fn)  ;; fn [bytes :cstr len :int stamp :float] :void
(midi-in-cancel-callback mi)
(midi-in-ignore-types mi :sysex true :timing true :active-sense true)

;; rtmidi/out
(midi-out-open mo i "Turmeric Out")  ;; => result<:void>
(midi-out-open-virtual mo "Turmeric Virtual Out")
(midi-out-close mo)
(midi-out-send mo bytes len)         ;; raw bytes; use msg constructors below

;; rtmidi/msg
(msg-note-on channel note velocity)    ;; => bytes :cstr, len 3
(msg-note-off channel note velocity)
(msg-control-change channel cc value)
(msg-program-change channel program)
(msg-pitch-bend channel lsb msb)
(msg-aftertouch channel pressure)
(msg-poly-aftertouch channel note pressure)
(msg-sysex bytes len)                  ;; wraps in 0xF0 ... 0xF7

;; Decode incoming bytes
(msg-status bytes)      ;; => :int  (top nibble: 0x80 note-off, 0x90 note-on, 0xB0 cc, ...)
(msg-channel bytes)     ;; => :int  (0-15)
(msg-data1 bytes)       ;; => :int
(msg-data2 bytes)       ;; => :int
```

### Implementation phases

- [ ] **RM0** -- `build.tur`; CPM RtMidi; `midi-in-new`, `midi-out-new`, port enumeration helpers
- [ ] **RM1** -- `midi-out-open`, `midi-out-close`, `midi-out-send`; `msg-note-on/off`, `msg-control-change`, `msg-program-change`
- [ ] **RM2** -- `midi-in-open`, `midi-in-set-callback`, `midi-in-ignore-types`; remaining `msg-*` constructors
- [ ] **RM3** -- `midi-in-open-virtual`, `midi-out-open-virtual`; sysex; `msg-status/channel/data1/data2` decoder helpers
- [ ] **RM4** -- tests (loopback with dummy API or virtual ports on macOS/Linux); README section; `rtmidi-v0.1.0` tag

---

## tur-wav

libsndfile is a C library for reading and writing files containing sampled audio data. It supports WAV, AIFF, FLAC, and other PCM formats through a single unified API.

### C dependency

| Field | Value |
|-------|-------|
| Library | libsndfile |
| URL | `https://github.com/libsndfile/libsndfile` |
| Pinned ref | `1.2.2` |
| Header | `#include <sndfile.h>` |
| cmake option | `BUILD_SHARED_LIBS OFF`, `BUILD_PROGRAMS OFF`, `BUILD_EXAMPLES OFF`, `BUILD_TESTING OFF` |

### Modules and exports

```
wav/reader  -- open and read PCM audio frames
wav/writer  -- create and write PCM audio files
wav/info    -- file metadata (sample rate, channels, frame count, format)
```

### API sketch

```turmeric
;; wav/info
(wav-open-read "file.wav")           ;; => result<wav :int>
(wav-open-write "out.wav"
  :sample-rate 44100
  :channels 2
  :format ":wav-pcm16")             ;; => result<wav :int>
(wav-close w)

;; wav/info
(wav-sample-rate w)                  ;; => :int
(wav-channels w)                     ;; => :int
(wav-frame-count w)                  ;; => :int  (total sample frames)
(wav-format w)                       ;; => :cstr  ("wav" "aiff" "flac" ...)

;; wav/reader -- returns interleaved float samples, channels * frames floats
(wav-read-float w buf frames)        ;; => :int  (frames actually read)
(wav-seek w frame-offset ":set")     ;; => result<:void>  (:set :cur :end)

;; wav/writer
(wav-write-float w buf frames)       ;; => :int  (frames written)
```

Format keyword constants: `:wav-pcm16`, `:wav-pcm24`, `:wav-pcm32`,
`:wav-float`, `:aiff-pcm16`, `:flac-pcm16`, `:flac-pcm24`.

### Implementation phases

- [ ] **WV0** -- `build.tur`; CPM libsndfile; `wav-open-read`, `wav-open-write`, `wav-close`
- [ ] **WV1** -- `wav-sample-rate`, `wav-channels`, `wav-frame-count`, `wav-format`
- [ ] **WV2** -- `wav-read-float`, `wav-write-float`; round-trip test (write then read back)
- [ ] **WV3** -- `wav-seek`; format constants for AIFF and FLAC variants; multi-channel test
- [ ] **WV4** -- tests; README section; `wav-v0.1.0` tag

---

## tur-png

libpng is the official Portable Network Graphics reference library. It reads and writes PNG files, exposing raw row data that the caller interprets as RGBA (or other channel layouts).

### C dependency

| Field | Value |
|-------|-------|
| Library | libpng |
| URL | `https://github.com/glennrp/libpng` |
| Pinned ref | `v1.6.43` |
| Header | `#include <png.h>` |
| cmake option | `PNG_SHARED OFF`, `PNG_STATIC ON`, `PNG_TESTS OFF`, `PNG_TOOLS OFF` |
| Link | `png_static` |
| Note | libpng requires zlib; use `find_package(ZLIB REQUIRED)` before CPM fetch |

### Modules and exports

```
png/reader  -- decode PNG file to raw pixel data
png/writer  -- encode raw pixel data to PNG file
png/info    -- image dimensions, bit depth, color type
```

### API sketch

```turmeric
;; png/reader
(png-read "image.png")               ;; => result<img :int>
                                     ;;    img holds width, height, channels, raw bytes
(img-free img)

;; png/info
(img-width img)                      ;; => :int
(img-height img)                     ;; => :int
(img-channels img)                   ;; => :int  (3 = RGB, 4 = RGBA)
(img-bit-depth img)                  ;; => :int  (8 or 16)
(img-pixels img)                     ;; => :cstr  (raw interleaved bytes, row-major)

;; png/writer
(png-write "out.png" img)            ;; => result<:void>
(png-write-raw "out.png"
  pixels width height channels
  :bit-depth 8)                      ;; => result<:void>

;; Pixel access helpers (8-bit RGBA)
(pixel-r img x y)                    ;; => :int
(pixel-g img x y)                    ;; => :int
(pixel-b img x y)                    ;; => :int
(pixel-a img x y)                    ;; => :int
(pixel-set! img x y r g b a)
```

### Implementation phases

- [ ] **PN0** -- `build.tur`; CPM libpng + zlib detection; `png-read`, `img-free`
- [ ] **PN1** -- `img-width`, `img-height`, `img-channels`, `img-bit-depth`, `img-pixels`
- [ ] **PN2** -- `png-write`, `png-write-raw`; round-trip test (read, write, re-read)
- [ ] **PN3** -- `pixel-r/g/b/a`, `pixel-set!`; 16-bit support; grayscale and grayscale+alpha (`img-channels` = 1 or 2)
- [ ] **PN4** -- tests; README section; `png-v0.1.0` tag

---

## Shared work

### turmeric-spices README

Add rows to the spice table for all five new spices once they reach v0.1.0:

| Spice | Description | Tier | C dep |
|-------|-------------|------|-------|
| `tur-postgres` | PostgreSQL client via libpq | 2 -- cmake-dep | libpq 16 |
| `tur-valkey` | Valkey/Redis client via hiredis | 2 -- cmake-dep | hiredis 1.2 |
| `tur-osc` | Open Sound Control via liblo | 2 -- cmake-dep | liblo 0.32 |
| `tur-rtaudio` | Cross-platform audio I/O via RtAudio | 2 -- cmake-dep | RtAudio 6.0 |
| `tur-rtmidi` | Cross-platform MIDI I/O via RtMidi | 2 -- cmake-dep | RtMidi 6.0 |
| `tur-wav` | WAV/PCM audio read/write via libsndfile | 2 -- cmake-dep | libsndfile 1.2 |
| `tur-png` | PNG image read/write via libpng | 2 -- cmake-dep | libpng 1.6 |

### Guides (docs/guides/ in turmeric core)

Each spice ships a usage guide once the v0.1.0 tag is cut:

- `docs/guides/postgres-guide.md`
- `docs/guides/valkey-guide.md`
- `docs/guides/osc-guide.md` (may live in a combined `audio-io-guide.md` with rtaudio/rtmidi)
- `docs/guides/rtaudio-guide.md`
- `docs/guides/rtmidi-guide.md`
- `docs/guides/wav-guide.md`
- `docs/guides/png-guide.md`

### Suggested build order

RtMidi and RtAudio share the same cmake pattern; build them together. OSC (liblo) is independent. Postgres and Valkey share the connection/result paradigm and can be built in parallel or back-to-back.

Suggested order for a single sprint: **RM** + **RA** in parallel, then **OS**, then **VK**, then **PG** (heaviest dependency). **WV** and **PN** are independent of all others and can be slotted in at any point; **PN** requires a zlib check before the CPM fetch.
