// spices.js -- mount the arcade's spice packages into a running module
// (raylib-web-playground-plan J2).  The packages are pack-spices.py's:
// BASE/index.json and one BASE/<name>.json per spice.
//
//   import { spicesFor, mountSpices } from './spices.js';
//   const want = spicesFor(src, index);           // what the program names
//   const r = await mountSpices(Module, BASE, want.names, index);
//
// spicesFor reads the spices a program asks for: the first segment of each
// `(import a/b ...)` (or sweet-exp `import a/b`) that names a packaged spice,
// and every key of a `defscript :spices #map{...}` header.  A defscript name
// that is not packaged is returned in `unknown` ("not in this build"); an
// import of an unknown root is not, since it may be the program's own module
// or the stdlib's.
//
// mountSpices fetches each name's package and its deps (the closure, from
// index.json), writes the files under /spices/<name>/src and puts that
// directory on the module search path with turi_wasm_add_include_dir, so the
// program's `(import raylib/core)` resolves.  The module must be running
// (after turi_wasm_init); nothing is relinked.  It goes through two exports,
// turi_wasm_put_spice_file and turi_wasm_add_include_dir, so it works on
// either module -- the arcade's engine, and the stock module its language
// server runs on, which exports no filesystem.

export async function loadIndex(base) {
  const r = await fetch(`${base}/index.json`);
  if (!r.ok) throw new Error(`no spice index at ${base}/index.json (${r.status})`);
  const spices = (await r.json()).spices || [];
  return new Map(spices.map((s) => [s.name, s]));
}

export function spicesFor(src, index) {
  const names = new Set();
  const unknown = new Set();
  for (const m of src.matchAll(/(?:^|[\s(])import\s+([A-Za-z0-9_.-]+)\//gm)) {
    if (index.has(m[1])) names.add(m[1]);
  }
  const header = src.match(/\(defscript[\s\S]*?:spices\s+#map\{/);
  if (header) {
    // The header's map, to its balancing brace; its top-level keys are the
    // spice names.
    let depth = 1, i = header.index + header[0].length, body = '';
    for (; i < src.length && depth; i++) {
      const c = src[i];
      if (c === '{') depth++;
      else if (c === '}') depth--;
      body += depth === 0 ? '' : (depth === 1 ? c : ' ');
    }
    for (const m of body.matchAll(/"([^"]+)"/g)) {
      if (index.has(m[1])) names.add(m[1]);
      else unknown.add(m[1]);
    }
  }
  return { names: [...names], unknown: [...unknown] };
}

export function closure(names, index) {
  const out = [];
  const visit = (n) => {
    if (out.includes(n) || !index.has(n)) return;
    for (const d of index.get(n).deps || []) visit(d);
    out.push(n);
  };
  for (const n of names) visit(n);
  return out;
}

// Call an int export with string arguments (the module's heap, freed after).
function callWithStrings(Module, fn, ...strs) {
  const ptrs = strs.map((s) => {
    const n = Module.lengthBytesUTF8(s) + 1;
    const p = Module._malloc(n);
    Module.stringToUTF8(s, p, n);
    return p;
  });
  try { return fn(...ptrs); } finally { ptrs.forEach((p) => Module._free(p)); }
}

export async function mountSpices(Module, base, names, index) {
  const order = closure(names, index);
  const mounted = [];
  for (const name of order) {
    const root = `/spices/${name}/src`;
    const r = await fetch(`${base}/${index.get(name).file}`);
    if (!r.ok) throw new Error(`spice ${name}: ${r.status} fetching its package`);
    const pkg = await r.json();
    for (const [rel, text] of Object.entries(pkg.files)) {
      if (callWithStrings(Module, Module._turi_wasm_put_spice_file, `${root}/${rel}`, text) !== 0)
        throw new Error(`spice ${name}: the module refused ${root}/${rel}`);
    }
    if (callWithStrings(Module, Module._turi_wasm_add_include_dir, root) <= 0)
      throw new Error(`spice ${name}: the module refused ${root} as an include dir`);
    mounted.push(name);
  }
  return { mounted };
}

