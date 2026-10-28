// share.js -- share links: the active buffer in the URL as #code=, written as
// you type, read back on load, and copied to the clipboard by Share.

import { encodeShareCode, decodeShareCode } from './share-codec.js';
import { setHashParam, getHashParam } from './hash-params.js';
import { getEditor } from './editor-ref.js';
import { showStatus } from './status.js';

/**
 * Update URL hash with current code
 */
let urlHashSeq = 0;
export async function updateUrlHash() {
    const editor = getEditor();
    if (!editor) return;
    // Encoding is async; a slower, older encode must not land after a newer one.
    const seq = ++urlHashSeq;
    const encoded = await encodeShareCode(editor.getValue());
    if (encoded && seq === urlHashSeq) {
        // Merge rather than assign: the docs pane keeps a `doc=` key in the
        // same hash, and clobbering it would close the pane mid-read.
        setHashParam('code', encoded);
    }
}

/**
 * Load code from URL hash
 */
export async function loadFromUrlHash() {
    const encoded = getHashParam('code');
    if (encoded) {
        const code = await decodeShareCode(encoded);
        const editor = getEditor();
        if (code && editor && code !== editor.getValue()) {
            editor.setValue(code);
        }
    }
}

/**
 * Share the current code
 */
export async function shareCode() {
    const editor = getEditor();
    if (!editor) return;
    
    const code = editor.getValue();
    const encoded = await encodeShareCode(code);
    
    if (encoded) {
        const url = `${window.location.origin}${window.location.pathname}#code=${encoded}`;
        
        // Copy to clipboard
        navigator.clipboard.writeText(url).then(() => {
            showStatus('URL copied to clipboard!', 'success');
        }).catch(() => {
            // Fallback: show the URL
            prompt('Copy this URL:', url);
        });
    } else {
        showStatus('Failed to encode code', 'error');
    }
}
