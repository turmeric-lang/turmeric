// engine.js -- which engine Run uses (wasm-jit-plan P4, 5.10).
//
//   interpret  the tree-walking interpreter (eval-worker.js): a REPL session
//              that keeps definitions between runs, and recursion bounded by
//              memory rather than the stack.  The default.
//   compile    the browser JIT (jit-worker.js): the buffer is compiled to C
//              and run on MIR's interpreter, as `tur jit` runs it, so inline C
//              works.  Each Run is a whole program; recursion is bounded by
//              the JS stack: about 300 Turmeric calls deep in Chromium's
//              worker (docs/reported/wasm-jit-recursion-depth-is-about-300.md).
//
// The choice is a per-browser convenience, kept in localStorage; when storage
// is unavailable it lasts the page's lifetime.  It is offered in the
// language popover, beside the #lang it complements, and the language button
// carries a chip while Compile is on so the choice is visible without
// opening the popover.

const STORAGE_KEY = 'tur-try-engine';
const TURNS_KEY = 'tur-try-compiled-turns';
export const ENGINES = ['interpret', 'compile'];

let current = (() => {
    try {
        const v = localStorage.getItem(STORAGE_KEY);
        return ENGINES.includes(v) ? v : 'interpret';
    } catch (_) {
        return 'interpret';
    }
})();

// aot-compiled-repl-plan C2 (`--enable=compiled-repl`, prototype): under
// Compile, the prompt's turns can run as compiled programs too -- the
// session's definitions plus the turn -- instead of on the module's
// interpreter.  Off unless asked for; kept like the engine choice.
let compiledTurns = (() => {
    try { return localStorage.getItem(TURNS_KEY) === '1'; } catch (_) { return false; }
})();

const listeners = [];
const turnsListeners = [];

export function getEngine() {
    return current;
}

/** Whether Compile runs every prompt turn as a compiled program. */
export function getCompiledTurns() {
    return compiledTurns;
}

export function setCompiledTurns(on) {
    on = !!on;
    if (on === compiledTurns) return;
    compiledTurns = on;
    try { localStorage.setItem(TURNS_KEY, on ? '1' : '0'); } catch (_) { /* session only */ }
    for (const fn of turnsListeners) fn(on);
}

export function onCompiledTurnsChange(fn) {
    turnsListeners.push(fn);
}

export function setEngine(engine) {
    if (!ENGINES.includes(engine) || engine === current) return;
    current = engine;
    try { localStorage.setItem(STORAGE_KEY, engine); } catch (_) { /* session only */ }
    for (const fn of listeners) fn(engine);
}

export function onEngineChange(fn) {
    listeners.push(fn);
}

const ROWS = [
    { engine: 'interpret', name: 'Interpret',
      note: 'REPL session; definitions persist between runs' },
    { engine: 'compile', name: 'Compile',
      note: 'compiles to C and runs it; inline C works; each Run is a whole program; recursion depth ~300' },
];

function render() {
    const host = document.getElementById('lang-engines');
    if (host && !host.childElementCount) {
        host.innerHTML = ROWS.map(r => `
            <label class="lang-row engine-row" title="${r.note}">
                <input type="radio" name="engine" value="${r.engine}">
                <span class="engine-row-text">
                    <span class="engine-row-name">${r.name}</span>
                    <span class="engine-row-note">${r.note}</span>
                </span>
            </label>`).join('');
        host.querySelectorAll('input[type=radio]').forEach(r =>
            r.addEventListener('change', () => { if (r.checked) setEngine(r.value); }));
        host.insertAdjacentHTML('beforeend', `
            <label class="lang-row engine-row engine-turns-row" id="engine-turns-row"
                   title="every prompt turn compiles the session's definitions plus the turn">
                <input type="checkbox" id="engine-turns">
                <span class="engine-row-text">
                    <span class="engine-row-name">Compile prompt turns too</span>
                    <span class="engine-row-note">experimental (compiled-repl); a change starts the prompt's session afresh</span>
                </span>
            </label>`);
        const box = host.querySelector('#engine-turns');
        box.addEventListener('change', () => setCompiledTurns(box.checked));
    }
    if (host) {
        host.querySelectorAll('input[type=radio]').forEach(r => { r.checked = (r.value === current); });
        const row = host.querySelector('#engine-turns-row');
        if (row) row.hidden = current !== 'compile';
        const box = host.querySelector('#engine-turns');
        if (box) box.checked = compiledTurns;
    }
    const chip = document.getElementById('engine-chip');
    if (chip) chip.hidden = current !== 'compile';
}

/** Render the Engine rows in the language popover and keep them in step. */
export function initEnginePicker() {
    render();
    onEngineChange(render);
    onCompiledTurnsChange(render);
}
