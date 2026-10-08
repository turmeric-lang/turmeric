#include "buf.h"

#include <stdlib.h>
#include <string.h>

static void grow(Buf *b, size_t need) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) {
        fprintf(stderr, "tur: out of memory\n");
        abort();
    }
    b->data = p;
    b->cap = cap;
}

void buf_init(Buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* Every append reserves one byte past `len` and stores a NUL there without
 * counting it, so a Buf any of these has written to is always readable as a C
 * string through `data` (see buf.h).  buf_vprintf got that for free from
 * vsnprintf; buf_putc/buf_write used to reserve exactly what they appended,
 * and a buf_puts of one of their Bufs was a heap overread
 * (docs/archive/buf-puts-breaks-the-incidental-nul-invariant.md). */
void buf_putc(Buf *b, char c) {
    if (b->len + 2 > b->cap) grow(b, b->len + 2);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void buf_write(Buf *b, const char *s, size_t n) {
    if (!n) return;
    if (b->len + n + 1 > b->cap) grow(b, b->len + n + 1);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s) {
    buf_write(b, s, strlen(s));
}

void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return;

    size_t need = (size_t)n + 1;
    if (b->len + need > b->cap) grow(b, b->len + need);

    int written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    if (written < 0) return;
    b->len += (size_t)written;
}

void buf_truncate(Buf *b, size_t len) {
    if (len >= b->len) return;
    b->len = len;
    b->data[len] = '\0';
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

int buf_to_file(const Buf *b, FILE *f) {
    if (b->len == 0) return 0;
    size_t n = fwrite(b->data, 1, b->len, f);
    return n == b->len ? 0 : -1;
}

int buf_to_path(const Buf *b, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = buf_to_file(b, f);
    fclose(f);
    return rc;
}

char *tur_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (!copy) {
        fprintf(stderr, "tur: out of memory\n");
        abort();
    }
    memcpy(copy, s, n);
    return copy;
}
