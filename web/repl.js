// repl.js -- the REPL prompt at the foot of the console, and its `:` commands.
//
// W1/W2: the prompt.
//
//
// The prompt was a bare <input> with Enter-to-submit and ArrowUp/ArrowDown
// history, and nothing the language server knows reached it -- no completion,
// no hover, no signature help, in the one place a beginner types the most.
//
// The fix is not a completion widget built over the <input>. It is a
// single-line Monaco editor, because then completion, hover and signature help
// at the prompt are *the providers that already exist* rather than a second
// copy of the state machine that drives them.
//
// W4: one table, three consumers.
//
//
// The meta-command vocabulary was about to exist in three places -- the
// dispatch chain, the hand-written `:help` text, and (once the prompt could
// complete) a suggestion list. Three copies is how the help text and the
// switch quietly stop agreeing about what the REPL can do.
//
// `arg` says what a command takes, which is also the completion context: a
// command taking 'sym' offers symbols after the space, one taking nothing
// offers nothing.

import { WASM_STATE, wasmState } from './eval-client.js';
import { escapeHtml } from './html.js';
import { getMonaco } from './editor-ref.js';
import { monacoThemeName } from './editor-theme.js';
import { appendToConsole } from './console.js';
import { docNames, wasmDocLookup, wasmTypeOf, wasmExplain } from './doc-panel.js';
import { loadDocsIndex, docsEntryFor, parseDocsRef, openDocsPane } from './docs-pane.js';
import { traceCode } from './trace.js';
import { lspClient } from './lsp-ref.js';
import { startLspClient } from './lsp.js';

// What the prompt borrows from main.js: running a line and resetting the
// session. Set by configureRepl() at load.
let deps = {
    executeCode: () => Promise.resolve(),
    resetWasm: () => {},
};

/** Hand the prompt what it borrows from main.js. Called once, at load. */
export function configureRepl(d) {
    deps = d;
}

export let replHistory = [];
export let replHistoryIndex = -1;

/** The prompt's Monaco editor, or null before init. */
export let promptEditor = null;

/** The synthetic document the prompt is analysed as (W2). */
export const REPL_DOC_URI = 'file:///project/repl.tur';
export let promptDoc = null;

/**
 * Source the interpreter session has actually accepted -- previous prompt
 * lines and Runs that evaluated without error.
 *
 * This, and not the active tab, is what the prompt may offer. The editor's
 * index is built from whatever file is open; the prompt evaluates against the
 * wasm session, and a `defn` typed in a tab that has never been Run is not
 * callable here. An offered name the session cannot resolve is not merely
 * unhelpful: accepting it produces an expression that fails to evaluate.
 */
export let replSessionSource = '';

export function replSessionAccept(source) {
    const text = String(source || '').trim();
    if (!text) return;
    replSessionSource = replSessionSource ? replSessionSource + '\n' + text : text;
    // Push it now rather than on the debounce, so the very next keystroke's
    // completion already knows about the name that was just defined.
    if (promptDoc) promptDoc.refresh();
}

export function replSessionReset() {
    replSessionSource = '';
    if (promptDoc) promptDoc.refresh();
}

/** Is the suggest widget open? Asked once per keystroke, by one handler. */
export function promptSuggestOpen() {
    if (!promptEditor) return false;
    const c = promptEditor.getContribution('editor.contrib.suggestController');
    // `model.state` is 0 when idle; the widget's own visibility flag is not
    // public, and the controller is.
    return !!(c && c.model && c.model.state !== 0);
}

export function promptValue() {
    return promptEditor ? promptEditor.getValue() : '';
}

export function promptSetValue(text) {
    if (!promptEditor) return;
    promptEditor.setValue(text);
    const line = promptEditor.getModel().getLineCount();
    promptEditor.setPosition({
        lineNumber: line,
        column: promptEditor.getModel().getLineMaxColumn(line),
    });
}

export function promptSetEnabled(on) {
    /* The host class is set whether or not the editor exists yet: the WASM
     * boot and the prompt's construction are independent, and whichever wins
     * the race must not leave the row looking disabled forever. The editor's
     * own initial readOnly is read from wasmState in initReplInput. */
    const host = document.getElementById('repl-input');
    if (host) host.classList.toggle('repl-input-disabled', !on);
    if (promptEditor) promptEditor.updateOptions({ readOnly: !on });
}

export async function promptSubmit() {
    const code = promptValue().trim();
    if (!code || wasmState() !== WASM_STATE.READY) return;

    replHistory.unshift(code);
    replHistoryIndex = -1;
    promptSetValue('');

    if (code.startsWith(':')) {
        appendToConsole(`<span class="console-prompt">turi&gt;</span> ${escapeHtml(code)}`);
        await dispatchReplMetaCommand(code);
    } else {
        const res = await deps.executeCode(code, '<span class="console-prompt">turi&gt;</span>');
        // Only a line the session actually accepted joins the prompt's
        // document. A line that failed defined nothing, and offering its
        // names would be the exact lie W2 exists to prevent.
        if (!res || !res.isError) replSessionAccept(code);
    }

    const consoleEl = document.getElementById('console');
    if (consoleEl) consoleEl.scrollTop = consoleEl.scrollHeight;
}

export function initReplInput() {
    const monaco = getMonaco();
    const host = document.getElementById('repl-input');
    if (!host) return;

    /* The suggest widget has to escape a 22px-tall, overflow:hidden row, so it
     * renders into a body-level node instead of inside the editor.
     *
     * `monaco-editor` on that node is mandatory, not decorative. Monaco ships
     * every widget rule scoped to a `.monaco-editor` ancestor
     * (`.monaco-editor .suggest-widget { ... }`) and declares the whole
     * `--vscode-*` color set on `.monaco-editor` itself. Hoisted out of the
     * editor without the class, the popup matches none of it: transparent
     * background, no border, unsized rows -- suggestions painted straight over
     * the prompt with nothing behind them. */
    let overlay = document.getElementById('repl-suggest-overlay');
    if (!overlay) {
        overlay = document.createElement('div');
        overlay.id = 'repl-suggest-overlay';
        overlay.className = 'repl-suggest-overlay monaco-editor';
        document.body.appendChild(overlay);
    }
    // An overlay left over from a previous init predates the class.
    overlay.classList.add('monaco-editor');

    promptEditor = monaco.editor.create(host, {
        value: '',
        language: 'turmeric',
        theme: monacoThemeName(),
        automaticLayout: true,
        lineNumbers: 'off',
        glyphMargin: false,
        folding: false,
        lineDecorationsWidth: 0,
        lineNumbersMinChars: 0,
        minimap: { enabled: false },
        overviewRulerLanes: 0,
        overviewRulerBorder: false,
        hideCursorInOverviewRuler: true,
        renderLineHighlight: 'none',
        scrollBeyondLastLine: false,
        scrollBeyondLastColumn: 0,
        wordWrap: 'off',
        contextmenu: false,
        fontFamily: 'var(--font-mono)',
        fontSize: 12.5,
        lineHeight: 20,
        padding: { top: 1, bottom: 1 },
        scrollbar: {
            vertical: 'hidden',
            horizontal: 'hidden',
            handleMouseWheel: false,
            alwaysConsumeMouseWheel: false,
        },
        overflowWidgetsDomNode: overlay,
        fixedOverflowWidgets: true,
        readOnly: wasmState() !== WASM_STATE.READY,
        quickSuggestions: { other: true, comments: false, strings: false },
        suggestOnTriggerCharacters: true,
        acceptSuggestionOnEnter: 'on',
        tabCompletion: 'off',
        parameterHints: { enabled: true },
    });

    host.classList.toggle('repl-input-disabled', wasmState() !== WASM_STATE.READY);
    host.classList.add('repl-input-empty');

    const model = promptEditor.getModel();

    promptEditor.onDidChangeModelContent(() => {
        const v = model.getValue();
        host.classList.toggle('repl-input-empty', v.length === 0);
        // One line, always. A paste can carry newlines and Monaco will happily
        // take them; the prompt submits a line, so it keeps one.
        if (v.indexOf('\n') >= 0) {
            const flat = v.replace(/\s*\n\s*/g, ' ');
            promptEditor.setValue(flat);
            promptEditor.setPosition({
                lineNumber: 1,
                column: model.getLineMaxColumn(1),
            });
        }
    });

    /* One handler, not several listeners.
     *
     * Enter submits, ArrowUp/ArrowDown walk history, and the suggest widget
     * wants all three. With two listeners the behaviour depends on
     * registration order, which a reader has to infer rather than read -- so
     * every key that is contested is decided here, in one place, by asking
     * whether the widget is open.
     */
    promptEditor.onKeyDown((e) => {
        const monaco = getMonaco();
        const widget = promptSuggestOpen();

        if (e.keyCode === monaco.KeyCode.Enter && !e.shiftKey) {
            if (widget) return;          // accept the highlighted suggestion
            e.preventDefault();
            e.stopPropagation();
            promptSubmit();
            return;
        }
        if (e.keyCode === monaco.KeyCode.UpArrow) {
            if (widget) return;          // move within the list
            e.preventDefault();
            e.stopPropagation();
            if (replHistoryIndex < replHistory.length - 1) {
                replHistoryIndex++;
                promptSetValue(replHistory[replHistoryIndex]);
            }
            return;
        }
        if (e.keyCode === monaco.KeyCode.DownArrow) {
            if (widget) return;
            e.preventDefault();
            e.stopPropagation();
            if (replHistoryIndex > 0) {
                replHistoryIndex--;
                promptSetValue(replHistory[replHistoryIndex]);
            } else {
                replHistoryIndex = -1;
                promptSetValue('');
            }
            return;
        }
        if (e.keyCode === monaco.KeyCode.Escape) {
            /* Dismissing a popup must not also close the docs pane or a menu.
             * The page has document-level Escape handlers; Monaco's own
             * dismissal happens on this same event, so the key is left to it
             * and only the propagation is stopped. */
            e.stopPropagation();
            return;
        }
    });

    /* W4: meta-command completion, on a `:` at the start of a line and nowhere
     * else. A `:foo` mid-expression is a keyword literal and a map key
     * (`#map{:name name}`), and offering the REPL's vocabulary there would be
     * noise on top of the language completions that already fire. */
    const metaProvider = {
        triggerCharacters: [':', ' '],
        provideCompletionItems(m, position) {
            if (m !== model) return { suggestions: [] };
            const upto = m.getLineContent(position.lineNumber)
                          .slice(0, position.column - 1);

            const typing = /^\s*(:[a-z-]*)$/.exec(upto);
            if (typing) {
                const word = m.getWordUntilPosition(position);
                const range = {
                    startLineNumber: position.lineNumber,
                    endLineNumber: position.lineNumber,
                    // The `:` is not a word character, so getWordUntilPosition
                    // stops after it; the replacement has to start on it or
                    // accepting a suggestion leaves `::doc`.
                    startColumn: position.column - typing[1].length,
                    endColumn: word.endColumn,
                };
                return {
                    suggestions: REPL_META_COMMANDS.map(c => ({
                        label: c.usage ? `${c.name} ${c.usage}` : c.name,
                        kind: getMonaco().languages.CompletionItemKind.Keyword,
                        insertText: c.arg ? c.name + ' ' : c.name,
                        detail: c.summary,
                        filterText: c.name,
                        range,
                    })),
                };
            }

            /* Second context: `:doc ` and `:type ` take a symbol, so
             * completion after them offers symbols rather than commands. One
             * line of context test, reusing the doc table the panel already
             * loaded. */
            const withArg = /^\s*(:[a-z-]+)\s+(\S*)$/.exec(upto);
            if (withArg) {
                const spec = REPL_META_COMMANDS.find(c => c.name === withArg[1]);
                if (!spec || (spec.arg !== 'sym' && spec.arg !== 'expr'))
                    return { suggestions: [] };
                const word = m.getWordUntilPosition(position);
                const range = {
                    startLineNumber: position.lineNumber,
                    endLineNumber: position.lineNumber,
                    startColumn: word.startColumn,
                    endColumn: word.endColumn,
                };
                return {
                    suggestions: (docNames || []).slice(0, 500).map(d => ({
                        label: d.name,
                        kind: getMonaco().languages.CompletionItemKind.Function,
                        insertText: d.name,
                        detail: d.summary ? String(d.summary).split('\n')[0] : undefined,
                        range,
                    })),
                };
            }
            return { suggestions: [] };
        },
    };
    monaco.languages.registerCompletionItemProvider('turmeric', metaProvider);

    /* Test surface. A spec cannot type into Monaco through `fill()` and should
     * not have to read the suggest widget's DOM to find out what was offered,
     * so the prompt's own state and its providers' answers are reachable
     * directly -- while Enter and the arrows still go through the real
     * onKeyDown, which is the part worth covering. */
    window._turiRepl = {
        focus: () => promptEditor && promptEditor.focus(),
        value: () => promptValue(),
        setValue: (t) => promptSetValue(t),
        submit: () => promptSubmit(),
        suggestOpen: () => promptSuggestOpen(),
        sessionSource: () => replSessionSource,
        metaCompletions: (line) => {
            promptSetValue(line);
            const r = metaProvider.provideCompletionItems(model, {
                lineNumber: 1,
                column: model.getLineMaxColumn(1),
            });
            return (r && r.suggestions ? r.suggestions : []).map(x => x.label);
        },
        completions: async (line) => {
            promptSetValue(line);
            if (!lspClient) return [];
            const r = await lspClient.completions(model, {
                lineNumber: 1,
                column: model.getLineMaxColumn(1),
            });
            return (r && r.suggestions ? r.suggestions : []).map(x => x.label);
        },
    };

    /* W2: the prompt's own document. It is opened against the same session the
     * editor's tabs are, which needs nothing from the server --
     * lsp_session_handle is already document-keyed and already holds several
     * documents at once. */
    const attachPromptDoc = () => {
        if (promptDoc || !lspClient) return;
        promptDoc = lspClient.attachDocument({
            model,
            uri: REPL_DOC_URI,
            getPrefix: () => replSessionSource,
        });
    };
    promptEditor.onDidFocusEditorText(() => {
        startLspClient().then(attachPromptDoc).catch(() => {});
    });
    if (lspClient) attachPromptDoc();
}

export const REPL_META_COMMANDS = [
    { name: ':help',    arg: null,     usage: '',        summary: 'show this help text' },
    { name: ':type',    arg: 'expr',   usage: '<expr>',  summary: 'print inferred type without evaluating' },
    { name: ':doc',     arg: 'sym',    usage: '<sym>',   summary: 'print builtin/standard operator documentation' },
    { name: ':docs',    arg: 'page',   usage: '[page]',  summary: 'open the docs browser (guides + API), offline-ready' },
    { name: ':reset',   arg: null,     usage: '',        summary: 'clear session and start fresh' },
    { name: ':explain', arg: 'code',   usage: '[code]',  summary: 'explain the most recent error, or a TUR-E#### code' },
    { name: ':trace',   arg: null,     usage: '',        summary: 'record this program and open the time-travel timeline' },
];

/* Rendered from the table, aligned on the widest label -- so adding a command
 * cannot leave the column crooked, which is the failure mode a hand-written
 * block has and a generated one cannot. */
export function replHelpText() {
    const labels = REPL_META_COMMANDS.map(c =>
        c.usage ? `${c.name} ${c.usage}` : c.name);
    const width = labels.reduce((w, l) => Math.max(w, l.length), 0);
    const rows = REPL_META_COMMANDS.map((c, i) =>
        `  ${labels[i].padEnd(width)}  ${c.summary}`);
    return 'Turmeric REPL Help\n' +
           '------------------\n' +
           rows.join('\n') + '\n' +
           '\n' +
           'Multi-line input: keep typing when parentheses are open;\n' +
           '  an empty line to cancel an incomplete expression.';
}

/**
 * Dispatch web REPL meta-commands locally.
 */
export async function dispatchReplMetaCommand(line) {
    const parts = line.trim().split(/\s+/);
    const cmd = parts[0].toLowerCase();
    const arg = parts.slice(1).join(' ').trim();

    if (cmd === ':help') {
        appendToConsole(
            `<pre class="console-output console-flush">${escapeHtml(replHelpText())}</pre>`);

    } else if (cmd === ':trace') {
        await traceCode();

    } else if (cmd === ':doc') {
        if (!arg) {
            appendToConsole('<span class="console-error">:doc requires a symbol name</span>');
            return;
        }
        const entry = docNames.find(d => d.name === arg);
        if (entry && entry.summary) {
            appendToConsole(`<pre class="console-output console-flush">${escapeHtml(entry.summary)}</pre>`);
        } else {
            const docText = await wasmDocLookup(arg);
            if (docText) {
                appendToConsole(`<pre class="console-output console-flush">${escapeHtml(docText)}</pre>`);
            } else {
                appendToConsole(`<span class="console-output">no documentation for \'${escapeHtml(arg)}\'</span>`);
            }
        }

    } else if (cmd === ':docs') {
        // `:docs` opens the browser at its last location; `:docs <page>` jumps
        // straight to one. A bare slug is resolved against guides first, then
        // API modules, so `:docs hkt-guide` and `:docs tur-list` both work
        // without the reader knowing the pack's section layout.
        await loadDocsIndex();
        let ref;
        if (arg) {
            const candidates = arg.includes('/')
                ? [arg]
                : [`guides/${arg}`, `guides/${arg}-guide`, `api/${arg}`, `spices/${arg}`];
            ref = candidates.find(c => docsEntryFor(parseDocsRef(c).ref));
            if (!ref) {
                appendToConsole(
                    `<span class="console-error">no documentation page '${escapeHtml(arg)}'`
                    + ' -- open :docs and search</span>');
                return;
            }
        }
        openDocsPane(ref);

    } else if (cmd === ':type') {
        if (!arg) {
            appendToConsole('<span class="console-error">:type requires an expression</span>');
            return;
        }
        const typeText = await wasmTypeOf(arg);
        if (typeText.startsWith('error:')) {
            appendToConsole(`<span class="console-error">${escapeHtml(typeText)}</span>`);
        } else {
            appendToConsole(`<span class="console-output">: ${escapeHtml(typeText)}</span>`);
        }

    } else if (cmd === ':explain') {
        const explainText = await wasmExplain(arg);
        appendToConsole(`<pre class="console-output console-flush">${escapeHtml(explainText)}</pre>`);

    } else if (cmd === ':reset') {
        deps.resetWasm();

    } else if (cmd === ':quit' || cmd === ':q') {
        appendToConsole('<span class="console-error">:quit is not supported in the browser REPL</span>');

    } else {
        appendToConsole(`<span class="console-error">unknown meta-command \'${escapeHtml(cmd)}\' -- try :help</span>`);
    }
}
