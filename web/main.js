/**
 * Try Turmeric -- the playground's entry point.
 *
 * This file puts the page together: it boots the eval Worker (initWasm),
 * creates the editor (initEditor), wires every control (initEventListeners)
 * and starts it all on load (init). What each feature does lives in its own
 * module, imported below; web/README.md lists them.
 */

import { isTutorialMode, debounce, resetWorkspace } from './storage.js';
import { EXAMPLES } from './examples.js';
import { provideEditor } from './editor-ref.js';
import { syncMinimapMenuItem, toggleMinimap, initMinimap } from './minimap.js';
import { initSplitHandle } from './split-handle.js';
import { initInstallAffordances } from './pwa-install.js';
import { initSymbolsMenu } from './outline.js';
import { docsPaneState, loadDocsIndex, showDocsPage, openDocsPane, closeDocsPane, docsPaneIsOpen, initDocsPane, syncDocsPaneFromHash } from './docs-pane.js';
import { configureTrace, traceState, traceCode, traceClose, traceSeek, traceInstallHandlers, traceInstallGutterHandler } from './trace.js';
import { monacoThemeName } from './editor-theme.js';
import { configureMonaco } from './editor-language.js';
import { buildZip } from './zip.js';
import { initHScrollDrag } from './hscroll-drag.js';
import { initConsoleHover } from './console-hover.js';
import { WASM_STATE, wasmState, setWasmState, hasWorker, startEvalWorker } from './eval-client.js';
import { fetchLangRegistry, reconcileLangPicker, initLangPicker } from './lang-picker.js';
import { refreshBuildStamp, forceUpdatePWA } from './pwa-update.js';
import { fetchDocNames, wasmDocLookup, showDocPanel, hideDocPanel } from './doc-panel.js';
import { tabs, activeId, tabsSnapshot, persistTabs, currentTab, ensureModel, switchTab, createTab, closeTab, renameTab, jumpStack, jumpBack, renderJumpBack, renderTabs, hydrateTabs, useTutorialTab } from './tabs.js';
import { showStatus, updateCursorPosition } from './status.js';
import { hydrateConsole, appendToConsole, clearConsole, copyConsole } from './console.js';
import { buildProjectEntries, downloadProject, parseProjectZip, loadProjectFromBytes, loadProjectFromFile, openProjectFile, initProjectDrop } from './project.js';
import { promptSetEnabled, initReplInput, configureRepl } from './repl.js';
import { startLspClient } from './lsp.js';
import { CONFIG } from './config.js';
import { updateUrlHash, loadFromUrlHash, shareCode } from './share.js';
import { stopRun, resetWasm, executeCode, definesMainEntry, runCode, formatCode, traceResetEnv } from './session.js';
import { initEnginePicker } from './engine.js';

// ============================================================================
// The editor, and the hookups that cannot be imports
// ============================================================================

let editor = null;
let monaco = null;

// The other modules read the editor through these getters (editor-ref.js).
provideEditor(() => editor, () => monaco);
// The prompt runs lines through the session, which imports the prompt.
configureRepl({ executeCode, resetWasm });

// ============================================================================
// WASM Module Loading
// ============================================================================

/**
 * Initialize the WASM module via the eval Worker.
 * All WASM calls (eval, format, doc lookup, reset) run inside the Worker so
 * that Atomics.wait is permitted and blocking select cannot freeze the tab.
 */
async function initWasm() {
    if (wasmState() !== WASM_STATE.INITIALIZING) return;

    setWasmState(WASM_STATE.LOADING);
    // performance-audit-plan WP5: boot marks, ms since navigation start. The
    // worker adds its own stage times on 'ready' (startEvalWorker).
    window._turiBoot = Object.assign(window._turiBoot || {}, { initWasmAt: performance.now() });

    const loadingOverlay = document.getElementById('loading-overlay');
    const wasmStatus = document.getElementById('wasm-status-text');
    if (loadingOverlay) loadingOverlay.style.display = 'flex';
    if (wasmStatus) wasmStatus.textContent = 'Loading WASM module...';

    try {
        await startEvalWorker();

        console.log('Turmeric WASM runtime initialized');
        setWasmState(WASM_STATE.READY);

        promptSetEnabled(true);

        await fetchDocNames();
        fetchLangRegistry();  // re-renders the picker from the C-side tables
        showStatus('Ready', 'success');
        window._turiBoot = Object.assign(window._turiBoot || {}, { statusReadyAt: performance.now() });
        loadFromUrlHash();

        if (loadingOverlay) loadingOverlay.style.display = 'none';

    } catch (error) {
        console.error('Failed to initialize WASM:', error);
        setWasmState(WASM_STATE.ERROR);
        if (loadingOverlay) loadingOverlay.style.display = 'none';

        if (error.message === 'shared-array-buffer-unavailable') {
            showStatus('Browser not supported', 'error');
            appendToConsole(
                '<span class="console-error">The Try Turmeric playground requires a browser with ' +
                '<a href="https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer#browser_compatibility" ' +
                'target="_blank" rel="noopener" style="color:inherit">SharedArrayBuffer</a> support.<br>' +
                'On iOS, please update to iOS 15.4 or later and use Safari.<br>' +
                'On desktop, Chrome, Firefox, and Edge all work.</span>'
            );
        } else {
            showStatus('Failed to load WASM', 'error');
            appendToConsole('<span class="console-error">Error: Failed to load WASM module. Please refresh the page.</span>');
        }
    }
}

/**
 * Initialize Monaco Editor
 */
async function initEditor() {
    // Hide loading spinner
    const editorLoading = document.getElementById('editor-loading');
    if (editorLoading) editorLoading.style.display = 'none';
    
    // Create editor
    editor = monaco.editor.create(document.getElementById('editor'), {
        value: CONFIG.DEFAULT_CODE,
        language: 'turmeric',
        theme: monacoThemeName(),
        automaticLayout: true,
        tabSize: 2,
        insertSpaces: true,
        detectIndentation: false,
        minimap: {
            // Resolved for real by applyMinimapPreference() below, once the
            // editor exists and its width can be measured. Starting false and
            // turning it on is the cheap direction: an editor that renders the
            // strip and then yanks it away on the first layout pass flickers.
            enabled: false,
            // Blocks, not glyphs. c2mp reached the same conclusion the hard
            // way (minimap.js:8-13): no browser hints glyphs at 2px, so
            // rendered characters are platform-dependent mush. Blocks are
            // legible and cheap.
            renderCharacters: false,
            showSlider: 'mouseover',
            size: 'proportional',
            maxColumn: 80,
        },
        scrollbar: {
            vertical: 'auto',
            horizontal: 'auto',
            verticalScrollbarSize: 8,
            horizontalScrollbarSize: 8
        },
        lineNumbers: 'on',
        lineDecorationsWidth: 10,
        folding: true,
        wordWrap: 'on',
        autoClosingBrackets: 'beforeWhitespace',
        autoClosingQuotes: 'beforeWhitespace',
        autoSurround: 'never',
        bracketMatching: true,
        bracketPairColorization: {
            enabled: true,
            independentColorPoolPerBracketType: false
        },
        guides: {
            bracketPairs: true,
            bracketPairsHorizontal: 'active',
            highlightActiveBracketPair: true
        },
        colorDecorators: true,
        contextmenu: true,
        cursorBlinking: 'blink',
        cursorStyle: 'line',
        disableLayerHinting: true,
        disableMonospaceOptimizations: false,
        dragAndDrop: true,
        find: {
            seedSearchStringFromSelection: true
        },
        hover: {
            enabled: true
        },
        links: true,
        mouseWheelZoom: false,
        multiCursorModifier: 'ctrlCmd',
        multiCursorPaste: 'full',
        occurrencesHighlight: true,
        overviewRulerBorder: false,
        // Three lanes is what puts error and warning ticks in the right-hand
        // strip -- the affordance c2mp builds by hand (minimap.js RULER_W).
        // Unlike the minimap this is not width-gated: it is a few pixels wide
        // at any pane size, and a diagnostic you can find by looking is worth
        // those pixels on a phone too.
        overviewRulerLanes: 3,
        quickSuggestions: {
            other: true,
            comments: false,
            strings: false
        },
        readOnly: false,
        roundedSelection: true,
        scrollBeyondLastColumn: 5,
        scrollBeyondLastLine: true,
        selectOnLineNumbers: true,
        selectionClipboard: true,
        selectionHighlight: true,
        showFoldingControls: 'mouseover',
        smoothScrolling: true,
        suggest: {
            showWords: true,
            showSnippets: true,
            showFiles: false
        },
        suggestOnTriggerCharacters: true,
        wordBasedSuggestions: true,
        fontFamily: "'Iosevka', 'SF Mono', 'Fira Code', 'Fira Mono', Consolas, monospace",
        fontSize: 13,
        fontLigatures: true,
        fontWeight: '400'
    });
    
    // Expose editor for smoke tests
    window._turiEditor = editor;
    // T3: line-number clicks jump the timeline while a recording is open.
    traceInstallGutterHandler(editor);
    // Timeline test surface, alongside _turiTabs below.
    // Eval watchdog test surface (W-3): the limit is settable so a test can
    // watch the automatic stop without waiting out the real 30 s.
    window._turiEval = {
        get timeoutMs() { return CONFIG.EXECUTION_TIMEOUT; },
        set timeoutMs(ms) { CONFIG.EXECUTION_TIMEOUT = ms; },
    };
    window._turiTrace = {
        state:   () => ({ active: traceState.active, steps: traceState.steps,
                          index: traceState.index, baseLine: traceState.baseLine,
                          frames: traceState.frames }),
        run:     () => traceCode(),
        seek:    (i) => traceSeek(i),
        close:   () => traceClose(),
    };
    // Multi-tab test surface. Read-only `tabs()` snapshot keeps tests from
    // accidentally mutating module state.
    window._turiTabs = {
        tabs: () => tabsSnapshot(),
        activeId: () => activeId,
        create: (opts) => createTab(opts),
        close:  (id) => closeTab(id),
        switch: (id) => switchTab(id),
        rename: (id, name) => renameTab(id, name),
        reorder: (id, toIdx) => {
            const fromIdx = tabs.findIndex(t => t.id === id);
            if (fromIdx < 0 || toIdx < 0 || toIdx > tabs.length - 1) return;
            const [m] = tabs.splice(fromIdx, 1);
            tabs.splice(toIdx, 0, m);
            renderTabs();
            persistTabs();
        },
        // Phase 2 surface: lets specs assert the logical project layout
        // without intercepting an actual browser download.
        projectEntries: () => buildProjectEntries().entries,
        // Same data as projectEntries(), but encoded into the zip bytes the
        // user would actually download. The test surface returns a plain
        // Array<number> because Playwright's evaluate() does not transfer
        // Uint8Array cleanly across the wire.
        projectZipBytes: async () => {
            const { entries } = buildProjectEntries();
            const blob = buildZip(entries);
            return Array.from(new Uint8Array(await blob.arrayBuffer()));
        },
        // Phase 3 surface: parse + load entry points for specs that need
        // to drive a fake zip into the app without juggling File objects.
        // `loadBytes` is fire-and-forget from the test's perspective; the
        // confirm() dialog is auto-accepted via page.on('dialog').
        loadBytes: async (bytes) => {
            const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            return loadProjectFromBytes(u8, 'spec.zip');
        },
        parseBytes: (bytes) => {
            const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            return parseProjectZip(u8);
        },
    };

    // Navigation test surface (M4): read-only buffers and the jump-back stack.
    window._turiNav = {
        readOnlyTabs: () => tabs.filter(t => t.readOnly)
            .map(t => ({ id: t.id, name: t.name, sourceUri: t.sourceUri })),
        stackDepth: () => jumpStack.length,
        back: () => jumpBack(),
        // Editor-level truth, not the tab record: what a spec cares about is
        // whether typing would do anything.
        isReadOnly: () => {
            try { return !!editor.getOption(monaco.editor.EditorOption.readOnly); }
            catch { return false; }
        },
    };

    // Hydrate the tab set. Priority order:
    //   1. URL hash share-link (handled later in loadFromUrlHash; it overwrites
    //      the active tab's content).
    //   2. localStorage multi-tab set (or migration from legacy single-buffer).
    //   3. Default Hello World tab.
    // Tutorial mode owns a single ephemeral tab and bypasses persistence.
    if (isTutorialMode()) {
        useTutorialTab(editor.getValue());
    } else {
        hydrateTabs(CONFIG.DEFAULT_CODE);
    }
    // Attach the active tab's model to the editor. The initial editor.create
    // call gave us a throwaway model with CONFIG.DEFAULT_CODE; swapping to the
    // active tab's model replaces it so all subsequent reads/writes go to the
    // tab record.
    const active = currentTab();
    if (active) {
        const orphan = editor.getModel();
        editor.setModel(ensureModel(active));
        if (orphan && !orphan.isDisposed()) {
            try { orphan.dispose(); } catch {}
        }
        try {
            if (active.cursor) editor.setPosition(active.cursor);
            if (typeof active.scrollTop === 'number') editor.setScrollTop(active.scrollTop);
        } catch {}
    }
    renderTabs();
    hydrateConsole();

    // Update cursor position display + per-tab cursor persistence.
    const persistCursor = debounce(() => {
        const t = currentTab();
        if (!t) return;
        const pos = editor.getPosition();
        if (pos) t.cursor = { lineNumber: pos.lineNumber, column: pos.column };
        persistTabs();
    }, 500);
    editor.onDidChangeCursorPosition(() => { updateCursorPosition(); persistCursor(); });

    // Per-tab scroll persistence.
    const persistScroll = debounce(() => {
        const t = currentTab();
        if (!t) return;
        t.scrollTop = editor.getScrollTop();
        persistTabs();
    }, 500);
    editor.onDidScrollChange(persistScroll);

    // Update URL hash on content change (debounced) + persist active tab.
    const persistContent = debounce(() => {
        const t = currentTab();
        if (!t) return;
        t.content = editor.getValue();
        persistTabs();
    }, 250);
    let urlHashTimer;
    editor.onDidChangeModelContent(() => {
        persistContent();
        clearTimeout(urlHashTimer);
        urlHashTimer = setTimeout(updateUrlHash, 1000);
        // Round-trip the picker from the text: typing the header by hand and
        // using the picker are the same operation.  Reads line 1 only.
        reconcileLangPicker();
    });
    
    // Handle Ctrl+Enter to run code
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter,
        () => runCode()
    );
    
    // Handle Ctrl+S to save (update URL hash)
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS,
        (e) => {
            e.preventDefault();
            updateUrlHash();
            showStatus('Code saved to URL', 'success');
        }
    );

    // Handle Alt+Shift+F to format
    editor.addCommand(
        monaco.KeyMod.Alt | monaco.KeyMod.Shift | monaco.KeyCode.KeyF,
        () => formatCode()
    );

    // F12 -> go to definition. Cmd/Ctrl+click already works through the
    // provider; F12 is the binding people reach for without a mouse, and
    // Monaco standalone does not bind it by default.
    editor.addCommand(monaco.KeyCode.F12, () => {
        try { editor.getAction('editor.action.revealDefinition')?.run(); } catch {}
    });

    // Ctrl/Cmd+Alt+- -> back to where the last jump started. Monaco standalone
    // has no navigation history service, so this drives our own stack.
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyMod.Alt | monaco.KeyCode.Minus,
        () => jumpBack()
    );

    // Register as Monaco document formatter so "Format Document" also works
    monaco.languages.registerDocumentFormattingEditProvider('turmeric', {
        provideDocumentFormattingEdits() {
            return formatCode().then(() => []);
        }
    });
    
    // Boot the language server the first time the user puts a cursor in the
    // editor. Reading code costs nothing; asking the editor a question is what
    // pays for the second WASM instance.
    const bootLsp = editor.onDidFocusEditorText(() => {
        bootLsp.dispose();
        startLspClient();
    });

    // Initialize cursor position display
    updateCursorPosition();
    
    // Store editor reference for global access
    window.turmericEditor = editor;
}

/**
 * Clear the editor
 */
function clearEditor() {
    if (editor) {
        editor.setValue('');
    }
}

/**
 * Load an example
 */
function loadExample(name) {
    if (EXAMPLES[name]) {
        if (editor) {
            editor.setValue(EXAMPLES[name]);
        }
        updateUrlHash();
        showStatus(`Loaded example: ${name}`, 'success');
    }
}

/**
 * Initialize event listeners
 */
function initEventListeners() {
    // Run button
    document.getElementById('run-btn')?.addEventListener('click', runCode);

    // Stop: shown by the eval watchdog while a request runs long (W-3) --
    // on either engine's worker.
    document.getElementById('stop-btn')?.addEventListener('click',
        () => stopRun('Stopped'));

    // Clear button (Shift+click = reset full workspace)
    document.getElementById('clear-btn')?.addEventListener('click', (e) => {
        if (e.shiftKey) {
            resetWorkspace();
        } else {
            clearEditor();
        }
    });

    // Back-from-definition button (M4). Hidden until the stack is non-empty.
    document.getElementById('jump-back-btn')?.addEventListener('click', jumpBack);
    renderJumpBack();

    // Time-travel timeline (try-turmeric-tracer-plan T3).
    traceInstallHandlers();

    // Format button
    document.getElementById('format-btn')?.addEventListener('click', formatCode);
    
    // Share button
    document.getElementById('share-btn')?.addEventListener('click', shareCode);

    // Download-project-as-zip button (Phase 2 of the multi-tab plan).
    document.getElementById('download-btn')?.addEventListener('click', downloadProject);

    // Open-project-zip (Phase 3): button + hidden <input type=file> + the
    // drag-drop overlay on the editor pane.
    document.getElementById('open-btn')?.addEventListener('click', openProjectFile);
    document.getElementById('open-project-input')?.addEventListener('change', async (e) => {
        const file = e.target.files?.[0];
        if (file) await loadProjectFromFile(file);
        e.target.value = '';
    });
    initProjectDrop();
    
    // Clear console button
    document.getElementById('clear-console-btn')?.addEventListener('click', clearConsole);
    
    // Copy console button
    document.getElementById('copy-console-btn')?.addEventListener('click', copyConsole);
    
    // Examples dropdown (hamburger button + popover). Mirrors the ⋯ overflow
    // menu below: reparent to <body> so it escapes ancestor overflow/stacking
    // contexts, anchor with position:fixed, close on outside-click / Escape.
    const examplesBtn = document.getElementById('examples-btn');
    const examplesMenu = document.getElementById('examples-menu');
    if (examplesBtn && examplesMenu) {
        if (examplesMenu.parentElement !== document.body) {
            document.body.appendChild(examplesMenu);
        }
        const closeExamples = () => {
            examplesMenu.hidden = true;
            examplesBtn.setAttribute('aria-expanded', 'false');
        };
        const openExamples = () => {
            examplesMenu.hidden = false;
            examplesBtn.setAttribute('aria-expanded', 'true');
            const r = examplesBtn.getBoundingClientRect();
            examplesMenu.style.top = `${r.bottom + 4}px`;
            examplesMenu.style.right = `${Math.max(4, window.innerWidth - r.right)}px`;
        };
        examplesBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            examplesMenu.hidden ? openExamples() : closeExamples();
        });
        examplesMenu.addEventListener('click', (e) => {
            const item = e.target.closest('.more-item');
            if (!item || !item.dataset.example) return;
            loadExample(item.dataset.example);
            closeExamples();
        });
        document.addEventListener('click', (e) => {
            if (!examplesMenu.hidden && !examplesMenu.contains(e.target) && e.target !== examplesBtn) {
                closeExamples();
            }
        });
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !examplesMenu.hidden) closeExamples();
        });
    }

    // Symbols dropdown (the outline)
    initSymbolsMenu();

    // Language picker (dialect radio group + layer checkboxes)
    initLangPicker();
    initEnginePicker();

    // Mobile overflow menu (⋯) — forwards each menu item to the underlying
    // button or the examples <select>, so the existing wiring stays the
    // single source of truth.
    const moreBtn = document.getElementById('more-btn');
    const moreMenu = document.getElementById('more-menu');
    if (moreBtn && moreMenu) {
        // Reparent to <body> so the popover escapes any ancestor
        // overflow:hidden / stacking-context (Monaco, editor-pane, etc.).
        if (moreMenu.parentElement !== document.body) {
            document.body.appendChild(moreMenu);
        }
        const closeMenu = () => {
            moreMenu.hidden = true;
            moreBtn.setAttribute('aria-expanded', 'false');
        };
        const openMenu = () => {
            // The minimap row's label *is* its state, so it has to be right
            // before the menu is painted, not after the click that changes it.
            syncMinimapMenuItem();
            refreshBuildStamp();
            moreMenu.hidden = false;
            moreBtn.setAttribute('aria-expanded', 'true');
            // Anchor below the button. position:fixed so the menu escapes
            // .editor-header's overflow:hidden clip on mobile.
            const r = moreBtn.getBoundingClientRect();
            moreMenu.style.top = `${r.bottom + 4}px`;
            moreMenu.style.right = `${Math.max(4, window.innerWidth - r.right)}px`;
        };
        moreBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            moreMenu.hidden ? openMenu() : closeMenu();
        });
        moreMenu.addEventListener('click', (e) => {
            const item = e.target.closest('.more-item');
            if (!item) return;
            // Stop here rather than letting the click reach document. Some
            // rows forward to a button that opens another popover (Symbols),
            // and the other popovers close themselves on any document click
            // whose target is outside them -- which this one is. Without this,
            // opening Symbols from the ⋯ menu opened and closed it in the same
            // event. Nothing else needs the click at document level: the only
            // listeners there close menus, and closeMenu() below does that
            // for this one.
            e.stopPropagation();
            const cmd = item.dataset.cmd;
            const example = item.dataset.example;
            const action = item.dataset.action;
            if (cmd) {
                document.getElementById(cmd)?.click();
            } else if (example) {
                loadExample(example);
            } else if (action === 'force-update') {
                showStatus('Updating...', 'info');
                forceUpdatePWA();
            } else if (action === 'toggle-minimap') {
                toggleMinimap();
            }
            closeMenu();
        });
        document.addEventListener('click', (e) => {
            if (!moreMenu.hidden && !moreMenu.contains(e.target) && e.target !== moreBtn) {
                closeMenu();
            }
        });
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !moreMenu.hidden) closeMenu();
        });
    }

    // Window resize
    window.addEventListener('resize', () => {
        if (editor) {
            editor.layout();
        }
    });
    
    // Handle hash changes (for sharing, and for #doc= docs deep links)
    window.addEventListener('hashchange', () => {
        loadFromUrlHash();
        syncDocsPaneFromHash();
    });

    // REPL input
    initReplInput();
    initConsoleHover();

    // Horizontal drag-to-scroll on the editor header (mobile / narrow widths)
    initHScrollDrag();

    // Draggable editor/console split (horizontal on desktop, vertical on mobile)
    initSplitHandle();

    // Minimap width gate + user toggle (M1). After initSplitHandle(), which
    // restores the persisted split and therefore the editor's real width.
    initMinimap();

    // PWA install affordances
    initInstallAffordances();
}

// ============================================================================
// Main Initialization
// ============================================================================

/**
 * Main initialization function
 */
async function init() {
    console.log('Initializing Try Turmeric...');
    
    // Load Monaco Editor
    try {
        // Configure Monaco workers BEFORE importing Monaco so workers are
        // available when Monaco initializes during the import itself.
        window.MonacoEnvironment = {
            getWorker: function (_moduleId, label) {
                if (label === 'json') {
                    return new Worker(new URL('monaco-editor/language/json/json.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'css' || label === 'scss' || label === 'less') {
                    return new Worker(new URL('monaco-editor/language/css/css.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'html' || label === 'handlebars' || label === 'razor') {
                    return new Worker(new URL('monaco-editor/language/html/html.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'typescript' || label === 'javascript') {
                    return new Worker(new URL('monaco-editor/language/typescript/ts.worker.js', import.meta.url), { type: 'module' });
                }
                return new Worker(new URL('monaco-editor/editor/editor.worker.js', import.meta.url), { type: 'module' });
            }
        };

        monaco = await import('monaco-editor');

        // Configure Monaco (must happen after import)
        configureMonaco();

        // Initialize editor
        await initEditor();
        
        // Initialize event listeners
        initEventListeners();
        
        // Initialize WASM
        await initWasm();
        
        console.log('Try Turmeric initialized successfully');
        
    } catch (error) {
        console.error('Failed to initialize Try Turmeric:', error);
        appendToConsole('<span class="console-error">Error: Failed to initialize editor. Please refresh the page.</span>');
    }
}

// Start initialization when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}

document.addEventListener('DOMContentLoaded', initDocsPane);

// Export for debugging
window.turmericApp = {
    runCode,
    clearEditor,
    clearConsole,
    formatCode,
    loadExample,
    shareCode,
    resetWasm,
    resetWorkspace,
    showDocPanel,
    hideDocPanel,
    wasmDocLookup,
    openDocsPane,
    closeDocsPane,
    showDocsPage,
    loadDocsIndex,
    getState: () => ({
        wasmState: wasmState(),
        hasEditor: !!editor,
        hasWasm: hasWorker() && wasmState() === WASM_STATE.READY,
        docsOpen: docsPaneIsOpen(),
        docsRef: docsPaneState().ref,
        docsGuideView: docsPaneState().guideView,
    })
};

// The trace resets the session through session.js, which imports the prompt,
// which imports the trace.
configureTrace({
    resetSession: traceResetEnv,
    definesMainEntry,
});

