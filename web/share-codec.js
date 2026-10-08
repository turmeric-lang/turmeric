/**
 * Share links: `#code=<payload>`, where the payload is the editor's source as
 * UTF-8, gzipped, in unpadded base64url.
 *
 * The gzip comes from the browser's own CompressionStream -- every browser the
 * playground runs in has it (it already needs SharedArrayBuffer), so there is
 * no dependency to bundle and nothing for the CSP to allow.  The format is the
 * one `pako.gzip` produced, which main.js called for a long time without ever
 * loading pako: every encode threw a ReferenceError and every share failed
 * (docs/archive/try-share-links-never-encode.md).  A link minted by a build
 * that did have pako decodes unchanged.
 *
 * Both directions are async (the streams are) and never throw: a payload that
 * cannot be produced or read comes back as ''.
 */

function bytesToBase64Url(bytes) {
    // In chunks: String.fromCharCode(...bytes) over a large program overflows
    // the argument stack.
    let bin = '';
    const CHUNK = 0x8000;
    for (let i = 0; i < bytes.length; i += CHUNK) {
        bin += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
    }
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function base64UrlToBytes(text) {
    const b64 = text.replace(/-/g, '+').replace(/_/g, '/');
    const bin = atob(b64 + '='.repeat((4 - (b64.length % 4)) % 4));
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

async function pipe(bytes, transform) {
    const stream = new Blob([bytes]).stream().pipeThrough(transform);
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

/** Source text -> share payload ('' if it cannot be encoded). */
export async function encodeShareCode(code) {
    try {
        const gz = await pipe(new TextEncoder().encode(code), new CompressionStream('gzip'));
        return bytesToBase64Url(gz);
    } catch (e) {
        console.error('Failed to encode share link:', e);
        return '';
    }
}

/** Share payload -> source text ('' if it is not one). */
export async function decodeShareCode(payload) {
    if (!payload) return '';
    try {
        const bytes = base64UrlToBytes(payload);
        // gzip's magic number.  Anything else is read as an uncompressed
        // payload -- the encoding a comment in main.js once said was the
        // interim plan -- and must be valid UTF-8, so a mangled or foreign
        // link loads nothing rather than mojibake.
        if (bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b) {
            const raw = await pipe(bytes, new DecompressionStream('gzip'));
            return new TextDecoder('utf-8', { fatal: true }).decode(raw);
        }
        return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    } catch (e) {
        console.error('Failed to decode share link:', e);
        return '';
    }
}
