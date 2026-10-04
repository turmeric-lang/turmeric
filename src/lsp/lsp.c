#include "lsp.h"
#include "lsp_io.h"
#include "lsp_sink.h"
#include "lsp_json.h"
#include "lsp_docs.h"
#include "lsp_sym.h"
#include "lsp_util.h"
#include "lsp_scope.h"

#include "buf.h"
#include "builtins.h"     /* builtin_describe() -- hover on `println`, `+`, ... */
#include "diag.h"
#include "fmt.h"          /* fmt_format_buffer() for textDocument/formatting */
#include "platform_fs.h"  /* mkstemps() on Windows */
#include "pkg.h"          /* R2: the manifest decides the file set */

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* input_pending() needs a "are there bytes waiting on stdin" primitive.  That
 * is poll() on POSIX and PeekNamedPipe() on Windows; see the function itself
 * for why select() is not an option there.  windows.h is pulled in here rather
 * than via a platform_*.h shim because this is the only TU in the compiler
 * that needs it, and the lean/NOGDI/NOMINMAX trio keeps its macros (ERROR,
 * min, max) from colliding with diag.h and friends above. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>       /* _get_osfhandle */
#else
#include <poll.h>
#endif
#include <unistd.h>

/* Declared in main.c (non-static wrapper around compile_to_c) */
extern int tur_check_only(const char *path);

/* -------------------------------------------------------------------------
 * JSON helpers
 * --------------------------------------------------------------------- */

static void json_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20)
                    buf_printf(b, "\\u%04x", (unsigned char)*p);
                else
                    buf_putc(b, *p);
        }
    }
    buf_putc(b, '"');
}

static void send_response(LspSink *sink, const char *id_raw, size_t id_len,
                          const char *result_json) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && id_len > 0)
        buf_write(&b, id_raw, id_len);
    else
        buf_puts(&b, "null");
    buf_puts(&b, ",\"result\":");
    buf_puts(&b, result_json ? result_json : "null");
    buf_puts(&b, "}");
    lsp_sink_send(sink, b.data, b.len);
    buf_free(&b);
}

/* A request must always be answered. Dropping one leaves a conforming client
 * waiting forever on an id that will never come back. */
static void send_error(LspSink *sink, const char *id_raw, size_t id_len,
                       int code, const char *message) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && id_len > 0)
        buf_write(&b, id_raw, id_len);
    else
        buf_puts(&b, "null");
    buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    json_str(&b, message);
    buf_puts(&b, "}}");
    lsp_sink_send(sink, b.data, b.len);
    buf_free(&b);
}

static void send_notification(LspSink *sink, const char *method,
                              const char *params_json) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    json_str(&b, method);
    buf_puts(&b, ",\"params\":");
    buf_puts(&b, params_json ? params_json : "null");
    buf_puts(&b, "}");
    lsp_sink_send(sink, b.data, b.len);
    buf_free(&b);
}

/* -------------------------------------------------------------------------
 * Symbol search helper
 * --------------------------------------------------------------------- */

static void doc_symbol_view(const LspDoc *doc,
                            const LspSymbol **out, int *count);

/* Looks through the document's own index, falling back to the stdlib surface
 * for a document that has never parsed -- so hover, go-to-definition, and
 * signature help over a stdlib name keep working in a file the user is still
 * getting to compile. */
static const LspSymbol *find_symbol(const LspDoc *doc, const char *name) {
    const LspSymbol *syms;
    int              count;
    doc_symbol_view(doc, &syms, &count);
    for (int i = 0; i < count; i++) {
        if (strcmp(syms[i].name, name) == 0)
            return &syms[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Compile + publish diagnostics + collect symbols
 * --------------------------------------------------------------------- */

/* Parse exactly 4 hex digits; returns the value, or -1 if they aren't hex. */
static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int d;
        char c = s[i];
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* Encode one Unicode scalar as UTF-8. Returns bytes written (1..4). */
static size_t utf8_encode(unsigned int cp, char *dst) {
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Decode a JSON string body into `dst`, which must have room for src_len + 1
 * bytes. That bound always holds: every escape shrinks (\uXXXX is 6 input
 * bytes for at most 3 output bytes; a surrogate pair is 12 for 4).
 *
 * \uXXXX must be decoded, not passed through. Dropping the backslash and
 * emitting the literal "u00e9" would corrupt the text *and* shift every byte
 * offset after it, so every diagnostic and hover position later in the file
 * would land on the wrong character. */
static size_t unescape_json(const char *src, size_t src_len, char *dst) {
    size_t di = 0;
    for (size_t si = 0; si < src_len; si++) {
        if (src[si] != '\\' || si + 1 >= src_len) {
            dst[di++] = src[si];
            continue;
        }
        si++;
        switch (src[si]) {
            case '"':  dst[di++] = '"';  break;
            case '\\': dst[di++] = '\\'; break;
            case '/':  dst[di++] = '/';  break;
            case 'b':  dst[di++] = '\b'; break;
            case 'f':  dst[di++] = '\f'; break;
            case 'n':  dst[di++] = '\n'; break;
            case 'r':  dst[di++] = '\r'; break;
            case 't':  dst[di++] = '\t'; break;
            case 'u': {
                if (si + 4 >= src_len) { dst[di++] = src[si]; break; }
                int hi = hex4(src + si + 1);
                if (hi < 0) { dst[di++] = src[si]; break; }
                si += 4;
                unsigned int cp = (unsigned int)hi;
                /* A high surrogate is only meaningful paired with the low one
                 * that follows; together they encode a non-BMP scalar. */
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (si + 6 < src_len && src[si + 1] == '\\' && src[si + 2] == 'u') {
                        int lo = hex4(src + si + 3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10)
                               + ((unsigned int)lo - 0xDC00u);
                            si += 6;
                        }
                    }
                }
                /* An unpaired surrogate is not a legal scalar; substituting
                 * U+FFFD keeps the output valid UTF-8 rather than emitting a
                 * byte sequence the compiler would choke on. */
                if (cp >= 0xD800u && cp <= 0xDFFFu) cp = 0xFFFDu;
                di += utf8_encode(cp, dst + di);
                break;
            }
            default: dst[di++] = src[si]; break;
        }
    }
    dst[di] = '\0';
    return di;
}

#define LSP_SYM_CAP 2048
/* Locals outnumber globals in any real file, so the binding table is sized
 * well above the symbol index. Overflow is reported (lsp_scope_truncated),
 * not swallowed -- see prepareRename. */
#define LSP_BIND_CAP 4096

/* -------------------------------------------------------------------------
 * Stdlib symbol cache
 *
 * Retaining the last good index (below) rescues a document that *used* to
 * parse. It does nothing for one that never has -- a file opened with a
 * syntax error already in it -- because there is no earlier revision to fall
 * back on. That is the case a user is least equipped to get out of unaided,
 * since completion is exactly what would help them fix the error.
 *
 * The stdlib is the honest answer there. Turmeric auto-loads it, so every
 * stdlib symbol genuinely *is* in scope for the broken file; this is not a
 * guess at what the document might contain, which is the part that cannot be
 * known while it does not parse. It is document-independent, so one
 * process-wide copy taken from the first successful analysis serves every
 * document that needs it.
 *
 * Membership is decided by path prefix against TUR_STDLIB_DIR rather than
 * "any symbol not from this document". The looser test would also sweep in
 * the first document's *imports* and then offer them to an unrelated file
 * that never imported them.
 * --------------------------------------------------------------------- */

static LspSymbol *stdlib_syms_      = NULL;
static int        stdlib_sym_count_ = 0;
/* The prime is attempted at most once per session, not once per process. It
 * was a function-local static, which is the same thing on stdio -- one session
 * is one process there. It is not the same thing anywhere a session can be
 * reset and the module keeps running: the latch stayed set while the cache it
 * guarded was freed, so every session after the first served an empty stdlib
 * fallback and a buffer opened with a syntax error had completion dead for the
 * life of the page. */
static int        stdlib_prime_tried_ = 0;

/* Set by resolve_stdlib_root() in main.c before any subcommand runs. NULL if
 * the stdlib could not be located, which disables the cache rather than
 * guessing. */
static const char *stdlib_root(void) {
    static const char *root = NULL;
    static int         tried = 0;
    if (!tried) {
        tried = 1;
        root = getenv("TUR_STDLIB_DIR");
        if (root && !*root) root = NULL;
    }
    return root;
}

static void stdlib_cache_fill(const LspSymbol *syms, int count) {
    const char *root = stdlib_root();
    if (stdlib_syms_ || !root || count <= 0) return;

    size_t rlen = strlen(root);
    int    n    = 0;
    for (int i = 0; i < count; i++) {
        if (syms[i].name[0] && strncmp(syms[i].file_path, root, rlen) == 0) n++;
    }
    if (n == 0) return;

    stdlib_syms_ = calloc((size_t)n, sizeof(LspSymbol));
    if (!stdlib_syms_) return;
    for (int i = 0, o = 0; i < count && o < n; i++) {
        if (syms[i].name[0] && strncmp(syms[i].file_path, root, rlen) == 0)
            stdlib_syms_[o++] = syms[i];
    }
    stdlib_sym_count_ = n;
}

static void stdlib_cache_free(void) {
    free(stdlib_syms_);
    stdlib_syms_        = NULL;
    stdlib_sym_count_   = 0;
    stdlib_prime_tried_ = 0;
}

/* Harvest the stdlib surface by analyzing an empty buffer.
 *
 * Piggy-backing on a real document's successful analysis is not enough: the
 * case that needs the fallback most is a server whose *only* open document is
 * the broken one, where no successful analysis ever happens to harvest from.
 * An empty file compiles -- the stdlib is auto-loaded, so it alone yields the
 * whole surface -- which makes this self-sufficient.
 *
 * Costs one compile, once per process, and only ever on the first request
 * that actually needs the fallback. A session where every document parses
 * never pays it. */
static void stdlib_cache_prime(void) {
    if (stdlib_syms_ || stdlib_prime_tried_) return;
    stdlib_prime_tried_ = 1;
    if (!stdlib_root()) return;

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tur_lsp_std_XXXXXX.tur",
             tur_temp_dir());
    int fd = mkstemps(tmp_path, 4);
    if (fd < 0) return;
    close(fd);   /* empty file is the point */

    LspSymbol *syms  = calloc((size_t)LSP_SYM_CAP, sizeof(LspSymbol));
    int        count = 0;
    if (syms) {
        /* Collect into a scratch diagnostic region so nothing from this
         * synthetic compile can reach the client as a diagnostic about a
         * document it owns. */
        diag_reset();
        diag_lsp_begin();
        tur_collect_symbols(tmp_path, NULL, syms, LSP_SYM_CAP, &count);
        diag_lsp_end();
        stdlib_cache_fill(syms, count);
        free(syms);
    }
    unlink(tmp_path);
}

/* The symbols to answer a request from: the document's own index when it has
 * ever had one, the cached stdlib surface when it has not. */
static void doc_symbol_view(const LspDoc *doc,
                            const LspSymbol **out, int *count) {
    if (!doc->ever_analyzed) {
        stdlib_cache_prime();   /* no-op once attempted */
        if (stdlib_syms_) {
            *out   = stdlib_syms_;
            *count = stdlib_sym_count_;
            return;
        }
    }
    *out   = doc->symbols;
    *count = doc->symbol_count;
}

/* DiagLspRelocateFn for run_doc_analysis: a diagnostic from a loaded or
 * imported file is drawn on the form that pulled that file in, or on the
 * first line when no form in this document names it (a file loaded by a
 * loaded file).  Its real location goes along as relatedInformation, so a
 * client can still jump there. */
static bool relocate_foreign_diag(void *ctx, const char *foreign_path,
                                  uint32_t *line0, uint32_t *col_start0,
                                  uint32_t *col_end0,
                                  char *uri_out, size_t uri_cap) {
    const LspDoc *doc = (const LspDoc *)ctx;
    unsigned l = 0, cs = 0, ce = 1;
    if (lsp_reference_anchor(doc->text, doc->text_len, foreign_path,
                             &l, &cs, &ce)) {
        *line0 = l;
        *col_start0 = cs;
        *col_end0 = ce;
    }
    /* `load` paths are recorded as written (cwd-relative), so resolve before
     * building a URI; a path that no longer resolves gets no related
     * location rather than a wrong one. */
    char abs_path[4096];
    const char *p = foreign_path;
    if (realpath(foreign_path, abs_path)) p = abs_path;
    else if (foreign_path[0] != '/') return false;
    lsp_path_to_uri(p, uri_out, uri_cap);
    return true;
}

static void run_doc_analysis(LspDoc *doc, LspSink *sink) {
    /* 1. Scan docstrings from source text */
    LspDocTable dtable;
    lsp_doc_table_init(&dtable);
    lsp_scan_docs(doc->text, doc->text_len, &dtable);

    /* 2. Write source to a temp file.
     *
     * The directory comes from tur_temp_dir() rather than a literal "/tmp":
     * on Windows a leading slash means "root of the current drive", so the
     * hardcoded path resolved to a C:\tmp that does not exist and every
     * analysis silently produced nothing. */
    char tmp_path[512];
    /* lsp-ignores-the-file-extension: the scratch file carries the
     * document's own extension.  The compiler picks the reader (and for
     * `.scm`, the language) from the extension of the file it opens -- which
     * is this one -- so a fixed `.tur` analysed every headerless `.scm` and
     * `.tur.sweet` buffer as plain Turmeric.  on_formatting already resolved
     * the document's reader from doc->path; this makes analysis agree. */
    const char *suffix = ".tur";
    if (reader_type_from_extension(doc->path) == READER_SWEET)
        suffix = ".tur.sweet";
    else if (reader_type_from_extension(doc->path) == READER_R7RS)
        suffix = ".scm";
    snprintf(tmp_path, sizeof(tmp_path), "%s/tur_lsp_XXXXXX%s",
             tur_temp_dir(), suffix);
    int tmp_fd = mkstemps(tmp_path, (int)strlen(suffix));
    if (tmp_fd < 0) {
        lsp_doc_table_free(&dtable);
        return;
    }
    const char *src = doc->text;
    size_t remaining = doc->text_len;
    while (remaining > 0) {
        ssize_t n = write(tmp_fd, src, remaining);
        if (n <= 0) break;
        src += n;
        remaining -= (size_t)n;
    }
    close(tmp_fd);

    /* 3. Collect into a scratch buffer, not straight into the document.
     *
     * A buffer that does not parse yields no symbols, and *not parsing is the
     * normal state while typing*: the moment the user types `(` or `(foo`, the
     * form is unbalanced. Overwriting the index at that point took completion,
     * hover, and go-to-definition from 200 answers to zero -- precisely when
     * they are wanted. So the new set is adopted only if it has something in
     * it (or the compile genuinely succeeded, which is what makes an
     * emptied-out file report empty rather than stay stale), and otherwise the
     * previous one is kept and flagged. */
    LspSymbol *fresh       = calloc((size_t)LSP_SYM_CAP, sizeof(LspSymbol));
    int        fresh_count = 0;
    LspBinding *fresh_b    = calloc((size_t)LSP_BIND_CAP, sizeof(LspBinding));
    int        fresh_bcount = 0;

    /* 4. Run the compiler: collect symbols + locals + gather diagnostics.
     *
     * One compile answers both. The scope walk hangs off the same elaboration
     * hook the symbol harvest does (see main.c), so bracketing it here is the
     * whole of what it costs. */
    diag_reset();
    diag_init(false);
    diag_lsp_begin();
    if (fresh_b)
        lsp_scope_begin(fresh_b, LSP_BIND_CAP, &fresh_bcount, tmp_path);
    int rc = tur_collect_symbols(tmp_path, doc->path, fresh,
                                 LSP_SYM_CAP, &fresh_count);
    int fresh_btrunc = lsp_scope_truncated() ? 1 : 0;
    lsp_scope_end();
    diag_lsp_remap_path(tmp_path, doc->path);

    /* 5. Populate docstrings and fix up file paths */
    for (int i = 0; i < fresh_count; i++) {
        LspSymbol *sym = &fresh[i];
        /* Fill docstring from the re-scanner */
        lsp_scan_docs_lookup(&dtable, sym->name,
                             sym->doc, sizeof(sym->doc));
        /* Remap temp file path to the real document path */
        if (strcmp(sym->file_path, tmp_path) == 0)
            strncpy(sym->file_path, doc->path, sizeof(sym->file_path) - 1);
    }

    /* A result is usable when the compile succeeded, or when it failed late
     * enough to still have collected symbols (a type error after a clean
     * parse). Either way it describes the current text, so it is adopted and
     * is not stale. */
    int usable = (rc == 0 || fresh_count > 0);

    if (usable) {
        lsp_doc_free_symbols(doc);
        doc->symbols       = fresh;
        doc->symbol_cap    = LSP_SYM_CAP;
        doc->symbol_count  = fresh_count;
        doc->symbols_stale = 0;
        doc->ever_analyzed = 1;
        /* The binding table rides the symbol index's adopt/retain decision
         * rather than making its own. Two tables from two revisions of the
         * text is how a rename ends up bounded by a scope that has moved. */
        lsp_doc_free_bindings(doc);
        doc->bindings           = fresh_b;
        doc->binding_cap        = LSP_BIND_CAP;
        doc->binding_count      = fresh_bcount;
        /* A table that could not be allocated is indistinguishable from one
         * with no locals in it, and those two call for opposite edits -- so it
         * reports as truncated, which is what makes rename refuse. */
        doc->bindings_truncated = fresh_b ? fresh_btrunc : 1;
        fresh_b = NULL;
        stdlib_cache_fill(fresh, fresh_count);
    } else if (!doc->ever_analyzed) {
        /* Nothing to retain -- this text has never parsed. Adopt the empty
         * result rather than leaving symbols NULL, and leave ever_analyzed
         * clear so doc_symbol_view() serves the stdlib surface instead of an
         * empty list. Testing `doc->symbols` here (as this once did) set the
         * pointer non-NULL on the first failure and made every later failure
         * retain the emptiness forever. */
        lsp_doc_free_symbols(doc);
        doc->symbols       = fresh;
        doc->symbol_cap    = LSP_SYM_CAP;
        doc->symbol_count  = 0;
        doc->symbols_stale = 0;
        lsp_doc_free_bindings(doc);
    } else {
        free(fresh);
        doc->symbols_stale = 1;
    }
    free(fresh_b);

    unlink(tmp_path);

    /* 6. Build and send publishDiagnostics notification */
    Buf params;
    buf_init(&params);
    buf_puts(&params, "{\"uri\":");
    json_str(&params, doc->uri);
    buf_puts(&params, ",\"diagnostics\":");
    /* lsp-publishes-other-files-diagnostics-under-one-uri: entries from
     * other files are moved onto this document rather than published at
     * their own coordinates under its URI.  One publish per URI instead
     * would overwrite that file's own diagnostics whenever it is also open
     * -- the last publish for a URI wins -- so they stay here, anchored. */
    diag_lsp_flush_array_for(&params, doc->path, relocate_foreign_diag, doc);
    buf_puts(&params, "}");
    buf_putc(&params, '\0'); /* NUL-terminate for C-string use; strlen stops here */
    diag_lsp_end();

    send_notification(sink, "textDocument/publishDiagnostics", params.data);
    buf_free(&params);
    lsp_doc_table_free(&dtable);
}

/* Analyze every document whose text changed since it was last looked at, and
 * publish its diagnostics.
 *
 * Analysis is deferred rather than run inline from didOpen/didChange because
 * run_doc_analysis writes the buffer to a temp file and runs a full compile on
 * this thread. Doing that per keystroke made a fast typist queue one compile
 * per character, and since there is no $/cancelRequest, every hover and
 * completion behind them waited too. */
static void flush_one_dirty(LspDoc *doc, void *ctx) {
    if (!doc->dirty) return;
    doc->dirty = 0;
    run_doc_analysis(doc, (LspSink *)ctx);
}

static void lsp_flush_dirty(LspSink *sink) {
    lsp_docs_iterate_mut(flush_one_dirty, sink);
}

/* Wait up to timeout_ms for another message to show up on fd_in.
 * Returns 1 if input is pending, 0 on timeout.
 *
 * Safe only because lsp_read_message reads straight from the fd with no
 * userspace buffer of its own — otherwise a buffered message could be sitting
 * in memory while poll() reported the fd quiet. */
#ifdef _WIN32
/* Windows has no poll() over stdio: select() is socket-only, and a pipe HANDLE
 * is not a waitable synchronization object -- WaitForSingleObject on one tells
 * you nothing about whether bytes are buffered.  PeekNamedPipe is the primitive
 * that actually answers the question, sampled on a short tick until the
 * deadline.
 *
 * A 10ms tick against the 200ms debounce is at most 20 wakeups per quiet
 * period and bounds the added latency at one tick, which is far finer than a
 * heuristic about human typing pauses needs.
 *
 * Anything that is not a pipe (a console stdin, i.e. someone driving the
 * server by hand) makes PeekNamedPipe fail; report "quiet" in that case.  That
 * is the safe direction: the caller analyzes the dirty document and loops, and
 * because the flush clears the dirty flag the next iteration falls through to
 * the blocking read.  Reporting "pending" instead would strand diagnostics
 * until the client happened to send another message. */
static int input_pending(int fd_in, int timeout_ms) {
    HANDLE h = (HANDLE)_get_osfhandle(fd_in);
    if (h == INVALID_HANDLE_VALUE) return 0;
    for (int waited = 0; ; waited += 10) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail > 0) return 1;
        if (waited >= timeout_ms) return 0;
        Sleep(10);
    }
}
#else
static int input_pending(int fd_in, int timeout_ms) {
    struct pollfd pfd = { .fd = fd_in, .events = POLLIN, .revents = 0 };
    for (;;) {
        int n = poll(&pfd, 1, timeout_ms);
        if (n < 0 && errno == EINTR) continue;  /* not a timeout; keep waiting */
        return n > 0;
    }
}
#endif

/* How long the client must go quiet before a changed document is analyzed. */
#define LSP_ANALYSIS_DEBOUNCE_MS 200

/* -------------------------------------------------------------------------
 * Message queue + $/cancelRequest
 *
 * The server is single-threaded and synchronous: a request is read, answered,
 * and only then is the next one read. That leaves no window in which a cancel
 * can arrive -- which is why there was no $/cancelRequest at all, and why the
 * one client that needed it grew request timeouts and a generation counter to
 * throw away replies for buffers that had already moved on.
 *
 * The window is opened here rather than by threading the server. The
 * expensive step is the analysis flush, so after flushing (and before doing
 * the per-request work) whatever the client sent meanwhile is drained off the
 * fd into a queue. Cancels found in that drain apply immediately; every other
 * message stays queued in order and is handled on the following iterations.
 * A cancel for a request whose answer is already on the wire is a no-op, which
 * is exactly what the spec asks for.
 * --------------------------------------------------------------------- */

typedef struct {
    char **items;
    size_t head, count, cap;
} MsgQueue;

static MsgQueue queue_ = {0};

static void queue_push(char *msg) {
    if (queue_.head + queue_.count == queue_.cap) {
        if (queue_.head > 0) {
            memmove(queue_.items, queue_.items + queue_.head,
                    queue_.count * sizeof(char *));
            queue_.head = 0;
        } else {
            queue_.cap = queue_.cap ? queue_.cap * 2 : 16;
            queue_.items = realloc(queue_.items, queue_.cap * sizeof(char *));
        }
    }
    queue_.items[queue_.head + queue_.count] = msg;
    queue_.count++;
}

static char *queue_pop(void) {
    if (queue_.count == 0) return NULL;
    char *m = queue_.items[queue_.head++];
    queue_.count--;
    if (queue_.count == 0) queue_.head = 0;
    return m;
}

static void queue_free(void) {
    while (queue_.count > 0) free(queue_pop());
    free(queue_.items);
    queue_.items = NULL;
    queue_.cap = queue_.head = queue_.count = 0;
}

/* Ids the client has asked us to abandon. Small and bounded: a cancel is only
 * ever consulted once, immediately after it is recorded, so old entries can be
 * evicted without losing anything a client would notice. */
#define LSP_CANCEL_MAX 64
static char cancelled_[LSP_CANCEL_MAX][40];
static int  cancelled_n_ = 0;

static void cancel_record(const char *id_raw, size_t id_len) {
    if (!id_raw || id_len == 0 || id_len >= sizeof(cancelled_[0])) return;
    if (cancelled_n_ == LSP_CANCEL_MAX) {
        memmove(cancelled_, cancelled_ + 1,
                (size_t)(LSP_CANCEL_MAX - 1) * sizeof(cancelled_[0]));
        cancelled_n_--;
    }
    memcpy(cancelled_[cancelled_n_], id_raw, id_len);
    cancelled_[cancelled_n_][id_len] = '\0';
    cancelled_n_++;
}

/* True (and forgets the id) if this request was cancelled while we worked. */
static bool cancel_take(const char *id_raw, size_t id_len) {
    if (!id_raw || id_len == 0) return false;
    for (int i = 0; i < cancelled_n_; i++) {
        if (strlen(cancelled_[i]) == id_len &&
            memcmp(cancelled_[i], id_raw, id_len) == 0) {
            memmove(cancelled_ + i, cancelled_ + i + 1,
                    (size_t)(cancelled_n_ - i - 1) * sizeof(cancelled_[0]));
            cancelled_n_--;
            return true;
        }
    }
    return false;
}

/* Pull everything the client has already sent off the fd without blocking,
 * recording cancels and queueing the rest. Bounded so a client that never
 * stops talking cannot keep us in here forever. */
static void drain_pending(int fd_in) {
    for (int i = 0; i < 64 && input_pending(fd_in, 0); i++) {
        char *msg = lsp_read_message(fd_in);
        if (!msg) return;

        size_t mlen;
        const char *m = lsp_json_str(msg, "method", &mlen);
        if (m && mlen == strlen("$/cancelRequest") &&
            memcmp(m, "$/cancelRequest", mlen) == 0) {
            size_t plen;
            const char *p = lsp_json_raw(msg, "params", &plen);
            size_t cid_len = 0;
            const char *cid = p ? lsp_json_raw(p, "id", &cid_len) : NULL;
            cancel_record(cid, cid_len);
            free(msg);
            continue;
        }
        queue_push(msg);
    }
}

/* -------------------------------------------------------------------------
 * Request / notification handlers
 * --------------------------------------------------------------------- */

static bool initialized_ = false;
static bool shutdown_    = false;

/* Does the client accept markdown in hover contents? Answering "yes"
 * unconditionally meant a plaintext-only client rendered the literal ``` fences
 * and had to strip them itself. */
static bool hover_markdown_ = true;

static void on_initialize(const char *id_raw, size_t id_len,
                          const char *params, LspSink *sink) {
    /* capabilities.textDocument.hover.contentFormat is an ordered array of
     * MarkupKind, most-preferred first. Absent means the 3.17 default, which
     * includes markdown. */
    hover_markdown_ = true;
    if (params) {
        size_t caps_len, td_len, hov_len, fmt_len;
        const char *caps = lsp_json_raw(params, "capabilities", &caps_len);
        const char *td   = caps ? lsp_json_raw(caps, "textDocument", &td_len) : NULL;
        const char *hov  = td   ? lsp_json_raw(td, "hover", &hov_len) : NULL;
        const char *fmt  = hov  ? lsp_json_raw(hov, "contentFormat", &fmt_len) : NULL;
        if (fmt) {
            /* A raw array slice; a substring test is enough to tell the two
             * legal MarkupKind values apart. */
            hover_markdown_ = false;
            for (size_t i = 0; i + 8 <= fmt_len; i++) {
                if (memcmp(fmt + i, "markdown", 8) == 0) {
                    hover_markdown_ = true;
                    break;
                }
            }
        }
    }

    send_response(sink, id_raw, id_len,
        "{\"capabilities\":{"
          /* LSP 3.17 positionEncoding. `character` offsets here have always
           * been byte offsets (see lsp_word_at_pos in lsp_util.c), not the
           * UTF-16 code units the spec defaults to. Declaring utf-8 makes that
           * the negotiated truth instead of a silent mismatch that only shows
           * up on lines containing non-ASCII. */
          "\"positionEncoding\":\"utf-8\","
          "\"textDocumentSync\":1,"
          "\"hoverProvider\":true,"
          "\"definitionProvider\":true,"
          "\"documentSymbolProvider\":true,"
          /* Occurrence highlight. The client's own fallback is a text match,
           * which matches inside strings and comments; this one is scanned
           * with the reader's regions skipped (lsp_scan_occurrences). */
          "\"documentHighlightProvider\":true,"
          /* prepareProvider, not the bare boolean. The refusals in
           * rename_refusal are half the feature -- a rename that quietly
           * corrupts a shadowed binding is worse than no rename -- and the
           * prepare form is the only way the reason reaches the user BEFORE
           * they have typed a new name. */
          "\"renameProvider\":{\"prepareProvider\":true},"
          "\"referencesProvider\":true,"
          "\"workspaceSymbolProvider\":true,"
          "\"documentFormattingProvider\":true,"
          /* Space, not `(`.  In a lisp nothing names the callee yet at
           * the instant `(` is typed -- the head comes after it -- so a `(`
           * trigger could only ever be answered with null.  After the space
           * that follows the head is the first position with an answer, and
           * a signature request does not force an analysis (see the
           * dispatcher), so firing on it is cheap. */
          "\"signatureHelpProvider\":{"
            "\"triggerCharacters\":[\" \"],"
            "\"retriggerCharacters\":[\" \"]"
          "},"
          /* Space was advertised as a completion trigger, which in a lisp
           * fires on nearly every keystroke and forced a full analysis behind
           * each one. Both known clients declined to honor it. Advertise only
           * what is actually affordable; a client that wants completion
           * elsewhere sends an explicit `invoked` request. */
          "\"completionProvider\":{"
            "\"triggerCharacters\":[\"(\"]"
          "}"
        "}}");
    initialized_ = true;
}

static void on_shutdown(const char *id_raw, size_t id_len, LspSink *sink) {
    shutdown_ = true;
    send_response(sink, id_raw, id_len, "null");
}

static void on_did_open(const char *params, size_t params_len, LspSink *sink) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;

    size_t text_raw_len;
    const char *text_raw = lsp_json_str(td, "text", &text_raw_len);

    char *text = NULL;
    size_t text_len = 0;
    if (text_raw) {
        text = malloc(text_raw_len + 1);
        text_len = unescape_json(text_raw, text_raw_len, text);
    }

    lsp_doc_open(uri, strlen(uri), text ? text : "", text_len);
    free(text);
    (void)sink;  /* analysis is deferred; see lsp_flush_dirty */
}

static void on_did_change(const char *params, size_t params_len, LspSink *sink) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;

    size_t cc_len;
    const char *cc_arr = lsp_json_raw(params, "contentChanges", &cc_len);
    if (!cc_arr) return;

    /* textDocumentSync is Full (1), so every element carries the whole
     * document and the last one wins. Reading only element 0 -- which is what
     * this did -- silently dropped the rest of a batched notification. A
     * client that ignores the negotiated sync kind and sends ranged changes
     * still gets the wrong answer, but it gets the *last* wrong answer rather
     * than the first, which is at least self-consistent. */
    const char *p    = cc_arr;
    const char *last = NULL;
    const char *cc_end = cc_arr + cc_len;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    for (;;) {
        while (p < cc_end && (*p == ' ' || *p == '\n' || *p == '\r' ||
                              *p == '\t' || *p == ',')) p++;
        if (p >= cc_end || *p != '{') break;
        last = p;
        /* Skip this object, honoring nesting and quoted braces. */
        int depth = 0;
        while (p < cc_end) {
            if (*p == '"') {
                p++;
                while (p < cc_end && *p != '"') { if (*p == '\\') p++; p++; }
            } else if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
    }
    if (!last) return;

    size_t text_raw_len;
    const char *text_raw = lsp_json_str(last, "text", &text_raw_len);
    if (!text_raw) return;

    char *text = malloc(text_raw_len + 1);
    size_t text_len = unescape_json(text_raw, text_raw_len, text);

    lsp_doc_change(uri, strlen(uri), text, text_len);
    free(text);
    (void)sink;  /* analysis is deferred; see lsp_flush_dirty */
}

static void on_did_close(const char *params, size_t params_len) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;
    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;
    lsp_doc_close(uri, strlen(uri));
}

/* -------------------------------------------------------------------------
 * LD2: textDocument/hover
 * --------------------------------------------------------------------- */

static void on_hover(const char *id_raw, size_t id_len,
                     const char *params, LspSink *sink) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(sink, id_raw, id_len, "null"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { send_response(sink, id_raw, id_len, "null"); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->symbols) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    char name[128];
    if (!lsp_word_at_pos(doc->text, doc->text_len,
                         line_0 + 1, char_0 + 1, name, sizeof(name))) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    const LspSymbol *sym = find_symbol(doc, name);
    if (!sym) {
        /* Not in the index. That is not the same as "not a thing": the index
         * is built from Bindings, and a compiler builtin has none -- so
         * `println`, `+`, `=` and `not` all land here, which are the names a
         * newcomer types first. The operator table is the only place that
         * knows their signatures; ask it before giving up. */
        char desc[512];
        if (builtin_describe(name, desc, sizeof(desc)) > 0) {
            Buf b;
            buf_init(&b);
            buf_puts(&b, "{\"contents\":{\"kind\":");
            buf_puts(&b, hover_markdown_ ? "\"markdown\"" : "\"plaintext\"");
            buf_puts(&b, ",\"value\":");
            Buf md;
            buf_init(&md);
            if (hover_markdown_) buf_puts(&md, "```\n");
            buf_puts(&md, desc);
            if (hover_markdown_) buf_puts(&md, "\n```");
            /* Say where this came from. A builtin has no source file and no
             * docstring, and a hover that looked like every other one would
             * imply a definition the user could go to. */
            buf_puts(&md, hover_markdown_
                ? "\n\n_built-in operator_"
                : "\n\n(built-in operator)");
            buf_putc(&md, '\0');
            json_str(&b, md.data);
            buf_free(&md);
            buf_puts(&b, "}}");
            buf_putc(&b, '\0');
            send_response(sink, id_raw, id_len, b.data);
            buf_free(&b);
            return;
        }
        send_response(sink, id_raw, id_len, "{\"contents\":\"\"}");
        return;
    }

    Buf result;
    buf_init(&result);
    buf_puts(&result, "{\"contents\":{\"kind\":");
    buf_puts(&result, hover_markdown_ ? "\"markdown\"" : "\"plaintext\"");
    buf_puts(&result, ",\"value\":");

    /* Build markdown: ```\n(name : type)\n```\n\ndocstring
     * A plaintext-only client gets the same text without the fences -- sending
     * them anyway just made the client strip them back out. */
    Buf md;
    buf_init(&md);
    if (hover_markdown_) buf_puts(&md, "```\n");
    buf_putc(&md, '(');
    buf_puts(&md, sym->name);
    if (sym->type_str[0]) {
        buf_puts(&md, " : ");
        buf_puts(&md, sym->type_str);
    }
    buf_putc(&md, ')');
    if (hover_markdown_) buf_puts(&md, "\n```");
    if (sym->doc[0]) {
        buf_puts(&md, "\n\n");
        buf_puts(&md, sym->doc);
    }
    buf_putc(&md, '\0');

    json_str(&result, md.data);
    buf_free(&md);
    buf_puts(&result, "}}");
    buf_putc(&result, '\0');

    send_response(sink, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * LD3: textDocument/definition
 * --------------------------------------------------------------------- */

/* TUR_LSP_DEBUG=1: trace why a request answered `null`.  Goes to stderr --
 * stdout is the protocol.  Every null return in this file is a silent
 * "nothing here" to the editor, which is indistinguishable from a genuine
 * absence; this says which branch produced it. */
#define LSPDBG(...) do { if (getenv("TUR_LSP_DEBUG")) { \
    fprintf(stderr, "lsp-debug: "); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static void on_definition(const char *id_raw, size_t id_len,
                          const char *params, LspSink *sink) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { LSPDBG("no textDocument"); send_response(sink, id_raw, id_len, "null"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        LSPDBG("no uri");
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { LSPDBG("no position"); send_response(sink, id_raw, id_len, "null"); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->symbols) {
        LSPDBG("doc=%p symbols=%p for uri %s", (void *)doc,
               doc ? (void *)doc->symbols : NULL, uri);
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    char name[128];
    if (!lsp_word_at_pos(doc->text, doc->text_len,
                         line_0 + 1, char_0 + 1, name, sizeof(name))) {
        LSPDBG("no word at %d:%d", line_0, char_0);
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    const LspSymbol *sym = find_symbol(doc, name);
    if (!sym || sym->file_path[0] == '\0') {
        LSPDBG("find_symbol(%s) -> %s", name, sym ? "found but empty file_path" : "NOT FOUND");
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    /* Build "file://" URI from sym->file_path */
    char def_uri[1024];
    lsp_path_to_uri(sym->file_path, def_uri, sizeof(def_uri));

    /* Convert 1-based spans to 0-based LSP positions */
    int def_line = sym->line > 0 ? sym->line - 1 : 0;
    int def_col  = sym->col_start > 0 ? sym->col_start - 1 : 0;
    int def_end  = sym->col_end > 0 ? sym->col_end - 1 : def_col;

    Buf result;
    buf_init(&result);
    buf_printf(&result,
        "{\"uri\":\"%s\","
        "\"range\":{"
          "\"start\":{\"line\":%d,\"character\":%d},"
          "\"end\":{\"line\":%d,\"character\":%d}"
        "}}",
        def_uri, def_line, def_col, def_line, def_end);

    send_response(sink, id_raw, id_len, result.data);
    buf_free(&result);
}

/* LSP SymbolKind for a symbol, from the kind the collector recorded.
 *
 * The type_str test below is the pre-kind mapping, kept as the fallback for
 * any LspSymbol that reached here without one -- a stale cache entry, or a
 * future collection path that forgets to pass a kind. It can only ever answer
 * function-or-not, which is why the kind field exists. */
static int lsp_symbol_kind(const LspSymbol *sym) {
    switch (sym->kind) {
        case LSP_KIND_FUNCTION: return 12;  /* Function */
        case LSP_KIND_VALUE:    return 13;  /* Variable */
        case LSP_KIND_STRUCT:   return 23;  /* Struct   */
        case LSP_KIND_ENUM:     return 10;  /* Enum     */
        case LSP_KIND_UNKNOWN:  break;
    }
    return (strncmp(sym->type_str, "(fn", 3) == 0) ? 12 : 13;
}

/* -------------------------------------------------------------------------
 * textDocument/documentHighlight
 * (try-turmeric-navigation-and-minimap-plan, M5)
 *
 * Without this the client falls back to Monaco's word-based selection
 * highlight, which is textual: it matches inside strings and comments, and it
 * cannot tell `total` from the `total` in `subtotal`. That was survivable
 * while the answer only appeared under the cursor. With the minimap on it is
 * painted down the whole file, so a wrong answer becomes a visible wrong
 * answer -- which is why this landed in the same plan as the strip.
 * --------------------------------------------------------------------- */

/* Which occurrences of a name are uses of the *particular* binding the caret
 * landed on.
 *
 * `bind` non-NULL means the caret is on a local, and the answer is its scope.
 * `bind` NULL means the caret is on a global -- and the answer is NOT "the
 * whole buffer": it is the whole buffer MINUS every region where a local of
 * the same name is in effect. Both halves are needed and they fail in
 * opposite directions. Without the first, renaming the inner `total` of
 * `(let [total ...] ...)` rewrites the outer one; without the second,
 * renaming the outer one rewrites the inner. The second is the half that
 * actually changes what a program means, because the shadowed occurrences
 * are the ones a reader is least likely to check. */
typedef struct {
    const LspBinding *bind;
    const LspBinding *tab;    /* the document's binding table */
    int               n_tab;
    const char       *name;
} ScopeGate;

/* Two ranges per binding, not one, and the gap between them is the point. A
 * binding's own initializer sits before its scope starts: `(let [x (+ x 1)]
 * x)` binds a new `x` whose init reads the OLD one. The binder itself is in
 * range so a caret on the declaration resolves to the declaration -- c2mp's
 * S11.3 found exactly that case resolving to whatever global shared the name,
 * and highlighting the whole file as a result. */
static bool bind_covers(const LspBinding *b, size_t off) {
    if (!b) return true;
    if (b->def_off_end > b->def_off_start &&
        off >= b->def_off_start && off < b->def_off_end)
        return true;
    return off >= b->scope_start_off && off < b->scope_end_off;
}

static bool gate_admits(const ScopeGate *g, size_t off) {
    if (g->bind) return bind_covers(g->bind, off);
    return lsp_scope_lookup_at(g->tab, g->n_tab, off, g->name) == NULL;
}

typedef struct {
    Buf      *out;
    int       emitted;
    int       def_line;   /* 0-based line of the definition, or -1 */
    int       def_col;    /* 0-based byte column of the definition */
    ScopeGate gate;
} HighlightCtx;

static void highlight_emit(size_t off, int line0, int col0, int len,
                           void *user) {
    HighlightCtx *ctx = (HighlightCtx *)user;
    if (!gate_admits(&ctx->gate, off)) return;
    /* The definition gets Write (3), every use gets Text (1). A client styles
     * the two differently, and "which one of these is the definition" is the
     * question a reader scanning a column of marks actually has. Read (2) is
     * not used: telling a read from a write needs the elaborated tree, and
     * claiming the distinction from a text scan would be a guess. */
    int kind = (line0 == ctx->def_line && col0 == ctx->def_col) ? 3 : 1;
    if (ctx->emitted > 0) buf_putc(ctx->out, ',');
    buf_printf(ctx->out,
        "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                   "\"end\":{\"line\":%d,\"character\":%d}},\"kind\":%d}",
        line0, col0, line0, col0 + len, kind);
    ctx->emitted++;
}

/* -------------------------------------------------------------------------
 * What a position names
 *
 * The one question highlight, rename and references all start from: the
 * identifier under the cursor, and whether it is a local (with a bounded
 * scope) or a global (with the whole file, and possibly the workspace).
 * --------------------------------------------------------------------- */

typedef struct {
    LspDoc           *doc;
    char              uri[1024];
    char              name[128];
    size_t            off;
    const LspBinding *bind;  /* non-NULL: a local, scope-bounded */
    const LspSymbol  *sym;   /* non-NULL: a known global */
} NameRef;

static bool resolve_name_at(const char *params, NameRef *ref) {
    memset(ref, 0, sizeof(*ref));
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return false;
    if (lsp_json_str_copy(td, "uri", ref->uri, sizeof(ref->uri)) < 0) return false;

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) return false;
    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(ref->uri, strlen(ref->uri));
    if (!doc || !doc->text) return false;
    ref->doc = doc;

    if (!lsp_word_at_pos(doc->text, doc->text_len, line_0 + 1, char_0 + 1,
                         ref->name, sizeof(ref->name)))
        return false;

    ref->off  = lsp_offset_at_pos(doc->text, doc->text_len,
                                  line_0 + 1, char_0 + 1);
    ref->bind = lsp_scope_lookup_at(doc->bindings, doc->binding_count,
                                    ref->off, ref->name);
    ref->sym  = ref->bind ? NULL : find_symbol(doc, ref->name);
    return true;
}

/* Line/column of a byte offset, 0-based, columns in bytes (utf-8 encoding is
 * what the server negotiated). */
static void offset_to_line_col(const char *text, size_t off,
                               int *line0, int *col0) {
    int    l  = 0;
    size_t ls = 0;
    for (size_t i = 0; i < off; i++) {
        if (text[i] == '\n') { l++; ls = i + 1; }
    }
    *line0 = l;
    *col0  = (int)(off - ls);
}

static void on_document_highlight(const char *id_raw, size_t id_len,
                                  const char *params, LspSink *sink) {
    NameRef ref;
    if (!resolve_name_at(params, &ref)) {
        /* Not on a word (or no such document). `null` rather than `[]`: the
         * spec's "no result", which lets the client keep whatever it was
         * already showing instead of clearing it as the caret crosses a
         * space. */
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    HighlightCtx ctx;
    Buf out;
    buf_init(&out);
    buf_putc(&out, '[');
    ctx.out      = &out;
    ctx.emitted  = 0;
    ctx.def_line = -1;
    ctx.def_col  = -1;
    ctx.gate.bind  = ref.bind;
    ctx.gate.tab   = ref.doc->bindings;
    ctx.gate.n_tab = ref.doc->binding_count;
    ctx.gate.name  = ref.name;

    if (ref.bind) {
        /* A local's definition is its binder. `def_line == 0` marks a
         * macro-introduced binding, whose "position" is a point in expanded
         * source -- there is no row in this file to mark. */
        if (ref.bind->def_line > 0) {
            ctx.def_line = (int)ref.bind->def_line - 1;
            ctx.def_col  = ref.bind->def_col_start > 0
                             ? (int)ref.bind->def_col_start - 1 : 0;
        }
    } else if (ref.sym && ref.sym->file_path[0] && ref.doc->path &&
               strcmp(ref.sym->file_path, ref.doc->path) == 0) {
        ctx.def_line = ref.sym->line > 0 ? ref.sym->line - 1 : 0;
        ctx.def_col  = ref.sym->col_start > 0 ? ref.sym->col_start - 1 : 0;
    }

    lsp_scan_occurrences(ref.doc->text, ref.doc->text_len, ref.name,
                         highlight_emit, &ctx);

    buf_putc(&out, ']');
    buf_putc(&out, '\0');
    send_response(sink, id_raw, id_len, out.data);
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * A2/A3: textDocument/prepareRename, textDocument/rename,
 *        textDocument/references
 * (editor-intelligence-follow-through-plan, 2.2 and 2.3)
 *
 * Rename is the feature the scope resolver was built for, and refusing is
 * half of it. c2mp lists rename as not covered in both its S11.4 and its
 * S13.4 for one reason: "the shadowing caveat is tolerable for a highlight
 * and is not tolerable for an edit that rewrites text." With S1 in place the
 * shadowing answer is right, and every case where it still is not -- a
 * macro-introduced binder, a truncated binding table, a name that belongs to
 * the stdlib or to another file -- gets a refusal with a reason rather than a
 * quiet corruption.
 *
 * That is what `prepareProvider` buys: the client asks first, and the
 * message lands before the user has typed a new name, not after the edit.
 * --------------------------------------------------------------------- */

/* Renaming a name a *fetched* spice may import edits an API we cannot see the
 * consumers of. Off by default; `tur lsp --rename-exports` says the operator
 * knows what the workspace's downstream is. */
static bool rename_exports_ = false;

void lsp_set_rename_exports(bool on) { rename_exports_ = on; }

/* The directory holding the nearest enclosing build.tur, or false.
 *
 * A bounded walk-up, the same shape `tur check` uses to find a spice from a
 * file inside it (CLAUDE.md, "Per-file Commands Inside a Spice"). Both
 * manifest spellings count -- `build.tur` and `build.tur.sweet` -- because
 * both are manifests everywhere else a manifest is read. */
#define LSP_SPICE_WALK_MAX 40

/* Last path separator, either spelling.
 *
 * The walk-up below used strrchr(cur, '/') alone.  A path that reached the
 * server from a Windows editor is spelled C:\dir\src\user.tur and contains no
 * forward slash at all, so the very first lookup returned NULL and the
 * function answered "not in a spice" for every file on the platform.  Nothing
 * reported an error -- cross-module definition, completion and rename simply
 * came back empty, which reads as "no such symbol" rather than "I never
 * looked".  `tur check` resolves the same spice correctly, because it has its
 * own walk-up that does not share this code.
 *
 * Same shape as the bug fixed in find_stdlib_beside_exe and
 * rewrite_autolink_relative_paths: a POSIX-only path assumption that is
 * invisible until a drive-lettered path arrives. */
static char *lsp_last_sep(char *p) {
    char *slash = strrchr(p, '/');
#ifdef _WIN32
    char *bs = strrchr(p, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    return slash;
}

static bool spice_root_of(const char *file_path, char *out, size_t cap) {
    if (!file_path || !out) return false;
    char cur[4096];
    snprintf(cur, sizeof(cur), "%s", file_path);
    char *slash = lsp_last_sep(cur);
    if (!slash) return false;
    *slash = '\0';

    for (int i = 0; i < LSP_SPICE_WALK_MAX; i++) {
        char cand[4200];
        struct stat st;
        snprintf(cand, sizeof(cand), "%s/build.tur", cur);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", cur);
            return true;
        }
        snprintf(cand, sizeof(cand), "%s/build.tur.sweet", cur);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", cur);
            return true;
        }
        slash = lsp_last_sep(cur);
        if (!slash || slash == cur) return false;
        *slash = '\0';
    }
    return false;
}

/* The module path a file is imported as: its path under `<root>/src/`, minus
 * the extension. `src/frame/schema.tur` is `frame/schema`, which is what an
 * `(import frame/schema)` in a sibling module spells. */
static bool module_name_of(const char *root, const char *file_path,
                           char *out, size_t cap) {
    char prefix[4200];
    int n = snprintf(prefix, sizeof(prefix), "%s/src/", root);
    if (n <= 0 || (size_t)n >= sizeof(prefix)) return false;
    if (strncmp(file_path, prefix, (size_t)n) != 0) return false;
    snprintf(out, cap, "%s", file_path + n);
    char *dot = strstr(out, ".tur");
    if (dot) *dot = '\0';
    return out[0] != '\0';
}

/* Does `text` contain an `(import <module> ...)` naming `module`?
 *
 * Cheap and deliberately so: the alternative is re-running module resolution
 * per candidate file, and the cost of a false positive here is scanning a
 * file that has no occurrences anyway. A false *negative* would be the
 * expensive direction, so the test is a token match after the word `import`
 * rather than anything cleverer. */
static bool file_imports_module(const char *text, size_t len,
                                const char *module) {
    size_t mlen = strlen(module);
    for (size_t i = 0; i + 6 <= len; i++) {
        if (memcmp(text + i, "import", 6) != 0) continue;
        if (i > 0 && (isalnum((unsigned char)text[i - 1]) ||
                      text[i - 1] == '-' || text[i - 1] == '_'))
            continue;
        size_t j = i + 6;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j + mlen > len) continue;
        if (memcmp(text + j, module, mlen) != 0) continue;
        char after = (j + mlen < len) ? text[j + mlen] : '\n';
        if (after == '/' ) continue;   /* a longer module path */
        if (isalnum((unsigned char)after) || after == '-' || after == '_')
            continue;
        return true;
    }
    return false;
}

/* Every `.tur` / `.tur.sweet` file under `dir`, depth-first.
 *
 * `tur build <dir>` descends into `src/` recursively (CLAUDE.md, manifest
 * -driven build descent), so a workspace scan that only read the top level
 * would miss exactly the nested `src/<pkg>/` layout the build supports. */
typedef void (*LspFileFn)(const char *path, void *user);

static void walk_tur_files(const char *dir, int depth, LspFileFn fn,
                           void *user) {
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_tur_files(full, depth + 1, fn, user);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        size_t len = strlen(ent->d_name);
        bool is_tur = (len > 4 && strcmp(ent->d_name + len - 4, ".tur") == 0) ||
                      (len > 10 && strcmp(ent->d_name + len - 10, ".tur.sweet") == 0);
        if (is_tur) fn(full, user);
    }
    closedir(d);
}

/* Read a whole file. Caller frees. */
static char *read_file_text(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

/* -------------------------------------------------------------------------
 * The workspace walk (R2), shared by rename and references
 * --------------------------------------------------------------------- */

/* How many workspace files one request will analyze.
 *
 * Each one costs a compile (see workspace_visit for why the compile is not
 * optional), so this is the line between "a rename takes a moment" and "the
 * editor appears to have hung". Overrun is reported, not silently dropped:
 * rename refuses, because an edit that skipped files is worse than no edit. */
#define LSP_WS_ANALYZE_MAX 200

typedef struct {
    /* Where the answer accumulates. Rename writes `"<uri>":[TextEdit...]`
     * pairs; references writes a flat Location array. */
    Buf        *out;
    int         files_emitted;
    int         hits;
    int         files_seen;
    bool        overflowed;
    /* A file we were about to edit but could not elaborate. Its locals are
     * unknown, so whether an occurrence in it is OUR name is unknown too. */
    char        unanalyzable[512];
    const char *name;
    const char *new_name;   /* NULL for references */
    const char *skip_path;  /* the open document, already handled */
    const char *module;     /* the defining module, for the import filter */
    bool        as_locations;
    /* Scratch for the per-file analysis, allocated once for the whole walk
     * rather than once per file -- the symbol destination alone is megabytes. */
    LspSymbol  *scratch_syms;
    LspBinding *scratch_binds;
} WorkspaceCtx;

typedef struct {
    Buf        *out;
    int         emitted;
    const char *new_name;
    const char *uri;
    bool        as_locations;
} FileEditCtx;

static void file_edit_emit(size_t off, int line0, int col0, int len,
                           void *user) {
    (void)off;
    FileEditCtx *c = (FileEditCtx *)user;
    if (c->emitted > 0) buf_putc(c->out, ',');
    if (c->as_locations) {
        buf_puts(c->out, "{\"uri\":");
        json_str(c->out, c->uri);
        buf_printf(c->out,
            ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                        "\"end\":{\"line\":%d,\"character\":%d}}}",
            line0, col0, line0, col0 + len);
    } else {
        buf_printf(c->out,
            "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                        "\"end\":{\"line\":%d,\"character\":%d}},\"newText\":",
            line0, col0, line0, col0 + len);
        json_str(c->out, c->new_name);
        buf_putc(c->out, '}');
    }
    c->emitted++;
}

/* The same emit, gated on the scope the name actually has.
 *
 * Only this document needs it: the workspace walk only ever runs for a
 * global, whose region is every file that imports it. A local's uses cannot
 * leave the form that binds it. */
typedef struct {
    FileEditCtx *fc;
    ScopeGate    gate;
    int          def_line;  /* 0-based, or -1 */
    int          def_col;
    bool         skip_definition;
} RenameScopeCtx;

static void rename_scoped_emit(size_t off, int line0, int col0, int len,
                               void *user) {
    RenameScopeCtx *c = (RenameScopeCtx *)user;
    if (!gate_admits(&c->gate, off)) return;
    if (c->skip_definition && line0 == c->def_line && col0 == c->def_col)
        return;
    file_edit_emit(off, line0, col0, len, c->fc);
}

static void workspace_visit(const char *path, void *user) {
    WorkspaceCtx *ws = (WorkspaceCtx *)user;
    if (ws->skip_path && strcmp(path, ws->skip_path) == 0) return;
    if (ws->overflowed) return;

    size_t len = 0;
    char *text = read_file_text(path, &len);
    if (!text) return;
    /* The import filter is the cheap half of "does this file mean OUR name":
     * a module that never imported the defining one cannot be referring to
     * it, whatever it happens to spell. */
    if (ws->module && *ws->module && !file_imports_module(text, len, ws->module)) {
        free(text);
        return;
    }
    if (++ws->files_seen > LSP_WS_ANALYZE_MAX) {
        ws->overflowed = true;
        free(text);
        return;
    }

    char uri[1200];
    lsp_path_to_uri(path, uri, sizeof(uri));

    /* Analyze the file for its OWN locals before editing it.
     *
     * The alternative -- rewriting every textual occurrence in a file that
     * imports the module -- is the cross-file half of the shadowing bug, and
     * it is the destructive half: a sibling module that binds a local named
     * `total` and separately imports the global `total` would have its local
     * silently renamed too. A compile per file is the only thing that can
     * tell those apart, and rename is a deliberate, occasional action that
     * can afford one. LSP_WS_ANALYZE_MAX bounds how many. */
    int nb = 0;
    bool analyzed = true;
    if (ws->scratch_syms && ws->scratch_binds) {
        int ns = 0;
        /* Diagnostics from these compiles are about files the client did not
         * ask about; they go into a scratch region and are dropped, the same
         * way stdlib_cache_prime's are. */
        diag_reset();
        diag_lsp_begin();
        lsp_scope_begin(ws->scratch_binds, LSP_BIND_CAP, &nb, path);
        int rc = tur_collect_symbols(path, NULL, ws->scratch_syms,
                                     LSP_SYM_CAP, &ns);
        lsp_scope_end();
        diag_lsp_end();
        /* A type error after a clean parse still leaves a complete binding
         * table; only a file that produced nothing at all is unusable. */
        analyzed = (rc == 0 || ns > 0 || nb > 0);
    }

    Buf edits;
    buf_init(&edits);
    FileEditCtx fc = { &edits, 0, ws->new_name, uri, ws->as_locations };
    RenameScopeCtx rc = {
        &fc, { NULL, ws->scratch_binds, nb, ws->name }, -1, -1, false
    };
    lsp_scan_occurrences(text, len, ws->name, rename_scoped_emit, &rc);
    free(text);

    if (fc.emitted > 0 && !analyzed && !ws->as_locations &&
        !ws->unanalyzable[0]) {
        /* Only a file we would actually have edited matters. A broken file
         * with no occurrences of the name is none of this rename's business,
         * and refusing over it would make an unrelated syntax error anywhere
         * in the workspace block every rename. */
        snprintf(ws->unanalyzable, sizeof(ws->unanalyzable), "%s", path);
    }
    if (fc.emitted > 0) {
        if (ws->files_emitted > 0 || ws->hits > 0) buf_putc(ws->out, ',');
        if (!ws->as_locations) {
            json_str(ws->out, uri);
            buf_putc(ws->out, ':');
            buf_putc(ws->out, '[');
        }
        buf_write(ws->out, edits.data, edits.len);
        if (!ws->as_locations) buf_putc(ws->out, ']');
        ws->files_emitted++;
        ws->hits += fc.emitted;
    }
    buf_free(&edits);
}

/* Scan every file the workspace can reach: the project's own `src/`, plus
 * each `:path`-based `:spices` dep's `src/`.
 *
 * The file set comes from the manifest, in C, and not from the client's open
 * documents -- c2mp's S13.2 result, and the reasoning transfers exactly. A
 * second implementation of the module search path in JavaScript will
 * eventually disagree with the compiler's, and a disagreement here does not
 * produce a cosmetic bug: it produces an edit applied to the wrong file. */
static void workspace_scan(const char *root, WorkspaceCtx *ws) {
    char src_dir[4200];
    snprintf(src_dir, sizeof(src_dir), "%s/src", root);
    walk_tur_files(src_dir, 0, workspace_visit, ws);

    char manifest_path[4200];
    if (!pkg_resolve_manifest_path(root, manifest_path, sizeof(manifest_path)))
        return;
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return;
    for (int i = 0; i < m.n_spices; i++) {
        const PkgSpice *sp = &m.spices[i];
        /* `:url`-backed and globally-installed deps are outside the
         * workspace: editing them would rewrite a fetched checkout and
         * desynchronise tur.lock. prepareRename already refused when the
         * symbol could cross that boundary; not walking them is the other
         * half of the same rule. */
        if (!sp->path) continue;
        char dep_src[4200];
        snprintf(dep_src, sizeof(dep_src), "%s/%s/src", root, sp->path);
        walk_tur_files(dep_src, 0, workspace_visit, ws);
    }
    pkg_manifest_free(&m);
}

/* Is the module this symbol is defined in listed in the manifest's
 * `:exports`? Such a name is published surface: a fetched consumer we cannot
 * see -- and cannot edit -- may import it. */
static bool module_is_exported(const char *root, const char *module) {
    if (!module || !*module) return false;
    char manifest_path[4200];
    if (!pkg_resolve_manifest_path(root, manifest_path, sizeof(manifest_path)))
        return false;
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return false;
    bool found = false;
    size_t mlen = strlen(module);
    for (int i = 0; i < m.n_exports && !found; i++) {
        const char *e = m.exports[i];
        if (!e) continue;
        /* :exports entries are written as source paths ("src/frame/schema.tur")
         * or as bare module names; match either spelling. */
        const char *tail = strstr(e, module);
        if (!tail) continue;
        char after = tail[mlen];
        if (after == '\0' || after == '.') found = true;
    }
    pkg_manifest_free(&m);
    return found;
}

/* -------------------------------------------------------------------------
 * prepareRename -- the honesty valve
 * --------------------------------------------------------------------- */

/* Why a rename cannot proceed, or NULL when it can.
 *
 * Also fills `*root` with the enclosing spice root (empty when there is
 * none) so the caller does not walk up twice. */
static const char *rename_refusal(const NameRef *ref, char *root,
                                  size_t root_cap, char *module,
                                  size_t module_cap) {
    root[0] = '\0';
    module[0] = '\0';

    /* A truncated binding table cannot tell "not a local" from "a local we
     * ran out of room for", and those two answers call for opposite edits. */
    if (ref->doc->bindings_truncated)
        return "file too large to rename safely";

    if (ref->bind) {
        if (ref->bind->def_line == 0)
            return "cannot rename a macro-introduced binding";
        return NULL;   /* R1: a local, bounded by its own scope */
    }

    if (!ref->sym)
        return "cannot rename: no definition found for this name";

    if (!ref->doc->path || !ref->sym->file_path[0])
        return "cannot rename: this name has no known definition site";

    if (strcmp(ref->sym->file_path, ref->doc->path) != 0) {
        const char *sr = stdlib_root();
        if (sr && strncmp(ref->sym->file_path, sr, strlen(sr)) == 0)
            return "cannot rename stdlib symbol";
        return "cannot rename a symbol defined in another file "
               "-- rename it at its definition";
    }

    if (spice_root_of(ref->doc->path, root, root_cap)) {
        module_name_of(root, ref->doc->path, module, module_cap);
        if (!rename_exports_ && module_is_exported(root, module))
            return "renaming an exported symbol needs --rename-exports";
    }
    return NULL;
}

static void on_prepare_rename(const char *id_raw, size_t id_len,
                              const char *params, LspSink *sink) {
    NameRef ref;
    if (!resolve_name_at(params, &ref)) {
        /* Not on an identifier: `null`, which the client shows as "no rename
         * available here" rather than as an error. */
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    char root[4096], module[1024];
    const char *why = rename_refusal(&ref, root, sizeof(root),
                                     module, sizeof(module));
    if (why) {
        /* InvalidRequest. The spec's way of getting a reason in front of the
         * user before they type a new name, which is the entire argument for
         * prepareProvider over the bare boolean. */
        send_error(sink, id_raw, id_len, -32600, why);
        return;
    }

    size_t s, e;
    if (!lsp_ident_range_at(ref.doc->text, ref.doc->text_len, ref.off, &s, &e)) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }
    int l0, c0, l1, c1;
    offset_to_line_col(ref.doc->text, s, &l0, &c0);
    offset_to_line_col(ref.doc->text, e, &l1, &c1);

    Buf out;
    buf_init(&out);
    buf_printf(&out,
        "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}},\"placeholder\":",
        l0, c0, l1, c1);
    json_str(&out, ref.name);
    buf_putc(&out, '}');
    buf_putc(&out, '\0');
    send_response(sink, id_raw, id_len, out.data);
    buf_free(&out);
}

static void on_rename(const char *id_raw, size_t id_len,
                      const char *params, LspSink *sink) {
    NameRef ref;
    if (!resolve_name_at(params, &ref)) {
        send_error(sink, id_raw, id_len, -32600, "no identifier at position");
        return;
    }

    char new_name[128];
    if (lsp_json_str_copy(params, "newName", new_name, sizeof(new_name)) < 0 ||
        !new_name[0]) {
        send_error(sink, id_raw, id_len, -32602, "rename needs a newName");
        return;
    }

    char root[4096], module[1024];
    const char *why = rename_refusal(&ref, root, sizeof(root),
                                     module, sizeof(module));
    if (why) {
        /* A conforming client asked prepareRename first and already saw this.
         * One that did not still must not get an edit. */
        send_error(sink, id_raw, id_len, -32600, why);
        return;
    }

    Buf changes;
    buf_init(&changes);
    buf_puts(&changes, "{\"changes\":{");

    /* R1 -- this document. A local is bounded by its scope; a global defined
     * here is the whole buffer. Either way the bound comes from the resolver,
     * which is what makes this half completely safe. */
    Buf edits;
    buf_init(&edits);
    FileEditCtx fc = { &edits, 0, new_name, ref.uri, false };
    /* Scope filtering rides the predicate the highlight uses, so a rename
     * edits exactly the marks the user was just shown. */
    RenameScopeCtx rc = {
        &fc,
        { ref.bind, ref.doc->bindings, ref.doc->binding_count, ref.name },
        -1, -1, false
    };
    lsp_scan_occurrences(ref.doc->text, ref.doc->text_len, ref.name,
                         rename_scoped_emit, &rc);
    json_str(&changes, ref.uri);
    buf_putc(&changes, ':');
    buf_putc(&changes, '[');
    buf_write(&changes, edits.data, edits.len);
    buf_putc(&changes, ']');
    buf_free(&edits);

    /* R2 -- the rest of the workspace. Only a global reaches here: a local's
     * scope cannot leave its own file. */
    if (!ref.bind && root[0]) {
        WorkspaceCtx ws;
        memset(&ws, 0, sizeof(ws));
        ws.out           = &changes;
        ws.name          = ref.name;
        ws.new_name      = new_name;
        ws.skip_path     = ref.doc->path;
        ws.module        = module;
        ws.hits          = 1;   /* the current document already wrote a pair */
        ws.scratch_syms  = calloc((size_t)LSP_SYM_CAP, sizeof(LspSymbol));
        ws.scratch_binds = calloc((size_t)LSP_BIND_CAP, sizeof(LspBinding));
        workspace_scan(root, &ws);
        free(ws.scratch_syms);
        free(ws.scratch_binds);
        if (ws.overflowed || !ws.scratch_binds || ws.unanalyzable[0]) {
            /* A partial or unverified WorkspaceEdit is the one outcome worse
             * than none: it renames the definition and leaves some callers
             * behind, which does not even compile. */
            char msg[640];
            if (ws.overflowed)
                snprintf(msg, sizeof(msg),
                         "too many files import this module to rename safely");
            else if (!ws.scratch_binds)
                snprintf(msg, sizeof(msg),
                         "out of memory analyzing the workspace");
            else
                snprintf(msg, sizeof(msg),
                         "cannot rename: %s uses this name but does not "
                         "compile, so its own bindings are unknown",
                         ws.unanalyzable);
            buf_free(&changes);
            send_error(sink, id_raw, id_len, -32600, msg);
            return;
        }
    }

    buf_puts(&changes, "}}");
    buf_putc(&changes, '\0');
    send_response(sink, id_raw, id_len, changes.data);
    buf_free(&changes);
}

/* references is R2's file walk without the edit, which is why it lands in the
 * same place: implementing the workspace scan and then not exposing it would
 * be leaving the cheapest feature in the plan on the floor. */
static void on_references(const char *id_raw, size_t id_len,
                          const char *params, LspSink *sink) {
    NameRef ref;
    if (!resolve_name_at(params, &ref)) {
        send_response(sink, id_raw, id_len, "[]");
        return;
    }

    /* `includeDeclaration` is honored rather than ignored: the definition is
     * a reference when the client asks for it, and is the `kind 3` mark in
     * highlight terms when it does not. */
    bool include_decl = true;
    size_t ctx_len = 0, flag_len = 0;
    const char *ctx = lsp_json_raw(params, "context", &ctx_len);
    const char *flag = ctx ? lsp_json_raw(ctx, "includeDeclaration", &flag_len)
                           : NULL;
    if (flag && flag_len >= 5 && memcmp(flag, "false", 5) == 0)
        include_decl = false;

    int def_line = -1, def_col = -1;
    if (ref.bind && ref.bind->def_line > 0) {
        def_line = (int)ref.bind->def_line - 1;
        def_col  = ref.bind->def_col_start > 0
                     ? (int)ref.bind->def_col_start - 1 : 0;
    } else if (ref.sym && ref.doc->path && ref.sym->file_path[0] &&
               strcmp(ref.sym->file_path, ref.doc->path) == 0) {
        def_line = ref.sym->line > 0 ? ref.sym->line - 1 : 0;
        def_col  = ref.sym->col_start > 0 ? ref.sym->col_start - 1 : 0;
    }

    Buf out;
    buf_init(&out);
    buf_putc(&out, '[');

    FileEditCtx fc = { &out, 0, NULL, ref.uri, true };
    RenameScopeCtx rc = {
        &fc,
        { ref.bind, ref.doc->bindings, ref.doc->binding_count, ref.name },
        def_line, def_col, !include_decl
    };
    lsp_scan_occurrences(ref.doc->text, ref.doc->text_len, ref.name,
                         rename_scoped_emit, &rc);

    if (!ref.bind && ref.sym && ref.doc->path &&
        strcmp(ref.sym->file_path, ref.doc->path) == 0) {
        char root[4096], module[1024];
        module[0] = '\0';
        if (spice_root_of(ref.doc->path, root, sizeof(root))) {
            module_name_of(root, ref.doc->path, module, sizeof(module));
            WorkspaceCtx ws;
            memset(&ws, 0, sizeof(ws));
            ws.out           = &out;
            ws.name          = ref.name;
            ws.skip_path     = ref.doc->path;
            ws.module        = module;
            ws.as_locations  = true;
            ws.hits          = fc.emitted;
            ws.scratch_syms  = calloc((size_t)LSP_SYM_CAP, sizeof(LspSymbol));
            ws.scratch_binds = calloc((size_t)LSP_BIND_CAP, sizeof(LspBinding));
            /* Overflow is not an error here the way it is for rename: an
             * incomplete list of references is a smaller list, and a client
             * that gets one has still been told the truth about every entry
             * in it. Only an incomplete EDIT is unsafe. */
            workspace_scan(root, &ws);
            free(ws.scratch_syms);
            free(ws.scratch_binds);
        }
    }

    buf_putc(&out, ']');
    buf_putc(&out, '\0');
    send_response(sink, id_raw, id_len, out.data);
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * LD4a: textDocument/documentSymbol
 * --------------------------------------------------------------------- */

static void on_document_symbol(const char *id_raw, size_t id_len,
                                const char *params, LspSink *sink) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(sink, id_raw, id_len, "[]"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(sink, id_raw, id_len, "[]");
        return;
    }

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->symbols) {
        send_response(sink, id_raw, id_len, "[]");
        return;
    }

    Buf result;
    buf_init(&result);
    buf_putc(&result, '[');
    int emitted = 0;

    for (int i = 0; i < doc->symbol_count; i++) {
        const LspSymbol *sym = &doc->symbols[i];
        if (!sym->name[0]) continue;
        /* Only emit symbols defined in this file */
        if (sym->file_path[0] && strcmp(sym->file_path, doc->path) != 0)
            continue;

        int kind  = lsp_symbol_kind(sym);
        int line  = sym->line > 0 ? sym->line - 1 : 0;
        int col_s = sym->col_start > 0 ? sym->col_start - 1 : 0;
        int col_e = sym->col_end   > 0 ? sym->col_end   - 1 : col_s;

        if (emitted > 0) buf_putc(&result, ',');
        buf_puts(&result, "{\"name\":");
        json_str(&result, sym->name);
        buf_printf(&result, ",\"kind\":%d", kind);
        buf_printf(&result,
            ",\"range\":{"
              "\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}"
            "},\"selectionRange\":{"
              "\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}"
            "}}",
            line, col_s, line, col_e,
            line, col_s, line, col_e);
        emitted++;
    }

    buf_putc(&result, ']');
    buf_putc(&result, '\0');
    send_response(sink, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * LD4b: workspace/symbol
 * --------------------------------------------------------------------- */

typedef struct {
    const char *query;
    size_t       query_len;
    Buf         *result;
    int          count;
} WsCtx;

/* Case-insensitive substring search.  Returns 1 if haystack contains needle. */
static int ci_contains(const char *haystack, const char *needle, size_t nlen) {
    if (nlen == 0) return 1;
    for (const char *p = haystack; *p; p++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            char a = p[j], b = needle[j];
            if (!a) { match = 0; break; }
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static void ws_collect_cb(const LspDoc *doc, void *ctx_) {
    WsCtx *ctx = ctx_;
    for (int i = 0; i < doc->symbol_count; i++) {
        const LspSymbol *sym = &doc->symbols[i];
        if (!sym->name[0]) continue;
        /* Only symbols defined in this document */
        if (sym->file_path[0] && strcmp(sym->file_path, doc->path) != 0)
            continue;
        /* Query filter */
        if (ctx->query_len > 0 &&
            !ci_contains(sym->name, ctx->query, ctx->query_len))
            continue;

        char uri[1024];
        lsp_path_to_uri(sym->file_path[0] ? sym->file_path : doc->path,
                        uri, sizeof(uri));
        int kind  = lsp_symbol_kind(sym);
        int line  = sym->line > 0 ? sym->line - 1 : 0;
        int col_s = sym->col_start > 0 ? sym->col_start - 1 : 0;
        int col_e = sym->col_end   > 0 ? sym->col_end   - 1 : col_s;

        if (ctx->count > 0) buf_putc(ctx->result, ',');
        buf_puts(ctx->result, "{\"name\":");
        json_str(ctx->result, sym->name);
        buf_printf(ctx->result, ",\"kind\":%d,\"location\":{\"uri\":", kind);
        json_str(ctx->result, uri);
        buf_printf(ctx->result,
            ",\"range\":{"
              "\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}"
            "}}}",
            line, col_s, line, col_e);
        ctx->count++;
    }
}

static void on_workspace_symbol(const char *id_raw, size_t id_len,
                                 const char *params, LspSink *sink) {
    size_t  query_len = 0;
    const char *query_raw = lsp_json_str(params, "query", &query_len);

    Buf result;
    buf_init(&result);
    buf_putc(&result, '[');

    WsCtx ctx;
    ctx.query     = query_raw;
    ctx.query_len = query_raw ? query_len : 0;
    ctx.result    = &result;
    ctx.count     = 0;
    lsp_docs_iterate(ws_collect_cb, &ctx);

    buf_putc(&result, ']');
    buf_putc(&result, '\0');
    send_response(sink, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * textDocument/formatting
 *
 * `tur fmt` already existed and worked; it was simply not reachable over LSP,
 * so both known clients shelled out to the binary -- one of them with a
 * blocking subprocess call that could freeze the editor for seconds. Wiring
 * the in-process formatter to a handler removes that stall and deletes the
 * duplicated client code.
 * --------------------------------------------------------------------- */

static void on_formatting(const char *id_raw, size_t id_len,
                          const char *params, LspSink *sink) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(sink, id_raw, id_len, "null"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->text) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    /* FormattingOptions.tabSize / insertSpaces are deliberately ignored: the
     * formatter is not configurable (2-space indent, 80 columns), and honoring
     * the request halfway would produce output `tur fmt --check` then rejects.
     * One formatter, one answer. */

    /* Collect rather than print: a parse error here belongs in the response,
     * not on the server's stderr where it becomes editor log noise. Diagnostics
     * for the buffer come from run_doc_analysis on its own schedule. */
    diag_lsp_begin();
    Buf out;
    int rc = fmt_format_document(doc->path, doc->text, doc->text_len,
                                 reader_type_from_extension(doc->path), &out);
    diag_lsp_end();

    if (rc != 0) {
        /* Unformattable (almost always mid-edit unbalanced parens). Returning
         * null is the spec's "no edits", which leaves the buffer untouched --
         * the right answer, and better than a hard error the editor surfaces
         * as a popup on every save. */
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    /* One full-document TextEdit. The end position is the last line and its
     * length in bytes -- positionEncoding is utf-8, so no transcoding. */
    int    end_line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < doc->text_len; i++) {
        if (doc->text[i] == '\n') { end_line++; line_start = i + 1; }
    }
    int end_char = (int)(doc->text_len - line_start);

    Buf result;
    buf_init(&result);
    buf_printf(&result,
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                    "\"end\":{\"line\":%d,\"character\":%d}},\"newText\":",
        end_line, end_char);
    buf_putc(&out, '\0');
    json_str(&result, out.data);
    buf_puts(&result, "}]");
    buf_putc(&result, '\0');

    send_response(sink, id_raw, id_len, result.data);
    buf_free(&result);
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * textDocument/signatureHelp
 * --------------------------------------------------------------------- */

/* Split the parameter list out of a rendered function type.
 *
 * type_str looks like "(fn [int int] : int)". The bracketed run is the
 * parameter types in order; everything else is the return type. Parameter
 * *names* are not carried on LspSymbol, so the labels are the types --
 * which is what a caller needs to know at the point of the call anyway. */
static int signature_params(const char *type_str, Buf *out_labels) {
    const char *lb = strchr(type_str, '[');
    if (!lb) return 0;
    /* Find the matching ']' by depth, not the first one: a parameter can
     * itself be a function type, as in "(fn [(fn [int] : int) int] : int)",
     * and strchr would stop inside it. */
    const char *rb = NULL;
    int d = 0;
    for (const char *q = lb; *q; q++) {
        if (*q == '[' || *q == '(') d++;
        else if (*q == ']' || *q == ')') {
            if (--d == 0) { rb = q; break; }
        }
    }
    if (!rb || *rb != ']') return 0;

    int count = 0;
    const char *p = lb + 1;
    while (p < rb) {
        while (p < rb && (*p == ' ' || *p == '\t')) p++;
        if (p >= rb) break;
        const char *tok = p;
        int depth = 0;
        while (p < rb) {
            char c = *p;
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') depth--;
            else if (depth == 0 && (c == ' ' || c == '\t')) break;
            p++;
        }
        if (p == tok) break;
        if (count > 0) buf_putc(out_labels, ',');
        buf_puts(out_labels, "{\"label\":");
        Buf lbl;
        buf_init(&lbl);
        buf_write(&lbl, tok, (size_t)(p - tok));
        buf_putc(&lbl, '\0');
        json_str(out_labels, lbl.data);
        buf_free(&lbl);
        buf_putc(out_labels, '}');
        count++;
    }
    return count;
}

static void on_signature_help(const char *id_raw, size_t id_len,
                              const char *params, LspSink *sink) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(sink, id_raw, id_len, "null"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { send_response(sink, id_raw, id_len, "null"); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    /* The call position comes from the text alone, so it is found before
     * deciding whether the index is good enough -- most requests from a
     * space trigger are not in argument position and end here, at no cost. */
    size_t cursor = lsp_offset_at_pos(doc->text, doc->text_len,
                                      line_0 + 1, char_0 + 1);
    char callee[128];
    int  active = 0;
    if (!lsp_enclosing_call(doc->text, doc->text_len, cursor,
                            callee, sizeof(callee), &active)) {
        send_response(sink, id_raw, id_len, "null");
        return;
    }

    /* Answer from the index as it stands when it knows the callee; analyze
     * the pending edit only when it does not (a function defined since the
     * last analysis, or a document never analyzed). */
    const LspSymbol *sym = find_symbol(doc, callee);
    if (!sym && doc->dirty) {
        lsp_flush_dirty(sink);
        sym = find_symbol(doc, callee);
    }

    /* Same builtin fallback as on_hover: `(println ` and `(+ ` are the calls
     * being typed when signature help is most wanted, and neither has a
     * Binding to find. A synthetic LspSymbol keeps the rendering below on one
     * path -- signature_params() reads type_str, so the first rendered
     * overload has to be a type_str for the parameter labels to come out. */
    LspSymbol builtin_sym;
    if (!sym) {
        char desc[512];
        if (builtin_describe(callee, desc, sizeof(desc)) > 0) {
            memset(&builtin_sym, 0, sizeof(builtin_sym));
            snprintf(builtin_sym.name, sizeof(builtin_sym.name), "%s", callee);
            /* desc's first line is "(name : (fn [...] : R))". Lift the type
             * out from after " : " up to the closing paren. */
            char *open = strstr(desc, " : ");
            char *nl   = strchr(desc, '\n');
            if (nl) *nl = '\0';
            size_t dlen = strlen(desc);
            if (open && dlen > 0 && desc[dlen - 1] == ')') {
                desc[dlen - 1] = '\0';
                snprintf(builtin_sym.type_str, sizeof(builtin_sym.type_str),
                         "%s", open + 3);
            }
            snprintf(builtin_sym.doc, sizeof(builtin_sym.doc),
                     "built-in operator");
            builtin_sym.kind = LSP_KIND_FUNCTION;
            sym = &builtin_sym;
        }
    }
    if (!sym) { send_response(sink, id_raw, id_len, "null"); return; }

    Buf plabels;
    buf_init(&plabels);
    int nparams = signature_params(sym->type_str, &plabels);
    buf_putc(&plabels, '\0');

    /* Clamp rather than drop: a variadic or over-applied call still wants the
     * signature shown, just with no parameter highlighted past the end. */
    if (nparams > 0 && active >= nparams) active = nparams - 1;
    if (active < 0) active = 0;

    Buf label;
    buf_init(&label);
    buf_putc(&label, '(');
    buf_puts(&label, sym->name);
    if (sym->type_str[0]) {
        buf_puts(&label, " : ");
        buf_puts(&label, sym->type_str);
    }
    buf_putc(&label, ')');
    buf_putc(&label, '\0');

    Buf result;
    buf_init(&result);
    buf_puts(&result, "{\"signatures\":[{\"label\":");
    json_str(&result, label.data);
    if (sym->doc[0]) {
        buf_puts(&result, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
        json_str(&result, sym->doc);
        buf_puts(&result, "}");
    }
    buf_puts(&result, ",\"parameters\":[");
    buf_puts(&result, plabels.data);
    buf_printf(&result, "],\"activeParameter\":%d}],", active);
    buf_printf(&result, "\"activeSignature\":0,\"activeParameter\":%d}", active);
    buf_putc(&result, '\0');

    send_response(sink, id_raw, id_len, result.data);
    buf_free(&result);
    buf_free(&label);
    buf_free(&plabels);
}

/* -------------------------------------------------------------------------
 * LD4: textDocument/completion
 * --------------------------------------------------------------------- */

/* Known stdlib module names for import-path completion */
static const char *stdlib_modules[] = {
    "stdlib/args",
    "stdlib/contract",
    "stdlib/fix",
    "stdlib/free",
    "stdlib/hamt",
    "stdlib/list",
    "stdlib/macros",
    "stdlib/map",
    "stdlib/option",
    "stdlib/pair",
    "stdlib/result",
    "stdlib/safe",
    "stdlib/str",
    "stdlib/vec",
    NULL
};

/* Case-insensitive "does `name` start with `prefix`". */
static int ci_starts_with(const char *name, const char *prefix, size_t plen) {
    for (size_t j = 0; j < plen; j++) {
        char a = name[j], b = prefix[j];
        if (!a) return 0;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* How many completion items one response will carry. The cap exists to keep
 * the payload bounded, not to rank -- see the two-pass emit below for why
 * that distinction matters. */
#define LSP_COMPLETION_MAX 200

/* LSP CompletionItemKind -- a different enum from SymbolKind, for the same
 * distinctions. A type completes as Struct/Enum so the menu shows a type icon
 * beside `Point`, which is what tells a reader it is not a function to call. */
static int lsp_completion_kind(const LspSymbol *sym) {
    switch (sym->kind) {
        case LSP_KIND_FUNCTION: return 3;   /* Function */
        case LSP_KIND_VALUE:    return 6;   /* Variable */
        case LSP_KIND_STRUCT:   return 22;  /* Struct   */
        case LSP_KIND_ENUM:     return 13;  /* Enum     */
        case LSP_KIND_UNKNOWN:  break;
    }
    return (strncmp(sym->type_str, "(fn", 3) == 0) ? 3 : 6;
}

static void emit_completion_item(Buf *result, const LspSymbol *sym, int *emitted) {
    int kind = lsp_completion_kind(sym);

    if (*emitted > 0) buf_putc(result, ',');
    buf_puts(result, "{\"label\":");
    json_str(result, sym->name);
    buf_printf(result, ",\"kind\":%d", kind);
    if (sym->type_str[0]) {
        buf_puts(result, ",\"detail\":");
        json_str(result, sym->type_str);
    }
    if (sym->doc[0]) {
        buf_puts(result,
            ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
        json_str(result, sym->doc);
        buf_puts(result, "}");
    }
    buf_putc(result, '}');
    (*emitted)++;
}

static void on_completion(const char *id_raw, size_t id_len,
                          const char *params, LspSink *sink) {
    static const char *const EMPTY_LIST =
        "{\"isIncomplete\":false,\"items\":[]}";

    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(sink, id_raw, id_len, EMPTY_LIST); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(sink, id_raw, id_len, EMPTY_LIST);
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { send_response(sink, id_raw, id_len, EMPTY_LIST); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc) { send_response(sink, id_raw, id_len, EMPTY_LIST); return; }

    /* The prefix is what has been typed *before* the cursor. Reading the word
     * *at* the cursor (which is what lsp_word_at_pos does, stepping right when
     * the cursor is not on an identifier character) made a request at the very
     * start of `(defn foo ...)` come back with the prefix "defn" and filter
     * every candidate away. */
    char prefix[128] = {0};
    size_t prefix_len = lsp_prefix_at_pos(doc->text, doc->text_len,
                                          line_0 + 1, char_0 + 1,
                                          prefix, sizeof(prefix));

    size_t cursor_off = lsp_offset_at_pos(doc->text, doc->text_len,
                                          line_0 + 1, char_0 + 1);

    /* Check if cursor is inside an (import ...) form for module completion. */
    int in_import = 0;
    if (doc->text && cursor_off >= 7) {
        size_t lo = cursor_off > 64 ? cursor_off - 64 : 0;
        for (size_t s = cursor_off - 7; ; s--) {
            if (memcmp(doc->text + s, "(import", 7) == 0) { in_import = 1; break; }
            if (s == lo) break;
        }
    }

    Buf result;
    buf_init(&result);
    Buf items;
    buf_init(&items);

    int emitted   = 0;
    int truncated = 0;

    /* Import path completions */
    if (in_import) {
        for (int i = 0; stdlib_modules[i]; i++) {
            const char *mod = stdlib_modules[i];
            if (prefix_len > 0 && !ci_starts_with(mod, prefix, prefix_len))
                continue;
            if (emitted > 0) buf_putc(&items, ',');
            buf_puts(&items, "{\"label\":");
            json_str(&items, mod);
            buf_puts(&items, ",\"kind\":9}");
            emitted++;
        }
    }

    /* Symbol completions, document-local first.
     *
     * The cap used to truncate in raw collection order, which put ~200 stdlib
     * entries in front of the buffer's own definitions -- the symbols the user
     * is most likely reaching for were the ones that got dropped. Two passes
     * fix the ordering without needing a full sort. */
    const LspSymbol *syms;
    int              sym_count;
    doc_symbol_view(doc, &syms, &sym_count);

    if (!in_import && syms) {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < sym_count; i++) {
                const LspSymbol *sym = &syms[i];
                if (!sym->name[0]) continue;

                int local = (sym->file_path[0] == '\0' ||
                             strcmp(sym->file_path, doc->path) == 0);
                if ((pass == 0) != (local != 0)) continue;

                if (prefix_len > 0 &&
                    !ci_starts_with(sym->name, prefix, prefix_len))
                    continue;

                if (emitted >= LSP_COMPLETION_MAX) { truncated = 1; break; }
                emit_completion_item(&items, sym, &emitted);
            }
            if (truncated) break;
        }
    }

    buf_putc(&items, '\0');

    /* A CompletionList rather than a bare CompletionItem[]. Both are legal,
     * but the asymmetry forced every client to carry a both-shapes branch, and
     * only the list shape can say `isIncomplete` -- which is how the client
     * learns to re-query as the prefix narrows instead of showing a silently
     * truncated menu. */
    buf_printf(&result, "{\"isIncomplete\":%s,\"items\":[",
               truncated ? "true" : "false");
    buf_puts(&result, items.data);
    buf_puts(&result, "]}");
    buf_putc(&result, '\0'); /* NUL-terminate for strlen in send_response */
    send_response(sink, id_raw, id_len, result.data);
    buf_free(&items);
    buf_free(&result);
}


/* -------------------------------------------------------------------------
 * Dispatch
 *
 * The `if (strcmp(method, ...))` chain below used to live inside
 * lsp_server_run's read loop, which welded every handler to a pair of file
 * descriptors. It is lifted out unchanged so the same dispatch can be driven
 * from a transport that has no descriptors at all -- a browser, where the
 * message arrives as a JS string and the replies go back as a JSON array
 * (see lsp_session.c).
 *
 * `fd_in` is the one remaining transport-specific input: it is the descriptor
 * the client's *next* message would arrive on, used to peek for a
 * $/cancelRequest while a slow analysis is running. A caller with nothing to
 * peek at passes -1 and gets the already-drained view -- which is the honest
 * answer in the browser, where the client's own event loop decides what to
 * send and when.
 * --------------------------------------------------------------------- */

/* lsp_docs_init() zeroes the table rather than freeing it, so calling it a
 * second time on a live session would strand every open document. */
static bool docs_ready_ = false;

void lsp_dispatch_init(void) {
    if (docs_ready_) return;
    lsp_docs_init();
    docs_ready_ = true;
}

void lsp_dispatch_flush(LspSink *sink) {
    lsp_flush_dirty(sink);
}

void lsp_dispatch_teardown(void) {
    queue_free();
    stdlib_cache_free();
    if (docs_ready_) {
        lsp_docs_free();
        docs_ready_ = false;
    }
    initialized_    = false;
    shutdown_       = false;
    hover_markdown_ = true;
    cancelled_n_    = 0;
}

/* Handle exactly one message. Returns false when the client asked to exit --
 * the caller decides whether that means tearing the process down or just this
 * session. `msg` stays owned by the caller. */
bool lsp_dispatch_message(const char *msg, LspSink *sink, int fd_in) {
    lsp_dispatch_init();

    size_t method_len;
    const char *method_raw = lsp_json_str(msg, "method", &method_len);
    if (!method_raw) return true;

    char method[128];
    size_t copy = method_len < sizeof(method) - 1 ? method_len : sizeof(method) - 1;
    memcpy(method, method_raw, copy);
    method[copy] = '\0';

    size_t id_len = 0;
    const char *id_raw = lsp_json_raw(msg, "id", &id_len);

    size_t params_len = 0;
    const char *params_raw = lsp_json_raw(msg, "params", &params_len);

    if (strcmp(method, "initialize") == 0) {
        on_initialize(id_raw, id_len, params_raw, sink);
    } else if (strcmp(method, "initialized") == 0) {
        /* no-op */
    } else if (strcmp(method, "$/cancelRequest") == 0) {
        /* Arrived on the main path rather than in a drain -- the request
         * it names is already answered, so there is nothing to abandon.
         * Recording it anyway would strand the id and reject a later
         * request that reuses the number. */
    } else if (strcmp(method, "shutdown") == 0) {
        on_shutdown(id_raw, id_len, sink);
    } else if (strcmp(method, "exit") == 0) {
        return false;
    } else if (strcmp(method, "textDocument/didOpen") == 0 && params_raw) {
        on_did_open(params_raw, params_len, sink);
    } else if (strcmp(method, "textDocument/didChange") == 0 && params_raw) {
        on_did_change(params_raw, params_len, sink);
    } else if (strcmp(method, "textDocument/didClose") == 0 && params_raw) {
        on_did_close(params_raw, params_len);
    } else if (strcmp(method, "textDocument/formatting") == 0) {
        /* Formatting reads doc->text, not doc->symbols, so it does not
         * need the analysis flush -- and skipping it keeps save-time
         * formatting off the slow path. */
        if (!params_raw)
            send_error(sink, id_raw, id_len, -32602, "Invalid params");
        else
            on_formatting(id_raw, id_len, params_raw, sink);
    } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
        /* Not in the flush group below: signature help reads doc->text,
         * which didChange updates at once, and needs from doc->symbols only
         * the callee's type -- which an index one edit old almost always
         * still has.  on_signature_help flushes itself when it does not.
         * Keeping the compile off this path is what makes a space trigger
         * affordable (signature-help-declines-at-its-own-trigger-character). */
        if (cancel_take(id_raw, id_len)) {
            send_error(sink, id_raw, id_len, -32800, "Request cancelled");
        } else if (!params_raw) {
            send_error(sink, id_raw, id_len, -32602, "Invalid params");
        } else {
            on_signature_help(id_raw, id_len, params_raw, sink);
        }
    } else if (strcmp(method, "textDocument/hover") == 0 ||
               strcmp(method, "textDocument/definition") == 0 ||
               strcmp(method, "textDocument/documentSymbol") == 0 ||
               strcmp(method, "textDocument/documentHighlight") == 0 ||
               strcmp(method, "textDocument/prepareRename") == 0 ||
               strcmp(method, "textDocument/rename") == 0 ||
               strcmp(method, "textDocument/references") == 0 ||
               strcmp(method, "workspace/symbol") == 0 ||
               strcmp(method, "textDocument/completion") == 0) {
        /* These all read doc->symbols, so any pending edit has to be
         * analyzed first — otherwise the answer describes the buffer as it
         * was before the user's last keystroke. This is what keeps the
         * debounce invisible to correctness: it delays work, never skips
         * it. */
        lsp_flush_dirty(sink);
        /* The flush is the one step slow enough for the client to change
         * its mind during. Read what it sent meanwhile before committing
         * to the rest of the work. A transport with no peekable descriptor
         * skips this: there is no socket to look at, and the client's own
         * scheduler already decided this request was still wanted. */
        if (fd_in >= 0) drain_pending(fd_in);
        if (cancel_take(id_raw, id_len)) {
            send_error(sink, id_raw, id_len, -32800, "Request cancelled");
        } else if (!params_raw) {
            send_error(sink, id_raw, id_len, -32602, "Invalid params");
        } else if (strcmp(method, "textDocument/hover") == 0) {
            on_hover(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "textDocument/definition") == 0) {
            on_definition(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
            on_document_symbol(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "textDocument/documentHighlight") == 0) {
            on_document_highlight(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "textDocument/prepareRename") == 0) {
            on_prepare_rename(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "textDocument/rename") == 0) {
            on_rename(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "textDocument/references") == 0) {
            on_references(id_raw, id_len, params_raw, sink);
        } else if (strcmp(method, "workspace/symbol") == 0) {
            on_workspace_symbol(id_raw, id_len, params_raw, sink);
        } else {
            on_completion(id_raw, id_len, params_raw, sink);
        }
    } else if (id_raw) {
        send_error(sink, id_raw, id_len, -32601, "Method not found");
    }

    return true;
}

/* True when the client completed the shutdown handshake before exiting.
 * Only the stdio loop cares: it is the difference between exit status 0 and 1. */
bool lsp_dispatch_shutdown_seen(void) {
    return shutdown_;
}

/* -------------------------------------------------------------------------
 * Main loop -- the stdio transport
 * --------------------------------------------------------------------- */

void lsp_server_run(int fd_in, int fd_out) {
    LspSink sink = lsp_sink_fd(fd_out);
    lsp_dispatch_init();

    for (;;) {
        /* Nothing new arrived while a document was waiting to be analyzed —
         * the client has stopped typing, so do the work now. A non-empty queue
         * means messages are already in hand, which is the opposite of quiet.
         *
         * This debounce is inherently stdio: it is a poll() on the input
         * descriptor. A browser client owns its own quiet-typing window (the
         * adapter's trailing debounce on didChange), which is why the session
         * entry point has no equivalent. */
        if (queue_.count == 0 && lsp_docs_any_dirty() &&
            !input_pending(fd_in, LSP_ANALYSIS_DEBOUNCE_MS)) {
            lsp_flush_dirty(&sink);
            continue;
        }

        char *msg = queue_pop();
        if (!msg) msg = lsp_read_message(fd_in);
        if (!msg) break;

        bool keep_going = lsp_dispatch_message(msg, &sink, fd_in);
        free(msg);

        if (!keep_going) {
            /* `exit` on stdio means the editor is gone; the process has
             * nothing left to serve. _exit rather than return so a client
             * that never sent `shutdown` still gets the non-zero status the
             * spec asks for, without unwinding through atexit handlers. */
            int status = lsp_dispatch_shutdown_seen() ? 0 : 1;
            lsp_dispatch_teardown();
            _exit(status);
        }
    }

    lsp_dispatch_teardown();
}
