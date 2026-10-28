// trace.js -- T3, the time-travel timeline.
//
//
// Run records; then you scrub the recording, forwards and backwards, watching
// the gutter follow the cursor and each live frame's bindings change under it.
//
// The page does not decode the .turtrace format.  Every question here --
// where am I, what are the frames, what had been printed by now -- is answered
// by turi_trace_replay_* through the WASM bridge, which is the same replay
// `tur dap` answers stepBack with.  One decoder for one format.

import { STORAGE_KEYS, safeWrite } from './storage.js';
import { escapeHtml } from './html.js';
import { getEditor, getMonaco } from './editor-ref.js';

// What the timeline borrows from main.js, which owns the eval worker and the
// REPL session. Set by configureTrace() at load.
let deps = {
    isReady: () => false,                        // the session can take a call
    workerCall: () => Promise.resolve(null),     // one trace-* request
    resetSession: () => Promise.resolve(false),  // a fresh env to record in
    setLangMode: () => {},
    getConsoleLog: () => [],
    setConsoleLog: () => {},
    showStatus: () => {},
    appendToConsole: () => {},
    updateExecTime: () => {},
    parseLangDirective: () => ({ lang: null }),
    definesMainEntry: () => false,
};

/** Hand the timeline what it borrows from main.js. Called once, at load. */
export function configureTrace(d) {
    deps = d;
}

/* The browser's cap is deliberately far below the recorder's own 1,000,000
 * default.  That number was chosen for a native process; here the interpreter
 * retains roughly 4 KiB per step of a trampolined loop on top of the ~13 bytes
 * a step costs the recording itself, and the tab is what pays.
 *
 * Raised from 50,000 alongside the recorder's move from line to expression
 * granularity.  A cap is a bound on the recording, but what it MEANS is how
 * much of a program fits under it, and a step is now about a third to a fifth
 * of what it used to be -- so holding 50,000 would have quietly cut the reach
 * of every recording by that factor.  The interpreter-side retention at the
 * cap is unchanged by construction: this is the same amount of program.
 * 250,000 steps is about 3 MB of trace and a session that stays responsive. */
const TRACE_MAX_STEPS = 250000;

export const traceState = {
    active: false,
    steps: 0,
    index: 0,
    baseLine: 1,
    frames: [],
    selectedFrame: 0,
    stats: null,
    savedConsoleHTML: null,
    savedConsoleLog: null,
    decorations: null,
    seekPending: false,
    seekQueued: null,
};

/**
 * Record the editor's program and open the timeline.
 */
export async function traceCode() {
    if (!deps.isReady()) {
        deps.showStatus('WASM not ready', 'error');
        return;
    }
    const code = getEditor().getValue();
    if (!code.trim()) {
        deps.appendToConsole('<span class="console-error">Error: No code to trace</span>');
        return;
    }

    deps.showStatus('Recording...', 'info');
    if (!await deps.resetSession()) {
        deps.appendToConsole('<span class="console-error">Trace failed: could not reset the session</span>');
        deps.showStatus('Trace failed', 'error');
        return;
    }

    const started = performance.now();
    // Forwarded for the same reason Run forwards it: turi_eval_typed strips an
    // inline #lang itself, but set_lang is what puts the session in the
    // program's base dialect.
    const { lang } = deps.parseLangDirective(code);
    if (lang !== null) deps.setLangMode(lang);

    // hasMain is the page's existing Run rule, not a second one: a program with
    // a top-level `main` loads its forms and then runs `(main)`, and the
    // recording covers the run rather than the definitions.
    // A stopped recording (the watchdog, or Stop) rejects; report it the way
    // any other failed recording is reported.
    const res = await deps.workerCall({
        type: 'trace-run',
        input: code,
        maxSteps: TRACE_MAX_STEPS,
        hasMain: deps.definesMainEntry(code),
        lang,
    }).catch((err) => ({ steps: -1, error: err.message }));

    if (!res || res.steps < 0) {
        const why = res && res.steps === -2
            ? 'this program declares a main entry point but did not define one'
            : (res && res.error) || 'the program did not load';
        deps.appendToConsole(`<span class="console-error">Trace failed: ${escapeHtml(why)}</span>`);
        deps.showStatus('Trace failed', 'error');
        return;
    }
    if (res.steps === 0) {
        deps.appendToConsole('<span class="console-error">Trace recorded nothing -- the program ran no interpreted steps.</span>');
        deps.showStatus('Ready', 'success');
        return;
    }

    deps.updateExecTime(performance.now() - started);
    traceState.stats = res.stats || null;
    traceState.steps = res.steps;
    traceState.baseLine = (res.stats && res.stats.baseLine) || 1;
    traceOpen();
    await traceSeek(0);
}

function traceOpen() {
    const panel = document.getElementById('trace-panel');
    if (!panel) return;
    if (!traceState.active) {
        // The transcript belongs to the user, so it is put back on close
        // rather than overwritten: while the timeline is open the console
        // shows what the program had printed by the cursor's step, which is
        // what the OUTPUT records exist for.
        const consoleEl = document.getElementById('console');
        traceState.savedConsoleHTML = consoleEl ? consoleEl.innerHTML : '';
        traceState.savedConsoleLog = deps.getConsoleLog().slice();
    }
    traceState.active = true;
    panel.hidden = false;

    const slider = document.getElementById('trace-slider');
    if (slider) {
        slider.min = '0';
        slider.max = String(Math.max(0, traceState.steps - 1));
        slider.value = '0';
    }

    const banner = document.getElementById('trace-banner');
    if (banner) {
        const st = traceState.stats;
        if (st && st.truncated) {
            banner.hidden = false;
            banner.textContent =
                `Recording stopped at the ${TRACE_MAX_STEPS.toLocaleString()}-step cap -- ` +
                `this is the beginning of the run, not all of it.`;
        } else if (st) {
            banner.hidden = false;
            banner.textContent =
                `${st.steps.toLocaleString()} steps, peak depth ${st.peakDepth}, ` +
                `${(st.bytes / 1024).toFixed(1)} KB recorded from a fresh session.`;
        } else {
            banner.hidden = true;
        }
    }
}

export function traceClose() {
    const panel = document.getElementById('trace-panel');
    if (panel) panel.hidden = true;
    if (traceState.decorations) {
        traceState.decorations.clear();
        traceState.decorations = null;
    }
    if (traceState.active) {
        const consoleEl = document.getElementById('console');
        if (consoleEl && traceState.savedConsoleHTML !== null) {
            consoleEl.innerHTML = traceState.savedConsoleHTML;
            consoleEl.scrollTop = consoleEl.scrollHeight;
        }
        if (traceState.savedConsoleLog) {
            deps.setConsoleLog(traceState.savedConsoleLog);
            safeWrite(STORAGE_KEYS.consol, traceState.savedConsoleLog);
        }
    }
    traceState.active = false;
    traceState.savedConsoleHTML = null;
    traceState.savedConsoleLog = null;
    traceState.frames = [];
    // Hand the megabyte back rather than letting it sit in WASM memory for the
    // rest of the session.
    deps.workerCall({ type: 'trace-release' });
}

/**
 * Move the cursor.  Seeks coalesce: a slider drag issues one seek at a time
 * and remembers only the most recent target, so dragging across a 50,000-step
 * recording costs one rebuild per settled position rather than one per pixel.
 */
export async function traceSeek(index) {
    if (!traceState.active && index !== 0) return;
    const target = Math.max(0, Math.min(index, traceState.steps - 1));
    if (traceState.seekPending) {
        traceState.seekQueued = target;
        return;
    }
    traceState.seekPending = true;
    const state = await deps.workerCall({
        type: 'trace-seek',
        index: target,
        // At the last step, ask for every OUTPUT record rather than the ones
        // before the cursor: a program whose final act is a println drains it
        // after the final STEP, and an empty console at the end of a run that
        // printed reads as a broken timeline rather than a precise one.
        wantFullOutput: target === traceState.steps - 1,
    });
    traceState.seekPending = false;

    if (state) traceRender(state);

    if (traceState.seekQueued !== null) {
        const next = traceState.seekQueued;
        traceState.seekQueued = null;
        await traceSeek(next);
    }
}

function traceRender(state) {
    traceState.index = state.index;
    traceState.frames = state.frames || [];
    if (traceState.selectedFrame >= traceState.frames.length) {
        traceState.selectedFrame = 0;
    }

    const slider = document.getElementById('trace-slider');
    if (slider && String(state.index) !== slider.value) slider.value = String(state.index);

    const pos = document.getElementById('trace-pos');
    if (pos) pos.textContent = `${state.index + 1} / ${state.steps}`;

    const top = traceState.frames[0];
    const site = document.getElementById('trace-site');
    if (site) {
        site.textContent = top
            ? `${top.fn || '(top level)'}  line ${traceEditorLine(top.line)}`
            : '';
    }

    traceRenderFrames();
    traceRenderOutput(state.fullOutput !== undefined ? state.fullOutput : (state.output || ''));
    traceHighlight(top ? traceEditorLine(top.line) : 0, top);
}

/* Interpreter line -> editor line.  The env accumulates every eval into one
 * blob, so an interpreter line is absolute in that blob; baseLine is where
 * this run's source started in it. */
function traceEditorLine(line) {
    const mapped = (line | 0) - traceState.baseLine + 1;
    return mapped > 0 ? mapped : 0;
}

function traceRenderFrames() {
    const framesEl = document.getElementById('trace-frames');
    const localsEl = document.getElementById('trace-locals');
    if (!framesEl || !localsEl) return;

    if (!traceState.frames.length) {
        framesEl.innerHTML = '<div class="trace-empty">no frames</div>';
        localsEl.innerHTML = '';
        return;
    }

    framesEl.innerHTML = traceState.frames.map((f, i) => {
        const cls = i === traceState.selectedFrame ? ' class="trace-frame active"' : ' class="trace-frame"';
        const line = traceEditorLine(f.line);
        return `<button${cls} data-frame="${i}">` +
               `<span class="trace-frame-fn">${escapeHtml(f.fn || '(top level)')}</span>` +
               `<span class="trace-frame-line">${line ? 'line ' + line : ''}</span>` +
               `</button>`;
    }).join('');

    const frame = traceState.frames[traceState.selectedFrame];
    const locals = (frame && frame.locals) || [];
    localsEl.innerHTML = locals.length
        ? locals.map(l =>
            `<div class="trace-local">` +
            `<span class="trace-local-name">${escapeHtml(l.name)}</span>` +
            `<span class="trace-local-val">${escapeHtml(l.repr)}</span>` +
            `</div>`).join('')
        : '<div class="trace-empty">no bindings in this frame yet</div>';
}

function traceRenderOutput(output) {
    const consoleEl = document.getElementById('console');
    if (!consoleEl) return;
    consoleEl.innerHTML = output
        ? `<span class="console-output">${escapeHtml(output)}</span>`
        : '<div class="console-welcome"><p>Nothing printed yet at this step.</p></div>';
    consoleEl.scrollTop = consoleEl.scrollHeight;
}

/**
 * Mark where the cursor is: the line always, and the expression within it when
 * the recording says which one.
 *
 * The second decoration is the visible half of recording per expression rather
 * than per line.  A step is one evaluation, so several consecutive steps share
 * a line and the line marker alone would sit still through all of them --
 * looking stuck rather than looking like progress.  `endCol` is 0 on a v1
 * recording (and absent on a wasm build older than the field), in which case
 * there is no range to draw and the line marker is the whole story.
 */
function traceHighlight(line, frame) {
    const editor = getEditor();
    const monaco = getMonaco();
    if (!editor) return;
    if (!traceState.decorations) {
        traceState.decorations = editor.createDecorationsCollection([]);
    }
    if (!line) {
        traceState.decorations.set([]);
        return;
    }
    const decorations = [{
        range: new monaco.Range(line, 1, line, 1),
        options: {
            isWholeLine: true,
            className: 'trace-current-line',
            /* No glyphMarginClassName: the editor is created without a glyph
             * margin, and turning one on would shift the whole gutter. */
            linesDecorationsClassName: 'trace-current-marker',
        },
    }];
    const col = frame ? (frame.col | 0) : 0;
    const endCol = frame ? (frame.endCol | 0) : 0;
    if (col > 0 && endCol > col) {
        decorations.push({
            range: new monaco.Range(line, col, line, endCol),
            options: { className: 'trace-current-expr' },
        });
    }
    traceState.decorations.set(decorations);
    editor.revealLineInCenterIfOutsideViewport(line);
}

/**
 * Jump to the next (dir > 0) or previous execution of an editor line.
 *
 * This is what a breakpoint is in a recording: the run already happened, so
 * "next hit on line 12" is a scan rather than a resume.  A miss lands on the
 * boundary and says so, which is the difference between "ran to the end" and
 * "found it".
 */
async function traceJumpToLine(editorLine, dir) {
    if (!traceState.active) return;
    const absolute = editorLine + traceState.baseLine - 1;
    const found = await deps.workerCall({
        type: 'trace-find-line',
        dir,
        file: '',          // "" matches any file; the tab is the only source here
        line: absolute,
    });
    if (!found) return;
    if (!found.hit) {
        deps.showStatus(`No ${dir > 0 ? 'later' : 'earlier'} step on line ${editorLine}`, 'info');
        return;
    }
    await traceSeek(found.index);
}

async function traceDownload() {
    const buf = await deps.workerCall({ type: 'trace-download' });
    if (!buf) {
        deps.showStatus('Nothing to download', 'error');
        return;
    }
    const blob = new Blob([buf], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'run.turtrace';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
}

export function traceInstallHandlers() {
    document.getElementById('trace-btn')?.addEventListener('click', traceCode);
    document.getElementById('trace-close')?.addEventListener('click', traceClose);
    document.getElementById('trace-download')?.addEventListener('click', traceDownload);
    document.getElementById('trace-first')?.addEventListener('click', () => traceSeek(0));
    document.getElementById('trace-last')?.addEventListener('click', () => traceSeek(traceState.steps - 1));
    document.getElementById('trace-back')?.addEventListener('click', () => traceSeek(traceState.index - 1));
    document.getElementById('trace-fwd')?.addEventListener('click', () => traceSeek(traceState.index + 1));

    document.getElementById('trace-slider')?.addEventListener('input', (e) => {
        traceSeek(parseInt(e.target.value, 10) || 0);
    });

    document.getElementById('trace-frames')?.addEventListener('click', (e) => {
        const btn = e.target.closest('[data-frame]');
        if (!btn) return;
        traceState.selectedFrame = parseInt(btn.dataset.frame, 10) || 0;
        traceRenderFrames();
    });

    document.addEventListener('keydown', (e) => {
        if (!traceState.active || !e.altKey) return;
        if (e.key === 'ArrowLeft')  { e.preventDefault(); traceSeek(traceState.index - 1); }
        if (e.key === 'ArrowRight') { e.preventDefault(); traceSeek(traceState.index + 1); }
    });
}

/* Click a line number while the timeline is open -> jump to that line's next
 * execution; Alt+click for the previous one. Registered from the editor setup
 * path, once Monaco exists. */
export function traceInstallGutterHandler(ed) {
    ed.onMouseDown((e) => {
        if (!traceState.active) return;
        if (e.target.type !== getMonaco().editor.MouseTargetType.GUTTER_LINE_NUMBERS) return;
        const line = e.target.position?.lineNumber;
        if (line) traceJumpToLine(line, e.event.altKey ? -1 : 1);
    });
}
