// lsp.js -- booting the language server (try-turmeric-lsp-plan): the client
// over the LSP worker, started lazily on first editor focus, and the status
// pill that says whether it is up. The client itself is lsp-ref.js's, so the
// modules that use it do not import this one.

import { createLspClient } from './lsp-client.js';
import { getEditor, getMonaco } from './editor-ref.js';
import { lspClient, setLspClient } from './lsp-ref.js';
import { docNames, fetchDocNames } from './doc-panel.js';
import { tabs, switchTab, openReadOnlyTab, pushJumpOrigin, notifyTabsChanged } from './tabs.js';

export function setLspStatus(state) {
    const wrap = document.getElementById('lsp-status');
    const dot = document.getElementById('lsp-status-dot');
    const text = document.getElementById('lsp-status-text');
    if (!wrap || !dot || !text) return;

    if (state === 'unavailable') {
        // Nothing to say. The playground worked without analysis for its whole
        // life and still does; a permanent red dot would read as a fault.
        wrap.hidden = true;
        return;
    }
    wrap.hidden = false;
    if (state === 'booting') {
        dot.className = 'dot loading';
        text.textContent = 'Starting analysis...';
    } else if (state === 'analyzing') {
        dot.className = 'dot loading';
        text.textContent = 'Analyzing...';
    } else {
        dot.className = 'dot online';
        text.textContent = 'Analysis ready';
    }
}

/**
 * Boot the language server.
 *
 * Lazy on purpose: this is a second WASM instance, which roughly doubles the
 * playground's wasm memory. It is the same /turmeric.js URL so it is a cache
 * hit rather than a second download, but the memory is real -- and a visitor
 * who lands on the page to read a snippet should not pay for an analysis they
 * never asked a question of. First editor focus is the signal that they are
 * going to.
 */
export function startLspClient() {
    const monaco = getMonaco();
    if (lspClient) return lspClient.start();

    // The hover fallback reads doc-names.json, which otherwise only arrives
    // after a successful WASM boot -- so it was absent for the whole window
    // where it is most wanted, and permanently absent when that boot failed.
    // Idempotent and not awaited: a hover that arrives first simply misses,
    // which is the same as having no fallback, which is where we started.
    fetchDocNames();

    setLspClient(createLspClient({
        monaco,
        languageId: 'turmeric',
        // Read-only stdlib buffers are deliberately not part of the document
        // set (M4). They are reference material the user cannot edit, and
        // announcing one under a file:///project/ uri would have the server
        // analyse the stdlib as if it had been pasted into the project --
        // publishing diagnostics against code nobody here can fix.
        getTabs: () => tabs.filter(t => !t.readOnly),
        onStatus: setLspStatus,
        // Hover fallback (M3). Same table the docs pane searches, already
        // fetched by the WASM boot -- no new asset, and no fetch on the hover
        // path: an unloaded table is an empty array, which is a miss.
        lookupDoc: (name) => (docNames || []).find(d => d.name === name) || null,
        onNavigate: (tab, range) => {
            const editor = getEditor();
            // Monaco's standalone editor cannot switch models on its own, so a
            // definition in another tab is a tab switch we perform.
            switchTab(tab.id);
            try {
                editor.revealRangeInCenterIfOutsideViewport(range);
                editor.setPosition({
                    lineNumber: range.startLineNumber,
                    column: range.startColumn,
                });
            } catch {}
        },
        // A definition in the stdlib (M4).
        onOpenExternal: (uri) => openReadOnlyTab(uri),
        // Record where a jump starts. Fired from inside the definition
        // provider, so F12, Cmd+click and the context menu all push -- there
        // is no keybinding here that could miss one of them.
        onBeforeNavigate: (model, position) => {
            const origin = tabs.find(t => t._model === model);
            if (origin) pushJumpOrigin(origin.id, position);
        },
    }));

    // Test surface: lets a spec await the server instead of sleeping on it.
    window._turiLsp = {
        ready: lspClient.start(),
        isAvailable: () => lspClient.isAvailable(),
        isBusy: () => lspClient.isBusy(),
        openDocuments: () => lspClient._openDocumentCount(),
        sync: () => notifyTabsChanged(),
        // Monaco is a module-scoped import, not a global, so a spec has no
        // other way to read the markers the adapter set.
        highlights: () => lspClient.documentHighlights(
            getEditor().getModel(), getEditor().getPosition()),
        markers: () => getMonaco().editor.getModelMarkers({ owner: 'turmeric' })
            .map(m => ({
                message: m.message,
                severity: m.severity,
                startLineNumber: m.startLineNumber,
                startColumn: m.startColumn,
                endColumn: m.endColumn,
            })),
    };

    return lspClient.start().then((ok) => {
        if (ok) notifyTabsChanged();
        return ok;
    });
}
