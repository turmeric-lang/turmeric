---
title: Deploying Turmeric Services to Cloudflare
category: Networking and Web
description: Three approaches for deploying Turmeric-written services to Cloudflare -- Containers (simplest, works today), Interpreter-in-WASM (portable edge execution), and AOT WASM via emit-c (fastest cold starts).
---

# Deploying Turmeric Services to Cloudflare

This guide covers three ways to deploy a Turmeric service on Cloudflare infrastructure.
They differ in complexity, performance, and what Cloudflare product they target:

| Approach | Product | Complexity | Works today |
|---|---|---|---|
| A. Cloudflare Containers | Containers + Workers | Low | Yes |
| B. Interpreter-in-WASM | Workers | Medium | With adaptation |
| C. AOT WASM via `emit-c` | Workers | High | Requires build tooling |

**Recommendation:** start with Approach A. It uses the native `tur` binary inside a
Docker container and requires no changes to your Turmeric code. Approaches B and C are
better fits once you need true edge distribution or sub-millisecond cold starts.

---

## Prerequisites

| Tool | How to get it |
|---|---|
| Turmeric (`tur`) built from source | `just build` |
| Docker | https://docs.docker.com/get-started/get-docker/ |
| Cloudflare account (paid, Workers Paid plan) | https://cloudflare.com |
| Wrangler CLI | `npm install -g wrangler` |

---

## The example service

All three approaches use the same logical service: a minimal HTTP server that
responds to any request with a plain-text greeting. The implementation differs per
approach.

---

## Approach A: Cloudflare Containers

**How it works:** the native `tur` binary runs inside a Docker container. Cloudflare
spins up the container on demand and a thin Cloudflare Worker routes incoming HTTP
requests to it.

### Step 1 -- write the Turmeric service

```turmeric
;; service.tur -- minimal HTTP service for Approach A (Cloudflare Containers)
;;
;; Listens on $PORT (default 8080), accepts connections concurrently using the
;; cooperative fiber scheduler, and returns a fixed HTTP 200 response.

(load "stdlib/async_socket.tur")
(load "stdlib/scheduler.tur")
(load "stdlib/fiber.tur")
(load "stdlib/env.tur")

(defn cstr-length [s : cstr] : int
  ```c
  return (int64_t)strlen(s);
  ```)

(defn http-ok [body : cstr len : int] : cstr
  (str-concat "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: "
              (str-concat (int->str len)
                          (str-concat "\r\n\r\n" body))))

(defn handle-client [fd : Fd] : nil
  (async-socket-recv fd 4096)
  (let [body "Hello from Turmeric!\n"
        resp (http-ok body 21)]
    (async-socket-send fd resp (cstr-length resp)))
  (async-socket-close fd))

(defn accept-loop [listen-fd : Fd sched : ptr<void>] : nil
  (let [client-fd (async-socket-accept listen-fd)
        f         (fiber-new (fn [] : nil (handle-client client-fd)) 0)]
    (scheduler-spawn sched f))
  (accept-loop listen-fd sched))

(defn main [] : int
  (let [port-str (env/get-raw "PORT")
        port     (if (= port-str 0) 8080 (cstr->parse-int (:: port-str :cstr)))
        sched    (scheduler-new)
        fd       (async-socket-listen port 128)
        accepter (fiber-new (fn [] : nil (accept-loop fd sched)) 0)]
    (println (str-concat "Listening on :" (int->str port)))
    (scheduler-spawn sched accepter)
    (scheduler-run-to-completion sched))
  0)
```
```sweet-exp
;; service.tur -- minimal HTTP service for Approach A (Cloudflare Containers)
;;
;; Listens on $PORT (default 8080), accepts connections concurrently using the
;; cooperative fiber scheduler, and returns a fixed HTTP 200 response.
load("stdlib/async_socket.tur")
load("stdlib/scheduler.tur")
load("stdlib/fiber.tur")
load("stdlib/env.tur")
defn cstr-length [s :cstr] :int
  ```c
  return (int64_t)strlen(s);
  ```
defn http-ok [body :cstr len :int] :cstr
  str-concat("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " str-concat(int->str(len) str-concat("\r\n\r\n" body)))
defn handle-client [fd :Fd] :nil
  async-socket-recv(fd 4096)
  let [body "Hello from Turmeric!\n"
       resp (http-ok body 21)]
    async-socket-send(fd resp cstr-length(resp))
  async-socket-close(fd)
defn accept-loop [listen-fd :Fd sched :ptr<void>] :nil
  let [client-fd (async-socket-accept listen-fd)
       f         (fiber-new (fn [] :nil (handle-client client-fd)) 0)]
    scheduler-spawn(sched f)
  accept-loop(listen-fd sched)
defn main [] :int
  let [port-str (env/get-raw "PORT")
       port     (if (= port-str 0) 8080 (cstr->parse-int (:: port-str :cstr)))
       sched    (scheduler-new)
       fd       (async-socket-listen port 128)
       accepter (fiber-new (fn [] :nil (accept-loop fd sched)) 0)]
    println(str-concat("Listening on :" int->str(port)))
    scheduler-spawn(sched accepter)
    scheduler-run-to-completion(sched)
  0
```

> **Owned-return note.** `http-ok` assembles a fresh response with nested
> `str-concat` and returns it as `cstr`; `handle-client` sends it and drops it
> immediately, so the buffer is a "caller frees" result that leaks each request.
> For a handler that builds and holds response strings, an owned `String`
> (adopt the `str-concat` buffer, or accumulate with a `StringBuilder`) frees
> exactly once and borrows to `cstr` for `async-socket-send`. See
> [strings-guide.md](strings-guide.md).

Test locally before containerizing:

```sh
just build
tur run service.tur
# in another terminal:
curl http://localhost:8080/
# Hello from Turmeric!
```

### Step 2 -- write the Dockerfile

```dockerfile
# Build stage: compile the service to a native binary
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake git python3 \
    && rm -rf /var/lib/apt/lists/*

# Install 'just'
RUN curl --proto '=https' --tlsv1.2 -sSf \
    https://github.com/casey/just/releases/latest/download/just-x86_64-unknown-linux-musl.tar.gz \
    | tar -xz -C /usr/local/bin just

WORKDIR /turmeric
COPY . .
RUN just release

# Compile the service to a standalone binary
RUN build/tur emit-c --output-dir /tmp/svc service.tur \
    && cd /tmp/svc \
    && gcc -O2 -o service service.c -lpthread

# Runtime stage: minimal image with only the compiled binary
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y libpthread-stubs0-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /tmp/svc/service /usr/local/bin/service

EXPOSE 8080
ENV PORT=8080

CMD ["service"]
```

**Alternative (simpler, larger image):** skip `emit-c` and ship the `tur` interpreter
alongside the source:

```dockerfile
FROM ubuntu:22.04

# ... install build deps, just build ...
COPY --from=builder /turmeric/build/tur /usr/local/bin/tur
COPY service.tur /app/service.tur

EXPOSE 8080
ENV PORT=8080
CMD ["tur", "run", "/app/service.tur"]
```

The interpreter-based image is bigger (~30 MB vs ~2 MB) but requires no `emit-c`
compilation step.

### Step 3 -- write the Cloudflare Worker

Create a new project directory:

```
my-service/
  Dockerfile
  service.tur
  worker.js
  wrangler.toml
```

**`worker.js`** -- the Worker proxies requests to the container:

```javascript
export default {
  async fetch(request, env) {
    // env.SERVICE is bound to the Container in wrangler.toml
    return env.SERVICE.fetch(request);
  }
};
```

**`wrangler.toml`**:

```toml
name = "my-turmeric-service"
main = "worker.js"
compatibility_date = "2024-01-01"

[[containers]]
name = "SERVICE"
image = "./Dockerfile"
max_instances = 5

[[containers.port]]
name = "http"
target = 8080
```

### Step 4 -- deploy

```sh
wrangler login
wrangler deploy
```

Wrangler builds the Docker image, pushes it to Cloudflare's registry, and deploys
the Worker. The Worker URL is printed on success.

### Tradeoffs

**Advantages**
- Works today with no code changes to your Turmeric program
- Full POSIX environment: all stdlib modules work, including `async_socket.tur`,
  `fs.tur`, `concurrent.tur`, threads, and effects
- No WASM porting or build flag changes needed
- Container scaling is handled by Cloudflare automatically

**Disadvantages**
- Cloudflare Containers is a paid product (Workers Paid plan required)
- Cold starts are measured in seconds (container boot), not milliseconds
- Not available at every Cloudflare PoP -- containers run in a limited set of
  locations vs. the full global Worker edge network
- Larger attack surface than a sandboxed Worker

---

## Approach B: Interpreter-in-WASM Worker

**How it works:** the Turmeric WASM module (the interpreter itself, compiled by
Emscripten) is bundled inside a Cloudflare Worker. Each request calls `turi_wasm_eval`
with a Turmeric expression and returns the result.

This approach is best for programs that can express their request handling as a pure
Turmeric expression -- without long-running loops, raw sockets, or filesystem access.

### Step 1 -- build the WASM module

From the Turmeric repository root:

```sh
just wasm
```

This produces:

```
build-wasm/wasm/turmeric.js
build-wasm/wasm/turmeric.wasm
```

### Step 2 -- adapt the Emscripten output for Workers

Emscripten's default output targets browser environments. Cloudflare Workers run in
a V8 isolate with no `window`, `document`, or `XMLHttpRequest`. The stock
`just wasm` build already passes `-sMODULARIZE=1` and
`-sEXPORT_NAME=TurmericModule`; what it does not set is the target environment:

```sh
# In src/CMakeLists.txt (the tur_wasm target) or a custom build script,
# add to the emcc link command:
#   -sENVIRONMENT=worker
```

Or patch the existing output to remove browser-specific checks. The exact changes
depend on the Emscripten version; see `src/web/wasm_glue.c` for the exported API.

### Step 3 -- write the service handler in Turmeric

Rather than a server loop, write a pure request handler. This runs inside the
interpreter on each incoming request:

```turmeric
;; handler.tur -- evaluated once per Worker request via turi_wasm_eval
;;
;; The Worker calls:
;;   turi_wasm_eval("(handle-request request-body)")
;; after loading the stdlib definitions below.

(defn handle-request [body : cstr] : cstr
  (str-concat "Hello from Turmeric! You sent: " body))
```
```sweet-exp
;; handler.tur -- evaluated once per Worker request via turi_wasm_eval
;;
;; The Worker calls:
;;   turi_wasm_eval("(handle-request request-body)")
;; after loading the stdlib definitions below.
defn handle-request [body:cstr] :cstr
  str-concat("Hello from Turmeric! You sent: " body)
```

The Worker loads these definitions at startup (cold) and calls `handle-request` on
each incoming request.

> **Owned-return note.** `handle-request` returns `cstr` because
> `turi_wasm_eval` marshals the result back to the JS host as a C string -- that
> host boundary is a legitimate keep-`cstr` seam. If the handler grows internal
> string-building steps beyond this one `str-concat`, build those as owned
> `String` and convert to `cstr` only at the return. See
> [strings-guide.md](strings-guide.md).

### Step 4 -- write the Worker

```javascript
// worker.js
import TurmericModule from './turmeric.js';

let tur = null;

async function initTurmeric() {
  if (tur) return tur;
  tur = await TurmericModule();
  // Load the handler definitions once at startup
  const src = await fetch('./handler.tur').then(r => r.text());
  const initResult = tur.ccall('turi_wasm_eval', 'string', ['string'], [src]);
  if (initResult?.startsWith('error:')) {
    throw new Error('Turmeric init failed: ' + initResult);
  }
  return tur;
}

export default {
  async fetch(request) {
    const module = await initTurmeric();
    const body = await request.text();
    // Escape body for safe embedding in a Turmeric string literal
    const escaped = body.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
    const expr = `(handle-request "${escaped}")`;
    const result = module.ccall('turi_wasm_eval', 'string', ['string'], [expr]);
    return new Response(result, {
      headers: { 'Content-Type': 'text/plain' }
    });
  }
};
```

### Step 5 -- wrangler.toml

```toml
name = "my-turmeric-wasm-service"
main = "worker.js"
compatibility_date = "2024-01-01"

[build]
command = "just wasm && cp build-wasm/wasm/turmeric.js . && cp build-wasm/wasm/turmeric.wasm ."

[[rules]]
type = "CompiledWasm"
globs = ["**/*.wasm"]

[[rules]]
type = "ESModule"
globs = ["**/*.js"]
```

### Tradeoffs

**Advantages**
- True edge execution: runs at every Cloudflare PoP worldwide
- No container overhead: isolate cold starts are measured in milliseconds
- No Docker or infrastructure to manage
- Free tier eligible (within Worker CPU/memory limits)

**Disadvantages**
- The full Turmeric interpreter ships with every Worker (~3 MB WASM) -- counts
  against the 1 MB compressed Worker size limit, which may require a paid plan
- Cloudflare Workers have a 10 ms CPU time limit per request on the free tier
  (50 ms on paid). Interpreter startup + Turmeric eval must fit within that budget.
- Emscripten output requires adaptation (Worker environment flags, no browser globals)
- POSIX I/O is not available: `async_socket.tur`, `fs.tur`, and threading do not
  work inside a Worker isolate. Handler code must be pure computation.
- WASM memory is limited to 128 MB in Workers

---

## Approach C: AOT WASM via `emit-c`

**How it works:** the Turmeric compiler emits C from your source file (`tur emit-c`).
Emscripten then compiles that C to a minimal, self-contained WASM module with no
interpreter overhead. The Worker imports the compiled WASM and calls the exported
handler directly.

This gives the best runtime performance but requires the most build tooling.

### Step 1 -- write a pure handler

Because POSIX sockets are not available in WASM, the handler must accept the request
as a parameter and return the response as a string. The Worker provides the HTTP
layer.

```turmeric
;; handler.tur -- compiled to WASM via emit-c + emcc
;;
;; Exports a single C function:
;;   char *tur_handle(const char *method, const char *path, const char *body)
;; which the Worker calls directly via WASM imports.
;;
;; All logic must be pure computation -- no sockets, threads, or filesystem.

(import "stdlib/str.tur")

(defn greet [name : cstr] : cstr
  (str-concat "Hello, " (str-concat name "!")))

(defn tur-handle [method : cstr path : cstr body : cstr] : cstr
  (greet (if (= (cstr-length body) 0) "world" body)))
```
```sweet-exp
;; handler.tur -- compiled to WASM via emit-c + emcc
;;
;; Exports a single C function:
;;   char *tur_handle(const char *method, const char *path, const char *body)
;; which the Worker calls directly via WASM imports.
;;
;; All logic must be pure computation -- no sockets, threads, or filesystem.
import "stdlib/str.tur"
defn greet [name :cstr] :cstr
  str-concat("Hello, " str-concat(name "!"))
defn tur-handle [method :cstr path :cstr body :cstr] :cstr
  greet(if({cstr-length(body) = 0} "world" body))
```

> **`cstr` at the seam, `String` inside.** `tur_handle`'s signature is the
> WASM/host C ABI (`char *tur_handle(...)`), so its return type genuinely must be
> `cstr` -- that boundary is a legitimate keep-`cstr` site. The internal builders
> are the owned-`String` cases: `greet` assembles a fresh `"Hello, name!"` with
> `str-concat` (a "caller frees" buffer that leaks here). Build such internal
> results as `String` and lower to `cstr` only at the exported seam
> (`(string/to-cstr (greet ...))`, or `string/adopt-cstr` the `str-concat`
> buffer). See [strings-guide.md](strings-guide.md).

### Step 2 -- compile to C

```sh
tur emit-c handler.tur > handler.c
```

Inspect `handler.c` to find the mangled name of `tur_handle`; it will be something
like `tur__handler__tur_handle`. You will export this function by its mangled name
in the next step.

### Step 3 -- write a C shim and compile with Emscripten

Create `shim.c` to export a stable symbol:

```c
/* shim.c -- stable export wrapping the Turmeric-generated symbol */
#include <stdint.h>
#include <emscripten.h>

/* Forward declaration of the Turmeric-generated function */
const char *tur__handler__tur_handle(const char *method,
                                      const char *path,
                                      const char *body);

EMSCRIPTEN_KEEPALIVE
const char *tur_handle(const char *method, const char *path, const char *body) {
    return tur__handler__tur_handle(method, path, body);
}
```

Compile both with emcc:

```sh
emcc -O2 \
  -sENVIRONMENT=worker \
  -sEXPORTED_FUNCTIONS='["_tur_handle","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8"]' \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=HandlerModule \
  --no-entry \
  handler.c shim.c \
  -o handler.js
```

This produces `handler.js` and `handler.wasm`.

### Step 4 -- write the Worker

```javascript
// worker.js
import HandlerModule from './handler.js';

let mod = null;

async function getModule() {
  if (mod) return mod;
  mod = await HandlerModule();
  return mod;
}

export default {
  async fetch(request) {
    const m = await getModule();
    const handle = m.cwrap('tur_handle', 'string', ['string', 'string', 'string']);

    const method = request.method;
    const path   = new URL(request.url).pathname;
    const body   = await request.text();

    const result = handle(method, path, body);
    return new Response(result, {
      headers: { 'Content-Type': 'text/plain' }
    });
  }
};
```

### Step 5 -- wrangler.toml

```toml
name = "my-turmeric-aot-service"
main = "worker.js"
compatibility_date = "2024-01-01"

[build]
command = "tur emit-c handler.tur > handler.c && emcc -O2 -sENVIRONMENT=worker -sMODULARIZE=1 -sEXPORT_NAME=HandlerModule --no-entry -sEXPORTED_FUNCTIONS='[\"_tur_handle\",\"_malloc\",\"_free\"]' handler.c shim.c -o handler.js"

[[rules]]
type = "CompiledWasm"
globs = ["**/*.wasm"]
```

### Tradeoffs

**Advantages**
- No interpreter overhead: the Worker runs compiled Turmeric code directly
- Smallest WASM payload: only the code your program actually uses is compiled in
- Fastest request-time CPU budget: no eval loop, just direct C function calls
- True edge distribution at every Cloudflare PoP

**Disadvantages**
- Requires Emscripten SDK installed in the build environment
- The mangled symbol name from `emit-c` is not stable across compiler versions --
  the shim approach handles this but requires a rebuild when the Turmeric version
  changes
- POSIX I/O still not available: same restrictions as Approach B (pure computation
  only, no sockets, threads, or filesystem)
- Effects that depend on the Turmeric runtime (algebraic effects, fibers) may not
  be available without linking in the full runtime support library
- Build pipeline is more complex (two compilers: `tur` then `emcc`)

---

## Summary

```
                     Containers (A)   Interp-WASM (B)   AOT WASM (C)
Cold start              seconds            ms                 ms
Request CPU budget      generous          10-50 ms           10-50 ms
Global edge             limited PoPs      all PoPs           all PoPs
Full stdlib             yes               no (pure only)     no (pure only)
Docker required         yes               no                 no
Emscripten required     no                yes (adapted)      yes
Build complexity        low               medium             high
Free tier eligible      no                yes (if <1 MB)     yes (if <1 MB)
Works today             yes               with adaptation    requires tooling
```

**Choose A** when you need the full Turmeric stdlib (sockets, threads, effects,
filesystem) or want the simplest possible path to production.

**Choose B** when your service is pure computation and you want global edge
distribution without container infrastructure.

**Choose C** when B's interpreter overhead is too large (either for payload size or
CPU budget) and your service is already expressed as a pure function.
