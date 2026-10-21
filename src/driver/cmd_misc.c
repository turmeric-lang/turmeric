/* cmd_misc.c -- the small subcommands: `tur image-info` / `tur image-verify`,
 * `tur doc`, `tur docs` (with its offline docs server), `tur smt`, `tur
 * experiments`, `tur dialects` and `tur explain`.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include <signal.h>

#include "arena.h"
#include "compiler/refine_smtlib.h" /* SX8a: `tur smt` -- the SMT-LIB2 seam */
#include "compiler/refine_solver.h" /* SX8a: the S0..S3 chain `tur smt` runs */
#include "runtime/image.h" /* AI6: tur image-info / image-verify */
#include "forms.h"
#include "reader.h"
#include "lang_dialects.h"  /* `tur dialects`: the `#lang` base axis */
#include "symbols.h"
#include "turi/docstrings.h"
#include "global.h"   /* use_json_output */

/* ---------------------------------------------------------------------------
 * AI6 (application-image-dumps-plan): `tur image-info` / `tur image-verify`.
 *
 * These inspect/validate an application image file (see src/runtime/image.h)
 * without resuming it. Images are loaded by the *application* binary, not by
 * `tur`, so build-stamp validation here is against a binary the user names
 * explicitly as the second argument; with no binary, verify checks only the
 * structural header (magic/version/CRC) and prints the embedded stamp.
 * --------------------------------------------------------------------------- */
static void tur_image_print_stamp(const uint8_t stamp[32]) {
    for (int i = 0; i < 32; i++) printf("%02x", stamp[i]);
}

int cmd_image_info(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tur image-info: cannot open '%s'\n", path); return 1; }
    TurImageHeader h;
    TurImageError e = tur_image_read_header(f, &h);
    fclose(f);
    if (e != IMAGE_OK) {
        fprintf(stderr, "tur image-info: %s: %s\n", path, tur_image_strerror(e));
        return 1;
    }
    printf("image:        %s\n", path);
    printf("magic:        TURI (0x%08x)\n", h.magic);
    printf("version:      %u\n", h.version);
    printf("build-stamp:  "); tur_image_print_stamp(h.build_stamp); printf("\n");
    printf("payload-len:  %llu bytes\n", (unsigned long long)h.payload_len);
    printf("created:      %llu ns since epoch\n", (unsigned long long)h.created_unix_ns);
    printf("globals-off:  %llu\n", (unsigned long long)h.globals_offset);
    printf("flags:        0x%08x\n", h.flags);
    printf("header-crc32: 0x%08x\n", h.header_crc32);
    return 0;
}

int cmd_image_verify(const char *path, const char *binary) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tur image-verify: cannot open '%s'\n", path); return 2; }
    TurImageHeader h;
    TurImageError e = tur_image_read_header(f, &h);
    /* The payload too (security-audit-plan M-1): its length against the file,
     * and its CRC when the header carries one. */
    if (e == IMAGE_OK) e = tur_image_verify_payload(f, &h);
    fclose(f);
    if (e != IMAGE_OK) {
        fprintf(stderr, "FAIL: %s: %s\n", path, tur_image_strerror(e));
        return 2;
    }
    if (binary) {
        uint8_t want[32];
        if (!tur_image_sha256_file(binary, want)) {
            fprintf(stderr, "tur image-verify: cannot read binary '%s'\n", binary);
            return 2;
        }
        if (memcmp(want, h.build_stamp, 32) != 0) {
            fprintf(stderr, "FAIL: build-stamp mismatch (image not produced by '%s')\n", binary);
            return 1;
        }
        printf("OK: header valid and build-stamp matches '%s'\n", binary);
        return 0;
    }
    printf("OK: header and payload valid (magic/version/flags/CRCs). build-stamp: ");
    tur_image_print_stamp(h.build_stamp); printf("\n");
    printf("note: pass a loader binary as the 2nd arg to verify the build-stamp.\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * OD4: stdlib docstring lookup for `tur doc`.
 *
 * The full docstrings already ship with every install: `tur run docs` generates
 * stdlib/docstrings.tur, and the formula installs stdlib/ under
 * <prefix>/share/turmeric/.  Rather than boot the interpreter and preload the
 * whole stdlib to answer one lookup, read the generated table directly -- see
 * src/turi/docstrings.c for the file-shape contract with tools/gendocs.py.
 * ------------------------------------------------------------------------ */

/* Look up `sym` in <stdlib>/docstrings.tur, or NULL when the table has no
 * entry (or is not installed).  The table reader is shared with the playground
 * doc panel and the interpreter's doc natives (src/turi/docstrings.c); the
 * returned string is owned by its cache. */
static const char *stdlib_docstring_lookup(const char *sym) {
    char path[4096];
    tur_stdlib_path("docstrings.tur", path, sizeof(path));
    return tur_docstring_lookup_in(path, sym);
}

/* Print one docstring, honouring --json. `owned` is freed when non-NULL. */
static int print_doc_result(const char *sym, const char *doc, char *owned) {
    if (use_json_output) {
        char esc_sym[128];
        drv_json_escape(sym, esc_sym, sizeof(esc_sym));
        /* Stdlib docstrings carry Parameters/Returns/Example blocks and run
         * well past the 512 bytes the old fixed buffer allowed. */
        size_t cap = strlen(doc) * 2 + 8;
        char *esc_doc = (char *)malloc(cap);
        if (!esc_doc) { free(owned); return 1; }
        drv_json_escape(doc, esc_doc, cap);
        printf("{\"name\":\"%s\",\"doc\":\"%s\"}\n", esc_sym, esc_doc);
        free(esc_doc);
    } else {
        printf("%s\n", doc);
    }
    free(owned);
    return 0;
}

/* E5 / OD4: tur doc <symbol> -- print documentation for a builtin, special
 * form, or stdlib definition. */
int cmd_doc_cli(const char *sym) {
    const char *doc = tur_doc_prefer(sym, stdlib_docstring_lookup(sym));
    if (doc) return print_doc_result(sym, doc, NULL);

    if (use_json_output)
        fprintf(stderr, "{\"error\":\"no documentation for '%s'\"}\n", sym);
    else
        fprintf(stderr, "no documentation for '%s'\n", sym);
    return 1;
}

/* ---------------------------------------------------------------------------
 * OD4: `tur docs` -- the rendered guides and API reference, offline.
 *
 * `tur doc <symbol>` above answers a symbol-sized question from the docstring
 * table. This answers the page-sized one: where are the rendered guides on
 * this machine, and how do I read them without a network?
 *
 * The docs are a separate artifact from the binary -- `tur run docs-tarball`
 * builds turmeric-docs-<version>.tar.gz, which a release attaches and a
 * package manager can unpack into <prefix>/share/doc/turmeric/. When they are
 * not installed, say so and point at the website rather than failing: an
 * absent optional artifact is not an error.
 * ------------------------------------------------------------------------ */

#define TUR_DOCS_URL "https://turmeric-lang.com/docs/html/guides/"

static int usage_docs(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur docs                  print where the rendered docs are installed\n"
        "  tur docs --open           open them in your browser\n"
        "  tur docs --serve [--port N]\n"
        "                            serve them from localhost (default port 8137)\n"
        "\n"
        "Looks for the docs in, in order:\n"
        "  $TUR_DOCS_DIR\n"
        "  <prefix>/share/doc/turmeric   (installed alongside the binary)\n"
        "  <repo>/docs/html              (a source checkout)\n"
        "\n"
        "--serve exists because some browsers refuse cross-page navigation\n"
        "under file://, which breaks the links between guides.\n"
        "\n"
        "For a single symbol, `tur doc <symbol>` needs no docs install at all.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* Locate the rendered docs root. Returns 1 and fills `out` on success. */
static int resolve_docs_root(char *out, size_t cap) {
    char probe[4096];

    const char *env = getenv("TUR_DOCS_DIR");
    if (env && *env) {
        int n = snprintf(probe, sizeof(probe), "%s/guides/index.html", env);
        if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
            snprintf(out, cap, "%s", env);
            return 1;
        }
        fprintf(stderr,
                "tur: ignoring TUR_DOCS_DIR=%s (no guides/index.html there)\n",
                env);
    }

    char exe[4096];
    if (drv_get_exe_path(exe, sizeof(exe)) != 0) return 0;
    char dir[4096];
    drv_dir_of_path(exe, dir, sizeof(dir));

    /* Same walk-up shape as resolve_stdlib_root: an installed prefix layout
     * and a source checkout are both ancestors of the binary. */
    for (int depth = 0; depth < 8; depth++) {
        int n = snprintf(probe, sizeof(probe),
                         "%s/share/doc/turmeric/guides/index.html", dir);
        if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
            snprintf(out, cap, "%s/share/doc/turmeric", dir);
            return 1;
        }
        n = snprintf(probe, sizeof(probe), "%s/docs/html/guides/index.html", dir);
        if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
            snprintf(out, cap, "%s/docs/html", dir);
            return 1;
        }
        char *slash = drv_last_path_sep(dir);
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return 0;
}

static void docs_not_installed_note(void) {
    fprintf(stderr,
        "tur: no rendered documentation found on this machine.\n"
        "\n"
        "  Read it online:   %s\n"
        "  Or install it:    download turmeric-docs-<version>.tar.gz from the\n"
        "                    release and unpack it, then point TUR_DOCS_DIR at\n"
        "                    the directory holding guides/ and api/.\n"
        "  Or build it:      tur run docs   (writes docs/html/ in a checkout)\n"
        "\n"
        "`tur doc <symbol>` works without any of this -- the docstring table\n"
        "ships with the binary.\n", TUR_DOCS_URL);
}

/* Hand a URL to the platform's browser opener. Returns 0 on success. */
static int open_in_browser(const char *url) {
#ifdef _WIN32
    const char *opener = "start \"\"";
#elif defined(__APPLE__)
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    /* WP2 (D-7): a bare `"%s"` left `$`, backtick and `\` live on POSIX.  The
     * URL here is one we built, but quoting it properly costs nothing and the
     * next caller may not be. */
    char q[8192];
    if (tur_shell_quote(url, q, sizeof q) != 0) return -1;
    char cmd[8300];
    int n = snprintf(cmd, sizeof(cmd), "%s %s", opener, q);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}

#ifndef _WIN32
static const char *docs_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js")   == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcmp(dot, ".woff") == 0) return "font/woff";
    if (strcmp(dot, ".txt")  == 0 || strcmp(dot, ".md") == 0)
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void docs_send_status(int fd, const char *status, const char *body) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, strlen(body));
    if (n > 0) { (void)!write(fd, head, (size_t)n); }
    (void)!write(fd, body, strlen(body));
}

/*
 * Resolve a request path to a file inside `root`, or return -1.
 *
 * realpath() on the joined path is the containment check: a request for
 * `/../../etc/passwd`, or a symlink pointing outside the tree, resolves to
 * something that does not start with the (also realpath'd) root, and is
 * refused. Doing it after the join rather than by inspecting the request
 * string means no clever encoding gets a second chance.
 */
static int docs_resolve_path(const char *root_real, const char *req,
                             char *out, size_t cap) {
    char joined[4096];
    const char *rel = req;
    while (*rel == '/') rel++;
    if (*rel == '\0') rel = "guides/index.html";

    int n = snprintf(joined, sizeof(joined), "%s/%s", root_real, rel);
    if (n < 0 || (size_t)n >= sizeof(joined)) return -1;

    char resolved[4096];
    if (!realpath(joined, resolved)) return -1;

    size_t rootlen = strlen(root_real);
    if (strncmp(resolved, root_real, rootlen) != 0) return -1;
    if (resolved[rootlen] != '\0' && resolved[rootlen] != '/') return -1;

    struct stat st;
    if (stat(resolved, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        char idx[4096];
        int m = snprintf(idx, sizeof(idx), "%s/index.html", resolved);
        if (m < 0 || (size_t)m >= sizeof(idx)) return -1;
        if (stat(idx, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
        snprintf(out, cap, "%s", idx);
        return 0;
    }
    if (!S_ISREG(st.st_mode)) return -1;
    snprintf(out, cap, "%s", resolved);
    return 0;
}

static void docs_serve_one(int fd, const char *root_real) {
    char req[8192];
    ssize_t got = read(fd, req, sizeof(req) - 1);
    if (got <= 0) return;
    req[got] = '\0';

    if (strncmp(req, "GET ", 4) != 0) {
        docs_send_status(fd, "405 Method Not Allowed", "only GET is served\n");
        return;
    }
    char *path = req + 4;
    char *sp = strchr(path, ' ');
    if (!sp) { docs_send_status(fd, "400 Bad Request", "malformed request\n"); return; }
    *sp = '\0';
    char *q = strchr(path, '?');
    if (q) *q = '\0';
    char *frag = strchr(path, '#');
    if (frag) *frag = '\0';

    char file[4096];
    if (docs_resolve_path(root_real, path, file, sizeof(file)) != 0) {
        docs_send_status(fd, "404 Not Found", "not found\n");
        return;
    }

    FILE *f = fopen(file, "rb");
    if (!f) { docs_send_status(fd, "404 Not Found", "not found\n"); return; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }

    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n\r\n",
                     docs_mime(file), size);
    if (n > 0) { (void)!write(fd, head, (size_t)n); }

    char buf[65536];
    size_t chunk;
    while ((chunk = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (write(fd, buf, chunk) < 0) break;
    }
    fclose(f);
}

/*
 * Serve `root` on 127.0.0.1:<port> until interrupted.
 *
 * Loopback only, GET only, and one connection at a time: this is a way to read
 * your own documentation, not a web server. Binding to a wildcard address
 * would put a filesystem-backed handler on the network for no benefit to the
 * one reader it exists for.
 */
static int cmd_docs_serve(const char *root, int port, bool open_browser) {
    char root_real[4096];
    if (!realpath(root, root_real)) {
        fprintf(stderr, "tur: cannot resolve docs directory '%s'\n", root);
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("tur docs: socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "tur docs: cannot bind 127.0.0.1:%d: %s\n",
                port, strerror(errno));
        close(srv);
        return 1;
    }
    if (listen(srv, 16) != 0) { perror("tur docs: listen"); close(srv); return 1; }

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/guides/index.html", port);
    printf("serving %s at %s\n", root_real, url);
    printf("press Ctrl-C to stop\n");
    fflush(stdout);

    if (open_browser) open_in_browser(url);

    /* SIGPIPE would kill the process when a browser closes a connection
     * mid-response, which is routine. */
    signal(SIGPIPE, SIG_IGN);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        docs_serve_one(fd, root_real);
        close(fd);
    }
    close(srv);
    return 0;
}
#endif /* !_WIN32 */

#define TUR_DOCS_DEFAULT_PORT 8137

int cmd_docs(int argc, char **argv) {
    bool want_open = false, want_serve = false;
    int port = TUR_DOCS_DEFAULT_PORT;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return usage_docs();
        if (strcmp(argv[i], "--open") == 0)  { want_open = true;  continue; }
        if (strcmp(argv[i], "--serve") == 0) { want_serve = true; continue; }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "tur docs: --port must be 1-65535\n");
                return 2;
            }
            want_serve = true;
            continue;
        }
        fprintf(stderr, "tur docs: unknown argument '%s'\n", argv[i]);
        return drv_usage_error(usage_docs);
    }

    char root[4096];
    if (!resolve_docs_root(root, sizeof(root))) {
        docs_not_installed_note();
        if (want_open) return open_in_browser(TUR_DOCS_URL) == 0 ? 0 : 1;
        return 1;
    }

    if (want_serve) {
#ifdef _WIN32
        fprintf(stderr, "tur docs: --serve is not available on Windows; "
                        "use `tur docs --open`\n");
        return 1;
#else
        return cmd_docs_serve(root, port, want_open);
#endif
    }

    char index[4096];
    snprintf(index, sizeof(index), "%s/guides/index.html", root);

    if (want_open) {
        char url[4200];
        snprintf(url, sizeof(url), "file://%s", index);
        if (open_in_browser(url) != 0) {
            fprintf(stderr, "tur docs: could not launch a browser; open this "
                            "by hand:\n  %s\n", url);
            return 1;
        }
        return 0;
    }

    printf("%s\n", root);
    printf("  guides: %s/guides/index.html\n", root);
    printf("  api:    %s/api/index.html\n", root);
    printf("\nOpen them with `tur docs --open`, or `tur docs --serve` if your\n"
           "browser blocks navigation between file:// pages.\n");
    return 0;
}

/* XF3 (experimental-flag-mechanism-plan): `tur experiments` -- list the
 * EXPERIMENTS[] registry.  This is the single source of truth; guides and the
 * docs site are generated from it, not maintained by hand. */
static const char *xf_lifecycle_str(ExperimentLifecycle lc) {
    return lc == XF_LIFECYCLE_BETA ? "beta" : "prototype";
}

static const char *xf_source_str(ExperimentSource src) {
    switch (src) {
        case XF_SRC_CLI:         return "cli";
        case XF_SRC_MANIFEST:    return "manifest";
        case XF_SRC_USER_CONFIG: return "user-config";
        default:                 return "-";
    }
}

/* Minimal JSON string escape (the registry fields are ASCII path/identifier
 * strings, but quote/backslash are handled for robustness). */
static void xf_json_puts(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

/* ------------------------------------------------------------------------- *
 * SX8a: `tur smt` -- run an SMT-LIB2 script through the standard chain.
 *
 * The solver `tur` already contains, answerable from outside the compile
 * pipeline.  Its point is interrogation, not competition: the reader accepts
 * the corpus subset of SMT-LIB2 over QF_UFLIA / QF_UFLRA, `unknown` is a
 * first-class answer, and parity with a production SMT solver is a non-goal.
 * What it buys is a public door -- an external harness can now differentially
 * test any solver against `tur` in BOTH directions (`TUR_REFINE_DUMP=1` writes
 * SMT-LIB out; this reads it back in) without `tur` ever linking one.
 *
 * See docs/archive/solver-extension-plan.md (SX8a).
 * ------------------------------------------------------------------------- */

/* Exit codes mirror the answer so a shell harness can branch on `$?` without
 * parsing stdout.  They are NOT the usual 0-is-success convention: `unsat` is
 * an answer, not a success, and `sat` is not a failure. */
enum { SMT_EXIT_UNSAT = 0, SMT_EXIT_SAT = 1, SMT_EXIT_UNKNOWN = 2,
       SMT_EXIT_ERROR = 3 };

/* Decide the assertion set currently in `vc` and print one SMT-LIB answer.
 * Shared by the batch path, each `(check-sat)` of a script, and the
 * interactive prompt, so all three answer the same way through the same
 * chain -- a window onto the solver has to show the same solver from every
 * angle. `*model_out` receives the witness when the answer is `sat`, for a
 * later `(get-model)`. */
static void smt_print_model(const RefineModel *m) {
    if (!m || !m->n) return;
    printf("(model\n");
    for (uint32_t i = 0; i < m->n; i++) {
        const RefineModelBinding *b = &m->bindings[i];
        if (b->is_real)
            printf("  (define-fun %s () Real %g)\n", b->name, b->rval);
        else if (b->is_bool)
            printf("  (define-fun %s () Bool %s)\n", b->name, b->ival ? "true" : "false");
        else
            printf("  (define-fun %s () Int %lld)\n", b->name, (long long)b->ival);
    }
    printf(")\n");
}

static int smt_answer(RefineVC *vc, Arena *arena, RefineModel **model_out) {
    /* The same chain, in the same order, as the compile path -- running a
     * different one here would make this window show something other than the
     * solver it is a window onto. */
    /* refine-chain-expands-the-same-dnf-four-times: the `_cc` stages, sharing
     * ONE run's cube set.  Each stage used to rebuild the same DNF from an
     * unchanged vc, so a query reaching S3 expanded it four times.  Same
     * verdicts by construction -- the set is identical, it is just built once.
     * The `refine_sN_decide` entry points still exist for a single-stage
     * caller; a chain has no use for them. */
    static const struct { const char *name;
                          RefineDecision (*fn)(RefineVC *, Arena *,
                                               RefineCubeCache *); } CHAIN[] = {
        { "S0 (trivial)",       refine_s0_decide_cc },
        { "S1 (EUF)",           refine_s1_decide_cc },
        { "S2 (arithmetic)",    refine_s2_decide_cc },
        { "S3 (Nelson-Oppen)",  refine_s3_decide_cc },
    };
    const char *decided_by = NULL;
    RefineVerdict v = RT_UNKNOWN;
    RefineCubeCache cc = {0};
    for (size_t i = 0; i < sizeof(CHAIN)/sizeof(CHAIN[0]); i++) {
        RefineDecision d = CHAIN[i].fn(vc, arena, &cc);
        if (d.verdict != RT_UNKNOWN) { v = d.verdict; decided_by = CHAIN[i].name; break; }
    }

    /* The goal is `false`, so `hyps |- false` VALID means the assertion set is
     * UNSAT.  The stages only ever prove; a model has to come from the bounded
     * counterexample search, which is the only thing in the solver allowed to
     * answer INVALID -- and it does so with a witness, never a guess. */
    int rc;
    if (model_out) *model_out = NULL;
    if (v == RT_VALID) {
        printf("unsat\n");
        rc = SMT_EXIT_UNSAT;
    } else {
        RefineModel *m = refine_model_search(vc, arena);
        if (m) {
            printf("sat\n");
            decided_by = "bounded model search";
            smt_print_model(m);
            if (model_out) *model_out = m;
            rc = SMT_EXIT_SAT;
        } else {
            printf("unknown\n");
            rc = SMT_EXIT_UNKNOWN;
        }
    }
    if (decided_by) fprintf(stderr, "tur smt: decided by %s\n", decided_by);
    return rc;
}

static int usage_smt(void) {
    fprintf(stderr,
        "usage: tur smt <file.smt2>\n"
        "       tur smt --interactive\n"
        "\n"
        "Run an SMT-LIB2 script through tur's in-house staged decision\n"
        "procedure (S0 trivial -> S1 EUF -> S2 linear arithmetic -> S3\n"
        "Nelson-Oppen) and print the answer.\n"
        "\n"
        "Scope: the corpus subset of SMT-LIB2 over QF_UFLIA / QF_UFLRA.\n"
        "A script using anything outside that fragment is refused whole,\n"
        "never partially parsed -- a partially parsed assertion set has\n"
        "weaker hypotheses than the script wrote, and `unsat` from it would\n"
        "be a claim about work not done.\n"
        "\n"
        "`unknown` is a first-class answer, not a failure. Parity with a\n"
        "production SMT solver is a non-goal.\n"
        "\n"
        "The script is run as a SESSION: (push)/(pop) scope the assertions and\n"
        "each (check-sat) is answered where it appears, so one script can ask\n"
        "several questions. A script with no (check-sat) at all is decided once\n"
        "at the end. --interactive reads the same commands from stdin and\n"
        "answers as they arrive.\n"
        "\n"
        "exit codes (the LAST answer, when a script asks more than once):\n"
        "  0  unsat      the assertion set is contradictory (proved)\n"
        "  1  sat        a model was found\n"
        "  2  unknown    no stage decided it\n"
        "  3  error      unreadable, or outside the accepted fragment\n");
    return 0;
}

/* An smt usage ERROR keeps the smt-specific status (3): the answer/exit
 * protocol above reserves 0-2 for sat/unsat/unknown.  `--help` is 0. */
static int usage_smt_error(void) { (void)usage_smt(); return SMT_EXIT_ERROR; }

/* Drive a fed session to exhaustion, answering each `(check-sat)`.  Returns the
 * last answer's exit code, or SMT_EXIT_ERROR on a refusal.  `*answered` says
 * whether any `(check-sat)` was seen, which is what lets a script that asks
 * nothing still get the single end-of-script answer SX8a's contract promises.
 *
 * `*last_model` and `*saw_exit` are the caller's, not locals, because the
 * interactive driver calls this once per typed form: a `(get-model)` arrives in
 * a later call than the `(check-sat)` whose witness it wants, and an `(exit)`
 * has to stop a loop this function does not own. */
static int smt_drive(SmtlibSession *s, Arena *arena, bool *answered, bool flush,
                     RefineModel **last_model, bool *saw_exit) {
    int rc = SMT_EXIT_UNKNOWN;
    for (;;) {
        SmtlibEvent ev = refine_smtlib_session_step(s);
        if (ev == SMT_EV_EXIT) { if (saw_exit) *saw_exit = true; }
        if (ev == SMT_EV_END || ev == SMT_EV_EXIT) return *answered ? rc : SMT_EXIT_UNKNOWN;
        if (ev == SMT_EV_ERROR) {
            printf("unknown\n");
            fprintf(stderr, "tur smt: outside the accepted fragment: %s\n",
                    refine_smtlib_session_err(s) ? refine_smtlib_session_err(s)
                                                 : "unsupported script");
            if (flush) fflush(stdout);
            return SMT_EXIT_ERROR;
        }
        if (ev == SMT_EV_CHECK_SAT) {
            rc = smt_answer(refine_smtlib_session_vc(s), arena, last_model);
            *answered = true;
        } else if (ev == SMT_EV_GET_MODEL) {
            /* Only meaningful after a `sat`; SMT-LIB leaves it an error
             * otherwise, and saying so is better than printing an empty
             * model that reads like "no bindings". */
            if (last_model && *last_model) smt_print_model(*last_model);
            else fprintf(stderr, "tur smt: (get-model) with no model in hand\n");
        }
        if (flush) fflush(stdout);
    }
}

/* Read one complete top-level s-expression from `in`, honouring `;` comments
 * and `|quoted|` / "string" atoms so a paren inside one does not throw the
 * balance off.  Returns false at EOF with nothing buffered.  This lives here
 * rather than in the reader because it is a TERMINAL concern: the reader parses
 * a buffer, and only an interactive driver needs to know when the user has
 * finished typing a form. */
static bool smt_read_form(FILE *in, char **buf, size_t *cap, size_t *len) {
    int depth = 0;
    bool any = false, in_line_comment = false, in_str = false, in_pipe = false;
    *len = 0;
    for (;;) {
        int c = fgetc(in);
        /* EOF mid-form still hands the buffered text to the reader rather than
         * dropping it: an unterminated form is a refusal the caller should see
         * ("unterminated list"), and silently discarding it would make a
         * truncated pipe look like a clean end of session. */
        if (c == EOF) return any && *len > 0;
        if (*len + 2 > *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return false;
            *buf = nb; *cap = nc;
        }
        (*buf)[(*len)++] = (char)c;
        (*buf)[*len] = '\0';

        if (in_line_comment) { if (c == '\n') in_line_comment = false; continue; }
        if (in_str)  { if (c == '"') in_str  = false; continue; }
        if (in_pipe) { if (c == '|') in_pipe = false; continue; }
        if (c == ';') { in_line_comment = true; continue; }
        if (c == '"') { in_str  = true; any = true; continue; }
        if (c == '|') { in_pipe = true; any = true; continue; }
        if (c == '(') { depth++; any = true; continue; }
        if (c == ')') {
            depth--;
            if (depth <= 0) return true;         /* one complete form */
            continue;
        }
        if (!isspace((unsigned char)c)) any = true;
    }
}

static int cmd_smt_interactive(void) {
    Arena arena;
    arena_init(&arena, 1 << 20);
    SmtlibSession *s = refine_smtlib_session_new(&arena);

    char  *line = NULL;
    size_t cap = 0, len = 0;
    int rc = SMT_EXIT_UNKNOWN;
    bool answered = false, saw_exit = false;
    RefineModel *last_model = NULL;
    while (!saw_exit && smt_read_form(stdin, &line, &cap, &len)) {
        refine_smtlib_session_feed(s, line, len);
        bool got = false;
        int r = smt_drive(s, &arena, &got, /*flush=*/true, &last_model, &saw_exit);
        if (r == SMT_EXIT_ERROR) { rc = r; break; }
        if (got) { rc = r; answered = true; }
    }
    free(line);
    arena_free(&arena);
    /* An interactive run that was never asked anything is not an error. */
    return answered || rc == SMT_EXIT_ERROR ? rc : SMT_EXIT_UNKNOWN;
}

int cmd_smt(int argc, char **argv) {
    const char *input = NULL;
    bool interactive = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return usage_smt();
        if (strcmp(argv[i], "--interactive") == 0 || strcmp(argv[i], "-i") == 0) {
            interactive = true;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1]) return usage_smt_error();
        if (input) return usage_smt_error();
        input = argv[i];
    }
    if (interactive) {
        if (input) return usage_smt_error();
        return cmd_smt_interactive();
    }
    if (!input) return usage_smt_error();

    FILE *f = fopen(input, "rb");
    if (!f) {
        fprintf(stderr, "tur smt: cannot open %s: %s\n", input, strerror(errno));
        return SMT_EXIT_ERROR;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); fprintf(stderr, "tur smt: cannot size %s\n", input); return SMT_EXIT_ERROR; }
    char *text = (char *)malloc((size_t)n + 1);
    if (!text) { fclose(f); fprintf(stderr, "tur smt: out of memory\n"); return SMT_EXIT_ERROR; }
    size_t got = fread(text, 1, (size_t)n, f);
    text[got] = '\0';
    fclose(f);

    Arena arena;
    arena_init(&arena, 1 << 20);

    /* SX8b: the script runs as a session, so `(push)`/`(pop)` scope the
     * assertions and each `(check-sat)` is answered where it appears.  A script
     * that never asks is still decided once at the end, which is SX8a's
     * contract and what every corpus benchmark relies on. */
    SmtlibSession *s = refine_smtlib_session_new(&arena);
    refine_smtlib_session_feed(s, text, got);
    bool answered = false;
    RefineModel *last_model = NULL;
    int rc = smt_drive(s, &arena, &answered, /*flush=*/false, &last_model, NULL);
    if (rc != SMT_EXIT_ERROR && !answered)
        rc = smt_answer(refine_smtlib_session_vc(s), &arena, NULL);

    /* What the script CLAIMED, when it claimed anything.  Reported, never
     * enforced: this command answers queries, it does not grade them, and a
     * disagreement here is for the caller to interpret. */
    SmtlibStatus status = refine_smtlib_session_status(s);
    if (status == SMT_STATUS_SAT || status == SMT_STATUS_UNSAT)
        fprintf(stderr, "tur smt: script claims :status %s\n",
                status == SMT_STATUS_SAT ? "sat" : "unsat");

    arena_free(&arena);
    free(text);
    return rc;
}

int cmd_experiments(int argc, char **argv) {
    /* `--json` is consumed by the global flag pass before dispatch (it sets
     * use_json_output); honour both that and a local --json for robustness. */
    bool json = use_json_output;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage:\n  tur experiments [--json]\n\n"
                   "List the compiler's experimental features (the --enable=<name>\n"
                   "registry).  --json emits the machine-readable form the docs site\n"
                   "consumes.\n");
            return 0;
        } else {
            fprintf(stderr, "tur experiments: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    /* UC-3: reflect the ambient configuration in the source column so a user
     * can see where each enabled flag came from.  CLI --enable= was already
     * applied in the global flag pass; consult the enclosing project manifest
     * (if any) and the user-level experiments file here, honoring the same
     * suppression rule as a compile. */
    {
        char cwd[4096];
        char *root = getcwd(cwd, sizeof(cwd)) ? drv_find_project_root(cwd) : NULL;
        char mp[4096];
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (root && pkg_resolve_manifest_path(root, mp, sizeof(mp))
            && pkg_manifest_read(mp, &m)) {
            apply_user_config_experiments(&m);
            apply_manifest_experiments(&m);
        } else {
            apply_user_config_experiments(NULL);
        }
        pkg_manifest_free(&m);
        free(root);
    }

    size_t n = experiment_count();

    if (json) {
        printf("[");
        for (size_t i = 0; i < n; i++) {
            const ExperimentDescriptor *d = experiment_at(i);
            if (i) printf(",");
            printf("\n  {");
            printf("\"name\":");        xf_json_puts(stdout, d->name);
            printf(",\"summary\":");    xf_json_puts(stdout, d->summary);
            printf(",\"lifecycle\":");  xf_json_puts(stdout, xf_lifecycle_str(d->lifecycle));
            printf(",\"introduced\":"); xf_json_puts(stdout, d->introduced);
            printf(",\"expires_at\":"); xf_json_puts(stdout, d->expires_at);
            printf(",\"plan\":");       xf_json_puts(stdout, d->plan_path);
            printf(",\"enabled\":%s",   experiment_is_enabled(d->name) ? "true" : "false");
            printf(",\"source\":");     xf_json_puts(stdout, xf_source_str(experiment_source_at(i)));
            printf("}");
        }
        printf("%s]\n", n ? "\n" : "");
        return 0;
    }

    if (n == 0) {
        printf("No experimental features are registered.\n\n"
               "The --enable=<name> mechanism is in place, but no feature is\n"
               "gated behind it right now.  See "
               "docs/guides/experimental-flags-guide.md.\n");
        return 0;
    }

    printf("%-22s %-9s %-10s %-10s %-9s %s\n",
           "NAME", "LIFECYCLE", "INTRODUCED", "EXPIRES", "ENABLED", "PLAN");
    for (size_t i = 0; i < n; i++) {
        const ExperimentDescriptor *d = experiment_at(i);
        char enabled[24];
        if (experiment_is_enabled(d->name))
            snprintf(enabled, sizeof(enabled), "yes(%s)",
                     xf_source_str(experiment_source_at(i)));
        else
            snprintf(enabled, sizeof(enabled), "no");
        printf("%-22s %-9s %-10s %-10s %-9s %s\n",
               d->name, xf_lifecycle_str(d->lifecycle), d->introduced,
               d->expires_at, enabled, d->plan_path);
    }
    printf("\n%zu experimental feature%s registered.\n", n, n == 1 ? "" : "s");
    return 0;
}

/* List the `#lang` BASE axis -- the (language, reader) pairs a base token can
 * name -- mirroring `tur experiments`.  `--json` emits the machine-readable
 * form.
 *
 * Was `tur lang-layers`, which listed two axes.  The layer axis was
 * decommissioned (docs/archive/lang-layers-decommission-plan.md), so the
 * command lists one axis and is named for it.  The listing itself stays: the
 * TUR-E0331 diagnostics point at it for the valid bases, and it is the only
 * way to ask a build which dialects it accepts. */
int cmd_dialects(int argc, char **argv) {
    bool json = use_json_output;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage:\n  tur dialects [--json]\n\n"
                   "List the base DIALECTS a `#lang` line may name: a language, "
                   "and\noptionally a reader after a slash.  `#lang <base>` takes "
                   "one base and\nnothing else -- a trailing token is an error "
                   "(TUR-E0330).  --json emits\nthe machine-readable form.\n");
            return 0;
        } else {
            fprintf(stderr, "tur dialects: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (json) {
        /* The array stays under a "dialects" key rather than becoming a bare
         * top-level array: a consumer reads one key, and the shape survives a
         * second thing ever being listed here. */
        printf("{\n  \"dialects\": ");
        lang_dialects_print_json();
        printf("\n}\n");
        return 0;
    }

    lang_dialects_print();
    printf("\n%zu `#lang` base dialect%s.\n", lang_bases_count(),
           lang_bases_count() == 1 ? "" : "s");
    return 0;
}

/* Phase 8: `tur explain <arg>` / `--explain <arg>` -- explain a diagnostic
 * code, or compile a code snippet and show its diagnostics in detail. */
int cmd_explain(const char *code) {
    /* Phase HKT-P5: If the argument looks like a diagnostic code
     * (TUR-E/W/D/I####), look it up in the explanation table instead of
     * compiling a snippet. */
    if (diag_looks_like_code(code)) {
        DiagCode dc = diag_code_from_string(code);
        if (dc == DIAG_CODE_NONE || !diag_explain(dc, stdout)) {
            fprintf(stderr, "tur: no explanation for '%s'\n", code);
            return 1;
        }
        return 0;
    }

    /* Legacy behaviour: compile a Turmeric source snippet and surface errors. */
    SourceFile file = {0};
    file.path = "<explain>";
    file.src = code;
    file.len = strlen(code);
    file.file_id = 0;
    file.reader_type = READER_TURMERIC;  /* Default for explain snippets */
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    if (!forms || diag_had_error()) {
        /* Error already emitted with enhanced diagnostics */
        symtab_free(&st);
        arena_free(&arena);
        return 1;
    }

    Expr *prog = elaborate_program(&arena, &st, forms, nforms, 0, ".", false, false, NULL,
                                    NULL, 0, NULL, NULL);
    if (!prog || diag_had_error()) {
        /* Error already emitted */
        symtab_free(&st);
        arena_free(&arena);
        return 1;
    }

    /* If no error, just say so */
    fprintf(stderr, "No errors found in the provided code.\n");

    symtab_free(&st);
    arena_free(&arena);
    return 0;
}
