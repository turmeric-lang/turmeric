/* cc.c -- driving the C toolchain over emitted C: `#include` hoisting, the
 * autolink markers and the flags they become, the runtime library and its
 * linkage mode, the one link command, and the stable temp paths the C is
 * written to.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"

#include "tur_argcheck.h"
#include "runtime/hamt.h"  /* tur_hamt_hash_xxh64: drv_stable_c_path */

/* The runtime-linkage mode; see the TUR_RT_* enum in driver/driver.h. */
int g_runtime_mode = TUR_RT_AUTO;

/* True while emitting C that `tur` itself will go on to compile and link. */
bool g_emit_for_link = false;

/* True while emitting C for `tur build --target wasm`.  emcc links that, and
 * no native runtime archive can go on its link line. */
bool g_emit_for_wasm = false;

/* `tur run --release` / `tur build --release`: compile the program at -O3
 * instead of -O2 (decision 2 of docs/archive/performance-audit-plan.md; WP2
 * measured -O3 at ~6% on the corpus).  -march=native is never on by default
 * -- its binaries need the machine that built them.  TUR_CC_FLAGS still
 * replaces the defaults whole, and the wasm target keeps emcc's -O2. */
bool g_cc_release = false;

/* Scan generated C source for "__tur_include__" comments and prepend each
 * captured line to the top of the buffer so the include lands at file
 * scope. The literal marker is the comment "__tur_include__: LINE" inside
 * a C block comment. Required for headers that declare top-level static
 * inline functions (mbedTLS, anything that uses psa/crypto.h); a plain
 * #include placed inside a function body would put those declarations at
 * block scope, which is not legal C.
 *
 * In-place: replaces *csrc with a new buffer containing
 * [hoisted-includes] + [original csrc]. No-op when no markers exist. */
void hoist_tur_include_directives(Buf *csrc) {
    /* Buf is not guaranteed NUL-terminated; append a sentinel before
     * any strstr() then drop it back off the logical length when done. */
    buf_putc(csrc, '\0');
    csrc->len--;

    const char *inc_mark = "/* __tur_include__: ";
    size_t inc_mlen = strlen(inc_mark);
    const char *p = csrc->data;
    /* TWO buckets, not one.  `__tur_include__` carries two different kinds of
     * payload -- preprocessor directives (`#include <stdlib.h>`) and file-scope
     * CODE (a `typedef`, or a `static` helper).  Concatenating them in source
     * order lets a code payload land ahead of a directive payload it depends
     * on, which is exactly what stdlib/httpd.tur does: the malloc-using
     * `httpd_conn_own_cstr` is hoisted from line 93 and `#include <stdlib.h>`
     * from line 3916, so the emitted TU called malloc with no declaration in
     * scope.  That compiled only because every cc invocation passed
     * -Wno-error=implicit-function-declaration, and it is UB regardless.
     *
     * So: all directive payloads first (in their own source order, which keeps
     * any feature-test `#define` ahead of the include it conditions), then all
     * code payloads (likewise in source order).  Relative order within each
     * bucket is preserved; only the two kinds are separated.
     * See docs/archive/history/hoisted-inline-c-precedes-includes.md. */
    Buf hdr, code;
    buf_init(&hdr);
    buf_init(&code);
    while (p && (p = strstr(p, inc_mark)) != NULL) {
        p += inc_mlen;
        const char *end = strstr(p, " */");
        if (!end) break;
        const char *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        int is_directive = (q < end && *q == '#')
            && (strncmp(q, "#include", 8) == 0 || strncmp(q, "#define", 7) == 0
                || strncmp(q, "#undef", 6) == 0 || strncmp(q, "#pragma", 7) == 0);
        Buf *dst = is_directive ? &hdr : &code;
        buf_write(dst, p, (size_t)(end - p));
        buf_putc(dst, '\n');
        p = end + 3;
    }
    if (code.len > 0) {
        buf_write(&hdr, code.data, code.len);
    }
    buf_free(&code);
    if (hdr.len > 0) {
        Buf new_csrc;
        buf_init(&new_csrc);
        /* Feature-test macros must precede any hoisted system #include.  A
         * hoisted header (e.g. <stdint.h> from an autolink __tur_include__)
         * pulls in <features.h>, which locks in the feature set on first
         * inclusion; a later `#define _DEFAULT_SOURCE` in the main preamble
         * then has no effect.  Under -std=c99 (strict ANSI) that leaves POSIX
         * functions like strdup() unprototyped, so the compiler assumes an
         * implicit `int` return and truncates the 64-bit pointer to 32 bits
         * -- corrupting, e.g., httpd request-attribute storage (SEGV on a
         * later strcmp/free).  Emitting the macro here guarantees it is seen
         * before any hoisted include. */
        buf_puts(&new_csrc, "#define _DEFAULT_SOURCE 1\n");
        /* The same reason, one header later: on macOS the layout of
         * ucontext_t is fixed by whichever header defines it first.  Defined
         * under _XOPEN_SOURCE it embeds the machine context (880 bytes on
         * arm64); defined by anything else -- <stdlib.h> reaches it through
         * <sys/wait.h> and <sys/signal.h> -- it is the 56-byte head alone.
         * libc's getcontext writes the machine context at its _XOPEN_SOURCE
         * offset either way, so a FiberBlock laid out with the short type had
         * every context switch write 816 bytes over the fields after its
         * contexts and into the next heap object (a fixture that hoists
         * <stdlib.h> -- every #lang r7rs program -- resumed fibers whose
         * saved pc was zero).  The preamble includes <ucontext.h> first for
         * this reason (emit_module.c, Phase T21), but a hoisted include lands
         * above the preamble, so the prefix repeats that step here, in the
         * preamble's order: the BSD networking headers first, while the full
         * Darwin feature set is still the default, then <ucontext.h> under
         * _XOPEN_SOURCE.  Include guards make the preamble's copy a no-op. */
        buf_puts(&new_csrc,
                 "#if defined(__APPLE__)\n"
                 "#include <sys/select.h>\n"
                 "#include <sys/socket.h>\n"
                 "#include <netinet/in.h>\n"
                 "#include <arpa/inet.h>\n"
                 "#define _XOPEN_SOURCE 700\n"
                 "#include <ucontext.h>\n"
                 "#undef _XOPEN_SOURCE\n"
                 "#endif\n");
        buf_write(&new_csrc, hdr.data, hdr.len);
        buf_write(&new_csrc, csrc->data, csrc->len);
        buf_free(csrc);
        *csrc = new_csrc;
    }
    buf_free(&hdr);
}

/* T2: resolve the turmeric source root -- the directory that holds both
 * `stdlib/` and `src/runtime/`.  It is the parent of the located stdlib
 * root (`<root>/stdlib` in the dev layout, `<prefix>/share/turmeric/stdlib`
 * when prefix-installed; in both, `src/runtime/` lives beside `stdlib/`).
 * Writes the path into `out` and returns it.  Falls back to "." when the
 * stdlib root could not be resolved to an absolute path (preserving the
 * legacy cwd-relative behavior). */
static const char *resolve_turmeric_root(char *out, size_t cap) {
    const char *sdir = resolve_stdlib_root();  /* e.g. "<root>/stdlib" or "stdlib" */
    snprintf(out, cap, "%s", sdir);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) { *slash = '\0'; return out; }
    if (slash == out) { out[1] = '\0'; return out; }  /* "/stdlib" -> "/" */
    snprintf(out, cap, ".");  /* bare "stdlib" -> cwd-relative, as before */
    return out;
}

/* T2: stdlib `__tur_autolink__` markers embed turmeric-tree-relative paths
 * (e.g. `src/runtime/hamt.c -Isrc/runtime` from stdlib/hamt.tur).  Those
 * only resolve when cwd is the turmeric source root -- true for `tur install`
 * driven dev builds, but NOT for project-mode `tur run` from an arbitrary
 * project directory, where cc fails with `src/runtime/hamt.c: No such file`.
 * Rewrite each whitespace-separated token in `flags` so that relative paths
 * (a bare path, or the argument of `-I` / `-L`) are anchored at `root`.
 * Absolute paths and non-path flags (`-l`, `-f`, `-D`, ...) pass through
 * unchanged.  The rewritten flags are appended to `out` (no trailing NUL). */
/* Is this path already absolute?  POSIX says a leading '/'; Windows also
 * accepts a leading '\' and a drive-qualified C:\dir or C:/dir.
 *
 * A '/'-only test reads C:\dir as RELATIVE, so the caller below anchors it at
 * the turmeric root and emits -LC:\root/C:\real\dir.  ld reports that as
 * `cannot find -lturt_runtime`, which reads as the -L having been dropped
 * rather than mangled -- and sends you looking in the wrong place. */
/* Last path separator, either spelling.
 *
 * Windows accepts '/' but does not produce it -- realpath() (_fullpath) hands
 * back C:\dir\sub -- so a walk-up that steps with strrchr(p, '/') alone finds
 * no separator, stops on its first iteration, and reports "not found" for the
 * entire platform.  Three loops below had that bug independently; this is the
 * one place to fix it in the driver (two of those loops now live in
 * src/compiler/spice_search.c, whose ss_last_path_sep is the same split).
 *
 * Fourth instance of the class, after find_stdlib_beside_exe,
 * rewrite_autolink_relative_paths and lsp.c's spice_root_of.  They all fail the
 * same way: by answering "nothing here" rather than by erroring. */
char *drv_last_path_sep(char *p) {
    char *slash = strrchr(p, '/');
#ifdef _WIN32
    char *bs = strrchr(p, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    return slash;
}

static bool path_is_absolute(const char *p) {
    if (!p || !*p) return false;
    if (p[0] == '/') return true;
#ifdef _WIN32
    if (p[0] == '\\') return true;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
        && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;
#endif
    return false;
}

static void rewrite_autolink_relative_paths(const char *flags,
                                             const char *root, Buf *out) {
    const char *p = flags;
    bool first = true;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - tok);
        if (!first) buf_putc(out, ' ');
        first = false;
        if (len > 2 && tok[0] == '-' && (tok[1] == 'I' || tok[1] == 'L') &&
            !path_is_absolute(tok + 2)) {
            /* -I<rel> / -L<rel>: anchor the path part at root. */
            buf_write(out, tok, 2);
            buf_printf(out, "%s/", root);
            buf_write(out, tok + 2, len - 2);
        } else if (tok[0] != '-' && !path_is_absolute(tok)) {
            /* bare relative path (source file or lib): anchor it at root. */
            buf_printf(out, "%s/", root);
            buf_write(out, tok, len);
        } else {
            buf_write(out, tok, len);
        }
    }
}

/* Build a deterministic path for the intermediate generated-C file so that
 * ccache can cache repeated compilations of the same .tur source.
 * Maps the input path to <tmpdir>/tur-build/<sanitized>.c where non-alphanumeric
 * characters (except '-') are replaced with '_'.
 * Creates <tmpdir>/tur-build/ on first call.
 *
 * The directory comes from tur_temp_dir(), not a literal "/tmp": on Windows a
 * leading "/" means the root of the current drive, so "/tmp/tur-build" would
 * point at a C:\tmp that does not exist. */
/* "<tmpdir>/tur-build/" (with trailing separator), created on first use.
 * Exposed so the unlink-vs-keep decision after cc can test membership against
 * the real prefix instead of pattern-matching a hardcoded "/tmp/...". */
/* WP2 (D-4): is `dir` a directory we can safely reuse?
 *
 * `<tmpdir>/tur-build` is a FIXED, world-readable path that every `tur` on the
 * box shares, and what lives in it is reused: the generated `.c` is written to
 * a name derived from the input path, and the prelude cache links
 * `prelude/<hash>.o` into the user's binary on nothing but "the file exists and
 * is non-empty".  `mkdir(dir, 0700)` with the result discarded does not make
 * that safe -- on a shared machine another user creates the directory first,
 * mode 0777, plants an object, and every subsequent build links it.
 *
 * So the directory is checked rather than assumed: it must be a real directory
 * (lstat, so a symlink pointing somewhere else is caught), owned by us, and
 * not writable by group or other.  A directory that fails is not repaired --
 * we do not own it -- we fall back to a private `tur-build-<uid>` beside it.
 *
 * Not applicable on Windows, which has no uid and gives each user a private
 * temp directory already. */
bool drv_tmp_dir_is_ours(const char *dir) {
#ifdef _WIN32
    (void)dir;
    return true;
#else
    struct stat st;
    if (lstat(dir, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != geteuid()) return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH)) return false;
    return true;
#endif
}

const char *drv_stable_c_prefix(void) {
    static char prefix_buf[512];
    static int  made = 0;
    if (!made) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/tur-build", tur_temp_dir());
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur: cannot create '%s': %s\n",
                    dir, strerror(errno));
        }
        if (!drv_tmp_dir_is_ours(dir)) {
#ifdef _WIN32
            fprintf(stderr, "tur: '%s' is not usable\n", dir);
#else
            char priv[512];
            snprintf(priv, sizeof(priv), "%s/tur-build-%u",
                     tur_temp_dir(), (unsigned)geteuid());
            fprintf(stderr,
                "tur: '%s' is not a directory this user owns and controls "
                "(a build would reuse whatever is in it); using '%s' instead\n",
                dir, priv);
            if (mkdir(priv, 0700) != 0 && errno != EEXIST) {
                fprintf(stderr, "tur: cannot create '%s': %s\n",
                        priv, strerror(errno));
            }
            if (drv_tmp_dir_is_ours(priv))
                snprintf(dir, sizeof(dir), "%s", priv);
#endif
        }
        snprintf(prefix_buf, sizeof(prefix_buf), "%s/", dir);
        made = 1;
    }
    return prefix_buf;
}

/* WP2 (D-4): open a generated file in the shared build directory for writing,
 * refusing to follow a symlink.
 *
 * The path is derived from the input's name, so it is predictable, and plain
 * `fopen(path, "wb")` follows a symlink planted at it -- writing the generated
 * C through to whatever the link names, with the invoking user's permissions.
 * O_NOFOLLOW refuses that.  O_EXCL is deliberately NOT used: the whole point
 * of the stable path is that ccache sees the same name every build, so the
 * file is expected to exist and be overwritten. */
FILE *drv_fopen_nofollow_w(const char *path) {
#ifdef _WIN32
    return fopen(path, "wb");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) close(fd);
    return f;
#endif
}

/* tur-build-intermediate-c-path-collides: the name is the input's base name,
 * sanitized, then a hash of its REAL path -- `m_tur-<16 hex>.c`.  It used to
 * be the path as typed, so two concurrent `tur build m.tur` in different
 * directories wrote, compiled and linked the same file and could swap
 * programs.  Still one stable name per source file, which is what ccache
 * needs. */
void drv_stable_c_path(const char *input, char *out, size_t cap) {
    const char *prefix = drv_stable_c_prefix();
    size_t plen = strlen(prefix);
    if (plen + 3 >= cap) { out[0] = '\0'; return; }
    memcpy(out, prefix, plen);
    size_t i = plen;
    const char *base = input;
    for (const char *p = input; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    for (const char *p = base; *p && i < cap - 21; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') {
            out[i++] = c;
        } else {
            out[i++] = '_';
        }
    }
    /* The hash reads 8-byte words, so it gets an aligned copy. */
    char *real = realpath(input, NULL);
    Buf key; buf_init(&key);
    buf_puts(&key, real ? real : input);
    uint64_t h = tur_hamt_hash_xxh64(key.data, key.len);
    buf_free(&key);
    free(real);
    if (i + 20 >= cap) { out[0] = '\0'; return; }
    i += (size_t)snprintf(out + i, cap - i, "-%016llx", (unsigned long long)h);
    out[i++] = '.';
    out[i++] = 'c';
    out[i] = '\0';
}

/* tur-link-and-build-split-plan Phase 3: scan generated C for
 * `__tur_autolink__` comments and collect the linker flags they embed (e.g.
 * -lturi from stdlib/turi/eval.tur).  The marker format is:
 *   slash-star __tur_autolink__: FLAGS star-slash
 * On return `autolink` holds the raw space-joined flag string (NUL-terminated
 * content when non-empty), before any SDK/ASan/anchor resolution -- except that
 * on Windows `-ldl` is dropped here (see below).  Shared by
 * cmd_build and cmd_compile so the two cannot drift. */
bool scan_autolink_markers(const Buf *csrc, Buf *autolink) {
    const char *marker = "/* __tur_autolink__: ";
    size_t mlen = strlen(marker);
    const char *p = csrc->data;
    while (p && (p = strstr(p, marker)) != NULL) {
        p += mlen;
        const char *end = strstr(p, " */");
        if (!end) break;
        /* WP2 (D-1): the marker body is repo-supplied -- it is whatever text
         * an inline-C block in any module wrote, a fetched spice's included --
         * and it used to be copied here verbatim and later spliced into the
         * string handed to system().  `-lfoo; touch pwned` ran touch.  Check
         * every token against the documented link vocabulary
         * (src/tur_argcheck.h) and refuse the build on anything else.
         *
         * Tokens, not the whole body: the string that leaves here is
         * space-joined by contract -- drv_append_include_tokens re-splits it for
         * the `cc -c` line, autolink_drop_bare_sources re-splits it to drop
         * bare .c args libturi.a supersedes, and the ASan probe scans it for
         * -L -- so quoting it instead was not available.  A grammar at this
         * boundary is what makes the space-joined contract safe to keep. */
        const char *prev_tok = NULL;
        char prev_buf[512];
        for (const char *q = p; q < end; ) {
            while (q < end && *q == ' ') q++;
            if (q >= end) break;
            const char *tstart = q;
            while (q < end && *q != ' ') q++;
            size_t tlen = (size_t)(q - tstart);
            char tok[1024];
            if (tlen >= sizeof(tok)) {
                fprintf(stderr,
                    "tur: __tur_autolink__ token is too long (%zu bytes)\n",
                    tlen);
                return false;
            }
            memcpy(tok, tstart, tlen);
            tok[tlen] = '\0';
            if (!tur_link_token_ok(tok, prev_tok)) {
                fprintf(stderr,
                    "tur: refusing link flag '%s' from an __tur_autolink__ "
                    "marker.\n"
                    "  marker: %.*s\n"
                    "  A module may contribute -l<name>, -L<dir>, -I<dir>, "
                    "-D<key>[=<val>], -framework <name>, -Wl,<...>, a source "
                    "or object path, or a bare toolchain flag -- nothing that "
                    "the shell would read as syntax.\n",
                    tok, (int)(end - p), p);
                return false;
            }
            if (autolink->len > 0) buf_putc(autolink, ' ');
            buf_write(autolink, tok, tlen);
            snprintf(prev_buf, sizeof(prev_buf), "%s", tok);
            prev_tok = prev_buf;
        }
        p = end + 3;
    }
    /* Two markers may name the same bare runtime source -- src/runtime/
     * rt_alloc.c rides hamt.tur's, string.tur's and sym-dynamic.tur's -- and
     * cc would then define its symbols twice.  A bare `.c` token keeps its
     * first occurrence only; every flag passes through as it came. */
    if (autolink->len > 0) {
        Buf keep; buf_init(&keep);
        const char *q = autolink->data, *lim = autolink->data + autolink->len;
        while (q < lim) {
            while (q < lim && *q == ' ') q++;
            const char *tok = q;
            while (q < lim && *q != ' ') q++;
            size_t tlen = (size_t)(q - tok);
            if (tlen == 0) continue;
            bool bare_c = tlen > 2 && tok[0] != '-'
                          && tok[tlen - 2] == '.' && tok[tlen - 1] == 'c';
            if (bare_c) {
                bool seen = false;
                const char *k = keep.data, *klim = keep.data + keep.len;
                while (k && k < klim) {
                    while (k < klim && *k == ' ') k++;
                    const char *kt = k;
                    while (k < klim && *k != ' ') k++;
                    if ((size_t)(k - kt) == tlen && memcmp(kt, tok, tlen) == 0) { seen = true; break; }
                }
                if (seen) continue;
            }
            if (keep.len > 0) buf_putc(&keep, ' ');
            buf_write(&keep, tok, tlen);
        }
        buf_free(autolink);
        *autolink = keep;
    }
#ifdef _WIN32
    /* Windows has no libdl.  dlopen/dlsym/dlclose are not a separate library
     * there; the emitted C reaches them through the tree's platform_dl.h shim
     * over kernel32, so there is nothing to link and the flag is simply absent
     * ("cannot find -ldl", which killed every jit-ffi fixture at link time).
     *
     * The marker itself is emitted UNCONDITIONALLY on purpose.  Its <dlfcn.h>
     * neighbour in emit_module.c is `#ifndef _WIN32`-guarded, but the marker
     * must not be: the emitted C is portable by design (WIN1 -- the platform
     * split lives in the OUTPUT as #ifdef, not in whichever host ran emit-c),
     * so a snapshot generated on Linux still has to carry it.  And this scanner
     * is a plain strstr over the text; it does not evaluate the preprocessor,
     * so wrapping the marker in a guard would not hide it from here anyway.
     *
     * Dropping it therefore belongs HERE, in the driver -- the first place that
     * knows the target.  The JIT engine's autolink loader already skips its
     * `dl` entry for the same reason. */
    if (autolink->len > 0) {
        Buf keep; buf_init(&keep);
        const char *q = autolink->data, *lim = autolink->data + autolink->len;
        while (q < lim) {
            while (q < lim && *q == ' ') q++;
            const char *tok = q;
            while (q < lim && *q != ' ') q++;
            size_t tlen = (size_t)(q - tok);
            if (tlen == 0) continue;
            if (tlen == 4 && strncmp(tok, "-ldl", 4) == 0) continue;
            if (keep.len > 0) buf_putc(&keep, ' ');
            buf_write(&keep, tok, tlen);
        }
        buf_free(autolink);
        *autolink = keep;
    }
#endif
    if (autolink->len > 0) buf_putc(autolink, '\0');
    return true;
}

/* ffi-spices-integration-plan S1: append the project manifest's
 * `:build-opts :link-libs` (as -l<name>) and `:link-flags` (verbatim) to the
 * link line.  `:link-libs` was parsed into the manifest (pkg.c) and then
 * consumed nowhere -- documented, round-tripped by `tur init`, and ignored by
 * every build path -- so S1 appended it next to the cmake-dep flags.  It
 * lived inline in drv_collect_build_aux, and cmd_build_multi_files carried a
 * hand-copied twin of the cmake read that predated S1 and never got it, which
 * is how `tur build --shared <dir>` came to ignore a spice's declared link
 * libraries while `tur build <file>` honoured them.  ONE helper, called from
 * both, is the only shape that cannot drift like that again.
 *
 * Entries in :link-libs are bare names (:link-libs ["m"] -> -lm), the same
 * spelling the cmake manifest uses.  :link-flags is the verbatim sibling --
 * no prefix is added, which is the only way to spell `-framework Cocoa` (see
 * the note on link_flags in pkg.h) or a `-L<dir>`.  No-op when proj_root has
 * no manifest. */
void append_manifest_link_flags(const char *proj_root, Buf *cmake_flags) {
    char bm[4096];
    if (!pkg_resolve_manifest_path(proj_root, bm, sizeof(bm))) return;
    PkgManifest pm;
    memset(&pm, 0, sizeof(pm));
    if (pkg_manifest_read(bm, &pm)) {
        /* WP2 (D-1): both vectors are grammar-checked in pkg_manifest_read
         * (check_link_vec), which is what makes appending them unquoted safe
         * -- a token that reached here cannot carry shell syntax.  They stay
         * unquoted because `:link-flags` is documented as the verbatim
         * sibling: `-framework Cocoa` is one entry and two arguments. */
        /* build-opts-c-flags-never-consumed: `:c-flags` was parsed, checked
         * (check_link_vec) and documented, and no build read it -- a
         * `-DMY_ANSWER=42` there was dropped.  It rides the same buffer, which
         * every caller passes to the compile as well as the link. */
        for (int i = 0; i < pm.n_c_flags; i++) {
            if (pm.c_flags[i] && pm.c_flags[i][0])
                buf_printf(cmake_flags, " %s", pm.c_flags[i]);
        }
        for (int i = 0; i < pm.n_link_libs; i++) {
            if (pm.link_libs[i] && pm.link_libs[i][0])
                buf_printf(cmake_flags, " -l%s", pm.link_libs[i]);
        }
        for (int i = 0; i < pm.n_link_flags; i++) {
            if (pm.link_flags[i] && pm.link_flags[i][0])
                buf_printf(cmake_flags, " %s", pm.link_flags[i]);
        }
    }
    pkg_manifest_free(&pm);
}

/* tur-link-and-build-split-plan Phase 3: collect the enclosing spice's cmake
 * dep flags (-I/-L/-l from cmake/spice-deps-manifest.json) plus its
 * `:c-includes` (-I) and `:c-sources` (vendored .c) by walking up from the
 * input file to the project root.  No-op when the input is not inside a
 * manifested project.  Shared by cmd_build and cmd_compile. */
/* Compose the `:web` emcc flags for a wasm link of `input` into `out`.
 *
 * Walks up from the input file the same way drv_collect_build_aux does, so a
 * single-file `tur build --target wasm game.tur` inside a project picks up
 * that project's `:web` block.  Returns false when the manifest's `:web :gl`
 * contradicts a cmake dep's `:wasm-options` -- caught here, before the link,
 * because the browser's own report of that mismatch names neither. */
bool drv_collect_web_link_flags(const char *input, Buf *out) {
    char input_dir[4096];
    strncpy(input_dir, input, sizeof(input_dir) - 1);
    input_dir[sizeof(input_dir) - 1] = '\0';
    char *slash = strrchr(input_dir, '/');
    if (slash) *slash = '\0';
    else strncpy(input_dir, ".", sizeof(input_dir));
    char abs_dir[4096];
    if (realpath(input_dir, abs_dir)) {
        strncpy(input_dir, abs_dir, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
    }
    char *proj_root = drv_find_project_root(input_dir);
    PkgWebOpts web;
    pkg_web_opts_defaults(&web);
    bool ok = true;
    if (proj_root) {
        char mpath[4096];
        PkgManifest m;
        if (pkg_resolve_manifest_path(proj_root, mpath, sizeof(mpath)) &&
            pkg_manifest_read(mpath, &m)) {
            ok = pkg_web_check_gl_agreement(&m.web, m.cmake_deps,
                                            m.n_cmake_deps);
            if (ok) pkg_web_compose_link_flags(&m.web, out);
            pkg_manifest_free(&m);
        } else {
            pkg_web_compose_link_flags(&web, out);
        }
        free(proj_root);
    } else {
        /* No manifest: still a valid wasm link, just with the defaults. */
        pkg_web_compose_link_flags(&web, out);
    }
    pkg_web_opts_free(&web);
    return ok;
}

void drv_collect_build_aux(const char *input, Buf *cmake_flags,
                           Buf *aux_includes, Buf *aux_sources,
                           const char *target) {
    /* Walk up from the input file's directory to find project root.  Resolve
     * to an absolute path first -- drv_find_project_root walks via strrchr('/'),
     * so a bare "." or "foo.tur" would stop after one step. */
    char input_dir[4096];
    strncpy(input_dir, input, sizeof(input_dir) - 1);
    input_dir[sizeof(input_dir) - 1] = '\0';
    char *slash = strrchr(input_dir, '/');
    if (slash) *slash = '\0';
    else strncpy(input_dir, ".", sizeof(input_dir));
    char abs_input_dir[4096];
    if (realpath(input_dir, abs_input_dir)) {
        strncpy(input_dir, abs_input_dir, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
    }
    char *proj_root = drv_find_project_root(input_dir);
    if (proj_root) {
        char manifest_path[4096];
        snprintf(manifest_path, sizeof(manifest_path), "%s/cmake/%s",
                 proj_root, pkg_cmake_manifest_name(target));
        PkgCmakeManifest cmake_manifest;
        if (pkg_cmake_manifest_read(manifest_path, &cmake_manifest)) {
            pkg_cmake_manifest_append_cc_flags(&cmake_manifest, cmake_flags);
            pkg_cmake_manifest_free(&cmake_manifest);
        }
        append_manifest_link_flags(proj_root, cmake_flags);
        collect_spice_aux_c(proj_root, aux_includes, aux_sources);
        free(proj_root);
    }
}

/* tur-link-and-build-split-plan Phase 3: append the -I tokens from a resolved
 * autolink/link flag string to `dst` (each space-prefixed).  The `-c` compile
 * of the generated TU needs the include dirs (e.g. -Isrc/runtime for inline-C
 * referencing runtime headers) but must not receive -l/-L or bare .c sources. */
void drv_append_include_tokens(Buf *dst, const char *flags) {
    if (!flags) return;
    const char *p = flags;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - start);
        if (tlen >= 2 && start[0] == '-' && start[1] == 'I') {
            buf_putc(dst, ' ');
            buf_write(dst, start, tlen);
        }
    }
}

/* tur-link-and-build-split-plan Phase 2/3c: true when the raw autolink string
 * carries at least one bare `.c` source arg (a runtime TU like
 * src/runtime/hamt.c).  These are exactly what --runtime=lib replaces with a
 * link against the prebuilt libturi.a. */
static int autolink_has_bare_c_source(const Buf *autolink) {
    if (!autolink || autolink->len == 0) return 0;
    const char *p = autolink->data;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        size_t t = (size_t)(p - s);
        if (t > 2 && s[0] != '-' && s[t - 2] == '.' && s[t - 1] == 'c') return 1;
    }
    return 0;
}

/* Probe `dir` for a runtime archive, preferring the lean non-sanitized
 * libturt_runtime.a over the full libturi.a.  On a hit, writes the linker name
 * (`turt_runtime` or `turi`) to `libname` and returns 1. */
static int probe_runtime_lib_in(const char *dir, char *libname, size_t ncap) {
    struct stat st;
    char probe[4096];
    snprintf(probe, sizeof(probe), "%s/libturt_runtime.a", dir);
    if (stat(probe, &st) == 0) { snprintf(libname, ncap, "turt_runtime"); return 1; }
    snprintf(probe, sizeof(probe), "%s/libturi.a", dir);
    if (stat(probe, &st) == 0) { snprintf(libname, ncap, "turi"); return 1; }
    return 0;
}

/* tur-link-and-build-split-plan Phase 2/3c + Phase 6 prereq: locate the runtime
 * archive to link under --runtime=lib.  Prefers the lean, non-sanitized
 * libturt_runtime.a (behaviorally identical to a bare-source recompile) and
 * falls back to the full libturi.a (which is ASan-instrumented in Debug builds).
 * Resolution order for the directory:
 *   1. $TUR_RUNTIME_LIB (an archive file path, or the directory holding one).
 *   2. <exe_dir>/src         (dev layout: build/tur -> build/src).
 *   3. <turmeric_root>/build/src.
 * A prefix-installed SDK's libturi.a is picked up separately by
 * resolve_autolink_flags step 1 once -lturi is on the line.  Returns 1 with the
 * dir in `libdir` and the linker name in `libname` on success, else 0. */
int locate_runtime_lib(char *libdir, size_t dcap,
                       char *libname, size_t ncap) {
    libdir[0] = '\0';
    libname[0] = '\0';
    struct stat st;
    const char *env = getenv("TUR_RUNTIME_LIB");
    if (env && *env && stat(env, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            if (probe_runtime_lib_in(env, libname, ncap)) {
                snprintf(libdir, dcap, "%s", env);
                return 1;
            }
            return 0;
        }
        /* A file path: derive libname from lib<name>.a and use its dir. */
        char d[4096];
        drv_dir_of_path(env, d, sizeof(d));
        snprintf(libdir, dcap, "%s", d);
        const char *base = strrchr(env, '/');
        base = base ? base + 1 : env;
        if (strncmp(base, "lib", 3) == 0) base += 3;
        snprintf(libname, ncap, "%s", base);
        char *dot = strrchr(libname, '.');
        if (dot) *dot = '\0';
        return libname[0] ? 1 : 0;
    }
    char exe[4096];
    if (drv_get_exe_path(exe, sizeof(exe)) == 0) {
        char d[4096];
        drv_dir_of_path(exe, d, sizeof(d));
        char sd[4200];
        snprintf(sd, sizeof(sd), "%s/src", d);
        if (probe_runtime_lib_in(sd, libname, ncap)) {
            snprintf(libdir, dcap, "%s", sd);
            return 1;
        }
        /* The FLAT layout -- the archive shape release.yml publishes for
         * linux-x86_64, linux-aarch64 and macos-arm64, where `tur` and the
         * runtime archive sit in one directory with no bin/ or lib/.
         *
         * Without this probe none of the four candidates matched an extracted
         * tarball, TUR_RT_AUTO fell back to source mode, and source mode wanted
         * the src/runtime sources that the archive does not ship -- so `tur run` on a
         * released build failed at the C compile step with "no such file or
         * directory: .../src/runtime/hamt.c".  Verified on macOS and Windows;
         * see docs/archive/release-archive-cannot-compile.md.
         *
         * Probing <exe_dir> rather than restructuring the archives is what keeps
         * this non-breaking: tvm already restages into bin/ + lib/, Homebrew and
         * Trowel consume the published shape, and all of those keep working. */
        if (probe_runtime_lib_in(d, libname, ncap)) {
            snprintf(libdir, dcap, "%s", d);
            return 1;
        }
        /* DEDUP-4b: the INSTALLED layout -- <prefix>/bin/tur next to
         * <prefix>/lib/libturt_runtime.a.  Without this probe the archive was
         * only ever findable in a dev build tree, so an installed toolchain
         * silently recompiled the runtime on every build and (since 4b) kept
         * running the emitted GC replica.  Checked after the dev layout so a
         * build tree still wins over a stale system install. */
        snprintf(sd, sizeof(sd), "%s/../lib", d);
        if (probe_runtime_lib_in(sd, libname, ncap)) {
            snprintf(libdir, dcap, "%s", sd);
            return 1;
        }
    }
    char root[4096];
    resolve_turmeric_root(root, sizeof(root));
    if (root[0]) {
        char bd[4200];
        snprintf(bd, sizeof(bd), "%s/build/src", root);
        if (probe_runtime_lib_in(bd, libname, ncap)) {
            snprintf(libdir, dcap, "%s", bd);
            return 1;
        }
    }
    return 0;
}

/* Rebuild `autolink` dropping every bare `.c` source token (a runtime TU),
 * keeping all flags.  Used when --runtime=lib replaces the recompiled sources
 * with a link against a runtime archive.  Appends into a fresh buffer. */
static void autolink_drop_bare_sources(const Buf *autolink, Buf *out) {
    const char *p = autolink->data;
    while (p && *p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        size_t t = (size_t)(p - s);
        int bare_c = t > 2 && s[0] != '-' && s[t - 2] == '.' && s[t - 1] == 'c';
        if (bare_c) continue;
        if (out->len > 0) buf_putc(out, ' ');
        buf_write(out, s, t);
    }
}

/* tur-link-and-build-split-plan Phase 2/3c/6: replace the bare runtime `.c`
 * autolink sources with a link against a prebuilt runtime archive so the
 * runtime TUs (hamt/symbols/tur_string) are linked instead of recompiled per
 * build.  Behavior by mode (see g_runtime_mode):
 *   AUTO (default) -- use the lean non-ASan libturt_runtime.a when locatable;
 *     otherwise leave the bare sources in place (recompile).  Never links the
 *     full libturi.a here, so a default build cannot regress into a link
 *     failure (no archive -> source) or an unexpected ASan/LSan run.
 *   LIB -- force the archive: lean preferred, else the full libturi.a, else a
 *     -lturi fallback (with a warning) that relies on a -L in TUR_CC_FLAGS.
 * No-op when the program autolinks no runtime sources or already links -lturi.
 * `autolink` is consumed and replaced in place. */
void apply_runtime_lib_mode(Buf *autolink) {
    if (g_runtime_mode == TUR_RT_SOURCE) return;
    if (!autolink_has_bare_c_source(autolink)) return;
    if (autolink->len > 0 && strstr(autolink->data, "-lturi")) return;

    char libdir[4096], libname[128];
    int found = locate_runtime_lib(libdir, sizeof(libdir), libname, sizeof(libname));

    /* AUTO only links the lean, guaranteed-non-ASan archive; anything else
     * (only the full libturi.a, or no archive at all) falls back to recompiling
     * the sources so a default build stays behaviorally identical to source. */
    if (g_runtime_mode == TUR_RT_AUTO &&
        !(found && strcmp(libname, "turt_runtime") == 0))
        return;

    Buf inj;
    buf_init(&inj);
    if (found) {
        buf_printf(&inj, "-l%s -L%s", libname, libdir);
    } else {
        /* Reachable only in explicit LIB mode: rely on a -L in TUR_CC_FLAGS. */
        buf_puts(&inj, "-lturi");
        fprintf(stderr,
                "tur: --runtime=lib could not locate a runtime archive; relying "
                "on a -L in TUR_CC_FLAGS (set TUR_RUNTIME_LIB to override)\n");
    }
    /* Keep every flag, drop the bare .c sources the archive supersedes.  The
     * drop helper inserts the separating space before each kept token. */
    if (autolink->len > 1) autolink_drop_bare_sources(autolink, &inj);
    buf_putc(&inj, '\0');
    buf_free(autolink);
    *autolink = inj;
}

/* regions-off-source-runtime-link-failure: a TUR_REGIONS=0 program pastes no
 * region runtime (emit_region_runtime_bodies), and its emitted C must name no
 * region symbol (tests/run-regions-seam.sh).  But hamt.c is host runtime and
 * calls tur_region_note_escape directly, so compiled here from source -- no
 * archive to supply region.o -- it failed to link.  Tell the runtime sources
 * that regions are off: hamt.c's note then compiles away, as the program's own
 * TUR_REGION_NOTE does.  An archive link needs nothing (region.o is in it). */
void autolink_regions_off(Buf *autolink) {
    if (regions_enabled() || !autolink_has_bare_c_source(autolink)) return;
    Buf out;
    buf_init(&out);
    if (autolink->len > 1) buf_write(&out, autolink->data, autolink->len - 1);
    buf_puts(&out, " -DTUR_REGIONS=0");
    buf_putc(&out, '\0');
    buf_free(autolink);
    *autolink = out;
}

/* Is `prefix` an installed SDK prefix -- <prefix>/include/turi/eval.h, the
 * layout a release archive ships (scripts/stage-release.sh)? */
static bool sdk_prefix_has_turi_headers(const char *prefix) {
    char probe[4200];
    struct stat st;
    snprintf(probe, sizeof(probe), "%s/include/turi/eval.h", prefix);
    return stat(probe, &st) == 0 && S_ISREG(st.st_mode);
}

/* tur-link-and-build-split-plan Phase 1: resolve the raw `__tur_autolink__`
 * flag string (as scanned out of the generated C, or read from a `.link`
 * sidecar) into the final link-ready flag set, and report whether the linked
 * libturi was ASan-instrumented.
 *
 * This is the shared link-side flag logic factored out of the old monolithic
 * cmd_build.  It performs, in order:
 *   1. -lturi SDK anchoring -- prepend absolute -I/-L for a prefix-installed SDK.
 *   2. ASan autodetect -- scan libturi.a for __asan_init via nm.
 *   3. Turmeric-tree-relative path anchoring (src/runtime/hamt.c -> abs).
 *   4. -lturi supersedes bare .c source args (drop them; libturi has the objs).
 *
 * `autolink` is consumed and replaced in place: on return it holds the resolved
 * flag string (NUL-terminated content, like the caller's original buffer).
 * `cc_flags` is read for -L paths during the ASan probe.  `*out_needs_asan` is
 * set true when a sanitized libturi.a is on the link line.
 *
 * Behavior is byte-for-byte identical to the inline blocks it replaces; both
 * cmd_build and `tur link` call it so they cannot drift. */
void resolve_autolink_flags(Buf *autolink, const char *cc_flags,
                             bool *out_needs_asan) {
    *out_needs_asan = false;

    /* 1. -lturi SDK anchoring.  Two SDK shapes:
     *   - a SOURCE TREE staged under <root>/share/turmeric/src (sdk_root), and
     *   - an installed PREFIX, <prefix>/include/turi/eval.h beside
     *     <prefix>/lib/libturi.a (sdk_prefix) -- the shape every release
     *     archive ships (scripts/stage-release.sh).  Probed at exactly
     *     <exe_dir>/.., the way resolve_stdlib_root and locate_runtime_lib
     *     find the same archive's stdlib and runtime: a walk-up would let a
     *     stale /usr/local/include/turi shadow a dev build under /usr/local/src.
     * Only the first was looked for, so no release from v0.47.0 on could build
     * a program that links -lturi: `turi/eval.h` not found
     * (docs/archive/release-tarball-turi-headers-are-unusable.md). */
    if (autolink->len > 0 && strstr(autolink->data, "-lturi")) {
        char sdk_root[4096] = "";
        char sdk_prefix[4096] = "";
        const char *sdk_env = getenv("TUR_SDK_ROOT");
        if (sdk_env && *sdk_env) {
            if (sdk_prefix_has_turi_headers(sdk_env))
                snprintf(sdk_prefix, sizeof(sdk_prefix), "%s", sdk_env);
            else
                snprintf(sdk_root, sizeof(sdk_root), "%s", sdk_env);
        } else {
            char exe_buf[4096] = "";
            if (drv_get_exe_path(exe_buf, sizeof(exe_buf)) == 0) {
                char dir[4096];
                drv_dir_of_path(exe_buf, dir, sizeof(dir));
                char parent[4096];
                snprintf(parent, sizeof(parent), "%s", dir);
                char *psep = drv_last_path_sep(parent);
                if (psep && psep != parent) {
                    *psep = '\0';
                    if (sdk_prefix_has_turi_headers(parent))
                        snprintf(sdk_prefix, sizeof(sdk_prefix), "%s", parent);
                }
                for (int d = 0; d < 8 && !sdk_prefix[0]; d++) {
                    char probe[4096];
                    struct stat sdk_st;
                    snprintf(probe, sizeof(probe),
                             "%s/share/turmeric/src/turi/eval.h", dir);
                    if (stat(probe, &sdk_st) == 0 && S_ISREG(sdk_st.st_mode)) {
                        snprintf(sdk_root, sizeof(sdk_root),
                                 "%s/share/turmeric", dir);
                        break;
                    }
                    char *sl = strrchr(dir, '/');
                    if (!sl || sl == dir) break;
                    *sl = '\0';
                }
            }
        }
        if (sdk_root[0]) {
            Buf sdk_flags;
            buf_init(&sdk_flags);
            buf_printf(&sdk_flags, "-I%s/src -I%s/src/compiler -I%s/src/runtime",
                       sdk_root, sdk_root, sdk_root);
            struct stat sdk_lib_st;
            char lib_probe[4096];
            snprintf(lib_probe, sizeof(lib_probe), "%s/lib/libturi.a", sdk_root);
            if (stat(lib_probe, &sdk_lib_st) == 0)
                buf_printf(&sdk_flags, " -L%s/lib", sdk_root);
            snprintf(lib_probe, sizeof(lib_probe), "%s/build/src/libturi.a", sdk_root);
            if (stat(lib_probe, &sdk_lib_st) == 0)
                buf_printf(&sdk_flags, " -L%s/build/src", sdk_root);
            Buf new_al;
            buf_init(&new_al);
            buf_write(&new_al, sdk_flags.data, sdk_flags.len);
            buf_free(&sdk_flags);
            buf_putc(&new_al, ' ');
            if (autolink->len > 1)
                buf_write(&new_al, autolink->data, autolink->len - 1);
            buf_putc(&new_al, '\0');
            buf_free(autolink);
            *autolink = new_al;
        } else if (sdk_prefix[0]) {
            /* One -I: the shipped headers spell each other src/-relative
             * ("compiler/diag.h"), and include/ mirrors that layout.  The
             * marker's own -Isrc/... flags are anchored by step 3 at
             * <prefix>/share/turmeric, which has no src/; cc ignores a
             * missing -I directory. */
            Buf new_al;
            buf_init(&new_al);
            buf_printf(&new_al, "-I%s/include", sdk_prefix);
            char lib_probe[4200];
            struct stat sdk_lib_st;
            snprintf(lib_probe, sizeof(lib_probe), "%s/lib/libturi.a", sdk_prefix);
            if (stat(lib_probe, &sdk_lib_st) == 0)
                buf_printf(&new_al, " -L%s/lib", sdk_prefix);
            buf_putc(&new_al, ' ');
            if (autolink->len > 1)
                buf_write(&new_al, autolink->data, autolink->len - 1);
            buf_putc(&new_al, '\0');
            buf_free(autolink);
            *autolink = new_al;
        } else {
            /* r7rs-lang-plan T4: an IN-TREE build -- `tur` at <root>/build/tur
             * with its libturi.a beside it in <build>/src.  `(import (scheme
             * eval))` links the interpreter, and a developer's `tur run` of
             * such a program should find the archive this very build made
             * without TUR_CC_FLAGS.  (The marker's own -I paths are anchored
             * at the source root by step 3.)  Skipped when TUR_CC_FLAGS
             * already names a -L, so the ctests' explicit flags still win. */
            char exe_buf[4096] = "";
            if (!(cc_flags && strstr(cc_flags, "-L")) &&
                drv_get_exe_path(exe_buf, sizeof(exe_buf)) == 0) {
                char dir[4096], probe[4200];
                struct stat st;
                drv_dir_of_path(exe_buf, dir, sizeof(dir));
                snprintf(probe, sizeof(probe), "%s/src/libturi.a", dir);
                if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
                    Buf new_al;
                    buf_init(&new_al);
                    buf_printf(&new_al, "-L%s/src ", dir);
                    if (autolink->len > 1)
                        buf_write(&new_al, autolink->data, autolink->len - 1);
                    buf_putc(&new_al, '\0');
                    buf_free(autolink);
                    *autolink = new_al;
                }
            }
        }
    }

    /* 2. ASan autodetect: sanitized libturi.a needs -fsanitize on the link.
     * The -L paths searched are TUR_CC_FLAGS' and (r7rs-lang-plan T4) the
     * autolink's own, which step 1 may have anchored. */
    if (autolink->len > 0 && strstr(autolink->data, "-lturi")) {
        char nm_cmd[512];
        Buf scan;
        buf_init(&scan);
        if (cc_flags) buf_puts(&scan, cc_flags);
        buf_putc(&scan, ' ');
        buf_puts(&scan, autolink->data);
        buf_putc(&scan, '\0');
        const char *cf = scan.data;
        while (cf && *cf) {
            const char *lf = strstr(cf, "-L");
            if (!lf) break;
            lf += 2;
            const char *lf_end = lf;
            while (*lf_end && *lf_end != ' ') lf_end++;
            if (lf_end > lf) {
                char lib_path[512];
                size_t plen = (size_t)(lf_end - lf);
                if (plen < sizeof(lib_path) - 20) {
                    memcpy(lib_path, lf, plen);
                    snprintf(lib_path + plen, sizeof(lib_path) - plen, "/libturi.a");
                    snprintf(nm_cmd, sizeof(nm_cmd),
                             "nm %s 2>/dev/null | grep -q __asan_init", lib_path);
                    if (cc_system(nm_cmd) == 0) {
                        *out_needs_asan = true;
                        break;
                    }
                }
            }
            cf = lf_end;
        }
        buf_free(&scan);
    }

    /* 3. Anchor turmeric-tree-relative autolink paths at the located root. */
    if (autolink->len > 1) {
        char tur_root[4096];
        resolve_turmeric_root(tur_root, sizeof(tur_root));
        if (tur_root[0] && strcmp(tur_root, ".") != 0) {
            Buf rewritten;
            buf_init(&rewritten);
            rewrite_autolink_relative_paths(autolink->data, tur_root, &rewritten);
            buf_putc(&rewritten, '\0');
            buf_free(autolink);
            *autolink = rewritten;
        }
    }

    /* 3b. r7rs-lang-plan T4: `@TUR_STDLIB_ROOT@` names the stdlib this `tur`
     * builds against, as a C string literal -- stdlib/r7rs/eval.tur bakes it
     * in (`-DTUR_R7RS_STDLIB=@TUR_STDLIB_ROOT@`) so a built program's embedded
     * evaluator finds the R7RS prelude without TUR_STDLIB_DIR.  A root the
     * shell quoting cannot carry leaves the define out (the program then
     * falls back to TUR_STDLIB_DIR and a cwd-relative `stdlib`). */
    if (autolink->len > 1 && strstr(autolink->data, "@TUR_STDLIB_ROOT@")) {
        const char *root = resolve_stdlib_root();
        char abs_root[4096];
        if (root && realpath(root, abs_root)) root = abs_root;
        bool quotable = root && *root && !strpbrk(root, "'\"\\ \t\n");
        Buf out;
        buf_init(&out);
        const char *p = autolink->data;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            size_t tlen = (size_t)(p - start);
            const char *at = strstr(start, "@TUR_STDLIB_ROOT@");
            if (at && at < p) {
                if (!quotable) continue;
                if (out.len > 0) buf_putc(&out, ' ');
                buf_putc(&out, '\'');
                buf_write(&out, start, (size_t)(at - start));
                buf_printf(&out, "\"%s\"", root);
                const char *after = at + strlen("@TUR_STDLIB_ROOT@");
                buf_write(&out, after, (size_t)(p - after));
                buf_putc(&out, '\'');
                continue;
            }
            if (out.len > 0) buf_putc(&out, ' ');
            buf_write(&out, start, tlen);
        }
        buf_putc(&out, '\0');
        buf_free(autolink);
        *autolink = out;
    }

    /* 4. -lturi supersedes bare .c source args; drop them to avoid duplicate
     * symbols (libturi.a already provides every runtime TU's object). */
    if (autolink->len > 1 && strstr(autolink->data, "-lturi")) {
        Buf filtered;
        buf_init(&filtered);
        const char *p = autolink->data;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            size_t tlen = (size_t)(p - start);
            bool is_c_source = tlen > 2 && start[0] != '-' &&
                               start[tlen - 2] == '.' && start[tlen - 1] == 'c';
            if (is_c_source) continue;
            if (filtered.len > 0) buf_putc(&filtered, ' ');
            buf_write(&filtered, start, tlen);
        }
        buf_putc(&filtered, '\0');
        buf_free(autolink);
        *autolink = filtered;
    }
}

/* WP2 (D-1): append `s` to a command string as ONE shell-quoted argument.
 *
 * The driver assembles its cc invocations as shell command strings -- see the
 * note in tur_argcheck.h for why they stay strings rather than becoming an
 * argv -- and every path it splices in is a path the user or the project
 * chose.  An unquoted one with a space in it silently became two arguments
 * (a checkout under `~/My Projects/` could not link at all), and one with a
 * quote in it ended the argument early.
 *
 * Returns false when the argument will not fit quoted.  Callers must treat
 * that as a hard failure: a truncated command string is worse than no command
 * at all (platform_proc.h says the same about tur_shell_quote). */
bool drv_buf_put_quoted(Buf *b, const char *s) {
    char q[9000];
    if (!s || tur_shell_quote(s, q, sizeof q) != 0) return false;
    /* drv_link_command_run reads the aux_includes/aux_sources buffers this
     * helper fills as C strings with no explicit terminator.  That is safe
     * because every Buf append keeps data[len] == '\0' (buf.h).  It used to
     * hold only for buf_printf, by vsnprintf's arithmetic, so this line had to
     * be a buf_printf: a buf_puts here read one byte past the allocation
     * (docs/archive/buf-puts-breaks-the-incidental-nul-invariant.md). */
    buf_puts(b, q);
    return true;
}

/* tur-link-and-build-split-plan Phase 1: assemble and run the final link `cc`
 * command.  `inputs` is the space-joined list of primary inputs -- the single
 * generated `.c` for the monolithic path, or one-or-more `.o`/`.c` args for the
 * split/`tur link` path.  Every other argument mirrors what the old inline
 * assembly appended, in the same order, so a monolithic build (inputs = the
 * generated `.c`) produces a byte-identical command.
 *
 * Returns 0 on success, 2 on cc failure or a failed exe settle.  Does not touch
 * the generated-C temp file (the caller owns that lifecycle). */
/* Copy a file's bytes to stderr (a held-back cc output; see drv_link_command_run). */
void drv_replay_to_stderr(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) fwrite(chunk, 1, got, stderr);
    fclose(f);
}

/* `cc_stderr_path`: NULL lets cc write to tur's stderr.  A path sends cc's
 * stderr there and keeps tur's own "cc invocation failed" line quiet: a
 * caller that may retry decides later whether the output is worth showing,
 * and must replay it when it is (drv_replay_to_stderr) -- a warning is the
 * user's either way. */
int drv_link_command_run(const char *cc, const char *cc_flags,
                         const char *inputs,
                         const Buf *aux_includes, const Buf *aux_sources,
                         const Buf *autolink, bool needs_asan,
                         const Buf *cmake_flags,
                         const char **include_dirs, int n_include_dirs,
                         const char *out_path, const char *cc_stderr_path) {
    Buf cmd;
    buf_init(&cmd);
    bool quoted_ok = true;
    buf_printf(&cmd, "%s %s" TUR_EMITTED_C_CC_FLAGS " -o ", cc, cc_flags);
    quoted_ok = drv_buf_put_quoted(&cmd, out_path) && quoted_ok;
    buf_putc(&cmd, ' ');
    /* `inputs` arrives already quoted per path -- both callers build it with
     * drv_buf_put_quoted, because it is a LIST and quoting it whole would make it
     * one argument. */
    buf_puts(&cmd, inputs);
    if (aux_includes && aux_includes->len > 0) buf_puts(&cmd, aux_includes->data);
    if (aux_sources  && aux_sources->len  > 0) buf_puts(&cmd, aux_sources->data);
    if (autolink && autolink->len > 0) buf_printf(&cmd, " %s", autolink->data);
    if (needs_asan) buf_puts(&cmd, " -fsanitize=address,undefined");
    if (cmake_flags && cmake_flags->len > 0) buf_puts(&cmd, cmake_flags->data);
    for (int _i = 0; _i < n_include_dirs; _i++) {
        if (include_dirs[_i] && include_dirs[_i][0]) {
            buf_puts(&cmd, " -I");
            quoted_ok = drv_buf_put_quoted(&cmd, include_dirs[_i]) && quoted_ok;
        }
    }
    if (!quoted_ok) {
        fprintf(stderr, "tur: a path on the link line was too long to quote; "
                        "refusing to run a truncated cc command\n");
        buf_free(&cmd);
        return 2;
    }
    buf_puts(&cmd, " -lm");
#ifdef _WIN32
    buf_puts(&cmd, " -lpthread -lws2_32 -lshlwapi");
#endif
    if (cc_stderr_path) {
        buf_puts(&cmd, " 2>");
        if (!drv_buf_put_quoted(&cmd, cc_stderr_path)) {
            buf_free(&cmd);
            return 2;
        }
    }
    buf_putc(&cmd, '\0');
    /* TUR_SHOW_CC=1: print the assembled cc command.  A link error names the
     * library it could not find, never the flags that failed to find it -- this
     * is what showed the doubled -L behind
     * docs/archive/release-archive-cannot-compile.md, after three plausible
     * theories (flag ordering, quoting, a missing archive) had each been tested
     * and eliminated. */
    if (getenv("TUR_SHOW_CC")) fprintf(stderr, "CC: %s\n", cmd.data);
    int sys_rc = cc_system(cmd.data);
    buf_free(&cmd);

    if (sys_rc == 0 && tur_settle_exe_output(out_path) != 0) {
        fprintf(stderr, "tur: could not place the linked binary at '%s'\n", out_path);
        return 2;
    }
    if (sys_rc != 0) {
        if (!cc_stderr_path)
            fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }
    return 0;
}
