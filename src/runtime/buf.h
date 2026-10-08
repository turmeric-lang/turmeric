#ifndef TUR_BUF_H
#define TUR_BUF_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

/* printf-style format checking for the wrappers below: under -Wformat=2 the
 * compiler checks every call's arguments against its format and refuses a
 * non-literal one (security audit WP5).  Off on Windows, where MinGW's printf
 * archetype disagrees with the C99 specifiers used here and -Werror is off. */
#ifndef TUR_PRINTF_FMT
#  if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#    define TUR_PRINTF_FMT(fmt_idx, first_arg) \
         __attribute__((format(printf, fmt_idx, first_arg)))
#  else
#    define TUR_PRINTF_FMT(fmt_idx, first_arg)
#  endif
#endif

/* A growable byte buffer.  Invariant: once anything has been appended,
 * `data[len] == '\0'` -- every append below reserves the byte and stores the
 * NUL without counting it in `len`, so `data` can be handed to anything that
 * takes a C string.  An appended '\0' (`buf_putc(&b, '\0')`) is counted like
 * any other byte; the terminator after it is still there.  A never-written Buf
 * has `data == NULL`.  Shorten one with buf_truncate, not by assigning `len`,
 * or the invariant holds only again after the next append. */
typedef struct Buf {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_putc(Buf *b, char c);
void  buf_write(Buf *b, const char *s, size_t n);
void  buf_puts(Buf *b, const char *s); /* NUL-terminated */
void  buf_printf(Buf *b, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
void  buf_vprintf(Buf *b, const char *fmt, va_list ap) TUR_PRINTF_FMT(2, 0);
void  buf_truncate(Buf *b, size_t len); /* shorten to len (no-op if longer) */
int   buf_to_file(const Buf *b, FILE *f); /* returns 0 on success */
int   buf_to_path(const Buf *b, const char *path); /* write to file by name, returns 0 on success */
char *tur_strdup(const char *s);

#endif
