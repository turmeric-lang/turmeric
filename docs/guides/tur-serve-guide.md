---
title: tur serve
category: CLI Tools
description: Serve a directory over HTTP with `tur serve`, the Turmeric spelling of `python3 -m http.server`
tags:
  - httpd
  - io
---

# `tur serve` -- serve a directory over HTTP

`tur serve` is the Turmeric spelling of `python3 -m http.server`: one line,
on a fresh `tur`, with nothing installed and no project.

```sh
tur serve                 # . on http://127.0.0.1:8000/
tur serve dist -p 8080    # a directory and a port
tur serve --bind any      # every interface, the way Python defaults
```

It prints one line when it is listening and then one line per request on
stderr, and runs until Ctrl-C:

```text
Serving dist on http://127.0.0.1:8080/ (Ctrl-C to stop)
GET / -> 200 1342 0ms
GET /app.js -> 200 20511 1ms
```

## Flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `[dir]` | `.` | the directory to serve |
| `-p N`, `--port N` | `8000` | the port; `0` picks a free one and the startup line names it |
| `-b ADDR`, `--bind ADDR` | `127.0.0.1` | `any` (every interface), `localhost`, or a numeric IPv4/IPv6 address |
| `--index NAME` | `index.html` | the file served for a directory |
| `--no-listing` | off | a directory without an index is a 404 instead of a listing |
| `--isolate[=MODE]` | `off` | send COOP/COEP so the page is cross-origin isolated; a bare `--isolate` is `require-corp`, see below |
| `-q`, `--quiet` | off | no request log |
| `-h`, `--help` | | the usage |

A bad flag, a port out of range, or a directory that does not exist prints
the problem and the usage, and exits 2. Every word after `serve` belongs to
the server, so `tur serve --json` is an unknown flag, not `tur --json`.

## What it serves

- **Files** come from the same audited static path as `stdlib/httpd.tur`'s
  `mw-static`: the request path is percent-decoded and must resolve, symlinks
  and all, to a regular file inside the directory. `..`, dotfiles (`.env`,
  `.git/config`; `.well-known` is allowed) and a backslash anywhere are 404.
  `Content-Type` comes from the extension (`.wasm` is `application/wasm`,
  `.mjs` is JavaScript), and a strong `ETag` makes a reload a `304`.
- **A directory** without a trailing `/` is a `301` to the same path with
  one. With one, it serves `index.html` (or `--index`) when there is one,
  else an HTML listing: directories first, then files, with sizes and
  modification times. Dotfiles are left out of the listing too.

## Loopback by default

`tur serve` listens on `127.0.0.1` unless told otherwise, like every stdlib
server (security-audit M-4): a dev server that the whole coffee shop can
reach by default is the one thing Python's gets wrong. To look at the page
from a phone on the same network, say so:

```sh
tur serve --bind any
```

## Cross-origin isolation: `--isolate`

A page can use `SharedArrayBuffer` -- which a WASM build with threads needs --
only when it is *cross-origin isolated*, and that takes two response headers.
`--isolate` sends them on every response, a 404 included:

| `--isolate` | `Cross-Origin-Opener-Policy` | `Cross-Origin-Embedder-Policy` |
| --- | --- | --- |
| absent, or `--isolate=off` | not sent | not sent |
| `--isolate`, or `--isolate=require-corp` | `same-origin` | `require-corp` |
| `--isolate=credentialless` | `same-origin` | `credentialless` |

```sh
tur serve --isolate dist                  # require-corp
tur serve dist --isolate=credentialless
```

The value has to be attached with `=`: a bare `--isolate` never takes the next
word, so `tur serve --isolate dist` serves `dist`. Any other value is a usage
error.

It is off by default because isolation has a cost. Under `require-corp` the
page may load something from another origin -- an image, a script, a font, an
iframe -- only if that server opts in (`Cross-Origin-Resource-Policy:
cross-origin`, or CORS). Anything that does not is blocked, and the console
says `ERR_BLOCKED_BY_RESPONSE`. `credentialless` lets such requests through
without cookies instead, but not every browser supports it.
`Cross-Origin-Opener-Policy: same-origin` also cuts the page off from popups
on other origins, which breaks sign-in popups that report back through
`window.opener`. A page that loads only its own files is unaffected, and
remember that `localhost` and `127.0.0.1` are different origins.

## Limits

It is a development server.

- Files are read whole into memory per request; a multi-gigabyte file is not
  a use case.
- No TLS, no range requests, no compression, no CGI.
- COOP/COEP only on request (`--isolate`, above). A WASM build that uses
  threads needs them; see
  [web-emscripten-tutorial](web-emscripten-tutorial.md#coopcoep-headers).

A program that needs more builds its own server from `stdlib/httpd.tur`
(`mw-static-with`, `mw-etag`, `httpd-compress`, the TLS hook) -- `tur serve`
is one too.

## How it runs

The server is a Turmeric program shipped with the stdlib,
`stdlib/tools/serve.tur`; `tur serve` finds it beside the stdlib it loads
everything else from and runs it with the rest of the command line as its
`*args*`. From a turmeric checkout these are the same:

```sh
tur serve dist -p 8080
tur run stdlib/tools/serve.tur dist -p 8080
```

Read it for a small, complete `stdlib/httpd.tur` program.

It runs on the JIT when this `tur` carries one (in process: the `tur` PID is
the server's), otherwise -- and always on Windows, where the JIT cannot read
the socket headers -- through the C compiler. `TUR_ENGINE=cc` or
`TUR_ENGINE=jit` picks one for debugging; `interp` is refused, since the
interpreter has no httpd. It searches no spices: inside a project or outside
one, `tur serve` means the same thing.

Stopping `tur` stops the server on either engine: SIGTERM and SIGHUP are
passed on to the compiled program.

A built-in command always wins, so `tur serve` shadows an installed
`tur-serve`; reach that one by its binary name or with `tur x serve`.

## See also

- [consuming-spices-guide](consuming-spices-guide.md#running-a-spices-tool-without-installing-it-tur-x) --
  `tur x`, which runs a spice's tool the same way.
- [tur-run-guide](tur-run-guide.md) -- `tur run <file> args` and engine
  selection.
