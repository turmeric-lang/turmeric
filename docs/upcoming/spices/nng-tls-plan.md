# Plan: `tur-nng` TLS -- `wss://` and `tls+tcp://` through a new `nng/tls` module

> Status: draft (2026-10-17). Spiked end to end against tur v0.65.5 (`main`
> 3d9e02a2), nng v1.12.4 and mbedTLS v3.6.2: native build, link, the existing
> suites, a Turmeric prototype of the API, and a real Chromium client. See
> "Spike record".
> Tracks: tur-nng README "Not in v0" (TLS / `wss://`; explicit dialer and
> listener handles), [nng-spice-plan](../../archive/nng-spice-plan.md).
> Scope: `spices/nng/` in turmeric-spices, plus one option added to the
> `mbedtls` entry of `tls`, `http` and `ws-client`. No compiler change is
> needed; five `tur` defects were found and filed (see "Found on the way").
> Builds on: tur-nng 0.1.0 with NG-A/B/C
> ([nng-async-plan](../../archive/nng-async-plan.md)), nng pinned v1.12.4.
> Type: Networking / spice (turmeric-spices).

---

## Overview

tur-nng builds nng with `NNG_ENABLE_TLS=OFF`, so `wss://` and `tls+tcp://`
addresses are `(err NNG_ENOTSUP)`. Browsers are the main reason to want TLS:
a page served over `https://` may only open `wss://`, and the README's
"Plain WebSocket peers (browsers)" section stops at plain `ws://`.

This plan turns TLS on **inside the nng spice's own native build**, sharing the
one mbedTLS 3.6.2 build that `tur-tls`, `tur-http` and `tur-ws-client` already
declare. It adds a Turmeric API as a new module, `nng/tls`, in the same spice:

```turmeric
(import nng/socket :refer [rep-open close])
(import nng/tls    :refer [tls-server-new tls-server-cert-key-file
                           tls-server-free listen-tls])

(let [rep (ok-val (rep-open))
      cfg (ok-val (tls-server-new))
      c   (tls-server-cert-key-file cfg "server.pem")   ;; cert, then key
      l   (listen-tls rep "wss://0.0.0.0:8443/rpc" cfg)]
  (tls-server-free cfg)              ;; the listener keeps its own reference
  ...
  (close rep))
```

```js
// and the browser side, unchanged from ws:// apart from the scheme
const ws = new WebSocket("wss://example.com:8443/rpc", "rep.sp.nanomsg.org");
```

## Why inside the nng spice, not a separate `nng-wss` spice

A separate spice that depends on `nng` and `tls` was the first idea. It does
not work today, for three reasons:

1. **TLS is fixed when libnng is built.** nng does have a runtime engine hook
   (`nng_tls_engine_register`), but `tls_common.c` compiles it, and everything
   else in TLS, only under `NNG_SUPP_TLS`. That is defined only when an engine
   (`mbed` or `wolf`) is chosen at configure time. A TLS-off libnng is all
   stubs. Another spice cannot add TLS to it at link time.
2. **A second spice cannot change the nng spice's options.** Re-declaring
   `"nng"` with TLS options gets deduplicated against the nng spice's entry,
   and the copy collected first keeps its options silently
   ([cmake-deps-duplicate-keeps-first-options](../../archive/cmake-deps-duplicate-keeps-first-options.md)).
   Even with a merge, `NNG_ENABLE_TLS` `OFF` against `ON` is a true conflict
   with no rule for who wins, and the nng spice's entry would also need
   configuring after `mbedtls`
   ([cmake-deps-no-cross-spice-override-or-order](../../archive/cmake-deps-no-cross-spice-override-or-order.md),
   closed 2026-10-21: a dependent's option now wins, and a `:spices`
   dependency's entries are configured first).
3. **A second nng build under another name cannot coexist.** Two `nng` CMake
   targets in one generated project is a configure error, and two copies of
   libnng's symbols in one link is a duplicate-symbol error.

The Turmeric half could live in a separate spice; `Socket`'s id is reachable
the same way `nng/ctx` reaches it. But the native half has to be in nng
regardless, so the API goes next to it. If the two `tur` gaps above close,
TLS can move back to an opt-in, and a separate spice becomes viable (see
"Follow-ups").

## Non-goals

- **Explicit dialer / listener handles.** `listen-tls` and `dial-tls` create
  and start an nng listener or dialer internally and tie its lifetime to the
  socket, exactly as `listen` / `dial` do via `nng_listen` / `nng_dial`. The
  handles stay in "Not in v0".
- **PSK, encrypted private keys, TLS version pinning.** nng has
  `nng_tls_config_psk`, a passphrase argument and `nng_tls_config_version`.
  Each is one binding when someone needs it.
- **A system CA store.** nng's engine trusts only what it is given. A client
  dialing a public server passes a CA bundle explicitly, as `ws-client`'s
  `ws-connect-with-ca` does.
- **Sharing TLS state with `tur-tls`.** The two spices share the mbedTLS
  *library*, not contexts. `tur-tls`'s `TlsCtx` wraps a socket fd that httpd
  owns, while nng drives mbedTLS through its own engine on its own threads.

## Native build (NT0)

### `spices/nng/build.tur`

As spiked, plus two pins of nng defaults the spike left implicit
(`NNG_TLS_ENGINE "mbed"`, `NNG_TRANSPORT_TLS "ON"`), so a future default flip
cannot change the build silently. Other keys are unchanged:

```turmeric
:cmake-deps #map{
  ;; Declared here, ahead of "nng": nng's CMake reuses an in-project mbedtls
  ;; only if the target already exists when nng configures. (Reaching it
  ;; through the tls spice would work too since 2026-10-21, when :spices
  ;; entries started configuring first --
  ;; cmake-deps-no-cross-spice-override-or-order.) Same :url and :ref as
  ;; tls / http / ws-client, so a program using any of them gets ONE mbedTLS
  ;; build.
  "mbedtls" #map{:url     "https://github.com/Mbed-TLS/mbedtls"
                 :ref     "v3.6.2"
                 :targets ["mbedtls" "mbedx509" "mbedcrypto"]
                 :options #map{:ENABLE_PROGRAMS    "OFF"
                               :ENABLE_TESTING     "OFF"
                               :USE_SHARED_MBEDTLS_LIBRARY "OFF"
                               :USE_STATIC_MBEDTLS_LIBRARY "ON"
                               :MBEDTLS_FATAL_WARNINGS "OFF"
                               ;; nng's install(EXPORT) names the mbedtls target;
                               ;; mbedTLS exports it only when this is OFF, and
                               ;; defaults it ON as a subproject. Without it the
                               ;; configure exits 1.
                               :DISABLE_PACKAGE_CONFIG_AND_INSTALL "OFF"}}
  "nng" #map{:url     "https://github.com/nanomsg/nng"
             :ref     "v1.12.4"
             ;; The mbedtls targets are repeated so their archives follow
             ;; libnng.a on the link line; GNU ld fails otherwise
             ;; (cmake-dep-link-walk-stops-at-archive).
             :targets ["nng" "mbedtls" "mbedx509" "mbedcrypto"]
             :options #map{:BUILD_SHARED_LIBS "OFF"
                           :NNG_TESTS         "OFF"
                           :NNG_TOOLS         "OFF"
                           :NNG_ENABLE_NNGCAT "OFF"
                           :NNG_ENABLE_TLS    "ON"
                           :NNG_TLS_ENGINE    "mbed"
                           :NNG_TRANSPORT_TLS "ON"
                           :NNG_TRANSPORT_WS  "ON"
                           :NNG_TRANSPORT_WSS "ON"}}}
```

The comment block above the `nng` entry that explains `NNG_ENABLE_TLS "OFF"`
and `NNG_TRANSPORT_WSS "OFF"` is rewritten to match.

### The sibling `mbedtls` entries (`tls`, `http`, `ws-client`)

Add `:DISABLE_PACKAGE_CONFIG_AND_INSTALL "OFF"` to each. Whichever copy of
the `mbedtls` entry is collected first is the one that gets built, options
and all ([cmake-deps-duplicate-keeps-first-options](../../archive/cmake-deps-duplicate-keeps-first-options.md)).
A program using nng and one of them gets that one's copy about half the
time, depending on the order of its `:spices` map. The option only enables
mbedTLS's own `install()` / export rules, which `tur` never runs, so it
changes nothing else for those spices. The edit becomes unnecessary once that
report's union fix lands, and should be reverted then.

Any third-party spice that declares `mbedtls` v3.6.2 without the option can
still win the race and break the configure. The README's troubleshooting note
names the error (CMake's
`install(EXPORT "nng-target" ...) includes target "nng" which requires target
"mbedtls" that is not in any export set`) and the one-line fix.

### What it costs every nng user

Measured on Linux x86-64, gcc 13.3, the deps' default (unset) CMake build
type:

| | TLS off (today) | TLS on |
| --- | --- | --- |
| `tur fetch` in `spices/nng`, cold | (not measured) | 51 s, nng + mbedTLS, 4 cores |
| minimal program (`pair-open` + `close`), stripped | 493,120 B | 1,583,752 B |

The size is paid by every nng program, TLS or not: nng initialises its TLS
engine at startup (`nni_tls_sys_init` calls `nng_tls_engine_init_mbed`), and
that pulls in the engine and the mbedTLS code it references. A program that
also uses `tls`, `http` or `ws-client` already links most of it.

nng's configure prints mbedTLS's licence notice. mbedTLS 3.6 is Apache-2.0 OR
GPL-2.0-or-later, and it is now inside every nng binary. The README says so.

## API (NT1) -- `nng/tls` (`src/nng/tls.tur`, net-new)

Two linear opaques rather than one. nng's own `nng_tls_config` carries its
mode, but the spike shows `nng_listener_set_ptr` accepts a *client*-mode
config on a listener and returns 0. Splitting the type turns that into a
compile error. The names carry the `Nng` prefix because opaque names resolve
globally and `tur-tls` already owns `TlsCtx` / `TlsConn` (the reasoning that
made `Ctx` `NngCtx`).

```turmeric
(defopaque NngTlsServer :int :linear)   ;; consumed by tls-server-free
(defopaque NngTlsClient :int :linear)   ;; consumed by tls-client-free

;; Peer verification. nng's defaults (mbedtls/tls.c config_init): a server
;; config starts at TlsVerifyNone (no client certs), a client config at
;; TlsVerifyRequired.
(defdata TlsVerify :copy
  (TlsVerifyNone)
  (TlsVerifyOptional)
  (TlsVerifyRequired))
```

| Function | Signature | Notes |
| --- | --- | --- |
| `tls-server-new` | `[] : (Result NngTlsServer int)` | `nng_tls_config_alloc(..., NNG_TLS_MODE_SERVER)` |
| `tls-server-own-cert` | `[^borrow s : NngTlsServer cert : cstr key : cstr] : (Result nil int)` | PEM text; an unencrypted key |
| `tls-server-cert-key-file` | `[^borrow s : NngTlsServer path : cstr] : (Result nil int)` | one PEM file, cert then key. Missing file: `NNG_ENOENT` |
| `tls-server-ca-chain` / `tls-server-ca-file` | `[^borrow s pem-or-path : cstr] : (Result nil int)` | CAs for client certs (mutual TLS) |
| `tls-server-verify` | `[^borrow s : NngTlsServer v : TlsVerify] : (Result nil int)` | `TlsVerifyRequired` = mutual TLS |
| `tls-server-free` | `[s : NngTlsServer] : nil` | safe right after `listen-tls`: nng refcounts the config |
| `tls-client-new` | `[] : (Result NngTlsClient int)` | |
| `tls-client-ca-chain` / `tls-client-ca-file` | `[^borrow c pem-or-path : cstr] : (Result nil int)` | what the client trusts |
| `tls-client-server-name` | `[^borrow c : NngTlsClient name : cstr] : (Result nil int)` | the name checked against the certificate. Needed when dialing by IP |
| `tls-client-verify` | `[^borrow c : NngTlsClient v : TlsVerify] : (Result nil int)` | `TlsVerifyNone` is the development-only escape, like `ws-connect-insecure` |
| `tls-client-own-cert` / `tls-client-cert-key-file` | as the server's | a client certificate, for mutual TLS |
| `tls-client-free` | `[c : NngTlsClient] : nil` | |
| `listen-tls` | `[^borrow s : Socket url : cstr ^borrow c : NngTlsServer] #fx{Net} : (Result nil int)` | create, attach, start |
| `listen-tls-bound-port` | same, `: (Result int int)` | for port 0, as `listen-bound-port` |
| `dial-tls` | `[^borrow s : Socket url : cstr ^borrow c : NngTlsClient] #fx{Net} : (Result nil int)` | synchronous first connect, as `dial` |
| `tls-handshake-failed?` | `[code : int] : bool` | `NNG_EPEERAUTH` (27) or `NNG_ECRYPTO` (26). A wrong server name is `ECRYPTO`, an untrusted certificate `EPEERAUTH` |
| `tls-engine-description` | `[] : cstr` | `"Mbed TLS 3.6.2"`, for diagnostics |

Rules the docstrings and README state, each observed in the spike:

- **A config is frozen once a listener or dialer has started with it.** A
  setter then returns `NNG_EBUSY`.
- **`listen-tls` on a non-TLS scheme** (`ws://`, `tcp://`) is `NNG_ENOTSUP`,
  straight from nng.
- **Plain `listen`, `listen-bound-port` and `dial` refuse the TLS schemes**
  (`wss`, `wss4`, `wss6`, `tls+tcp`, `tls+tcp4`, `tls+tcp6`) with
  `(err NNG_ENOTSUP)` before calling nng. Otherwise nng accepts
  `nng_listen("wss://...")` with no certificate and returns 0, leaving a
  listener that fails every handshake (its client sees `NNG_ECONNSHUT`). That
  is a silent broken server; refusing keeps today's observable behaviour,
  `wss://` from `listen` is `ENOTSUP`, and points at `listen-tls`.

Implementation notes:

- Every body follows `nng/socket`'s pattern: `nng_socket sk; sk.id =
  (uint32_t)s;`, then `tur_ok_int` / `tur_err_int`. A config handle is the
  `nng_tls_config *` as `intptr_t`, like `Payload`.
- **No region store hooks are needed.** Nothing stores a caller's word: nng
  `strdup`s the server name, and mbedTLS parses PEM text into its own
  structures during the call. The config pointer is C heap, not a Turmeric
  node.
- `nng_tls_config_alloc`, set, start: when start fails, close the half-made
  listener or dialer before returning the error, so a failed `listen-tls`
  leaves nothing bound (the same rule `listen-bound-port` keeps).

## Tests (NT2)

`tests/nng/tls_test.tur`, network on loopback only, every listener on port 0:

- A self-signed certificate (CN and SAN `localhost`, 100-year expiry) is
  embedded as PEM text, as `ws-server/tests/tls_echo_test.tur` does. With
  `tls-server-own-cert` and `tls-client-ca-chain` taking PEM text, nothing is
  written to disk.
- REQ/REP round trip over `wss://` and over `tls+tcp://`, verifying the
  server.
- A binary payload with NUL bytes crosses `wss://` intact.
- A client with no CA: `NNG_EPEERAUTH`. The right CA, wrong server name:
  `NNG_ECRYPTO`. `tls-handshake-failed?` is true for both.
- `TlsVerifyNone` on the client connects with no CA.
- Both configs freed right after `listen-tls` / `dial-tls`: the round trip
  still works.
- A setter on a config in use: `NNG_EBUSY`.
- `listen-tls` on `ws://`: `NNG_ENOTSUP`. Plain `listen` / `dial` on `wss://`
  and `tls+tcp://`: `NNG_ENOTSUP`, and nothing is left bound.
- A missing certificate file: `NNG_ENOENT`.
- Mutual TLS: a server with `TlsVerifyRequired` refuses a client with no
  certificate and accepts one with.

`errors/` gets six linear fixtures (double free, use after free, never freed,
for each of `NngTlsServer` and `NngTlsClient`), and `errors/run.sh` lists
them.

`tests/nng/ws_test.tur`'s case "has no wss:// -- TLS is not compiled in" is
the one existing assertion the build change breaks (spiked: everything else
in `tests/nng` passes with TLS on). It becomes "plain listen refuses wss://
-- use listen-tls", with the same assertion.

A browser over `wss://` is not something CI can run without a browser. The
README gives the manual check (see "Spike record") instead.

## Docs (NT3)

- README: a "TLS: `wss://` and `tls+tcp://`" section; "Plain WebSocket peers
  (browsers)" gains the `wss://` case (the browser must trust the certificate:
  a real one, or `mkcert` for development); the "Not in v0" TLS bullet goes,
  leaving PSK and ZeroTier; the build-cost table and licence note above; the
  troubleshooting note for the `install(EXPORT)` configure error. The
  `## Testing` assertion counts change.
- The nng guide in turmeric-spices `docs/guides/` gets the same TLS section.
- `build.tur`: `"nng/tls"` in `:exports`; `:version` bump (minor).
- Archive this plan when NT0-NT3 land.

## Phases

| Phase | Lands | Depends on |
| --- | --- | --- |
| NT0 | `build.tur` changes in nng, tls, http, ws-client; `ws_test` retitled; plain `listen` / `dial` refuse TLS schemes | -- |
| NT1 | `nng/tls` module | NT0 |
| NT2 | `tls_test.tur`, `errors/` fixtures | NT1 |
| NT3 | README, guide, exports, version | NT1 |

One turmeric-spices PR is the expected shape; NT0 has no value shipped alone.

## Risks and open questions

1. **Another spice's `mbedtls` copy without the option.** Covered above.
   Retires when [cmake-deps-duplicate-keeps-first-options](../../archive/cmake-deps-duplicate-keeps-first-options.md)
   is fixed by union.
2. **mbedTLS version coupling.** nng's mbedTLS is now whatever `tls` pins.
   Bumping mbedTLS means moving every copy together, which the hard error on
   mismatched `:ref` already enforces.
3. **Windows (MSYS2/UCRT64).** Not spiked. nng's mbedTLS engine and mbedTLS
   both build there upstream; the spices CI's Windows leg, if it covers nng,
   is the check.
4. **macOS.** Not spiked here. ld64 is insensitive to archive order, so the
   `:targets` workaround is harmless there. `MBEDTLS_FATAL_WARNINGS "OFF"`
   covers the AppleClang warning `tls` already works around.
5. **Leak checking.** `tls_test.tur` will not import `reactor`, so spices CI
   runs it without LeakSanitizer
   ([spice-suites-without-libturi-run-unsanitized](../../reported/spice-suites-without-libturi-run-unsanitized.md)).
   Run it once locally with `TUR_CC_FLAGS="-fsanitize=address,undefined"`
   before landing.
6. **The size cost.** If ~1 MB per nng program turns out to matter to
   someone, the answer is the follow-up below, not a second build of nng.

## Follow-ups (not this plan)

- **TLS as an opt-in.** A consumer can now set an option on a dependency's
  entry and get it configured after `mbedtls`
  ([cmake-deps-no-cross-spice-override-or-order](../../archive/cmake-deps-no-cross-spice-override-or-order.md),
  closed 2026-10-21), so TLS can go back off by default, with a program or a
  thin `nng-tls` spice opting in. The `nng/tls` API does not change.
- **Explicit dialer / listener handles**, and with them per-endpoint
  WebSocket options (extra headers, frame sizes).
- **A raw-WebSocket-over-TLS test client** in `ws_test.tur`, so the
  browser-shaped checks run over `wss://` too. It needs mbedTLS from inline C
  in the test, which the build now provides.

## Spike record (2026-10-17)

Linux x86-64, gcc 13.3.0, GNU ld 2.42, CMake 3.28.3. tur v0.65.5 built from
`main` 3d9e02a2 (Debug, no sanitizers, no JIT); turmeric-spices `main`
fa42ece.

1. **CMake only, shaped like `tur fetch`'s generated project** (one project,
   `mbedtls` then `nng`, options `FORCE`d into the cache): nng's
   `if (TARGET mbedtls)` picks up the in-project mbedTLS. A C program did a
   REQ/REP round trip over `wss://` with the client verifying the server
   (`nng_tls_engine_description()` = "Mbed TLS 3.6.2"). A client with no CA
   was refused with "Peer could not be authenticated". The configure exited
   1 until `DISABLE_PACKAGE_CONFIG_AND_INSTALL` was `OFF`.
2. **Edge cases**, the same harness: `tls+tcp://` round trip; configs freed
   right after attach still work; wrong server name is `NNG_ECRYPTO` (26) and
   no CA is `NNG_EPEERAUTH` (27); plain `nng_listen` on `wss://` with no
   certificate returns 0 and its dialer gets `NNG_ECONNSHUT`; a client-mode
   config is accepted on a listener (0); a config on a `ws://` listener is
   `NNG_ENOTSUP`; a setter after start is `NNG_EBUSY`; a missing file is
   `NNG_ENOENT`.
3. **Chromium** (Playwright 1.56, `--ignore-certificate-errors` for the
   self-signed certificate): `new WebSocket("wss://127.0.0.1:18443/feed",
   "pub.sp.nanomsg.org")` against an nng PUB listener received
   `["tick 27","tick 28","tick 29"]` (the earlier ticks predate the
   connection, the slow-joiner rule).
4. **Through tur**, on a copy of `spices/nng` with the `build.tur` above:
   `tur fetch` exit 0 in 51 s. `tur test tests/nng`: every suite passes except
   the one `ws_test` case NT0 retitles. A Turmeric prototype of
   `tls-server-new` / `tls-client-new` / `tls-server-own-cert` /
   `tls-client-ca-chain` / `tls-client-server-name` / `listen-tls-bound-port`
   / `dial-tls`, written with the two linear opaques, passed "round-trips
   REQ/REP over wss:// with a verified server" and "refuses a server the
   client has no CA for".
5. **Without the `:targets` workaround** the same link fails under GNU ld
   with undefined `mbedtls_asn1_*` / `mbedtls_cipher_*` references
   ([cmake-dep-link-walk-stops-at-archive](../../archive/cmake-dep-link-walk-stops-at-archive.md)).

## Found on the way

All five were filed in `docs/reported/`; all five are resolved since; none blocks this plan.

| Report | Severity | Effect here |
| --- | --- | --- |
| [cmake-deps-duplicate-keeps-first-options](../../archive/cmake-deps-duplicate-keeps-first-options.md) | medium | resolved 2026-10-21 by union: the sibling `mbedtls` entries no longer need editing, and only the `nng` entry needs the extra option |
| [cmake-deps-no-cross-spice-override-or-order](../../archive/cmake-deps-no-cross-spice-override-or-order.md) | low | resolved 2026-10-21: a dependent's option wins over its dependency's, and dependencies configure first, which is what TLS-as-an-opt-in (the follow-up above) needs |
| [cmake-dep-link-walk-stops-at-archive](../../archive/cmake-dep-link-walk-stops-at-archive.md) | medium | resolved 2026-10-21: the link walk follows an archive's own dependencies, so the `nng` entry no longer needs to list the mbedTLS targets |
| [cmake-dir-markers-print-link-flag-warnings](../../archive/cmake-dir-markers-print-link-flag-warnings.md) | low | resolved 2026-10-18: the markers are dropped, so no warnings |
| [cmake-dep-path-through-missing-cmake-dir](../../archive/cmake-dep-path-through-missing-cmake-dir.md) | medium | resolved 2026-10-18; none here (nng has no `:path` deps) |

## See also

- [nng-spice-plan](../../archive/nng-spice-plan.md),
  [nng-async-plan](../../archive/nng-async-plan.md)
- nng: `src/supplemental/tls/tls_common.c`, `src/supplemental/tls/mbedtls/`,
  `include/nng/supplemental/tls/tls.h` at v1.12.4
- turmeric-spices: `spices/tls/build.tur`, `spices/ws-client/README.md`
  (the verify-by-default stance this API copies)
