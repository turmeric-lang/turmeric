---
title: Building for the Web with Emscripten
category: Tutorials
description: Compile a Turmeric project to WebAssembly and run it in a browser
---

# Building for the Web with Emscripten

This tutorial walks you through compiling Turmeric's WASM module, hosting it in
a browser page, and calling the WASM API from JavaScript. It covers three
progressively more capable projects: a zero-bundler "Hello WASM" page, a
file-fetching example, and a Vite-based production app.

This guide is distinct from `web/README.md` (which documents the internal
try-REPL build) and `docs/guides/c-integration-guide.md` (which covers native C
interop). The target audience is a developer who wants to embed or ship a
Turmeric program on a web page.

---

## Prerequisites

| Requirement | How to get it |
|---|---|
| Turmeric compiler (`tur`) built from source | `tur run build` |
| Emscripten SDK (`emsdk`) | https://emscripten.org/docs/getting_started |
| CMake 3.20+ | system package manager or cmake.org |
| Node.js 18+ (for the dev server) | https://nodejs.org |

Verify your environment before starting:

```sh
tur --version
emcc --version
cmake --version
node --version
```

---

## How the Turmeric WASM Build Works

The build pipeline has four stages:

1. `tur` compiles `.tur` source files to C99.
2. Emscripten (`emcc`) compiles that C99 to `wasm32`, producing `turmeric.wasm`
   plus a JS loader (`turmeric.js`).
3. The JS loader is either imported by a bundler (Vite) or loaded via a plain
   `<script>` tag.
4. The browser instantiates the WASM module and calls exported C functions from
   JavaScript.

Key files in the Turmeric repository:

| File | Purpose |
|---|---|
| `src/web/wasm_glue.h` | Exported C API (`turi_wasm_init`, `turi_wasm_eval`, etc.) |
| `src/web/wasm_glue.c` | Implementation of the glue layer |
| `src/CMakeLists.txt` (`add_custom_target(tur_wasm ...)`, ~line 1499) | How the `tur_wasm` CMake target is defined |
| `web/public/eval-worker.js` | Reference JS integration using the WASM module |

The `emcc` link command in `CMakeLists.txt` exports these functions:

```
_turi_wasm_init  _turi_wasm_reset  _turi_wasm_shutdown  _turi_wasm_eval
_turi_wasm_eval_ex  _turi_wasm_version  _turi_wasm_format
_turi_wasm_lsp_request  _turi_wasm_lsp_flush  _turi_wasm_lsp_reset
_turi_wasm_set_lang  _turi_wasm_get_lang  _turi_wasm_lang_registry
_turi_doc_lookup  _turi_type_of  _turi_explain  _malloc  _free
```

And these runtime methods:

```
stringToUTF8  UTF8ToString  lengthBytesUTF8
```

The JS module is exported as `TurmericModule` (via `-sEXPORT_NAME=TurmericModule
-sMODULARIZE=1`), so it is a factory function you call to obtain an initialized
module object.

---

## Project A: "Hello WASM" -- Inline Script, No Bundler

The simplest possible end-to-end example. You create a single `index.html` that
loads the pre-built `turmeric.js` / `turmeric.wasm` from the Turmeric repo and
evaluates a one-liner.

### Step 1: Build the WASM module

From the Turmeric repository root:

```sh
tur run wasm
```

This runs `tur run docs` first (so `stdlib/docstrings.tur` is current) then invokes
CMake with `TUR_WASM=ON` and builds the `tur_wasm` target. Outputs:

```
build-wasm/wasm/turmeric.js
build-wasm/wasm/turmeric.wasm
```

Both files are also copied automatically to `web/public/`.

### Step 2: Create a project directory

```
hello-wasm/
  index.html
  turmeric.js    <- copy from build-wasm/wasm/turmeric.js
  turmeric.wasm  <- copy from build-wasm/wasm/turmeric.wasm
```

```sh
mkdir hello-wasm
cp build-wasm/wasm/turmeric.js  hello-wasm/
cp build-wasm/wasm/turmeric.wasm hello-wasm/
```

### Step 3: Write index.html

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>Hello Turmeric WASM</title>
</head>
<body>
  <pre id="output"></pre>

  <!-- Load the Emscripten-generated JS loader. -->
  <!-- MODULARIZE=1 means this defines TurmericModule as a factory function, -->
  <!-- not an immediately-live module, so evaluation is safe inside .then(). -->
  <script src="turmeric.js"></script>
  <script>
    TurmericModule().then(function(Module) {
      // Must be called once before any evaluation.
      Module._turi_wasm_init();

      var input  = '(+ 1 2)';

      // Strings must be copied into WASM linear memory before passing to C.
      // Malloc a buffer and write the NUL-terminated UTF-8 string into it.
      var len    = Module.lengthBytesUTF8(input) + 1;
      var ptr    = Module._malloc(len);
      Module.stringToUTF8(input, ptr, len);

      // _turi_wasm_eval returns a malloc'd char* with the result.
      var resPtr = Module._turi_wasm_eval(ptr);

      // UTF8ToString reads a NUL-terminated string out of linear memory.
      var result = Module.UTF8ToString(resPtr);

      document.getElementById('output').textContent = result;

      // Free the input buffer, and the result buffer too -- the eval result
      // is a malloc'd copy the caller owns.
      Module._free(ptr);
      if (resPtr) Module._free(resPtr);
    });
  </script>
</body>
</html>
```

Why these helpers are needed:

- **`lengthBytesUTF8(str)` + `_malloc` + `stringToUTF8(str, ptr, len)`** --
  allocate WASM memory and write the JS string into it as NUL-terminated
  UTF-8. You must `_free` the pointer when done.
- **`UTF8ToString(ptr)`** -- reads a NUL-terminated C string from WASM memory
  back into a JS string.
- **`Module._turi_wasm_eval`** -- note the leading underscore; Emscripten
  prefixes all exported C functions with `_`.

### Step 4: Serve locally

WASM modules require a real HTTP server (browsers block `file://` WASM loads).
`tur` carries one:

```sh
tur serve hello-wasm -p 8080
```

Then open `http://localhost:8080`.  It sends `.wasm` as `application/wasm`;
`npx serve hello-wasm` or `python3 -m http.server 8080 --directory hello-wasm`
work as well.

### COOP/COEP headers

Turmeric's WASM build uses pthreads via `SharedArrayBuffer`. Browsers only
expose `SharedArrayBuffer` in cross-origin isolated contexts, which require two
response headers on every page and asset:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`tur serve` sends both when asked:

```sh
tur serve --isolate hello-wasm -p 8080
```

`npx serve` does not set these by default. For a quick fix, create a
`serve.json` config alongside your `index.html`:

```json
{
  "headers": [
    {
      "source": "**",
      "headers": [
        { "key": "Cross-Origin-Opener-Policy",   "value": "same-origin" },
        { "key": "Cross-Origin-Embedder-Policy",  "value": "require-corp" }
      ]
    }
  ]
}
```

Then run `npx serve hello-wasm`. The `hello-wasm/` directory layout becomes:

```
hello-wasm/
  index.html
  serve.json
  turmeric.js
  turmeric.wasm
```

---

## Project B: Evaluating a Full .tur File

Extend the previous example to load a `.tur` source file at runtime via `fetch`
rather than embedding a string literal in the HTML.

### Step 1: Write a Turmeric source file

Create `program.tur` in your project directory:

```turmeric
(defn greet [name] : str
  (str-append "Hello, " name "!"))

(println (greet "Web"))
```

### Step 2: Fetch and evaluate the file

Replace the inline `<script>` block from Project A with:

```html
<script src="turmeric.js"></script>
<script>
  TurmericModule().then(async function(Module) {
    Module._turi_wasm_init();

    var src = await fetch('program.tur').then(function(r) { return r.text(); });
    var len = Module.lengthBytesUTF8(src) + 1;
    var ptr = Module._malloc(len);
    Module.stringToUTF8(src, ptr, len);
    var res = Module._turi_wasm_eval(ptr);

    document.getElementById('output').textContent = Module.UTF8ToString(res);
    Module._free(ptr);
    if (res) Module._free(res);
  });
</script>
```

### Step 3: Project layout

```
hello-wasm/
  index.html
  program.tur
  serve.json
  turmeric.js
  turmeric.wasm
```

### Step 4: Using turi_wasm_eval_ex for separate result/error

`turi_wasm_eval` collapses result and error into a single string. For proper
error handling use `turi_wasm_eval_ex`:

```js
// Allocate two output pointers in WASM memory (each is 4 bytes in wasm32).
var resultPtrAddr = Module._malloc(4);
var errorPtrAddr  = Module._malloc(4);

var inputLen = Module.lengthBytesUTF8(src) + 1;
var inputPtr = Module._malloc(inputLen);
Module.stringToUTF8(src, inputPtr, inputLen);
var status   = Module._turi_wasm_eval_ex(inputPtr, resultPtrAddr, errorPtrAddr);

// Read the pointer values out of WASM memory.
var resultPtr = Module.getValue(resultPtrAddr, 'i32');
var errorPtr  = Module.getValue(errorPtrAddr,  'i32');

if (status === 0) {
  console.log('result:', Module.UTF8ToString(resultPtr));
  Module._free(resultPtr);
} else {
  console.error('error:', Module.UTF8ToString(errorPtr));
  Module._free(errorPtr);
}

Module._free(inputPtr);
Module._free(resultPtrAddr);
Module._free(errorPtrAddr);
```

Note: both `*out_result` and `*out_error` are malloc'd strings the caller
owns; free each non-NULL one with `_free`. (The C-side helper
`turi_wasm_free_string` is a plain `free()` and is not in the exported
function list, so `_free` is the way to release them from JS.)

---

## Project C: A Vite-Based Web App

A production-ready setup using Vite as a bundler -- mirroring the approach used
by the Turmeric repository's own `web/` directory.

### Step 1: Scaffold the project

```sh
npm create vite@latest my-tur-app -- --template vanilla
cd my-tur-app
npm install
```

### Step 2: Copy the WASM artifacts

```sh
mkdir -p public
cp /path/to/turmeric/build-wasm/wasm/turmeric.js  public/
cp /path/to/turmeric/build-wasm/wasm/turmeric.wasm public/
```

Both files go into `public/` so Vite serves them verbatim without bundling.

### Step 3: Add COOP/COEP headers in vite.config.js

Open `vite.config.js` and add a `server.headers` block:

```js
import { defineConfig } from 'vite';

export default defineConfig({
  server: {
    headers: {
      'Cross-Origin-Opener-Policy':  'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
});
```

### Step 4: Create a turmeric.js wrapper module

Create `src/turmeric.js` to wrap the WASM module in a singleton:

```js
import createModule from '/turmeric.js';

let _mod = null;

export async function init() {
  if (_mod) return _mod;
  _mod = await createModule();
  _mod._turi_wasm_init();
  return _mod;
}

export async function evalCode(src) {
  var mod = await init();
  var len = mod.lengthBytesUTF8(src) + 1;
  var ptr = mod._malloc(len);
  mod.stringToUTF8(src, ptr, len);
  var rp  = mod._turi_wasm_eval(ptr);
  var out = mod.UTF8ToString(rp);
  mod._free(ptr);
  if (rp) mod._free(rp);
  return out;
}

export async function version() {
  var mod = await init();
  var rp  = mod._turi_wasm_version();
  return mod.UTF8ToString(rp);
}

export async function reset() {
  var mod = await init();
  mod._turi_wasm_reset();
}
```

### Step 5: Wire into main.js

Replace the generated `src/main.js` with:

```js
import { evalCode, version } from './turmeric.js';

document.querySelector('#app').innerHTML = `
  <h1>Turmeric WASM</h1>
  <textarea id="code" rows="6" cols="60">(+ 1 2)</textarea><br>
  <button id="run">Run</button>
  <pre id="output"></pre>
  <p id="ver"></p>
`;

version().then(function(v) {
  document.getElementById('ver').textContent = 'Turmeric ' + v;
});

document.getElementById('run').addEventListener('click', async function() {
  var src = document.getElementById('code').value;
  var out = await evalCode(src);
  document.getElementById('output').textContent = out;
});
```

### Step 6: Start the dev server

```sh
npm run dev
```

Open the URL that Vite prints (usually `http://localhost:5173`). Type an
expression in the textarea and click Run.

---

## Rebuilding the WASM Module

When you modify Turmeric itself or the glue layer (`src/web/wasm_glue.c`):

```sh
# From the Turmeric repository root
tur run wasm

# Copy updated artifacts into your project
cp build-wasm/wasm/turmeric.js  my-tur-app/public/
cp build-wasm/wasm/turmeric.wasm my-tur-app/public/
```

`tur run wasm` always runs `tur run docs` first, which regenerates
`stdlib/docstrings.tur` and `web/public/doc-names.json` so that the runtime
doc-lookup table stays current.

To force a complete rebuild from scratch (useful if CMake's dependency tracking
gets confused after a large refactor):

```sh
tur run clean-wasm
tur run wasm
```

---

## Using the Doc-Lookup API

`turi_doc_lookup` retrieves the stdlib documentation string for any named
function or macro. It backs the doc panel in the web REPL.

```js
async function docLookup(name) {
  var mod = await init();           // reuse the singleton from Project C: A Vite-Based Web App
  var len = mod.lengthBytesUTF8(name) + 1;
  var ptr = mod._malloc(len);
  mod.stringToUTF8(name, ptr, len);
  var rp  = mod._turi_doc_lookup(ptr);
  var doc = rp ? mod.UTF8ToString(rp) : '(no documentation)';
  mod._free(ptr);
  return doc;
}

// Example
docLookup('map').then(console.log);
```

The returned pointer is a static C string owned by the runtime -- do NOT free
it.

Doc strings are generated by `python3 tools/gendocs.py` (`tur run docs`) and
baked into `stdlib/docstrings.tur`, which the runtime loads on `turi_wasm_init`.
If you add or update `;;;` doc comments in a stdlib file, re-run `tur run docs`
and rebuild the WASM module to see the changes.

---

## COOP/COEP and pthreads -- Common Pitfalls

| Problem | Cause | Fix |
|---|---|---|
| `SharedArrayBuffer is not defined` | Missing COOP/COEP headers | Add both headers to every HTTP response (including `.wasm`) |
| `_turi_wasm_init is not a function` | Missing `-sEXPORTED_FUNCTIONS` flag | Re-run `tur run wasm` with the correct CMake configuration |
| `TypeError: WebAssembly.instantiate` fails | Server sends `.wasm` as `text/plain` | Configure MIME type `application/wasm` on your server |
| Page hangs on first call | pthread pool not yet started | Call `_turi_wasm_init` inside the `.then()` callback, not at top level |
| `stringToUTF8 is not a function` | Missing `EXPORTED_RUNTIME_METHODS` | Ensure `stringToUTF8,UTF8ToString,lengthBytesUTF8` are in the link flags (`allocateUTF8` is NOT exported -- use `_malloc` + `stringToUTF8`) |
| `.wasm` file not found (404) | Emscripten looks for `.wasm` next to `.js` | Copy both files to the same directory; do not rename them |

---

## Deploying

### Deploying the Turmeric REPL (tur run deploy-web)

The `web/` directory in the Turmeric repository deploys via Cloudflare Pages.
The full pipeline is:

```sh
tur run deploy-web   # runs: tur run web, then cd web && npm run deploy
```

COOP/COEP headers are set by the Cloudflare Worker in `web/worker.js` so they
apply to every response including the `.wasm` file.

### Deploying your own app

**Cloudflare Pages / Netlify**: Create a `_headers` file in your `public/`
directory (or the build output root):

```
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
```

**GitHub Pages** cannot set custom response headers. Use a Service Worker
instead:

```js
// sw.js
self.addEventListener('fetch', function(event) {
  event.respondWith(
    fetch(event.request).then(function(response) {
      var headers = new Headers(response.headers);
      headers.set('Cross-Origin-Opener-Policy',  'same-origin');
      headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
      return new Response(response.body, {
        status:     response.status,
        statusText: response.statusText,
        headers:    headers,
      });
    })
  );
});
```

Register it in `index.html` before the WASM module loads:

```html
<script>
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register('/sw.js').then(function(reg) {
      if (!reg.active) { location.reload(); }
    });
  }
</script>
```

**Vite base path for sub-paths**: If deploying to
`username.github.io/my-tur-app/`, set `base` in `vite.config.js`:

```js
export default defineConfig({
  base: '/my-tur-app/',
  // ...
});
```

---

## Next Steps

- `docs/guides/c-integration-guide.md` -- calling C from Turmeric (native builds)
- `docs/guides/effects-system-guide.md` -- algebraic effects in Turmeric
- `web/README.md` -- the full try-REPL build and deployment workflow
- `src/web/wasm_glue.h` -- complete WASM C API reference
