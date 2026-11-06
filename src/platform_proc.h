/* platform_proc.h - portable pieces of a shell-string subprocess.
 *
 * Every `system()` / `popen()` call site in the package and install paths
 * builds a SHELL COMMAND STRING.  That string is handed to /bin/sh on POSIX and
 * to cmd.exe on Windows, and the two agree on almost none of its syntax.  Two
 * differences account for every failure observed so far:
 *
 *   1. The null device.  `2>/dev/null` is a redirect to a path cmd.exe cannot
 *      resolve; it prints "The system cannot find the path specified." and the
 *      command fails.  cmd.exe spells it `NUL`.
 *
 *   2. Quoting.  cmd.exe does not treat `'` as a quote character AT ALL, so a
 *      single-quoted argument is passed through with the quotes still attached
 *      and the callee sees a literal `'C:\path'`.  Windows wants double quotes.
 *
 * Both failures are SILENT in the way that matters: the command runs, does
 * nothing, and returns a status the caller may or may not check.  `tur new`
 * scaffolded a project and skipped `git init` while exiting 0, and reported
 * "git config user.name not set" on a machine where it was plainly set --
 * because the probe that read it had failed, not because the value was absent.
 *
 * On every non-Windows platform this compiles to the POSIX spellings, so it is
 * safe (and cheap) to include unconditionally.
 *
 * See docs/archive/windows-subprocess-and-shared-lib-gaps.md.
 */

#ifndef TUR_PLATFORM_PROC_H
#define TUR_PLATFORM_PROC_H

#include <stddef.h>
#include <string.h>

/* TUR_NO_SUBPROCESS: the platform forbids starting another program.
 *
 * iOS, tvOS, watchOS and visionOS (TARGET_OS_IPHONE covers all four) mark
 * fork() and system() unavailable in their SDKs, and a sandboxed app may not
 * spawn a process anyway.  Code that would shell out or fork tests this and
 * reports the operation unsupported instead: the interpreter's
 * process/spawn-raw native answers -ENOSYS (as it does under Emscripten) and
 * the REPL's spice auto-discovery, which builds a dylib in a `tur`
 * subprocess and dlopens it, finds no project.  See
 * docs/archive/turi-ios-readiness-unverified.md, items 3 and 6. */
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#  define TUR_NO_SUBPROCESS 1
#else
#  define TUR_NO_SUBPROCESS 0
#endif

/* The null device, as the platform's shell spells it.  Use in a redirect:
 *   snprintf(cmd, sizeof cmd, "git ... 2>" TUR_DEVNULL);
 * It is a string literal so it concatenates into a format string. */
#ifdef _WIN32
#  define TUR_DEVNULL "NUL"
#else
#  define TUR_DEVNULL "/dev/null"
#endif

/* `cd`, spelled so it actually changes directory.
 *
 * cmd.exe keeps a current directory PER DRIVE: from C:, a bare
 * `cd D:\x` succeeds, sets D:'s current directory, and leaves the
 * process on C:.  A command of the form `cd <dir> && <build>` then builds in
 * the wrong place while reporting nothing.  /d makes it change drive too. */
#ifdef _WIN32
#  define TUR_CD "cd /d"
#else
#  define TUR_CD "cd"
#endif

/* The separator between entries in $PATH.
 *
 * Windows uses ";", and getting this wrong is worse than finding nothing: a
 * walk that splits on ":" tears every entry at its DRIVE COLON, so
 * `C:\\Users\\x` becomes the two entries `C` and
 * `\\Users\\x` and every candidate path built from them is
 * malformed. */
#ifdef _WIN32
#  define TUR_PATH_LIST_SEP ';'
#else
#  define TUR_PATH_LIST_SEP ':'
#endif

/* Do these two spellings name the same directory, as far as a string compare
 * can tell?
 *
 * On Windows both the separator and the case vary freely between one part of
 * the system and another: tur records its bin dir as `C:/Users/.../bin` while
 * the PATH a shell exports spells it `C:\\Users\\...\\bin`, and an exact
 * strcmp then says two spellings of one directory are different places.  On
 * POSIX a path is case-sensitive and a backslash is an ordinary character, so
 * this is a plain comparison and must stay one. */
static inline int tur_path_seg_eq(const char *a, size_t alen,
                                  const char *b, size_t blen) {
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
#ifdef _WIN32
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
#endif
        if (ca != cb) return 0;
    }
    return 1;
}

/* Quote one argument for the platform's shell.
 *
 * Writes a NUL-terminated quoted form of `in` into `out`; returns 0 on success
 * and -1 if it would not fit (the caller must treat that as a hard failure --
 * a truncated command string is worse than no command at all).
 *
 * POSIX: single quotes, with the standard `'\''` break-out for an embedded
 * quote, which is safe against every metacharacter sh has.
 *
 * Windows: double quotes.  Inside them cmd.exe leaves &, |, <, >, ^ alone, and
 * the CRT's argument parser handles the rest.  An embedded double quote is
 * escaped as \", and a run of backslashes immediately BEFORE such a quote has
 * to be doubled -- that is the CRT's documented rule, and getting it wrong is
 * how a trailing separator in a directory path (`C:\dir\`) silently escapes the
 * closing quote and swallows the next argument.
 */
static inline int tur_shell_quote(const char *in, char *out, size_t cap) {
    if (!in || !out || cap < 3) return -1;
    size_t o = 0;
#ifdef _WIN32
    out[o++] = '"';
    size_t bs = 0;                       /* pending backslashes */
    for (const char *p = in; *p; p++) {
        if (*p == '\\') { bs++; continue; }
        if (*p == '"') {
            /* double the run that precedes the quote, then escape the quote */
            for (size_t i = 0; i < bs * 2 + 1; i++) {
                if (o + 2 >= cap) return -1;
                out[o++] = '\\';
            }
            bs = 0;
            if (o + 2 >= cap) return -1;
            out[o++] = '"';
            continue;
        }
        for (size_t i = 0; i < bs; i++) {
            if (o + 2 >= cap) return -1;
            out[o++] = '\\';
        }
        bs = 0;
        if (o + 2 >= cap) return -1;
        out[o++] = *p;
    }
    /* a trailing run abuts the closing quote, so it must be doubled too */
    for (size_t i = 0; i < bs * 2; i++) {
        if (o + 2 >= cap) return -1;
        out[o++] = '\\';
    }
    if (o + 2 >= cap) return -1;
    out[o++] = '"';
#else
    out[o++] = '\'';
    for (const char *p = in; *p; p++) {
        if (*p == '\'') {
            if (o + 5 >= cap) return -1;
            out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\'';
            continue;
        }
        if (o + 2 >= cap) return -1;
        out[o++] = *p;
    }
    if (o + 2 >= cap) return -1;
    out[o++] = '\'';
#endif
    out[o] = '\0';
    return 0;
}

/* Wrap a complete shell command string for system()/popen().
 *
 * POSIX: nothing to do.
 *
 * Windows: system() runs `cmd.exe /c <string>`, and cmd.exe has a rule that
 * catches every caller eventually -- if the string begins with a quote, it
 * strips the FIRST and LAST quote characters and runs what is left.  With one
 * quoted program path that is harmless; with a quoted program AND quoted
 * arguments it tears the command in half, and cmd.exe reports "The filename,
 * directory name, or volume label syntax is incorrect."  The documented
 * remedy is an extra enclosing pair, so the pair cmd.exe eats is one we added.
 *
 * Returns 0 on success, -1 if it would not fit.
 */
static inline int tur_shell_command(const char *cmd, char *out, size_t cap) {
    if (!cmd || !out) return -1;
#ifdef _WIN32
    size_t n = strlen(cmd);
    if (n + 3 > cap) return -1;
    out[0] = '"';
    memcpy(out + 1, cmd, n);
    out[n + 1] = '"';
    out[n + 2] = '\0';
#else
    size_t n = strlen(cmd);
    if (n + 1 > cap) return -1;
    memcpy(out, cmd, n + 1);
#endif
    return 0;
}

#endif /* TUR_PLATFORM_PROC_H */
