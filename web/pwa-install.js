// pwa-install.js -- the Install button, and the Add to Home Screen hint on iOS.

let deferredInstallPrompt = null;

export function initInstallAffordances() {
    // Capture the install prompt for browsers that support it (Chrome/Edge/Android).
    window.addEventListener('beforeinstallprompt', (e) => {
        e.preventDefault();
        deferredInstallPrompt = e;
        showInstallButton();
    });

    window.addEventListener('appinstalled', () => {
        deferredInstallPrompt = null;
        hideInstallButton();
        try { localStorage.setItem('tur.try.installed.v1', '1'); } catch {}
    });

    if (window.matchMedia && window.matchMedia('(display-mode: standalone)').matches) {
        return;
    }

    // iOS Safari has no beforeinstallprompt -- show a one-time Add to Home Screen hint.
    const ua = navigator.userAgent || '';
    const isIOS = /iPad|iPhone|iPod/.test(ua) && !window.MSStream;
    const isSafari = /^((?!chrome|android|crios|fxios).)*safari/i.test(ua);
    if (isIOS && isSafari) {
        let hinted = false;
        try { hinted = localStorage.getItem('tur.try.ios-a2hs-hint.v1') === '1'; } catch {}
        if (!hinted) {
            setTimeout(showIosA2hsHint, 3000);
        }
    }
}

function showInstallButton() {
    if (document.getElementById('install-btn')) return;
    const host = document.querySelector('.editor-actions');
    if (!host) return;
    const btn = document.createElement('button');
    btn.id = 'install-btn';
    btn.className = 'btn btn-icon';
    btn.title = 'Install Turmeric as an app';
    btn.innerHTML = `<svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
        <path d="M5 20h14v-2H5v2zM19 9h-4V3H9v6H5l7 7 7-7z"/>
    </svg>Install`;
    btn.addEventListener('click', async () => {
        if (!deferredInstallPrompt) return;
        deferredInstallPrompt.prompt();
        try { await deferredInstallPrompt.userChoice; } catch {}
        deferredInstallPrompt = null;
        hideInstallButton();
    });
    host.appendChild(btn);
}

function hideInstallButton() {
    const btn = document.getElementById('install-btn');
    if (btn) btn.remove();
}

function showIosA2hsHint() {
    if (document.getElementById('ios-a2hs-hint')) return;
    const hint = document.createElement('div');
    hint.id = 'ios-a2hs-hint';
    hint.setAttribute('role', 'status');
    hint.innerHTML = `
        <span>Install Turmeric: tap <strong>Share</strong> then <strong>Add to Home Screen</strong>.</span>
        <button class="btn btn-icon btn-sm" id="ios-a2hs-close" aria-label="Dismiss">&times;</button>
    `;
    document.body.appendChild(hint);
    const dismiss = () => {
        hint.remove();
        try { localStorage.setItem('tur.try.ios-a2hs-hint.v1', '1'); } catch {}
    };
    hint.querySelector('#ios-a2hs-close').addEventListener('click', dismiss);
    setTimeout(dismiss, 12000);
}
