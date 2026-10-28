// engine.js -- which engine Run uses (wasm-jit-plan P4, 5.10).
//
//   interpret  the tree-walking interpreter (eval-worker.js): a REPL session
//              that keeps definitions between runs, and recursion bounded by
//              memory rather than the stack.  The default.
//   compile    the browser JIT (jit-worker.js): the buffer is compiled to C
//              and run on MIR's interpreter, as `tur jit` runs it, so inline C
//              works.  Each Run is a whole program; recursion is bounded by
//              the JS stack, about 1,700 interpreted frames (5.8).
//
// The choice is a per-browser convenience, kept in localStorage; when storage
// is unavailable it lasts the page's lifetime.  It is offered in the
// language popover, beside the #lang it complements, and the language button
// carries a chip while Compile is on so the choice is visible without
// opening the popover.

const STORAGE_KEY = 'tur-try-engine';
export const ENGINES = ['interpret', 'compile'];

let current = (() => {
    try {
        const v = localStorage.getItem(STORAGE_KEY);
        return ENGINES.includes(v) ? v : 'interpret';
    } catch (_) {
        return 'interpret';
    }
})();

const listeners = [];

export function getEngine() {
    return current;
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
      note: 'compiles to C and runs it; inline C works; each Run is a whole program; recursion depth ~1,700' },
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
    }
    if (host) {
        host.querySelectorAll('input[type=radio]').forEach(r => { r.checked = (r.value === current); });
    }
    const chip = document.getElementById('engine-chip');
    if (chip) chip.hidden = current !== 'compile';
}

/** Render the Engine rows in the language popover and keep them in step. */
export function initEnginePicker() {
    render();
    onEngineChange(render);
}
