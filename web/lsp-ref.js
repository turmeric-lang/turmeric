// lsp-ref.js -- the language-server client, once lsp.js has booted it.
//
// A module of its own so the modules that ask the server things (tabs, the
// outline, the prompt, project load) can import it without importing lsp.js,
// which imports them. Read it as a live binding: it is null until the first
// startLspClient().

export let lspClient = null;

/** Set by lsp.js when it creates the client. */
export function setLspClient(client) {
    lspClient = client;
}
