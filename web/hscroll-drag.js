// hscroll-drag.js -- drag-to-scroll for the editor header's tab and action
// rows, which overflow sideways at narrow widths.

export function initHScrollDrag() {
    const rows = document.querySelectorAll('.editor-tabs, .editor-actions');
    rows.forEach(initHScrollDragRow);
}

function initHScrollDragRow(el) {
    if (!el) return;

    const updateOverflow = () => {
        el.classList.toggle('has-overflow', el.scrollWidth > el.clientWidth + 1);
    };
    updateOverflow();
    if (typeof ResizeObserver !== 'undefined') {
        // The row's own box never changes when a button appears -- its width
        // comes from the pane -- so observing only `el` misses the overflow
        // that Solve or Back-from-definition unhiding creates. Watch the
        // children too, and re-attach as the child list changes (tabs are
        // re-rendered wholesale by renderTabs()).
        const ro = new ResizeObserver(updateOverflow);
        const observeChildren = () => {
            ro.disconnect();
            ro.observe(el);
            for (const child of el.children) ro.observe(child);
        };
        observeChildren();
        if (typeof MutationObserver !== 'undefined') {
            new MutationObserver(() => {
                observeChildren();
                updateOverflow();
            }).observe(el, { childList: true, attributes: true, subtree: true,
                             attributeFilter: ['hidden', 'style'] });
        }
    }
    window.addEventListener('resize', updateOverflow);

    let startX = 0;
    let startScroll = 0;
    let dragging = false;
    let pointerId = null;

    const isInteractive = (target) => {
        return !!target.closest('button, select, input, a, [role="button"]');
    };

    el.addEventListener('pointerdown', (e) => {
        if (isInteractive(e.target)) return;
        startX = e.clientX;
        startScroll = el.scrollLeft;
        pointerId = e.pointerId;
        dragging = false;
        try { el.setPointerCapture(pointerId); } catch {}
    });

    el.addEventListener('pointermove', (e) => {
        if (pointerId !== e.pointerId) return;
        const dx = e.clientX - startX;
        if (!dragging && Math.abs(dx) > 6) {
            dragging = true;
            el.dataset.dragging = '1';
        }
        if (dragging) {
            el.scrollLeft = startScroll - dx;
            e.preventDefault();
        }
    });

    const endDrag = (e) => {
        if (pointerId !== null) {
            try { el.releasePointerCapture(pointerId); } catch {}
        }
        pointerId = null;
        if (dragging) {
            const swallow = (ev) => {
                ev.stopPropagation();
                ev.preventDefault();
            };
            el.addEventListener('click', swallow, { capture: true, once: true });
            setTimeout(() => { delete el.dataset.dragging; }, 0);
        }
        dragging = false;
    };
    el.addEventListener('pointerup', endDrag);
    el.addEventListener('pointercancel', endDrag);

    // Tab-button click handlers (switch + scroll-into-view) are attached by
    // renderTabs() in the multi-tab state module.
}
