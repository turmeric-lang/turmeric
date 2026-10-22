/* tur_argcheck.h -- grammars for repo-supplied text that reaches a command
 * line or a filesystem path.
 *
 * The compiler driver takes text out of files the project it is compiling
 * supplies -- a `build.tur` manifest's `:link-flags`, a transitive spice's
 * `:c-sources`, the `__tur_autolink__` marker a module's inline C emits -- and
 * splices it into a shell command string that `system()` runs, or into a
 * directory path it then writes to.  Neither splice examined what it was
 * splicing, so `-lfoo; touch pwned` in any of them ran `touch`, and a spice
 * named `../..` chose where `tur fetch` wrote.
 *
 * Quoting alone is not the whole answer here.  The autolink string has a
 * space-joined contract three later passes depend on (they re-split it to pull
 * `-I` tokens, to drop bare `.c` sources superseded by `-lturi`, and to scan
 * for `-L`), so it cannot be quoted wholesale -- and a path a manifest chose
 * has to be *contained*, which no amount of quoting does.  So the boundary
 * gets a grammar: say what these strings are allowed to be, reject the rest
 * with a diagnostic, and quote the paths the driver itself owns.
 *
 * Header-only and dependency-free on purpose: the driver, `src/compiler/pkg.c`
 * and `src/lsp/mcp.c` all need it, and one copy of the rules is the only shape
 * that cannot drift the way the two link-flag readers did before
 * `append_manifest_link_flags` unified them.
 *
 * docs/upcoming/security-audit-plan.md, WP2 (D-1, D-5, D-6, D-9).
 */

#ifndef TUR_ARGCHECK_H
#define TUR_ARGCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Characters /bin/sh or cmd.exe reads as syntax rather than as text.  A string
 * free of every one of them is inert in either shell even unquoted, which is
 * what lets the autolink string stay space-joined.
 *
 * Space is NOT in this set: a token is checked after splitting, and a path a
 * manifest supplies is quoted rather than grammar-checked for spaces.  The
 * callers that need "no spaces either" say so themselves. */
static inline bool tur_arg_is_shell_safe(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case ';': case '&': case '|': case '`': case '$':
            case '(': case ')': case '<': case '>': case '\n':
            case '\r': case '"': case '\'': case '\\': case '*':
            case '?': case '[': case ']': case '{': case '}':
            case '!': case '#': case '~': case '^': case '\t':
                return false;
            default:
                if (*p < 0x20 || *p == 0x7f) return false;
        }
    }
    return true;
}

/* A path a manifest supplies, to be joined onto a directory we chose.
 *
 * Contained: no absolute path, no `..` segment, no Windows drive letter, no
 * backslash (which is a separator on the platform where `..` would otherwise
 * be spelled around it), and shell-safe so the join is quotable.  An empty
 * path is rejected -- it means "the parent", which every caller here gets
 * wrong. */
static inline bool tur_rel_path_ok(const char *p) {
    if (!p || !*p) return false;
    if (!tur_arg_is_shell_safe(p)) return false;
    if (p[0] == '/' || p[0] == '-') return false;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':')
        return false;                      /* C:\... */
    for (const char *q = p; *q; q++)
        if (*q == '\\') return false;
    /* No `..` as a whole segment.  `a..b` is an ordinary name. */
    const char *seg = p;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return false;
        if (!slash) break;
        seg = slash + 1;
    }
    return true;
}

/* One path SEGMENT -- a spice name, or a git ref used as half of a directory
 * name.  Stricter than a relative path: no separator at all, so
 * `spices/<name>-<ref>` cannot become `spices/../..`.  A leading `-` is
 * refused because the same string is also handed to git as a positional
 * argument, where it would read as an option. */
static inline bool tur_path_segment_ok(const char *s) {
    if (!s || !*s) return false;
    if (s[0] == '-' || s[0] == '.') return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '+';
        if (!ok) return false;
    }
    return true;
}

/* A git ref, used both as a positional argument to git and as half of the
 * `spices/<name>-<ref>` directory name.  git's own rules allow `/` in a ref
 * (`origin/main`, `release/1.2`), so this is a path segment plus `/` -- and
 * the `..` check has to come back with it, because git also forbids `..` in a
 * ref and `spices/<name>-../..` is the reason we are here. */
static inline bool tur_git_ref_ok(const char *s) {
    if (!s || !*s) return false;
    if (s[0] == '-' || s[0] == '.' || s[0] == '/') return false;
    size_t n = strlen(s);
    if (s[n - 1] == '/') return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '+' || c == '/';
        if (!ok) return false;
        if (c == '.' && p[1] == '.') return false;
    }
    return true;
}

/* A URL a manifest hands to git or to cmake's FetchContent.  Shell-safe, and
 * not option-shaped.  Deliberately not a scheme allowlist: `tur fetch` is
 * documented to take whatever git takes, including `git@host:path` and a local
 * path, and narrowing that here would break working manifests for no security
 * gain -- the injection risk is the metacharacters, which are gone. */
static inline bool tur_url_ok(const char *s) {
    if (!s || !*s) return false;
    if (s[0] == '-') return false;
    return tur_arg_is_shell_safe(s) && strchr(s, ' ') == NULL;
}

/* -------------------------------------------------------------------------
 * Link-line tokens
 * ---------------------------------------------------------------------- */

/* The value half of a flag like `-L<dir>` or `-I<dir>`: shell-safe and
 * non-empty.  Spaces are refused here, unlike in a quoted path, because these
 * ride inside a space-joined string. */
static inline bool tur_link_value_ok(const char *v) {
    if (!v || !*v) return false;
    if (strchr(v, ' ')) return false;
    return tur_arg_is_shell_safe(v);
}

/* The bare flags a contributed token may be, with no value attached.  Kept
 * short on purpose: anything that changes where the toolchain looks for things
 * has a spelling with a value and goes through the prefix table instead. */
static inline bool tur_link_bare_flag_ok(const char *t) {
    static const char *const bare[] = {
        "-pthread", "-lpthread", "-shared", "-static", "-rdynamic", "-fPIC",
        "-fpic", "-g", "-O0", "-O1", "-O2", "-O3", "-Os", "-Oz",
        "-fno-strict-aliasing", "-fvisibility=hidden", "-nostdlib",
        "-no-pie", "-pie", NULL
    };
    for (int i = 0; bare[i]; i++)
        if (strcmp(t, bare[i]) == 0) return true;
    return false;
}

/* True when `t` looks like a source or object file we would hand to cc as a
 * positional: a path ending in one of the extensions the driver already
 * splices in (`src/runtime/hamt.c` from an autolink marker, a vendored `.c`
 * from a spice's `:c-sources`, a prebuilt archive from `:link-flags`). */
static inline bool tur_link_input_path_ok(const char *t) {
    if (!t || !*t || t[0] == '-') return false;
    if (!tur_link_value_ok(t)) return false;
    static const char *const ext[] = {
        ".c", ".o", ".a", ".so", ".dylib", ".cc", ".cpp", ".m", ".obj",
        ".lib", ".dll", NULL
    };
    size_t n = strlen(t);
    for (int i = 0; ext[i]; i++) {
        size_t e = strlen(ext[i]);
        if (n > e && strcmp(t + n - e, ext[i]) == 0) return true;
    }
    return false;
}

/* One token of a link line contributed by repo-supplied text.
 *
 * `-framework` is the one two-token form in the vocabulary; a caller that
 * splits on spaces sees it as two tokens and must pass `prev` so the name half
 * is accepted.  Pass NULL for `prev` when there is no preceding token. */
static inline bool tur_link_token_ok(const char *t, const char *prev) {
    if (!t || !*t) return false;
    if (prev && strcmp(prev, "-framework") == 0)
        return tur_link_value_ok(t) && t[0] != '-';
    if (strcmp(t, "-framework") == 0) return true;
    if (t[0] != '-') return tur_link_input_path_ok(t);
    if (tur_link_bare_flag_ok(t)) return true;
    static const char *const pfx[] = {
        "-l", "-L", "-I", "-D", "-Wl,", "-Wa,", "-Wp,", "-isystem", "-F",
        "-include", "-std=", "-U", NULL
    };
    for (int i = 0; pfx[i]; i++) {
        size_t n = strlen(pfx[i]);
        if (strncmp(t, pfx[i], n) == 0) {
            if (!t[n]) return false;       /* `-l` with no library */
            return tur_link_value_ok(t);
        }
    }
    return false;
}

/* A cmake `set(<key> <val> CACHE BOOL "" FORCE)` pair, written unquoted into a
 * generated CMakeLists.txt.  A cmake identifier is narrow, and the value is
 * documented as a BOOL, so both can be tight. */
static inline bool tur_cmake_ident_ok(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static inline bool tur_cmake_value_ok(const char *s) {
    if (!s || !*s) return false;
    if (strchr(s, ' ')) return false;
    /* `$` would open a cmake variable reference, `"` would close the quoting,
     * `;` is cmake's list separator.  tur_arg_is_shell_safe covers all three
     * and more. */
    return tur_arg_is_shell_safe(s);
}

#endif /* TUR_ARGCHECK_H */
