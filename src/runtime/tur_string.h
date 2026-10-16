/* tur_string.h -- owned, immutable, refcounted String (owned-string-type-plan).
 *
 * A String is a heap payload `{ int64_t rc; int64_t len; char bytes[len+1] }`,
 * NUL-terminated so a borrowed cstr view is O(1).  The public handle is a
 * pointer to that header; callers treat it as an opaque `ptr<void>` carrier.
 *
 * Immutable + refcounted: `tur_string_retain` is an O(1) Clone, structural
 * sharing as a persistent Map/Set key is free, and there is no torn read.
 * Every transform (`concat`, `substring`, `to_upper`, ...) returns a FRESH
 * String with refcount 1; the inputs are untouched.
 *
 * Lifetimes: a freshly constructed String has refcount 1 and is owned by its
 * creator, who must `tur_string_release` it (or hand ownership to a container
 * that will).  `tur_string_cstr` returns a borrowed pointer into the payload
 * that stays valid only as long as the String is retained.
 *
 * This translation unit is deliberately independent of hamt.c except for the
 * two owned-map-key bridges at the bottom, which forward-declare the two
 * `tur_hamt_box_key*` entry points they need rather than pulling in hamt.h.
 */
#ifndef TUR_STRING_H
#define TUR_STRING_H

#include <stddef.h>
#include <stdint.h>

/* ---- construction / conversion ---------------------------------------- */

/* Copy `n` bytes from `src` into a fresh refcount-1 String (NUL-terminated).
 * `src` may be NULL when `n == 0`. */
void *tur_string_from_bytes(const char *src, int64_t n);

/* Copy a NUL-terminated C string into a fresh refcount-1 String. */
void *tur_string_from_cstr(const char *s);

/* Take OWNERSHIP of a freshly heap-allocated cstr `s`: copy its bytes into a
 * fresh String and free(s).  This is the owned-adoption bridge for the many
 * stdlib functions that return a malloc'd `cstr` ("caller frees the result") --
 * it turns such a borrowed-typed owned buffer into a real `String` with no
 * leak and no second ownership question.  `s` MUST be a heap buffer the caller
 * owns; never pass a literal or a borrow. */
void *tur_string_adopt_cstr(const char *s);

/* Format a signed 64-bit integer as a fresh decimal String (owned counterpart
 * of int->str; builds the String directly, no cstr intermediate). */
void *tur_string_from_int(int64_t v);

/* Borrow the NUL-terminated payload (no copy, no ownership transfer). */
const char *tur_string_cstr(void *s);

/* ---- refcount --------------------------------------------------------- */

/* Increment the refcount and return the same handle (the O(1) Clone). */
void *tur_string_retain(void *s);

/* Decrement the refcount; free the payload when it reaches zero. NULL-safe. */
void tur_string_release(void *s);

/* ---- queries ---------------------------------------------------------- */

int64_t tur_string_len(void *s);        /* byte length (not counting NUL) */
int     tur_string_empty(void *s);      /* 1 iff len == 0 (NULL counts empty) */
int64_t tur_string_byte_at(void *s, int64_t i); /* 0..255, or -1 out of range */
int     tur_string_eq(void *a, void *b);         /* content equality */
int64_t tur_string_cmp(void *a, void *b);        /* lexicographic: -1 / 0 / 1 */
int64_t tur_string_hash(void *s);                /* content hash over len bytes */
int     tur_string_starts_with(void *s, void *prefix);
int     tur_string_ends_with(void *s, void *suffix);
int     tur_string_contains(void *s, void *needle);

/* ---- transforms (each returns a fresh refcount-1 String) -------------- */

void *tur_string_concat(void *a, void *b);
void *tur_string_substring(void *s, int64_t start, int64_t len); /* clamped */
void *tur_string_to_upper(void *s);     /* ASCII */
void *tur_string_to_lower(void *s);     /* ASCII */
void *tur_string_trim(void *s);         /* ASCII leading/trailing whitespace */

/* ---- StringBuilder ---------------------------------------------------- */

/* Mutable growable byte buffer.  `finish` freezes the accumulated bytes into
 * an immutable String and frees the builder, so a builder is single-use. */
void   *tur_sb_new(void);
void    tur_sb_push_cstr(void *b, const char *s);
void    tur_sb_push_string(void *b, void *s);
void    tur_sb_push_byte(void *b, int64_t c);
void    tur_sb_push_range(void *b, void *s, int64_t off, int64_t len); /* clamped */
void    tur_sb_push_int(void *b, int64_t v);           /* decimal digits */
void    tur_sb_push_repeat(void *b, int64_t c, int64_t n); /* n copies of byte c */
void    tur_sb_push_slice(void *b, void *sl);          /* a StringSlice's bytes */
int64_t tur_sb_len(void *b);
void   *tur_sb_finish(void *b);         /* -> String; frees the builder */

/* ---- StringSlice: a zero-copy, bounds-checked view into a String ------ */

/* A StringSlice is `{ rc; parent String; offset; len }`.  It holds a RETAINED
 * reference to the parent String, so the borrowed byte range stays valid AND
 * immutable for the slice's lifetime -- genuinely safe zero-copy ranged access,
 * unlike a raw pointer+len view over a `cstr` whose backing buffer can vanish.
 * Sub-slicing is O(1) (a new view retaining the same parent); materialize to an
 * owned String / cstr only at the boundary where you need one. */

/* View [off, off+len) of String `s` (offset/len clamped to s's bounds).
 * Retains `s`. */
void *tur_string_slice(void *s, int64_t off, int64_t len);

/* Convenience: slice a raw cstr safely by first copying it into an owned String
 * the returned slice solely owns (freed when the slice's refcount hits zero). */
void *tur_string_slice_cstr(const char *s, int64_t off, int64_t len);

void   *tur_slice_retain(void *sl);
void    tur_slice_release(void *sl);      /* releases the parent String at rc 0 */
int64_t tur_slice_len(void *sl);
int     tur_slice_empty(void *sl);
int64_t tur_slice_byte_at(void *sl, int64_t i);   /* 0..255, or -1 out of range */
void   *tur_slice_sub(void *sl, int64_t off, int64_t len); /* sub-view, clamped */
void   *tur_slice_to_string(void *sl);    /* fresh owned String copy of the range */
const char *tur_slice_to_cstr(void *sl);  /* fresh malloc'd NUL-terminated copy */
int     tur_slice_eq(void *a, void *b);
int64_t tur_slice_cmp(void *a, void *b);  /* lexicographic: -1 / 0 / 1 */
int64_t tur_slice_hash(void *sl);         /* content hash over the range */

/* ---- owned-map-key bridge (MapKey[String], mk-owned? = 1) ------------- */

/* Copy the String's bytes into a tur_hamt_box_key box the map will own and
 * free.  This is what makes a String key safe where a borrowed cstr dangles. */
void *tur_string_box_key(void *s);

/* Address of tur_hamt_box_key_eq (the content comparator over boxed keys),
 * returned as an int so MapKey[String].mk-cmp can hand it to the runtime. */
int64_t tur_string_key_eq_addr(void);

#endif /* TUR_STRING_H */
