/* mcp.c -- MCP (Model Context Protocol) server for tur.
 *
 * Implements the MCP 2024-11-05 protocol over JSON-RPC 2.0 / stdio.
 * Start with:  tur mcp
 *
 * Supported tools:
 *   check_file   -- type-check a .tur file; return diagnostics JSON
 *   symbols      -- list all symbols defined in a .tur file
 *   hover        -- type + docstring for symbol at cursor position
 *   definition   -- source location of a symbol definition
 *   complete     -- completion candidates at cursor position
 *   doc          -- ;;; docstring for a named symbol
 *   format       -- format a .tur file via `tur format`
 *   build        -- compile a project directory via `tur build`
 */

#include "mcp.h"
#include "lsp_json.h"
#include "lsp_docs.h"
#include "lsp_sym.h"
#include "lsp_util.h"

#include "buf.h"
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#  include <fcntl.h>    /* _O_BINARY */
#  include <io.h>       /* _pipe, _dup, _dup2, _close, _read */
#  include <process.h>  /* _spawnvp, _cwait */
#else
#  include <sys/wait.h>
#endif
#include <dirent.h>

/* -------------------------------------------------------------------------
 * MCP stdio transport (MCP 2024-11-05 spec: newline-delimited JSON-RPC)
 *
 * Each message is a single UTF-8 JSON object terminated by exactly one '\n'.
 * This differs from the LSP transport used by `tur lsp`, which uses
 * Content-Length headers.  MCP clients (opencode, Claude Code, VS Code
 * Copilot in agent mode, etc.) all implement the newline-delimited variant.
 * --------------------------------------------------------------------- */

/* Read one newline-terminated message from fd_in.
 * Returns a heap-allocated NUL-terminated JSON string, or NULL on EOF/error.
 * Blank lines (e.g. bare \r\n from HTTP-style clients) are silently skipped. */
static char *mcp_read_message(int fd_in) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        char c;
        ssize_t n = read(fd_in, &c, 1);
        if (n <= 0) {
            /* EOF */
            if (len == 0) { free(buf); return NULL; }
            break;
        }
        if (c == '\n') {
            if (len == 0) continue;  /* skip blank lines */
            break;
        }
        if (c == '\r') continue;     /* strip CR from CRLF sequences */
        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return buf;
}

/* Write one message to fd_out, terminated by a single '\n'. */
static void mcp_write_message(int fd_out, const char *json, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd_out, json + written, len - written);
        if (n <= 0) return;
        written += (size_t)n;
    }
    ssize_t nl = write(fd_out, "\n", 1);
    (void)nl;
}

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif

#define MCP_SYM_CAP 2048

/* -------------------------------------------------------------------------
 * JSON write helpers
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

static void send_response(int fd_out, const char *id_raw, size_t id_len,
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
    mcp_write_message(fd_out, b.data, b.len);
    buf_free(&b);
}

/* Wrap tool output text in the standard MCP content response. */
static void send_tool_result(int fd_out, const char *id_raw, size_t id_len,
                             const char *text, int is_error) {
    Buf r;
    buf_init(&r);
    buf_puts(&r, "{\"content\":[{\"type\":\"text\",\"text\":");
    json_str(&r, text ? text : "");
    buf_puts(&r, "}],\"isError\":");
    buf_puts(&r, is_error ? "true" : "false");
    buf_puts(&r, "}");
    buf_putc(&r, '\0');
    send_response(fd_out, id_raw, id_len, r.data);
    buf_free(&r);
}

static void send_error_response(int fd_out, const char *id_raw, size_t id_len,
                                int code, const char *msg) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && id_len > 0)
        buf_write(&b, id_raw, id_len);
    else
        buf_puts(&b, "null");
    buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    json_str(&b, msg);
    buf_puts(&b, "}}");
    mcp_write_message(fd_out, b.data, b.len);
    buf_free(&b);
}

/* -------------------------------------------------------------------------
 * Subprocess helper (safe fork/exec -- no shell interpolation)
 * --------------------------------------------------------------------- */

#ifdef _WIN32
/*
 * Windows has no fork, so the redirection has to happen in the parent: point
 * stdout/stderr at the pipe, spawn (the child inherits them), then put the
 * parent's own descriptors back.  _spawnvp is execvp+fork in one call.
 *
 * The write end is closed in the parent immediately after the spawn, so that
 * the read loop below sees EOF once the child exits and its inherited copies
 * are closed.  Without that, the read would block forever on a pipe that still
 * has a live writer -- us.
 */
static int run_subprocess(const char *cmd, char *const argv[], Buf *out) {
    int pfd[2];
    if (_pipe(pfd, 65536, _O_BINARY) != 0) return -1;

    int saved_out = _dup(1);
    int saved_err = _dup(2);
    if (saved_out < 0 || saved_err < 0) {
        _close(pfd[0]); _close(pfd[1]);
        if (saved_out >= 0) _close(saved_out);
        if (saved_err >= 0) _close(saved_err);
        return -1;
    }

    fflush(stdout);
    fflush(stderr);
    _dup2(pfd[1], 1);
    _dup2(pfd[1], 2);

    intptr_t child = _spawnvp(_P_NOWAIT, cmd, (const char *const *)argv);

    /* Restore ours before touching the pipe, so a failure below still leaves
     * this process with working stdout/stderr. */
    _dup2(saved_out, 1); _close(saved_out);
    _dup2(saved_err, 2); _close(saved_err);
    _close(pfd[1]);

    if (child == -1) {
        _close(pfd[0]);
        return -1;
    }

    char tmp[4096];
    int  n;
    while ((n = _read(pfd[0], tmp, sizeof(tmp))) > 0)
        buf_write(out, tmp, (size_t)n);
    _close(pfd[0]);

    int status = 0;
    if (_cwait(&status, child, 0) == -1) return -1;
    /* _cwait yields the exit code directly -- no POSIX status word to unpack. */
    return status;
}
#else
static int run_subprocess(const char *cmd, char *const argv[], Buf *out) {
    int pfd[2];
    if (pipe(pfd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp(cmd, argv);
        _exit(127);
    }

    close(pfd[1]);
    char tmp[4096];
    ssize_t n;
    while ((n = read(pfd[0], tmp, sizeof(tmp))) > 0)
        buf_write(out, tmp, (size_t)n);
    close(pfd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif /* _WIN32 */

/* -------------------------------------------------------------------------
 * Per-request file analysis
 * --------------------------------------------------------------------- */

typedef struct {
    LspSymbol  syms[MCP_SYM_CAP];
    int        sym_count;
    LspDocTable dtable;
    char       *text;
    size_t      text_len;
} McpAnalysis;

static void mcp_analysis_free(McpAnalysis *a) {
    lsp_doc_table_free(&a->dtable);
    free(a->text);
    a->text = NULL;
}

static int mcp_load_and_analyze(const char *path, McpAnalysis *a) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return -1; }

    a->text = malloc((size_t)sz + 1);
    if (!a->text) { fclose(f); return -1; }
    size_t rd = fread(a->text, 1, (size_t)sz, f);
    a->text[rd] = '\0';
    a->text_len = rd;
    fclose(f);

    lsp_doc_table_init(&a->dtable);
    lsp_scan_docs(a->text, a->text_len, &a->dtable);

    diag_reset();
    diag_init(false);
    diag_lsp_begin();
    tur_collect_symbols(path, NULL, a->syms, MCP_SYM_CAP, &a->sym_count);
    diag_lsp_end();

    for (int i = 0; i < a->sym_count; i++) {
        lsp_scan_docs_lookup(&a->dtable, a->syms[i].name,
                             a->syms[i].doc, sizeof(a->syms[i].doc));
    }
    return 0;
}

static const LspSymbol *mcp_find_symbol(const McpAnalysis *a, const char *name) {
    for (int i = 0; i < a->sym_count; i++)
        if (strcmp(a->syms[i].name, name) == 0)
            return &a->syms[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * Tool implementations
 * --------------------------------------------------------------------- */

static void mcp_tool_check_file(const char *args, Buf *out, int *is_error) {
    char path[1024];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }

    diag_reset();
    diag_init(false);
    diag_lsp_begin();
    tur_check_only(path);
    diag_lsp_flush_array(out);
    diag_lsp_end();
}

static void mcp_tool_symbols(const char *args, Buf *out, int *is_error) {
    char path[1024];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }

    McpAnalysis *a = calloc(1, sizeof(McpAnalysis));
    if (!a) { buf_puts(out, "Out of memory"); *is_error = 1; return; }
    if (mcp_load_and_analyze(path, a) < 0) {
        buf_printf(out, "Could not read or analyze: %s", path);
        *is_error = 1;
        free(a);
        return;
    }

    buf_putc(out, '[');
    int count = 0;
    for (int i = 0; i < a->sym_count; i++) {
        const LspSymbol *sym = &a->syms[i];
        if (!sym->name[0]) continue;
        if (count > 0) buf_putc(out, ',');
        buf_puts(out, "{\"name\":");
        json_str(out, sym->name);
        buf_puts(out, ",\"type\":");
        json_str(out, sym->type_str);
        buf_puts(out, ",\"doc\":");
        json_str(out, sym->doc);
        buf_puts(out, ",\"file\":");
        json_str(out, sym->file_path);
        buf_printf(out, ",\"line\":%d,\"col\":%d}",
                   sym->line, sym->col_start);
        count++;
    }
    buf_putc(out, ']');

    mcp_analysis_free(a);
    free(a);
}

static void mcp_tool_hover(const char *args, Buf *out, int *is_error) {
    char path[1024];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }
    int64_t line_0 = lsp_json_int(args, "line");
    int64_t char_0 = lsp_json_int(args, "col");
    if (line_0 < 0 || char_0 < 0) {
        buf_puts(out, "Missing required arguments: line, col (0-based)");
        *is_error = 1;
        return;
    }

    McpAnalysis *a = calloc(1, sizeof(McpAnalysis));
    if (!a) { buf_puts(out, "Out of memory"); *is_error = 1; return; }
    if (mcp_load_and_analyze(path, a) < 0) {
        buf_printf(out, "Could not read or analyze: %s", path);
        *is_error = 1;
        free(a);
        return;
    }

    char name[128];
    if (!lsp_word_at_pos(a->text, a->text_len,
                         (int)line_0 + 1, (int)char_0 + 1,
                         name, sizeof(name))) {
        buf_puts(out, "No symbol at position");
        mcp_analysis_free(a);
        free(a);
        return;
    }

    const LspSymbol *sym = mcp_find_symbol(a, name);
    if (!sym) {
        buf_printf(out, "No type information for '%s'", name);
    } else {
        buf_puts(out, sym->name);
        if (sym->type_str[0]) {
            buf_puts(out, " : ");
            buf_puts(out, sym->type_str);
        }
        if (sym->doc[0]) {
            buf_puts(out, "\n\n");
            buf_puts(out, sym->doc);
        }
    }

    mcp_analysis_free(a);
    free(a);
}

static void mcp_tool_definition(const char *args, Buf *out, int *is_error) {
    char path[1024];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }
    int64_t line_0 = lsp_json_int(args, "line");
    int64_t char_0 = lsp_json_int(args, "col");
    if (line_0 < 0 || char_0 < 0) {
        buf_puts(out, "Missing required arguments: line, col (0-based)");
        *is_error = 1;
        return;
    }

    McpAnalysis *a = calloc(1, sizeof(McpAnalysis));
    if (!a) { buf_puts(out, "Out of memory"); *is_error = 1; return; }
    if (mcp_load_and_analyze(path, a) < 0) {
        buf_printf(out, "Could not read or analyze: %s", path);
        *is_error = 1;
        free(a);
        return;
    }

    char name[128];
    if (!lsp_word_at_pos(a->text, a->text_len,
                         (int)line_0 + 1, (int)char_0 + 1,
                         name, sizeof(name))) {
        buf_puts(out, "No symbol at position");
        mcp_analysis_free(a);
        free(a);
        return;
    }

    const LspSymbol *sym = mcp_find_symbol(a, name);
    if (!sym || !sym->file_path[0]) {
        buf_printf(out, "No definition found for '%s'", name);
        mcp_analysis_free(a);
        free(a);
        return;
    }

    /* Return as JSON: { file, line, col } (1-based, matching LspSymbol) */
    buf_puts(out, "{\"file\":");
    json_str(out, sym->file_path);
    buf_printf(out, ",\"line\":%d,\"col\":%d}", sym->line, sym->col_start);

    mcp_analysis_free(a);
    free(a);
}

/* Cached list of stdlib module names ("stdlib/<basename>"), discovered on
 * first use by scanning the resolved stdlib directory.  Re-scans nothing
 * after the first call -- the stdlib set is process-static.  Returns a
 * NULL-terminated array; on any I/O failure returns an empty array so the
 * caller still emits a well-formed JSON response. */
static const char **mcp_stdlib_modules(void) {
    static const char **cached = NULL;
    static int          attempted = 0;
    if (attempted) return cached;
    attempted = 1;

    const char *dir = getenv("TUR_STDLIB_DIR");
    if (!dir || !*dir) dir = "stdlib";

    DIR *d = opendir(dir);
    if (!d) {
        static const char *empty[] = { NULL };
        cached = empty;
        return cached;
    }

    /* Two passes: first count, then collect, so we can size the array. */
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len <= 4) continue;
        if (strcmp(n + len - 4, ".tur") != 0) continue;
        if (n[0] == '.') continue;
        count++;
    }
    rewinddir(d);

    const char **arr = calloc((size_t)count + 1, sizeof(char *));
    if (!arr) {
        closedir(d);
        static const char *empty[] = { NULL };
        cached = empty;
        return cached;
    }

    int i = 0;
    while ((e = readdir(d)) != NULL && i < count) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len <= 4) continue;
        if (strcmp(n + len - 4, ".tur") != 0) continue;
        if (n[0] == '.') continue;
        size_t base_len = len - 4;
        char *mod = malloc(7 + base_len + 1);  /* "stdlib/" + base + NUL */
        if (!mod) continue;
        memcpy(mod, "stdlib/", 7);
        memcpy(mod + 7, n, base_len);
        mod[7 + base_len] = '\0';
        arr[i++] = mod;
    }
    arr[i] = NULL;
    closedir(d);

    /* Sort alphabetically for stable completion ordering. */
    for (int a = 0; a < i - 1; a++) {
        for (int b = a + 1; b < i; b++) {
            if (strcmp(arr[a], arr[b]) > 0) {
                const char *t = arr[a];
                arr[a] = arr[b];
                arr[b] = t;
            }
        }
    }

    cached = arr;
    return cached;
}

static void mcp_tool_complete(const char *args, Buf *out, int *is_error) {
    char path[1024];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }
    int64_t line_0 = lsp_json_int(args, "line");
    int64_t char_0 = lsp_json_int(args, "col");
    if (line_0 < 0 || char_0 < 0) {
        buf_puts(out, "Missing required arguments: line, col (0-based)");
        *is_error = 1;
        return;
    }

    McpAnalysis *a = calloc(1, sizeof(McpAnalysis));
    if (!a) { buf_puts(out, "Out of memory"); *is_error = 1; return; }
    if (mcp_load_and_analyze(path, a) < 0) {
        buf_printf(out, "Could not read or analyze: %s", path);
        *is_error = 1;
        free(a);
        return;
    }

    char prefix[128] = {0};
    lsp_word_at_pos(a->text, a->text_len,
                    (int)line_0 + 1, (int)char_0 + 1,
                    prefix, sizeof(prefix));
    size_t prefix_len = strlen(prefix);

    /* Detect (import context */
    int in_import = 0;
    if (a->text && a->text_len > 0) {
        const char *p = a->text;
        int cur_line = 1;
        while (p < a->text + a->text_len && cur_line < (int)line_0 + 1) {
            if (*p == '\n') cur_line++;
            p++;
        }
        int cur_col = 1;
        while (p < a->text + a->text_len && *p != '\n' &&
               cur_col < (int)char_0 + 1) {
            p++;
            cur_col++;
        }
        const char *scan = p > a->text + 64 ? p - 64 : a->text;
        for (; scan < p - 7; scan++) {
            if (strncmp(scan, "(import", 7) == 0) {
                in_import = 1;
                break;
            }
        }
    }

    const char **stdlib_modules = mcp_stdlib_modules();

    buf_putc(out, '[');
    int emitted = 0;

    if (in_import) {
        for (int i = 0; stdlib_modules[i]; i++) {
            const char *mod = stdlib_modules[i];
            if (prefix_len > 0) {
                int match = 1;
                for (size_t j = 0; j < prefix_len; j++) {
                    char a2 = prefix[j], b = mod[j];
                    if (a2 >= 'A' && a2 <= 'Z') a2 += 32;
                    if (b  >= 'A' && b  <= 'Z') b  += 32;
                    if (a2 != b) { match = 0; break; }
                }
                if (!match) continue;
            }
            if (emitted > 0) buf_putc(out, ',');
            buf_puts(out, "{\"label\":");
            json_str(out, mod);
            buf_puts(out, ",\"kind\":9}");
            emitted++;
        }
    }

    if (!in_import) {
        for (int i = 0; i < a->sym_count && emitted < 200; i++) {
            const LspSymbol *sym = &a->syms[i];
            if (!sym->name[0]) continue;
            if (prefix_len > 0) {
                int match = 1;
                for (size_t j = 0; j < prefix_len; j++) {
                    char a2 = prefix[j], b = sym->name[j];
                    if (a2 >= 'A' && a2 <= 'Z') a2 += 32;
                    if (b  >= 'A' && b  <= 'Z') b  += 32;
                    if (a2 != b) { match = 0; break; }
                }
                if (!match) continue;
            }
            int kind = (strncmp(sym->type_str, "(fn", 3) == 0) ? 3 : 6;
            if (emitted > 0) buf_putc(out, ',');
            buf_puts(out, "{\"label\":");
            json_str(out, sym->name);
            buf_printf(out, ",\"kind\":%d", kind);
            if (sym->type_str[0]) {
                buf_puts(out, ",\"detail\":");
                json_str(out, sym->type_str);
            }
            if (sym->doc[0]) {
                buf_puts(out, ",\"doc\":");
                json_str(out, sym->doc);
            }
            buf_putc(out, '}');
            emitted++;
        }
    }

    buf_putc(out, ']');
    mcp_analysis_free(a);
    free(a);
}

static void mcp_tool_doc(const char *args, Buf *out, int *is_error) {
    char path[1024];
    char name[128];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }
    if (lsp_json_str_copy(args, "name", name, sizeof(name)) < 0) {
        buf_puts(out, "Missing required argument: name");
        *is_error = 1;
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        buf_printf(out, "Cannot open: %s", path);
        *is_error = 1;
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *text = NULL;
    if (sz > 0) {
        text = malloc((size_t)sz + 1);
        size_t rd = fread(text, 1, (size_t)sz, f);
        text[rd] = '\0';
        sz = (long)rd;
    }
    fclose(f);

    LspDocTable dtable;
    lsp_doc_table_init(&dtable);
    if (text) lsp_scan_docs(text, (size_t)sz, &dtable);
    free(text);

    char doc[512];
    if (lsp_scan_docs_lookup(&dtable, name, doc, sizeof(doc))) {
        buf_puts(out, doc);
    } else {
        buf_printf(out, "No docstring found for '%s' in %s", name, path);
    }

    lsp_doc_table_free(&dtable);
}

/* WP2 (D-8): make a path safe to pass as a positional argument.
 *
 * `run_subprocess("tur", {"tur", "format", path, NULL})` handed the editor's
 * path straight to a `tur` subcommand, so a path beginning with `-` arrived as
 * an option.  The usual fix is a `--` separator, but neither `tur format` nor
 * `tur build` parses one (both print usage and exit 0), so adding it would
 * break the tool rather than harden it.  `./-x` names the same file as `-x`
 * and is not option-shaped, which is the whole fix.
 *
 * Returns false when there is nothing usable to pass. */
static bool mcp_positional_path(const char *in, char *out, size_t cap) {
    if (!in || !*in) return false;
    if (in[0] == '-') {
        if (strlen(in) + 3 > cap) return false;
        snprintf(out, cap, "./%s", in);
    } else {
        if (strlen(in) + 1 > cap) return false;
        snprintf(out, cap, "%s", in);
    }
    return true;
}

static void mcp_tool_format(const char *args, Buf *out, int *is_error) {
    char path[1024];
    if (lsp_json_str_copy(args, "path", path, sizeof(path)) < 0) {
        buf_puts(out, "Missing required argument: path");
        *is_error = 1;
        return;
    }

    char safe[1030];
    if (!mcp_positional_path(path, safe, sizeof(safe))) {
        buf_puts(out, "Invalid argument: path");
        *is_error = 1;
        return;
    }
    char *argv[] = { "tur", "format", safe, NULL };
    Buf sub;
    buf_init(&sub);
    int rc = run_subprocess("tur", argv, &sub);
    buf_putc(&sub, '\0');
    if (rc != 0) {
        buf_printf(out, "tur format exited with status %d: %s", rc, sub.data);
        *is_error = 1;
    } else {
        buf_puts(out, sub.data);
    }
    buf_free(&sub);
}

static void mcp_tool_build(const char *args, Buf *out, int *is_error) {
    char dir[1024];
    if (lsp_json_str_copy(args, "dir", dir, sizeof(dir)) < 0) {
        buf_puts(out, "Missing required argument: dir");
        *is_error = 1;
        return;
    }

    char safe[1030];
    if (!mcp_positional_path(dir, safe, sizeof(safe))) {
        buf_puts(out, "Invalid argument: dir");
        *is_error = 1;
        return;
    }
    char *argv[] = { "tur", "build", safe, NULL };
    Buf sub;
    buf_init(&sub);
    int rc = run_subprocess("tur", argv, &sub);
    buf_putc(&sub, '\0');
    buf_printf(out, "exit_status: %d\n", rc);
    buf_puts(out, sub.data);
    buf_free(&sub);
    if (rc != 0) *is_error = 1;
}

/* -------------------------------------------------------------------------
 * tools/list response (static -- tools don't change at runtime)
 * --------------------------------------------------------------------- */

static const char TOOLS_LIST_JSON[] =
    "{\"tools\":["
      "{\"name\":\"check_file\","
       "\"description\":\"Type-check a Turmeric source file and return diagnostics as a JSON array.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"}},"
         "\"required\":[\"path\"]}},"
      "{\"name\":\"symbols\","
       "\"description\":\"List all symbols defined in a Turmeric source file.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"}},"
         "\"required\":[\"path\"]}},"
      "{\"name\":\"hover\","
       "\"description\":\"Return type and docstring for the symbol at a cursor position.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{"
           "\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"},"
           "\"line\":{\"type\":\"integer\",\"description\":\"0-based line number.\"},"
           "\"col\":{\"type\":\"integer\",\"description\":\"0-based column number.\"}},"
         "\"required\":[\"path\",\"line\",\"col\"]}},"
      "{\"name\":\"definition\","
       "\"description\":\"Return the source location where a symbol is defined.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{"
           "\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"},"
           "\"line\":{\"type\":\"integer\",\"description\":\"0-based line number.\"},"
           "\"col\":{\"type\":\"integer\",\"description\":\"0-based column number.\"}},"
         "\"required\":[\"path\",\"line\",\"col\"]}},"
      "{\"name\":\"complete\","
       "\"description\":\"Return completion candidates at a cursor position.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{"
           "\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"},"
           "\"line\":{\"type\":\"integer\",\"description\":\"0-based line number.\"},"
           "\"col\":{\"type\":\"integer\",\"description\":\"0-based column number.\"}},"
         "\"required\":[\"path\",\"line\",\"col\"]}},"
      "{\"name\":\"doc\","
       "\"description\":\"Return the ;;; docstring for a named symbol in a source file.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{"
           "\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"},"
           "\"name\":{\"type\":\"string\",\"description\":\"Symbol name to look up.\"}},"
         "\"required\":[\"path\",\"name\"]}},"
      "{\"name\":\"format\","
       "\"description\":\"Format a Turmeric source file using tur format.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the .tur source file.\"}},"
         "\"required\":[\"path\"]}},"
      "{\"name\":\"build\","
       "\"description\":\"Compile a Turmeric project directory using tur build. Returns diagnostics and exit status.\","
       "\"inputSchema\":{\"type\":\"object\","
         "\"properties\":{\"dir\":{\"type\":\"string\",\"description\":\"Absolute filesystem path to the project directory.\"}},"
         "\"required\":[\"dir\"]}}"
    "]}";

/* -------------------------------------------------------------------------
 * Main server loop
 * --------------------------------------------------------------------- */

void mcp_server_run(int fd_in, int fd_out) {
    if (getenv("TUR_NO_MCP")) {
        fprintf(stderr, "tur mcp: disabled by TUR_NO_MCP environment variable\n");
        return;
    }
    char *msg;
    while ((msg = mcp_read_message(fd_in)) != NULL) {
        size_t method_len;
        const char *method_raw = lsp_json_str(msg, "method", &method_len);
        if (!method_raw) { free(msg); continue; }

        char method[128];
        size_t copy = method_len < sizeof(method) - 1 ? method_len : sizeof(method) - 1;
        memcpy(method, method_raw, copy);
        method[copy] = '\0';

        size_t id_len = 0;
        const char *id_raw = lsp_json_raw(msg, "id", &id_len);

        size_t params_len = 0;
        const char *params_raw = lsp_json_raw(msg, "params", &params_len);

        if (strcmp(method, "initialize") == 0) {
            Buf r;
            buf_init(&r);
            buf_puts(&r, "{\"protocolVersion\":\"2024-11-05\","
                         "\"capabilities\":{\"tools\":{}},"
                         "\"serverInfo\":{\"name\":\"turmeric\",\"version\":");
            json_str(&r, TUR_VERSION);
            buf_puts(&r, "}}");
            buf_putc(&r, '\0');
            send_response(fd_out, id_raw, id_len, r.data);
            buf_free(&r);

        } else if (strcmp(method, "notifications/initialized") == 0 ||
                   strcmp(method, "initialized") == 0) {
            /* no-op notification */

        } else if (strcmp(method, "ping") == 0) {
            send_response(fd_out, id_raw, id_len, "{}");

        } else if (strcmp(method, "tools/list") == 0) {
            send_response(fd_out, id_raw, id_len, TOOLS_LIST_JSON);

        } else if (strcmp(method, "tools/call") == 0 && params_raw) {
            char tool_name[64] = {0};
            lsp_json_str_copy(params_raw, "name", tool_name, sizeof(tool_name));

            size_t args_len = 0;
            const char *args = lsp_json_raw(params_raw, "arguments", &args_len);
            if (!args) args = "{}";

            Buf out;
            buf_init(&out);
            int is_error = 0;

            if (strcmp(tool_name, "check_file") == 0) {
                mcp_tool_check_file(args, &out, &is_error);
            } else if (strcmp(tool_name, "symbols") == 0) {
                mcp_tool_symbols(args, &out, &is_error);
            } else if (strcmp(tool_name, "hover") == 0) {
                mcp_tool_hover(args, &out, &is_error);
            } else if (strcmp(tool_name, "definition") == 0) {
                mcp_tool_definition(args, &out, &is_error);
            } else if (strcmp(tool_name, "complete") == 0) {
                mcp_tool_complete(args, &out, &is_error);
            } else if (strcmp(tool_name, "doc") == 0) {
                mcp_tool_doc(args, &out, &is_error);
            } else if (strcmp(tool_name, "format") == 0) {
                mcp_tool_format(args, &out, &is_error);
            } else if (strcmp(tool_name, "build") == 0) {
                mcp_tool_build(args, &out, &is_error);
            } else {
                buf_printf(&out, "Unknown tool: %s", tool_name);
                is_error = 1;
            }

            buf_putc(&out, '\0');
            send_tool_result(fd_out, id_raw, id_len, out.data, is_error);
            buf_free(&out);

        } else if (id_raw) {
            send_error_response(fd_out, id_raw, id_len, -32601, "Method not found");
        }

        free(msg);
    }
}
