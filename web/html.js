// html.js -- escaping for strings interpolated into markup or attributes.

const HTML_ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

/**
 * Escape text for HTML -- element content or a quoted attribute value alike.
 *
 * It used to go through textContent -> innerHTML, which escapes `&`, `<` and
 * `>` but leaves both quotes alone, so the same helper was unsafe inside an
 * attribute. It was used there: a `#lang` line naming a base with a `"` in it
 * closed the language picker's value="..." and added its own attributes --
 * onfocus, autofocus -- to the radio button (security-audit-plan W-2). Quotes
 * are escaped now, so no caller has to know which context it is in.
 */
export function escapeHtml(text) {
    return (text == null ? '' : String(text)).replace(/[&<>"']/g, (c) => HTML_ESCAPES[c]);
}

/**
 * Escape text that is going inside a quoted HTML attribute. The same as
 * escapeHtml now that that escapes quotes; kept as its own name so an
 * attribute site says what it is.
 */
export function escapeAttr(text) {
    return escapeHtml(text);
}
