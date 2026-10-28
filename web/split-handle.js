// split-handle.js -- the draggable handle between the editor and the console.

import { safeRead, safeWrite, debounce } from './storage.js';
import { getEditor } from './editor-ref.js';
import { applyMinimapPreference } from './minimap.js';

const SPLIT_KEY_H = 'tur.try.split.h.v1';
const SPLIT_KEY_V = 'tur.try.split.v.v1';
const SPLIT_MIN = 0.15;
const SPLIT_MAX = 0.85;

const isMobileSplit = () =>
    window.matchMedia && window.matchMedia('(max-width: 1024px)').matches;

function applySplit(fraction, vertical) {
    const editor = getEditor();
    const container = document.querySelector('.repl-container');
    if (!container) return;
    const f = Math.min(SPLIT_MAX, Math.max(SPLIT_MIN, fraction));
    container.style.setProperty(vertical ? '--split-v' : '--split-h', String(f));
    if (editor) {
        requestAnimationFrame(() => {
            try { editor.layout(); } catch {}
            // The split is the one thing that can make the editor narrow on a
            // desktop, so the width gate is re-evaluated from here as well as
            // from the ResizeObserver -- layout() is synchronous, the observer
            // is not, and a gate that lags a drag by a frame reads as a bug.
            applyMinimapPreference();
        });
    }
}

function hydrateSplit() {
    const h = safeRead(SPLIT_KEY_H);
    if (typeof h === 'number' && isFinite(h)) applySplit(h, false);
    const v = safeRead(SPLIT_KEY_V);
    if (typeof v === 'number' && isFinite(v)) applySplit(v, true);
}

export function initSplitHandle() {
    const handle = document.getElementById('split-handle');
    const container = document.querySelector('.repl-container');
    if (!handle || !container) return;

    hydrateSplit();

    const refreshAria = () => {
        handle.setAttribute('aria-orientation',
            isMobileSplit() ? 'horizontal' : 'vertical');
    };
    refreshAria();
    window.addEventListener('resize', refreshAria);

    let pointerId = null;
    let vertical = false;
    let rect = null;

    handle.addEventListener('pointerdown', (e) => {
        if (e.button !== undefined && e.button !== 0) return;
        pointerId = e.pointerId;
        vertical = isMobileSplit();
        rect = container.getBoundingClientRect();
        try { handle.setPointerCapture(pointerId); } catch {}
        handle.classList.add('is-dragging');
        document.body.classList.add('split-dragging');
        document.body.style.cursor = vertical ? 'row-resize' : 'col-resize';
        e.preventDefault();
    });

    handle.addEventListener('pointermove', (e) => {
        if (pointerId !== e.pointerId) return;
        const size = vertical ? rect.height : rect.width;
        if (size <= 0) return;
        const offset = vertical ? (e.clientY - rect.top) : (e.clientX - rect.left);
        const fraction = offset / size;
        applySplit(fraction, vertical);
        e.preventDefault();
    });

    const persistSplit = debounce(() => {
        const cs = getComputedStyle(container);
        const h = parseFloat(cs.getPropertyValue('--split-h'));
        const v = parseFloat(cs.getPropertyValue('--split-v'));
        if (isFinite(h)) safeWrite(SPLIT_KEY_H, h);
        if (isFinite(v)) safeWrite(SPLIT_KEY_V, v);
    }, 100);

    const endDrag = (e) => {
        if (pointerId === null) return;
        try { handle.releasePointerCapture(pointerId); } catch {}
        pointerId = null;
        handle.classList.remove('is-dragging');
        document.body.classList.remove('split-dragging');
        document.body.style.cursor = '';
        persistSplit();
    };
    handle.addEventListener('pointerup', endDrag);
    handle.addEventListener('pointercancel', endDrag);

    // Keyboard: arrows nudge by 2%, Home/End jump to bounds
    handle.addEventListener('keydown', (e) => {
        const mobile = isMobileSplit();
        const key = e.key;
        const axisKeys = mobile
            ? { dec: 'ArrowUp', inc: 'ArrowDown' }
            : { dec: 'ArrowLeft', inc: 'ArrowRight' };
        const cs = getComputedStyle(container);
        let current = parseFloat(
            cs.getPropertyValue(mobile ? '--split-v' : '--split-h')
        );
        if (!isFinite(current)) current = 0.5;
        let next = current;
        if (key === axisKeys.dec) next = current - 0.02;
        else if (key === axisKeys.inc) next = current + 0.02;
        else if (key === 'Home') next = SPLIT_MIN;
        else if (key === 'End') next = SPLIT_MAX;
        else return;
        e.preventDefault();
        applySplit(next, mobile);
        persistSplit();
    });

    // Double-click resets to 50/50
    handle.addEventListener('dblclick', () => {
        const mobile = isMobileSplit();
        applySplit(0.5, mobile);
        persistSplit();
    });
}
