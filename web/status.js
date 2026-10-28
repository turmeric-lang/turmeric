// status.js -- the status bar: the status line and its dot, the cursor
// position, and the last run's time.

import { WASM_STATE, wasmState } from './eval-client.js';
import { getEditor } from './editor-ref.js';

/**
 * Show status message
 */
export function showStatus(message, type = 'info') {
    const statusEl = document.getElementById('wasm-status-text');
    const dotEl = document.querySelector('.dot');
    
    statusEl.textContent = message;
    
    if (dotEl) {
        dotEl.className = 'dot';
        if (type === 'success') dotEl.classList.add('online');
        else if (type === 'error') dotEl.classList.add('offline');
        else dotEl.classList.add('loading');
    }
    
    // Reset after a delay
    setTimeout(() => {
        if (wasmState() === WASM_STATE.READY) {
            statusEl.textContent = 'Ready';
            if (dotEl) {
                dotEl.className = 'dot online';
            }
        }
    }, 3000);
}

/**
 * Update cursor position display
 */
export function updateCursorPosition() {
    const editor = getEditor();
    if (!editor) return;
    const cursor = editor.getPosition();
    const posEl = document.getElementById('cursor-pos');
    if (posEl) {
        posEl.textContent = `Line ${cursor.lineNumber}, Column ${cursor.column}`;
    }
}

/**
 * Update execution time display
 */
export function updateExecTime(timeMs) {
    const timeEl = document.getElementById('exec-time');
    if (timeEl) {
        timeEl.textContent = timeMs < 1 ? 'Ready' : `${timeMs.toFixed(0)}ms`;
    }
}
