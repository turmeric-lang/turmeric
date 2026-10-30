// lsp-worker.js -- the raylib playground's language server
// (raylib-web-playground-plan P2): `tur lsp` (src/web/wasm_lsp.c) in the
// stock module, turmeric.{js,wasm}, in a classic Worker, with every spice the
// engine offers mounted so `(import raylib/core ...)` resolves in the editor as
// it does when the program runs.  build.sh puts it beside the arcade module;
// a page starts it as `new Worker(ENGINE_DIR + '/lsp-worker.js')`.
//
// The protocol is Try Turmeric's (web/public/lsp-worker.js), so its Monaco
// adapter (web/lsp-client.js) drives this unchanged:
//
//   page -> worker   {type: 'init'}
//   worker -> page   {type: 'ready', spices} | {type: 'init-error', error}
//   page -> worker   {type: 'lsp', id, request}     one JSON-RPC message
//                    {type: 'lsp-flush', id}  {type: 'lsp-reset', id}
//   worker -> page   {type: 'lsp-result', id, messages}  the server's replies,
//                                                        notifications included
//                    {type: 'lsp-error', id, error}
//
// A classic Worker, because the module is an Emscripten MODULARIZE script that
// only importScripts can load; so the spice packages are mounted here rather
// than through spices.js (an ES module), with the same two exports:
// turi_wasm_put_spice_file and turi_wasm_add_include_dir.  The stock module
// exports no filesystem, and those accept only spice sources under /spices/.

const ENGINE_DIR = new URL('.', self.location.href).href.replace(/\/$/, '');

let mod = null;
let ready = false;
const queue = [];

function withStrings(fn, ...strs) {
    const ptrs = strs.map((s) => {
        const n = mod.lengthBytesUTF8(s) + 1;
        const p = mod._malloc(n);
        mod.stringToUTF8(s, p, n);
        return p;
    });
    try { return fn(...ptrs); } finally { ptrs.forEach((p) => mod._free(p)); }
}

function readOut(ptr) {
    if (!ptr) return '[]';
    const out = mod.UTF8ToString(ptr);
    mod._free(ptr);
    return out;
}

// A malformed reply is an empty batch: a language server failing must never
// break the editor.
function parseBatch(json, id) {
    try {
        const parsed = JSON.parse(json);
        return Array.isArray(parsed) ? parsed : [];
    } catch (err) {
        self.postMessage({ type: 'lsp-error', id, error: 'bad reply: ' + String(err) });
        return [];
    }
}

// Every offered spice, so any import the editor sees resolves: the packages
// are small (raylib is 12 files) and fetched once per worker.
async function mountSpices() {
    const r = await fetch(`${ENGINE_DIR}/spices/index.json`);
    if (!r.ok) return [];
    const spices = (await r.json()).spices || [];
    const mounted = [];
    for (const s of spices) {
        const p = await fetch(`${ENGINE_DIR}/spices/${s.file}`);
        if (!p.ok) continue;
        const pkg = await p.json();
        const root = `/spices/${s.name}/src`;
        let ok = true;
        for (const [rel, text] of Object.entries(pkg.files || {}))
            ok = withStrings(mod._turi_wasm_put_spice_file, `${root}/${rel}`, text) === 0 && ok;
        if (ok && withStrings(mod._turi_wasm_add_include_dir, root) > 0) mounted.push(s.name);
    }
    return mounted;
}

function handleMessage(msg) {
    const id = msg.id;
    try {
        if (msg.type === 'lsp') {
            const raw = readOut(withStrings(mod._turi_wasm_lsp_request, msg.request));
            self.postMessage({ type: 'lsp-result', id, messages: parseBatch(raw, id) });
        } else if (msg.type === 'lsp-flush') {
            const raw = readOut(mod._turi_wasm_lsp_flush());
            self.postMessage({ type: 'lsp-result', id, messages: parseBatch(raw, id) });
        } else if (msg.type === 'lsp-reset') {
            mod._turi_wasm_lsp_reset();
            self.postMessage({ type: 'lsp-result', id, messages: [] });
        }
    } catch (err) {
        self.postMessage({ type: 'lsp-error', id, error: String(err) });
    }
}

self.addEventListener('message', async (e) => {
    const msg = e.data || {};
    if (msg.type === 'init') {
        if (typeof SharedArrayBuffer === 'undefined') {
            self.postMessage({ type: 'init-error', error: 'shared-array-buffer-unavailable' });
            return;
        }
        try {
            importScripts(`${ENGINE_DIR}/turmeric.js`);
            // The analysis's stderr is the user's squiggles already; swallowed.
            mod = await self.TurmericModule({
                locateFile: (p) => `${ENGINE_DIR}/${p}`,
                print: () => {},
                printErr: () => {},
            });
            if (typeof mod._turi_wasm_lsp_request !== 'function' ||
                typeof mod._turi_wasm_put_spice_file !== 'function') {
                self.postMessage({ type: 'init-error', error: 'lsp-export-missing' });
                return;
            }
            // No turi_wasm_init(): that boots the interpreter environment; the
            // language server compiles from source.
            const spices = await mountSpices();
            ready = true;
            self.postMessage({ type: 'ready', spices });
            queue.splice(0).forEach(handleMessage);
        } catch (err) {
            self.postMessage({ type: 'init-error', error: String(err && err.message || err) });
        }
        return;
    }
    if (!ready) { queue.push(msg); return; }
    handleMessage(msg);
});
