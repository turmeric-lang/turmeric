// project.js -- the tabs as a project: Download writes them to a .zip (phase 2
// of try-turmeric-multi-tab-and-projects-plan), and Open or a drop onto the
// editor reads one back in place of the current set (phase 3). The zip format
// itself is zip.js.

import { STORAGE_KEYS, safeWrite } from './storage.js';
import { buildZip, readStoreZip, MAX_ZIP_BYTES } from './zip.js';
import { getEditor } from './editor-ref.js';
import { showStatus } from './status.js';
import { tabs, activeId, replaceTabs, tabsSnapshot, captureActiveTabState, currentTab, ensureModel,
         genTabId, sanitizeTabName, applyReadOnlyState, renderTabs, renderJumpBack,
         ensureAllTabModels } from './tabs.js';

// The language-server client is booted and owned by main.js.
let deps = { getLspClient: () => null };

/** Hand the project code what it borrows from main.js. Called once, at load. */
export function configureProject(d) {
    deps = d;
}

// Slugify a tab name into something tur build will accept as a source
// filename. Strips path separators and shell-unfriendly characters,
// guarantees the .tur extension, and disambiguates collisions.
function slugifyTabName(raw, used) {
    let name = (raw || 'untitled.tur').trim().replace(/[\/\\<>:"|?*\x00-\x1f]/g, '_');
    if (!/\.tur$/i.test(name)) name += '.tur';
    if (!used.has(name)) return name;
    const base = name.replace(/\.tur$/i, '');
    for (let n = 2; n < 10000; n++) {
        const candidate = `${base}-${n}.tur`;
        if (!used.has(candidate)) return candidate;
    }
    return `untitled-${Date.now()}.tur`;
}

// Build the project zip's logical entries from the current tab set. Exposed
// (via a wrapper below) so Playwright can drive it without intercepting the
// browser download.
export function buildProjectEntries() {
    captureActiveTabState();
    // Read-only stdlib buffers are not the user's files. A downloaded project
    // zip that carried copies of list.tur would build differently from the
    // workspace it came from -- the real stdlib is already on the path.
    const snapshot = tabsSnapshot().filter(t => !t.readOnly);
    const used = new Set();
    const files = snapshot.map(t => {
        const fileName = slugifyTabName(t.name, used);
        used.add(fileName);
        return { fileName, tab: t };
    });
    // Pick the main: prefer main.tur if present, else the first tab.
    const mainEntry = files.find(f => f.fileName === 'main.tur') || files[0];

    const buildTur = [
        '(project',
        '  :name "try-turmeric-project"',
        `  :main "${mainEntry.fileName}"`,
        `  :src [${files.map(f => `"${f.fileName}"`).join(' ')}]`,
        ')',
        '',
    ].join('\n');

    const workspaceJson = JSON.stringify({
        version: 1,
        activeId: activeId,
        tabs: files.map(f => ({
            id: f.tab.id,
            name: f.tab.name,
            file: `src/${f.fileName}`,
            cursor: f.tab.cursor,
            scrollTop: f.tab.scrollTop,
            createdAt: f.tab.createdAt,
        })),
    }, null, 2);

    const entries = [
        { path: 'build.tur', content: buildTur },
        ...files.map(f => ({ path: `src/${f.fileName}`, content: f.tab.content })),
        { path: '.turmeric/workspace.json', content: workspaceJson },
    ];
    return { entries, mainEntry };
}

export function downloadProject() {
    const { entries } = buildProjectEntries();
    const blob = buildZip(entries);
    const ts = new Date().toISOString().replace(/[:.]/g, '-').replace(/-\d{3}Z$/, 'Z');
    const filename = `try-turmeric-${ts}.zip`;
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    // Free the blob URL after the click has had a chance to start the
    // download -- some browsers will revoke too eagerly otherwise.
    setTimeout(() => URL.revokeObjectURL(url), 0);
    showStatus(`Downloaded ${filename}`, 'success');
}

// Take the raw entry map and shape it into a Tab[] (no Monaco models yet --
// those get created when the project actually applies). Returns
// `{ tabs, activeId }` or throws a user-readable error.
export function parseProjectZip(bytes) {
    const dec = new TextDecoder('utf-8');
    const raw = readStoreZip(bytes);
    // Pull workspace.json first (optional) -- a missing one is fine.
    let ws = null;
    if (raw['.turmeric/workspace.json']) {
        try { ws = JSON.parse(dec.decode(raw['.turmeric/workspace.json'])); }
        catch { ws = null; }
    }
    // Collect *.tur entries under src/.
    const srcFiles = {};
    for (const path of Object.keys(raw)) {
        if (!path.startsWith('src/')) continue;
        if (!/\.tur$/i.test(path)) continue;
        if (path.length === 'src/'.length) continue;
        srcFiles[path] = dec.decode(raw[path]);
    }
    // Reject the zip only when there's nothing meaningful to load -- per
    // the plan, "a zip whose src/ is empty OR whose only .tur files are
    // 0 bytes is rejected." Individual zero-byte tabs in a mixed zip are
    // kept; the user gets a blank tab to fill in.
    const fileNames = Object.keys(srcFiles);
    if (fileNames.length === 0) {
        throw new Error('ZIP contains no .tur source files under src/');
    }
    if (!fileNames.some(f => srcFiles[f].length > 0)) {
        throw new Error('ZIP src/ contains only empty .tur files');
    }
    // Build the tab list. When workspace.json is present we honor its order
    // and metadata for matching files; any extra src/ entries get appended
    // alphabetically so nothing is silently dropped.
    const seen = new Set();
    const tabsOut = [];
    if (ws && Array.isArray(ws.tabs)) {
        for (const t of ws.tabs) {
            const file = typeof t.file === 'string' ? t.file : null;
            if (!file || !(file in srcFiles)) continue;  // referenced file missing
            const content = srcFiles[file];
            tabsOut.push({
                id: typeof t.id === 'string' ? t.id : genTabId(),
                name: sanitizeTabName(t.name || file.replace(/^src\//, '')) || 'main.tur',
                content,
                cursor: t.cursor && typeof t.cursor.lineNumber === 'number'
                    ? { lineNumber: t.cursor.lineNumber, column: t.cursor.column || 1 }
                    : { lineNumber: 1, column: 1 },
                scrollTop: typeof t.scrollTop === 'number' ? t.scrollTop : 0,
                createdAt: typeof t.createdAt === 'number' ? t.createdAt : Date.now(),
                _model: null,
            });
            seen.add(file);
        }
    }
    const leftovers = Object.keys(srcFiles)
        .filter(f => !seen.has(f))
        .sort();
    for (const file of leftovers) {
        tabsOut.push({
            id: genTabId(),
            name: sanitizeTabName(file.replace(/^src\//, '')) || 'untitled.tur',
            content: srcFiles[file],
            cursor: { lineNumber: 1, column: 1 },
            scrollTop: 0,
            createdAt: Date.now(),
            _model: null,
        });
    }
    // Pick the active tab. workspace.json's activeId wins if it points at a
    // tab we kept; else main.tur; else alphabetical first.
    let activeIdOut = null;
    if (ws && typeof ws.activeId === 'string') {
        if (tabsOut.find(t => t.id === ws.activeId)) activeIdOut = ws.activeId;
    }
    if (!activeIdOut) {
        const main = tabsOut.find(t => t.name === 'main.tur');
        activeIdOut = main ? main.id
            : [...tabsOut].sort((a, b) => a.name.localeCompare(b.name))[0].id;
    }
    return { tabs: tabsOut, activeId: activeIdOut };
}

// Replace the current tab set with `parsed`. Persistence and UI re-render
// happen synchronously; callers (drag-drop, file picker) are responsible
// for the user confirmation step before invoking this.
function applyProjectLoad(parsed) {
    const editor = getEditor();
    const lspClient = deps.getLspClient();
    // Dispose old models so we don't leak them into Monaco's global model
    // store.
    for (const t of tabs) {
        if (t._model && !t._model.isDisposed()) {
            try { t._model.dispose(); } catch {}
        }
    }
    replaceTabs(parsed.tabs, parsed.activeId);
    renderJumpBack();
    const active = currentTab();
    if (active && editor) {
        editor.setModel(ensureModel(active));
        applyReadOnlyState(active);
        try {
            if (active.cursor) editor.setPosition(active.cursor);
            if (typeof active.scrollTop === 'number') editor.setScrollTop(active.scrollTop);
        } catch {}
    }
    renderTabs();
    safeWrite(STORAGE_KEYS.tabs, tabsSnapshot());
    safeWrite(STORAGE_KEYS.activeTab, activeId);
    // A wholesale replacement, not an edit. Re-opening the new tabs into a
    // session that still holds the old project's documents would leave
    // workspace/symbol answering with names from a workspace the user closed.
    if (lspClient && lspClient.isAvailable()) {
        ensureAllTabModels();
        lspClient.resetWorkspace();
    }
    showStatus(`Loaded ${tabs.length} tab${tabs.length === 1 ? '' : 's'}`, 'success');
}

// Common entry point: take bytes (Uint8Array) from a File / drop / picker,
// validate, prompt to replace, apply. Resolves when the operation is done.
export async function loadProjectFromBytes(bytes, sourceLabel = 'project') {
    if (bytes.length > MAX_ZIP_BYTES) {
        showStatus(`${sourceLabel} is too large (${(bytes.length / 1024 / 1024).toFixed(1)} MB; limit 5 MB)`, 'error');
        return false;
    }
    let parsed;
    try {
        parsed = parseProjectZip(bytes);
    } catch (e) {
        showStatus(`${sourceLabel}: ${e.message}`, 'error');
        return false;
    }
    // Replace, not merge. Implementation note from the plan: don't write
    // the new tab set to localStorage until the user confirms -- a reject
    // leaves the existing tab set intact.
    const ok = confirm(
        `Loading this project will replace your current ${tabs.length} ` +
        `tab${tabs.length === 1 ? '' : 's'}. The current workspace is in ` +
        `your browser's localStorage and can be recovered by reloading ` +
        `without confirming.`);
    if (!ok) return false;
    applyProjectLoad(parsed);
    return true;
}

export async function loadProjectFromFile(file) {
    if (!file) return false;
    const bytes = new Uint8Array(await file.arrayBuffer());
    return loadProjectFromBytes(bytes, file.name || 'file');
}

export function openProjectFile() {
    const input = document.getElementById('open-project-input');
    if (input) {
        input.value = '';  // allow re-selecting the same file
        input.click();
    }
}

// Drag-and-drop overlay on the editor pane. dragenter / dragover are the
// signals; the visible overlay sits inside .editor-pane and toggles via a
// .drag-over class. dragleave fires too eagerly on child enter/leave, so
// we count enters / leaves and only clear the overlay when the count hits
// zero -- the standard pattern for whole-element drop targets.
export function initProjectDrop() {
    const pane = document.querySelector('.editor-pane');
    if (!pane) return;
    let depth = 0;
    const isZipDrag = (e) => Array.from(e.dataTransfer?.types || []).includes('Files');
    pane.addEventListener('dragenter', (e) => {
        if (!isZipDrag(e)) return;
        e.preventDefault();
        depth++;
        pane.classList.add('drag-over');
    });
    pane.addEventListener('dragover', (e) => {
        if (!isZipDrag(e)) return;
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
    });
    pane.addEventListener('dragleave', (e) => {
        if (!isZipDrag(e)) return;
        depth = Math.max(0, depth - 1);
        if (depth === 0) pane.classList.remove('drag-over');
    });
    pane.addEventListener('drop', async (e) => {
        if (!isZipDrag(e)) return;
        e.preventDefault();
        depth = 0;
        pane.classList.remove('drag-over');
        const file = e.dataTransfer?.files?.[0];
        if (!file) return;
        if (!/\.zip$/i.test(file.name) && file.type !== 'application/zip') {
            showStatus(`${file.name}: not a .zip file`, 'error');
            return;
        }
        await loadProjectFromFile(file);
    });
}
