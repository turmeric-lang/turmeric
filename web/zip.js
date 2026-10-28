// zip.js -- the store-only (uncompressed) ZIP the playground writes a project
// to on Download and reads back on Open (try-turmeric-multi-tab-and-projects-plan,
// phases 2 and 3). No dependency: the files are a few KB of text.

// CRC-32 table (one-time init), used by both the local headers and the
// central directory for store-only entries.
const CRC32_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
        t[n] = c >>> 0;
    }
    return t;
})();

function crc32(bytes) {
    let c = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) {
        c = CRC32_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
}

// Hand-rolled store-only ZIP writer. No deflate -- text files compress
// poorly, the dep surface stays flat, and Phase 3's unzipper can do the
// same. Each entry is `{ path: string, content: string }`. Returns a Blob.
export function buildZip(entries) {
    const enc = new TextEncoder();
    const localChunks = [];      // bytes for the local file part
    const centralChunks = [];    // bytes for the central directory part
    let offset = 0;

    for (const entry of entries) {
        const nameBytes = enc.encode(entry.path);
        const dataBytes = enc.encode(entry.content);
        const c = crc32(dataBytes);
        const size = dataBytes.length;

        // Local file header (PK\x03\x04). All multi-byte fields little-endian.
        const local = new Uint8Array(30 + nameBytes.length);
        const lv = new DataView(local.buffer);
        lv.setUint32(0,  0x04034b50, true);   // signature
        lv.setUint16(4,  20,         true);   // version needed
        lv.setUint16(6,  0,          true);   // flags
        lv.setUint16(8,  0,          true);   // method: 0 = store
        lv.setUint16(10, 0,          true);   // mod time (unset)
        lv.setUint16(12, 0x21,       true);   // mod date (jan 1 1980)
        lv.setUint32(14, c,          true);   // crc-32
        lv.setUint32(18, size,       true);   // compressed size
        lv.setUint32(22, size,       true);   // uncompressed size
        lv.setUint16(26, nameBytes.length, true);
        lv.setUint16(28, 0,          true);   // extra field length
        local.set(nameBytes, 30);

        localChunks.push(local, dataBytes);

        // Central directory entry (PK\x01\x02).
        const central = new Uint8Array(46 + nameBytes.length);
        const cv = new DataView(central.buffer);
        cv.setUint32(0,  0x02014b50, true);
        cv.setUint16(4,  20,         true);   // version made by
        cv.setUint16(6,  20,         true);   // version needed
        cv.setUint16(8,  0,          true);
        cv.setUint16(10, 0,          true);
        cv.setUint16(12, 0,          true);
        cv.setUint16(14, 0x21,       true);
        cv.setUint32(16, c,          true);
        cv.setUint32(20, size,       true);
        cv.setUint32(24, size,       true);
        cv.setUint16(28, nameBytes.length, true);
        cv.setUint16(30, 0,          true);   // extra field length
        cv.setUint16(32, 0,          true);   // comment length
        cv.setUint16(34, 0,          true);   // disk number
        cv.setUint16(36, 0,          true);   // internal attrs
        cv.setUint32(38, 0,          true);   // external attrs
        cv.setUint32(42, offset,     true);   // local header offset
        central.set(nameBytes, 46);
        centralChunks.push(central);

        offset += local.length + dataBytes.length;
    }

    const localSize   = localChunks.reduce((s, a) => s + a.length, 0);
    const centralSize = centralChunks.reduce((s, a) => s + a.length, 0);

    // End of central directory record (PK\x05\x06).
    const eocd = new Uint8Array(22);
    const ev = new DataView(eocd.buffer);
    ev.setUint32(0,  0x06054b50,     true);
    ev.setUint16(4,  0,              true);   // disk
    ev.setUint16(6,  0,              true);
    ev.setUint16(8,  entries.length, true);
    ev.setUint16(10, entries.length, true);
    ev.setUint32(12, centralSize,    true);
    ev.setUint32(16, localSize,      true);
    ev.setUint16(20, 0,              true);   // comment length

    return new Blob([...localChunks, ...centralChunks, eocd],
                    { type: 'application/zip' });
}

export const MAX_ZIP_BYTES   = 5  * 1024 * 1024;   // user-facing size guard
const PARSE_ZIP_LIMIT = 50 * 1024 * 1024;   // safety belt against OOM

// Store-only ZIP reader. Mirrors buildZip(): we only accept method 0
// because that's all we write. Anything else throws and the caller
// surfaces a toast. Returns `{ [path]: Uint8Array }`.
export function readStoreZip(bytes) {
    if (bytes.length > PARSE_ZIP_LIMIT) {
        throw new Error('ZIP exceeds parse limit');
    }
    if (bytes.length < 22 ||
        bytes[0] !== 0x50 || bytes[1] !== 0x4b ||
        bytes[2] !== 0x03 || bytes[3] !== 0x04) {
        throw new Error('Not a ZIP file (bad magic bytes)');
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    // Scan back from the end for the EOCD signature 0x06054b50.
    let eocd = -1;
    for (let i = bytes.length - 22; i >= 0 && i > bytes.length - 65557; i--) {
        if (view.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error('EOCD not found -- truncated or not a ZIP');
    const total    = view.getUint16(eocd + 10, true);
    const cdSize   = view.getUint32(eocd + 12, true);
    const cdOffset = view.getUint32(eocd + 16, true);
    const dec = new TextDecoder('utf-8');
    const out = {};
    let p = cdOffset;
    for (let i = 0; i < total; i++) {
        if (view.getUint32(p, true) !== 0x02014b50) {
            throw new Error(`Bad central directory entry at byte ${p}`);
        }
        const method   = view.getUint16(p + 10, true);
        const compSize = view.getUint32(p + 20, true);
        const nameLen  = view.getUint16(p + 28, true);
        const extraLen = view.getUint16(p + 30, true);
        const cmtLen   = view.getUint16(p + 32, true);
        const localOff = view.getUint32(p + 42, true);
        const name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen));
        if (method !== 0) {
            throw new Error(`Unsupported compression method ${method} for ${name}`);
        }
        const lNameLen  = view.getUint16(localOff + 26, true);
        const lExtraLen = view.getUint16(localOff + 28, true);
        const dataOff = localOff + 30 + lNameLen + lExtraLen;
        out[name] = bytes.subarray(dataOff, dataOff + compSize);
        p += 46 + nameLen + extraLen + cmtLen;
    }
    if (p - cdOffset !== cdSize) throw new Error('Central directory size mismatch');
    return out;
}
