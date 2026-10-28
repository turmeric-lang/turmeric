// hash-params.js -- read and write individual keys of the URL hash (#code=, #doc=).

/**
 * The URL hash as an ordered list of [key, value] pairs.
 *
 * `key=value&key=value`, the shape the share feature already used for
 * `#code=<compressed>` -- but parsed by hand rather than with URLSearchParams,
 * because a docs deep link should read `#doc=guides/hkt-guide`, not
 * `#doc=guides%2Fhkt-guide`. Values are written literally and only `&` and `%`
 * are escaped; neither of our two values (url-safe base64 for `code`, a slug
 * path for `doc`) contains either, so in practice the hash stays plain text.
 */
function parseHashPairs() {
    const raw = window.location.hash.slice(1);
    if (!raw) return [];
    return raw.split('&').filter(Boolean).map(part => {
        const eq = part.indexOf('=');
        const key = eq === -1 ? part : part.slice(0, eq);
        const value = eq === -1 ? '' : part.slice(eq + 1);
        return [key, value.replace(/%26/gi, '&').replace(/%25/g, '%')];
    });
}

/**
 * Merge a key into the URL hash without disturbing the others.
 *
 * The share feature writes `code`; the docs pane writes `doc`. A shared link
 * that also points at a guide round-trips, and neither key clobbers the other.
 * Passing null removes the key.
 */
export function setHashParam(key, value) {
    const pairs = parseHashPairs().filter(([k]) => k !== key);
    if (value !== null && value !== undefined) {
        pairs.push([key, String(value).replace(/%/g, '%25').replace(/&/g, '%26')]);
    }
    const next = pairs.map(([k, v]) => (v === '' ? k : `${k}=${v}`)).join('&');
    // replaceState, not `location.hash =`: a docs navigation should not push a
    // history entry per page, and it must not fire our own hashchange handler.
    const url = `${window.location.pathname}${window.location.search}${next ? '#' + next : ''}`;
    window.history.replaceState(null, '', url);
}

export function getHashParam(key) {
    const hit = parseHashPairs().find(([k]) => k === key);
    return hit ? hit[1] : null;
}
