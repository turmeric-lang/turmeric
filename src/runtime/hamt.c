/* hamt.c - Persistent Hash Array Mapped Trie implementation (Phase P1)
 *
 * A HAMT is an immutable hash map with structural sharing.
 * Uses 5-bit hash chunks (32 slots per level) for balanced trie depth.
 *
 * Memory management: reference counting (Phase P1 v1).
 * Collision handling: linked list in v1.
 * Hash function: xxHash64 (speed-optimised, non-cryptographic).
 *
 * See docs/hamt-plan.md for full design and phase breakdown.
 */

#include "hamt.h"
#include "region.h"   /* region-lock-hardening: the store-side note in tur_hamt_set */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* r7rs-gc: allocate through the archive's hook (rt_alloc.h) -- libc unless a
 * collector installed itself.  Object-like, so a `free` passed by name is the
 * hook's too. */
#include "rt_alloc.h"
#define malloc  tur_rt_malloc
#define calloc  tur_rt_calloc
#define realloc tur_rt_realloc
#define free    tur_rt_free

/* ============================================================================
 * xxHash64 - Minimal implementation for HAMT
 * ============================================================================
 */

#define XXH_rotl64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))

#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x27D4EB2F165667C5ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL

static uint64_t xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = XXH_rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

static uint64_t xxh64_finalize(uint64_t acc, uint64_t input) {
    acc ^= xxh64_round(0, input);
    acc *= XXH_PRIME64_3;
    acc ^= acc >> 33;
    acc *= XXH_PRIME64_2;
    acc ^= acc >> 29;
    acc *= XXH_PRIME64_3;
    acc ^= acc >> 32;
    return acc;
}

uint64_t tur_hamt_hash_xxh64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t acc = 0;

    if (len >= 32) {
        uint64_t v1 = XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = XXH_PRIME64_2;
        uint64_t v3 = 0;
        uint64_t v4 = XXH_PRIME64_1;

        do {
            v1 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v1 = XXH_rotl64(v1, 31);
            v1 *= XXH_PRIME64_1;
            p += 8;

            v2 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v2 = XXH_rotl64(v2, 31);
            v2 *= XXH_PRIME64_1;
            p += 8;

            v3 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v3 = XXH_rotl64(v3, 31);
            v3 *= XXH_PRIME64_1;
            p += 8;

            v4 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v4 = XXH_rotl64(v4, 31);
            v4 *= XXH_PRIME64_1;
            p += 8;

            len -= 32;
        } while (len >= 32);

        acc = XXH_rotl64(v1, 1) + XXH_rotl64(v2, 7) + XXH_rotl64(v3, 12) + XXH_rotl64(v4, 18);
        acc = xxh64_round(acc, v1);
        acc = xxh64_round(acc, v2);
        acc = xxh64_round(acc, v3);
        acc = xxh64_round(acc, v4);
    } else {
        acc = XXH_PRIME64_5 + len;

        while (len >= 8) {
            uint64_t k = *(uint64_t *)p;
            acc = xxh64_round(acc, k);
            p += 8;
            len -= 8;
        }

        if (len >= 4) {
            acc = xxh64_round(acc, *(uint32_t *)p);
            p += 4;
            len -= 4;
        }

        while (len > 0) {
            acc = xxh64_round(acc, *p);
            p += 1;
            len -= 1;
        }
    }

    return xxh64_finalize(acc, len);
}

uint64_t tur_hamt_hash_str(const char *str) {
    return tur_hamt_hash_xxh64(str, strlen(str));
}

uint64_t tur_hamt_hash_ptr(void *ptr) {
    return (uint64_t)(uintptr_t)ptr;
}

/* ============================================================================
 * HAMT Constants
 * ============================================================================
 */

#define HAMT_BITS_PER_LEVEL 5
#define HAMT_SLOTS_PER_LEVEL (1 << HAMT_BITS_PER_LEVEL)
#define HAMT_MASK_PER_LEVEL ((1 << HAMT_BITS_PER_LEVEL) - 1)

/* ============================================================================
 * Memory Allocation
 * ============================================================================
 */

static void *hamt_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "hamt: out of memory\n");
        abort();
    }
    return ptr;
}

/* Allocate a HAMT node with initial ref_count=1 */
HamtNode *tur_hamt_node_alloc(size_t size) {
    HamtNode *n = (HamtNode *)hamt_malloc(size);
    n->ref_count = 1;
    return n;
}

/* Retain a node (increment ref count) */
void tur_hamt_node_retain(HamtNode *n) {
    if (n) {
        n->ref_count++;
    }
}

/* Release a node (decrement ref count; free if reaches zero) */
void tur_hamt_node_release(HamtNode *n);

/* Allocate a Hamt root with initial ref_count=1 */
static Hamt *hamt_alloc_empty(void) {
    Hamt *m = (Hamt *)hamt_malloc(sizeof(Hamt));
    m->root = NULL;
    m->count = 0;
    m->ref_count = 1;
    m->key_ops.retain = NULL;
    m->key_ops.release = NULL;
    m->key_ops.eq = NULL;  /* GDE3: no stamped comparator initially */
    m->val_owned = false;  /* multi-word-value boxing: off by default */
    m->val_ops.retain = NULL;
    m->val_ops.release = NULL;
    return m;
}

/* ============================================================================
 * Bitmap Node Helpers
 * ============================================================================
 */

static uint32_t popcount32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return x & 0x3F;
#endif
}

/* Node sizes.  HamtNode is a tagged union whose largest arm is the 32-slot
 * array, so sizeof(HamtNode) is 264 B on a 64-bit host.  A bitmap node holds
 * only popcount(bitmap) children and a collision node two words, so each is
 * allocated at the size of its own arm -- a full-size allocation made every
 * entry of a persistent map cost ~360 B, most of it padding
 * (docs/archive/hamt-nodes-allocated-at-full-array-size.md).  Only the array
 * arm is ever allocated at sizeof(HamtNode). */
#define HAMT_BITMAP_NODE_SIZE(child_count) \
    (offsetof(HamtNode, as.bitmap.children) + sizeof(HamtNode *) * (child_count))
#define HAMT_COLLISION_NODE_SIZE \
    (offsetof(HamtNode, as) + sizeof(HamtNodeCollision))

static HamtNode *bitmap_node_create(uint32_t bitmap) {
    size_t child_count = popcount32(bitmap);
    HamtNode *n = tur_hamt_node_alloc(HAMT_BITMAP_NODE_SIZE(child_count));
    n->type = HAMT_NODE_BITMAP;
    n->as.bitmap.bitmap = bitmap;
    return n;
}

static HamtNode *array_node_create(void) {
    HamtNode *n = tur_hamt_node_alloc(sizeof(HamtNode));
    n->type = HAMT_NODE_ARRAY;
    for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
        n->as.array.children[i] = NULL;
    }
    return n;
}

static HamtNode *collision_node_create(uint32_t hash_prefix,
                                       uint64_t hash, void *key, void *val) {
    HamtNode *n = tur_hamt_node_alloc(HAMT_COLLISION_NODE_SIZE);
    n->type = HAMT_NODE_COLLISION;
    n->as.collision.hash_prefix = hash_prefix;
    n->as.collision.entries = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
    n->as.collision.entries->hash = hash;
    n->as.collision.entries->key = key;
    n->as.collision.entries->val = val;
    n->as.collision.entries->next = NULL;
    return n;
}

/* ============================================================================
 * Hash utilities
 * ============================================================================
 */

static uint32_t hash_chunk(uint64_t hash, uint32_t level) {
    return (uint32_t)((hash >> (level * HAMT_BITS_PER_LEVEL)) & HAMT_MASK_PER_LEVEL);
}

/* ============================================================================
 * Key equality override (TCE4)
 * ============================================================================
 *
 * By default the HAMT treats keys as opaque words and compares them by
 * identity (`a == b`). That is correct for int-carrier keys, but wrong for
 * content-typed keys such as strings, where two distinct pointers may hold
 * equal text. The `_eq` family of public entry points installs a key
 * equality function for the duration of one operation; comparison sites go
 * through `keys_equal`, which falls back to identity when no override is set.
 *
 * The override is thread-local (each thread has its own current map context)
 * and saved/restored around each `_eq` call so nested operations -- e.g. a
 * map whose values are themselves maps -- compose correctly.
 */

static _Thread_local bool (*g_hamt_key_eq)(int64_t, int64_t) = NULL;

/* Context-carrying comparator (turi-map-set-hamt-interpreter-gap.md, prereq 2a).
 * The plain g_hamt_key_eq cannot carry state, so a Turmeric *closure* comparator
 * (which needs its captured environment + a TuriEnv* to run) has no way to ride
 * along to the collision-time compare.  g_hamt_key_eq_ctx_fn adds the missing
 * `void *ctx` word.  It is installed by the tur_hamt_*_eq_ctx family and, when
 * set, takes precedence over the no-ctx hook in keys_equal.  The two families
 * are mutually exclusive per dynamic scope: every entry point clears the other
 * family's hook for its duration (see hamt_save_eq_hooks), so a plain op nested
 * inside a ctx op -- or vice versa -- composes correctly. */
static _Thread_local bool (*g_hamt_key_eq_ctx_fn)(int64_t, int64_t, void *) = NULL;
static _Thread_local void *g_hamt_key_eq_ctx = NULL;

static inline bool keys_equal(void *a, void *b) {
    if (g_hamt_key_eq_ctx_fn) {
        return g_hamt_key_eq_ctx_fn((int64_t)(intptr_t)a, (int64_t)(intptr_t)b,
                                    g_hamt_key_eq_ctx);
    }
    if (g_hamt_key_eq) {
        return g_hamt_key_eq((int64_t)(intptr_t)a, (int64_t)(intptr_t)b);
    }
    return a == b;
}

/* Save/restore of BOTH comparator hooks (plain + ctx).  A plain _eq op and a
 * ctx _eq op each install their own hook and clear the other's, so exactly one
 * is active during keys_equal; on restore the caller's hooks resume.  Clearing
 * a NULL hook is a no-op, so threading this through the existing plain _eq
 * entry points is behavior-preserving for every current caller (none of which
 * installs a ctx hook). */
typedef struct {
    bool (*eq)(int64_t, int64_t);
    bool (*eq_ctx_fn)(int64_t, int64_t, void *);
    void *eq_ctx;
} hamt_eq_hook_save;

static inline hamt_eq_hook_save hamt_save_eq_hooks(void) {
    hamt_eq_hook_save s = { g_hamt_key_eq, g_hamt_key_eq_ctx_fn, g_hamt_key_eq_ctx };
    return s;
}

static inline void hamt_restore_eq_hooks(hamt_eq_hook_save s) {
    g_hamt_key_eq        = s.eq;
    g_hamt_key_eq_ctx_fn = s.eq_ctx_fn;
    g_hamt_key_eq_ctx    = s.eq_ctx;
}

/* ============================================================================
 * Boxed-key ownership (WKC2)
 * ============================================================================
 *
 * A boxed key is `[refcount|size][payload bytes]`; the public payload pointer
 * points past the 16-byte header (so payload alignment is malloc's, i.e. good
 * for any standard key type).  Ownership retain/release run through a
 * thread-local hook installed by the _eq_owned operations and by tur_hamt_free
 * (which reads it back from the map's stored ops).  When the hook is NULL --
 * the default for one-word/inline keys -- retain/release are no-ops and the
 * HAMT never touches key lifetime, identical to pre-WKC behavior.
 */

typedef struct {
    uint64_t refcount;
    uint64_t size;
} tur_hamt_box_hdr;

void *tur_hamt_box_key(const void *src, size_t n) {
    tur_hamt_box_hdr *h = (tur_hamt_box_hdr *)hamt_malloc(sizeof(tur_hamt_box_hdr) + n);
    h->refcount = 1;
    h->size = (uint64_t)n;
    void *payload = (char *)h + sizeof(tur_hamt_box_hdr);
    if (src && n) memcpy(payload, src, n);
    return payload;
}

void tur_hamt_box_retain(void *boxed_key) {
    if (!boxed_key) return;
    tur_hamt_box_hdr *h =
        (tur_hamt_box_hdr *)((char *)boxed_key - sizeof(tur_hamt_box_hdr));
    h->refcount++;
}

void tur_hamt_box_release(void *boxed_key) {
    if (!boxed_key) return;
    tur_hamt_box_hdr *h =
        (tur_hamt_box_hdr *)((char *)boxed_key - sizeof(tur_hamt_box_hdr));
    if (--h->refcount == 0) free(h);
}

tur_hamt_key_ops tur_hamt_box_key_ops(void) {
    tur_hamt_key_ops ops = { tur_hamt_box_retain, tur_hamt_box_release, NULL };
    return ops;
}

bool tur_hamt_box_key_eq(int64_t a, int64_t b) {
    void *pa = (void *)(intptr_t)a;
    void *pb = (void *)(intptr_t)b;
    if (pa == pb) return true;
    if (!pa || !pb) return false;
    const tur_hamt_box_hdr *ha =
        (const tur_hamt_box_hdr *)((const char *)pa - sizeof(tur_hamt_box_hdr));
    const tur_hamt_box_hdr *hb =
        (const tur_hamt_box_hdr *)((const char *)pb - sizeof(tur_hamt_box_hdr));
    if (ha->size != hb->size) return false;
    return memcmp(pa, pb, (size_t)ha->size) == 0;
}

/* Thread-local key-ownership hook, mirroring g_hamt_key_eq: saved/restored
 * around each _eq_owned call and around tur_hamt_free so nested operations
 * (and the deferred free of structurally-shared nodes) see the right ops. */
static _Thread_local void (*g_hamt_key_retain)(void *) = NULL;
static _Thread_local void (*g_hamt_key_release)(void *) = NULL;

static inline void key_retain_hook(void *k) {
    if (g_hamt_key_retain && k) g_hamt_key_retain(k);
}
static inline void key_release_hook(void *k) {
    if (g_hamt_key_release && k) g_hamt_key_release(k);
}

/* Multi-word-value boxing: value-box ownership hooks, exact mirror of the key
 * hooks above.  An owned value is a tur_hamt_box_key box (same as an owned key),
 * so the retain/release ops are always the box refcount ops -- the hook is
 * NULL (no-op) unless the current operation / free is over a val_owned map. */
static _Thread_local void (*g_hamt_val_retain)(void *) = NULL;
static _Thread_local void (*g_hamt_val_release)(void *) = NULL;

static inline void val_retain_hook(void *v) {
    if (g_hamt_val_retain && v) g_hamt_val_retain(v);
}
static inline void val_release_hook(void *v) {
    if (g_hamt_val_release && v) g_hamt_val_release(v);
}

/* ============================================================================
 * Entry lookup in collision nodes
 * ============================================================================
 */

static HamtEntry *find_entry(HamtEntry *entries, uint64_t hash, void *key) {
    for (HamtEntry *e = entries; e; e = e->next) {
        if (e->hash == hash && keys_equal(e->key, key)) {
            return e;
        }
    }
    return NULL;
}

static bool collision_has(HamtEntry *entries, uint64_t hash, void *key) {
    return find_entry(entries, hash, key) != NULL;
}

static void *collision_get(HamtEntry *entries, uint64_t hash, void *key) {
    HamtEntry *e = find_entry(entries, hash, key);
    return e ? e->val : NULL;
}

/* ============================================================================
 * Node deep free (for cleanup)
 * ============================================================================
 */

static void node_free_recursive(HamtNode *n) {
    if (!n) return;

    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            for (uint32_t i = 0; i < 32; i++) {
                if (bitmap & (UINT32_C(1) << i)) {
                    uint32_t child_idx = 0;
                    uint32_t mask = 1;
                    for (uint32_t j = 0; j < i; j++, mask <<= 1) {
                        if (bitmap & mask) child_idx++;
                    }
                    tur_hamt_node_release(n->as.bitmap.children[child_idx]);
                }
            }
            break;
        }
        case HAMT_NODE_ARRAY: {
            for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                tur_hamt_node_release(n->as.array.children[i]);
            }
            break;
        }
        case HAMT_NODE_COLLISION: {
            HamtEntry *e = n->as.collision.entries;
            while (e) {
                HamtEntry *next = e->next;
                /* WKC2: this entry's reference to its (boxed) key goes away. */
                key_release_hook(e->key);
                val_release_hook(e->val);   /* ... and its (boxed) value */
                free(e);
                e = next;
            }
            break;
        }
    }
}

void tur_hamt_node_release(HamtNode *n) {
    if (n && --n->ref_count == 0) {
        node_free_recursive(n);
        free(n);
    }
}

/* ============================================================================
 * Node copy functions (for structural sharing)
 * ============================================================================
 */

static HamtNode *bitmap_node_copy(HamtNode *src) {
    uint32_t bitmap = src->as.bitmap.bitmap;
    uint32_t child_count = popcount32(bitmap);
    HamtNode *dst = tur_hamt_node_alloc(HAMT_BITMAP_NODE_SIZE(child_count));
    dst->type = HAMT_NODE_BITMAP;
    dst->as.bitmap.bitmap = bitmap;

    for (uint32_t i = 0; i < child_count; i++) {
        dst->as.bitmap.children[i] = src->as.bitmap.children[i];
        if (dst->as.bitmap.children[i]) {
            tur_hamt_node_retain(dst->as.bitmap.children[i]);
        }
    }

    return dst;
}

static HamtNode *array_node_copy(HamtNode *src) {
    HamtNode *dst = tur_hamt_node_alloc(sizeof(HamtNode));
    dst->type = HAMT_NODE_ARRAY;

    for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
        dst->as.array.children[i] = src->as.array.children[i];
        if (dst->as.array.children[i]) {
            tur_hamt_node_retain(dst->as.array.children[i]);
        }
    }

    return dst;
}

static HamtNode *collision_node_copy(HamtNode *src) {
    HamtNode *dst = tur_hamt_node_alloc(HAMT_COLLISION_NODE_SIZE);
    dst->type = HAMT_NODE_COLLISION;
    dst->as.collision.hash_prefix = src->as.collision.hash_prefix;

    HamtEntry **tail = &dst->as.collision.entries;
    for (HamtEntry *e = src->as.collision.entries; e; e = e->next) {
        *tail = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
        (*tail)->hash = e->hash;
        (*tail)->key = e->key;
        (*tail)->val = e->val;
        (*tail)->next = NULL;
        /* WKC2: the duplicated entry is a new reference to the (shared) key;
         * retain so the box outlives whichever copy is freed last. */
        key_retain_hook((*tail)->key);
        val_retain_hook((*tail)->val);   /* ... and to the (shared) boxed value */
        tail = &(*tail)->next;
    }

    return dst;
}

/* ============================================================================
 * Insert operation
 * ============================================================================
 */

static HamtNode *node_insert(HamtNode *n, uint64_t hash, void *key, void *val,
                             uint32_t level);

static HamtNode *node_insert(HamtNode *n, uint64_t hash, void *key, void *val,
                             uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);

    if (!n) {
        return collision_node_create(chunk, hash, key, val);
    }

    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;

            bool has_child = (bitmap & (UINT32_C(1) << chunk)) != 0;

            if (has_child) {
                uint32_t idx = 0;
                uint32_t mask = 1;
                for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                    if (bitmap & mask) idx++;
                }

                HamtNode *new_child = node_insert(n->as.bitmap.children[idx], hash, key, val, level + 1);

                if (new_child == n->as.bitmap.children[idx]) {
                    return n;
                }

                HamtNode *new_node = bitmap_node_copy(n);
                /* bitmap_node_copy retained children[idx]; release it before overwriting. */
                tur_hamt_node_release(new_node->as.bitmap.children[idx]);
                /* new_child has ref_count=1 from node_insert; new_node is the sole owner. */
                new_node->as.bitmap.children[idx] = new_child;
                return new_node;
            } else {
                uint32_t new_bitmap = bitmap | (UINT32_C(1) << chunk);
                HamtNode *new_node = bitmap_node_create(new_bitmap);

                uint32_t new_idx = 0;
                uint32_t pos = 0;
                for (uint32_t i = 0; i < 32; i++) {
                    if (bitmap & (UINT32_C(1) << i)) {
                        new_node->as.bitmap.children[new_idx] = n->as.bitmap.children[pos];
                        tur_hamt_node_retain(new_node->as.bitmap.children[new_idx]);
                        new_idx++;
                        pos++;
                    } else if (i == chunk) {
                        new_node->as.bitmap.children[new_idx] = collision_node_create(chunk, hash, key, val);
                        new_idx++;
                    }
                }

                return new_node;
            }
        }

        case HAMT_NODE_ARRAY: {
            HamtNode *new_child = node_insert(n->as.array.children[chunk], hash, key, val, level + 1);

            if (new_child == n->as.array.children[chunk]) {
                return n;
            }

            HamtNode *new_node = array_node_copy(n);
            /* array_node_copy retained children[chunk]; release it before overwriting. */
            tur_hamt_node_release(new_node->as.array.children[chunk]);
            /* new_child has ref_count=1 from node_insert; new_node is the sole owner. */
            new_node->as.array.children[chunk] = new_child;

            return new_node;
        }

        case HAMT_NODE_COLLISION: {
            /* Use the actual hash chunk of existing entries at the CURRENT level,
             * not the stored hash_prefix (which was set at the creation level). */
            uint32_t existing_chunk = hash_chunk(n->as.collision.entries->hash, level);

            if (existing_chunk != chunk) {
                /* Different slot at this level — expand into a bitmap node. */
                uint32_t new_bitmap = (UINT32_C(1) << existing_chunk) | (UINT32_C(1) << chunk);

                HamtNode *new_node = bitmap_node_create(new_bitmap);

                uint32_t idx = 0;
                uint32_t mask = 1;
                for (uint32_t i = 0; i < existing_chunk; i++, mask <<= 1) {
                    if (new_bitmap & mask) idx++;
                }
                new_node->as.bitmap.children[idx] = n;
                tur_hamt_node_retain(n);

                idx = 0;
                mask = 1;
                for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                    if (new_bitmap & mask) idx++;
                }
                /* collision_node_create returns ref_count=1; new_node is the sole owner. */
                new_node->as.bitmap.children[idx] = collision_node_create(chunk, hash, key, val);

                return new_node;
            }

            /* Same chunk at this level — check for exact key match (update). */
            for (HamtEntry **e = &n->as.collision.entries; *e; e = &(*e)->next) {
                if ((*e)->hash == hash && keys_equal((*e)->key, key)) {
                    HamtNode *new_node = collision_node_copy(n);
                    for (HamtEntry *f = new_node->as.collision.entries; f; f = f->next) {
                        if (f->hash == hash && keys_equal(f->key, key)) {
                            /* Update replaces the value: collision_node_copy just
                             * retained the OLD boxed value, and the incoming `val`
                             * carries the caller's fresh reference -- so release the
                             * old box once (undo the copy-retain) before overwriting.
                             * The key is unchanged, so its copy-retain stands. */
                            val_release_hook(f->val);
                            f->val = val;
                            break;
                        }
                    }
                    return new_node;
                }
            }

            /* Same chunk, no key match.  Check whether this is a true hash collision
             * (all existing entries share the same full 64-bit hash) or just a
             * chunk collision at this level that must be split deeper. */
            if (n->as.collision.entries->hash == hash) {
                /* True hash collision — add new entry to the chain. */
                HamtNode *new_node = collision_node_copy(n);
                HamtEntry *new_entry = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
                new_entry->hash = hash;
                new_entry->key = key;
                new_entry->val = val;
                new_entry->next = new_node->as.collision.entries;
                new_node->as.collision.entries = new_entry;
                return new_node;
            }

            /* Different full hash but same chunk at this level — must diverge at
             * level+1.  Recurse into this collision node one level deeper and wrap
             * the result in a single-slot bitmap so the level invariant is maintained. */
            HamtNode *inner = node_insert(n, hash, key, val, level + 1);
            uint32_t wrapper_bitmap = (1u << chunk);
            HamtNode *wrapper = bitmap_node_create(wrapper_bitmap);
            /* inner has ref_count=1 from the recursive call; wrapper is the sole owner. */
            wrapper->as.bitmap.children[0] = inner;
            return wrapper;
        }
    }

    return n;
}

/* ============================================================================
 * Delete operation
 * ============================================================================
 */

static HamtNode *node_delete(HamtNode *n, uint64_t hash, void *key, uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);

    if (!n) {
        return NULL;
    }

    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;

            if (!(bitmap & (UINT32_C(1) << chunk))) {
                return n;
            }

            uint32_t idx = 0;
            uint32_t mask = 1;
            for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                if (bitmap & mask) idx++;
            }

            HamtNode *new_child = node_delete(n->as.bitmap.children[idx], hash, key, level + 1);

            if (new_child == n->as.bitmap.children[idx]) {
                return n;
            }

            if (!new_child) {
                /* The deleted child is simply omitted from the freshly built
                 * new_node below (only surviving siblings are copied + retained).
                 * Do NOT release children[idx] here: it belongs to the old,
                 * shared node `n`, which is persistent and still owns that
                 * reference. Releasing it drops `n`'s ref and, once `n` is later
                 * freed, node_free_recursive releases the child a second time --
                 * a double free. */
                uint32_t new_bitmap = bitmap & ~(UINT32_C(1) << chunk);
                uint32_t new_child_count = popcount32(new_bitmap);

                if (new_child_count == 0) {
                    return NULL;
                }

                HamtNode *new_node = bitmap_node_create(new_bitmap);
                uint32_t old_idx = 0;
                uint32_t new_idx = 0;
                for (uint32_t i = 0; i < 32; i++) {
                    if (bitmap & (UINT32_C(1) << i)) {
                        if (i == chunk) {
                            old_idx++;
                        } else {
                            new_node->as.bitmap.children[new_idx] = n->as.bitmap.children[old_idx];
                            tur_hamt_node_retain(new_node->as.bitmap.children[new_idx]);
                            new_idx++;
                            old_idx++;
                        }
                    }
                }
                return new_node;
            } else {
                HamtNode *new_node = bitmap_node_copy(n);
                /* bitmap_node_copy retained children[idx]; release before overwriting. */
                tur_hamt_node_release(new_node->as.bitmap.children[idx]);
                /* new_child has ref_count=1 from node_delete; new_node is the sole owner. */
                new_node->as.bitmap.children[idx] = new_child;
                return new_node;
            }
        }

        case HAMT_NODE_ARRAY: {
            HamtNode *new_child = node_delete(n->as.array.children[chunk], hash, key, level + 1);

            if (new_child == n->as.array.children[chunk]) {
                return n;
            }

            if (!new_child) {
                /* Same invariant as the bitmap collapse arm: the deleted child
                 * is omitted from the freshly built new_node (surviving siblings
                 * are copied + retained). Releasing children[chunk] here would
                 * drop a reference the old, shared node `n` still owns, causing a
                 * double free when `n` is later freed. */
                HamtNode *new_node = array_node_create();
                for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                    if (i == chunk) {
                        new_node->as.array.children[i] = NULL;
                    } else {
                        new_node->as.array.children[i] = n->as.array.children[i];
                        if (new_node->as.array.children[i]) {
                            tur_hamt_node_retain(new_node->as.array.children[i]);
                        }
                    }
                }
                return new_node;
            } else {
                HamtNode *new_node = array_node_copy(n);
                /* array_node_copy retained children[chunk]; release before overwriting. */
                tur_hamt_node_release(new_node->as.array.children[chunk]);
                /* new_child has ref_count=1 from node_delete; new_node is the sole owner. */
                new_node->as.array.children[chunk] = new_child;
                return new_node;
            }
        }

        case HAMT_NODE_COLLISION: {
            for (HamtEntry *e = n->as.collision.entries; e; e = e->next) {
                if (e->hash == hash && keys_equal(e->key, key)) {
                    HamtNode *new_node = collision_node_copy(n);
                    HamtEntry *new_prev = NULL;
                    for (HamtEntry *new_e = new_node->as.collision.entries; new_e; new_e = new_e->next) {
                        if (new_e->hash == hash && keys_equal(new_e->key, key)) {
                            if (new_prev) {
                                new_prev->next = new_e->next;
                            } else {
                                new_node->as.collision.entries = new_e->next;
                            }
                            /* WKC2: collision_node_copy retained this key; the
                             * removed entry's reference goes away here. */
                            key_release_hook(new_e->key);
                            val_release_hook(new_e->val);   /* ... and its boxed value */
                            free(new_e);
                            break;
                        }
                        new_prev = new_e;
                    }

                    if (new_node->as.collision.entries == NULL) {
                        free(new_node);
                        return NULL;
                    }

                    return new_node;
                }
            }

            return n;
        }
    }

    return n;
}

/* ============================================================================
 * Lookup operation
 * ============================================================================
 */

static void *node_get(HamtNode *n, uint64_t hash, void *key, uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);

    if (!n) {
        return NULL;
    }

    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;

            if (!(bitmap & (UINT32_C(1) << chunk))) {
                return NULL;
            }

            uint32_t idx = 0;
            uint32_t mask = 1;
            for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                if (bitmap & mask) idx++;
            }

            return node_get(n->as.bitmap.children[idx], hash, key, level + 1);
        }

        case HAMT_NODE_ARRAY: {
            return node_get(n->as.array.children[chunk], hash, key, level + 1);
        }

        case HAMT_NODE_COLLISION: {
            return collision_get(n->as.collision.entries, hash, key);
        }
    }

    return NULL;
}

/* ============================================================================
 * Has operation
 * ============================================================================
 */

static bool node_has(HamtNode *n, uint64_t hash, void *key, uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);

    if (!n) {
        return false;
    }

    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;

            if (!(bitmap & (UINT32_C(1) << chunk))) {
                return false;
            }

            uint32_t idx = 0;
            uint32_t mask = 1;
            for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                if (bitmap & mask) idx++;
            }

            return node_has(n->as.bitmap.children[idx], hash, key, level + 1);
        }

        case HAMT_NODE_ARRAY: {
            return node_has(n->as.array.children[chunk], hash, key, level + 1);
        }

        case HAMT_NODE_COLLISION: {
            return collision_has(n->as.collision.entries, hash, key);
        }
    }

    return false;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================
 */

Hamt *tur_hamt_new(void) {
    return hamt_alloc_empty();
}

void tur_hamt_free(Hamt *m) {
    if (!m || --m->ref_count != 0) {
        return;
    }

    if (m->root) {
        /* WKC2: install this map's key-release hook so any entries that
         * actually free during the node cascade release their boxed keys.
         * Saved/restored to compose with an enclosing operation. */
        void (*save_release)(void *) = g_hamt_key_release;
        void (*save_val_release)(void *) = g_hamt_val_release;
        g_hamt_key_release = m->key_ops.release;
        /* Multi-word-value boxing: an owned value is always a tur_hamt_box_key
         * box, so its release op is the box refcount release. */
        g_hamt_val_release = m->val_ops.release;
        tur_hamt_node_release(m->root);
        g_hamt_key_release = save_release;
        g_hamt_val_release = save_val_release;
    }
    free(m);
}

Hamt *tur_hamt_retain(Hamt *m) {
    if (m) {
        m->ref_count++;
    }
    return m;
}

uint32_t tur_hamt_count(Hamt *m) {
    if (!m) return 0;
    return m->count;
}

Hamt *tur_hamt_set(Hamt *m, uint64_t hash, void *key, void *val) {
    /* region-lock-hardening: the store-side region note (src/runtime/region.h).
     * hamt.c is host runtime, never pasted into a program, so it cannot spell
     * the emitted TUR_REGION_NOTE macro; it calls the runtime directly --
     * region.c sits beside it in every library that carries hamt.c, and a
     * program that pastes its own region.c defines the symbol before the
     * archive is searched.  With no generation open on this thread (every
     * TUR_REGIONS=0 program) the note returns at its first compare.  Every
     * persistent setter funnels through here; the transient one
     * (tur_hamt_transient_set) reaches node_insert on its own and carries the
     * same two notes.  Those are the places a key or value word enters HAMT
     * memory -- malloc'd, and outliving any bracket the insert ran inside. */
    tur_region_note_escape(key);
    tur_region_note_escape(val);
    /* A NULL base means "the empty map".  We need a real one to read count /
     * key_ops / val_ops off, but it is OURS, not the caller's -- so it has to
     * be freed again before returning the derived map, or every insert into a
     * NULL base leaks a 64-byte root.  (Freeing it is safe: it is a fresh
     * ref_count=1 allocation with a NULL root, so tur_hamt_free just frees the
     * struct.)  The one exception is the `return m` path below, where the temp
     * IS the returned map and ownership passes to the caller. */
    bool m_is_temp = false;
    if (!m) {
        m = hamt_alloc_empty();
        m_is_temp = true;
    }

    bool key_exists = tur_hamt_has(m, hash, key);
    HamtNode *new_root = node_insert(m->root, hash, key, val, 0);

    if (new_root == m->root) {
        /* Structurally unchanged, so the result IS the input map.  Hand back an
         * OWNED reference: the documented contract is "returns a new HAMT; the
         * original is unchanged", and callers free the result and the original
         * independently.  Returning a bare alias made both handles free the one
         * allocation -- the second tur_hamt_free then decremented ref_count
         * through freed memory and, when the garbage landed on zero, freed it
         * again.  (Our own temp base needs no retain: it is already a fresh
         * ref_count=1 allocation and m_is_temp suppresses the free below, so
         * ownership simply passes through.) */
        if (!m_is_temp) tur_hamt_retain(m);
        return m;
    }

    Hamt *new_map = hamt_alloc_empty();
    /* new_root has ref_count=1 from node_insert; new_map is the sole owner. */
    new_map->root = new_root;
    new_map->key_ops = m->key_ops;  /* WKC2: inherit boxed-key ownership */
    new_map->val_owned = m->val_owned;  /* inherit value-box ownership */
    new_map->val_ops   = m->val_ops;    /* ... and its caller-supplied ops */

    if (key_exists) {
        new_map->count = m->count;
    } else {
        new_map->count = m->count + 1;
    }

    if (m_is_temp) tur_hamt_free(m);  /* our own empty base, not the caller's */
    return new_map;
}

Hamt *tur_hamt_del(Hamt *m, uint64_t hash, void *key) {
    if (!m) {
        return m;
    }

    /* Both no-op paths below return the input map, so they must hand back an
     * OWNED reference for the same reason tur_hamt_set's identity path does:
     * the caller frees the result and the original independently, and a bare
     * alias makes both frees land on one allocation.  This is what broke
     * tests/fixtures/hamt-delete on macOS -- deleting an absent key produced
     * m3 == m2, and freeing both double-freed the map. */
    if (!tur_hamt_has(m, hash, key)) {
        return tur_hamt_retain(m);
    }

    HamtNode *new_root = node_delete(m->root, hash, key, 0);

    if (new_root == m->root) {
        return tur_hamt_retain(m);
    }

    Hamt *new_map = hamt_alloc_empty();
    /* new_root has ref_count=1 from node_delete (a fresh copy, never a shared
     * node); new_map is the sole owner -- do NOT retain again, or the root
     * never reaches refcount 0 on free (leaking the node and its boxed keys). */
    new_map->root = new_root;
    new_map->key_ops = m->key_ops;  /* WKC2: inherit boxed-key ownership */
    new_map->val_owned = m->val_owned;  /* inherit value-box ownership */
    new_map->val_ops   = m->val_ops;    /* ... and its caller-supplied ops */
    new_map->count = m->count - 1;

    return new_map;
}

bool tur_hamt_has(Hamt *m, uint64_t hash, void *key) {
    if (!m) return false;
    return node_has(m->root, hash, key, 0);
}

void *tur_hamt_get(Hamt *m, uint64_t hash, void *key) {
    if (!m) return NULL;
    return node_get(m->root, hash, key, 0);
}

/* ----------------------------------------------------------------------------
 * Key-equality-aware variants (TCE4)
 *
 * Each installs `eq` (a bool(*)(int64_t,int64_t) passed as an opaque word --
 * e.g. a Turmeric closure handle) as the key comparison for the duration of
 * the call, then restores the previous override. Passing eq == NULL is
 * identical to the plain entry point (pointer identity).
 * --------------------------------------------------------------------------*/

Hamt *tur_hamt_set_eq(Hamt *m, uint64_t hash, void *key, void *val, tur_hamt_keyeq_fn eq) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = eq; g_hamt_key_eq_ctx_fn = NULL; g_hamt_key_eq_ctx = NULL;
    Hamt *r = tur_hamt_set(m, hash, key, val);
    hamt_restore_eq_hooks(save);
    /* GDE3: stamp the key comparator so tur_hamt_eq_dynamic can read it. */
    if (r && r != m) r->key_ops.eq = eq;
    return r;
}

Hamt *tur_hamt_del_eq(Hamt *m, uint64_t hash, void *key, tur_hamt_keyeq_fn eq) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = eq; g_hamt_key_eq_ctx_fn = NULL; g_hamt_key_eq_ctx = NULL;
    Hamt *r = tur_hamt_del(m, hash, key);
    hamt_restore_eq_hooks(save);
    /* GDE3: propagate stamped comparator to the new map if one was created. */
    if (r && r != m) r->key_ops.eq = eq;
    return r;
}

bool tur_hamt_has_eq(Hamt *m, uint64_t hash, void *key, tur_hamt_keyeq_fn eq) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = eq; g_hamt_key_eq_ctx_fn = NULL; g_hamt_key_eq_ctx = NULL;
    bool r = tur_hamt_has(m, hash, key);
    hamt_restore_eq_hooks(save);
    return r;
}

void *tur_hamt_get_eq(Hamt *m, uint64_t hash, void *key, tur_hamt_keyeq_fn eq) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = eq; g_hamt_key_eq_ctx_fn = NULL; g_hamt_key_eq_ctx = NULL;
    void *r = tur_hamt_get(m, hash, key);
    hamt_restore_eq_hooks(save);
    return r;
}

/* ----------------------------------------------------------------------------
 * Content-keyed cstr convenience entry points
 *
 * One-call set/del/has/get for NUL-terminated string keys: hash by content
 * (xxHash64 of the bytes) and compare by content on collision, mirroring the
 * MapKey[cstr] comparator of the typed-Map path.  These exist so the P3
 * `^persistent` lowering never routes a cstr key through hash-ptr + identity
 * compare -- a scheme that only ever "worked" when the C compiler merged
 * identical string literals, which C11 6.4.5p7 leaves unspecified (gcc/clang
 * merge, c2mir does not).  Keys built at runtime (concatenation, parsed
 * input) were silently lost.
 * --------------------------------------------------------------------------*/

static bool hamt_cstr_key_eq(int64_t a, int64_t b) {
    const char *p = (const char *)(intptr_t)a;
    const char *q = (const char *)(intptr_t)b;
    if (p == q) return true;
    if (!p || !q) return false;
    return strcmp(p, q) == 0;
}

Hamt *tur_hamt_set_cstr(Hamt *m, const char *key, void *val) {
    return tur_hamt_set_eq(m, tur_hamt_hash_str(key), (void *)key, val,
                           hamt_cstr_key_eq);
}

Hamt *tur_hamt_del_cstr(Hamt *m, const char *key) {
    return tur_hamt_del_eq(m, tur_hamt_hash_str(key), (void *)key,
                           hamt_cstr_key_eq);
}

bool tur_hamt_has_cstr(Hamt *m, const char *key) {
    return tur_hamt_has_eq(m, tur_hamt_hash_str(key), (void *)key,
                           hamt_cstr_key_eq);
}

void *tur_hamt_get_cstr(Hamt *m, const char *key) {
    return tur_hamt_get_eq(m, tur_hamt_hash_str(key), (void *)key,
                           hamt_cstr_key_eq);
}

/* ----------------------------------------------------------------------------
 * Context-carrying key-equality variants (prereq 2a)
 *
 * Identical to the _eq family, but `eq` takes a third `void *ctx` argument that
 * is threaded to every collision-time compare.  This is the ABI groundwork that
 * lets the interpreter pass a turi closure + TuriEnv* (packed into ctx) and a
 * trampoline `eq` that calls turi_call -- content-keyed maps whose comparator is
 * a Turmeric closure rather than a C function pointer.  The compiled path keeps
 * calling the no-ctx family; passing ctx == NULL here is exactly the no-ctx
 * path with one extra ignored argument, so no codegen change and no fixture
 * regen.  Each entry clears the no-ctx hook for its scope (see hamt_save_eq_hooks).
 * --------------------------------------------------------------------------*/

Hamt *tur_hamt_set_eq_ctx(Hamt *m, uint64_t hash, void *key, void *val,
                          tur_hamt_keyeq_ctx_fn eq, void *ctx) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = NULL; g_hamt_key_eq_ctx_fn = eq; g_hamt_key_eq_ctx = ctx;
    Hamt *r = tur_hamt_set(m, hash, key, val);
    hamt_restore_eq_hooks(save);
    return r;
}

Hamt *tur_hamt_del_eq_ctx(Hamt *m, uint64_t hash, void *key,
                          tur_hamt_keyeq_ctx_fn eq, void *ctx) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = NULL; g_hamt_key_eq_ctx_fn = eq; g_hamt_key_eq_ctx = ctx;
    Hamt *r = tur_hamt_del(m, hash, key);
    hamt_restore_eq_hooks(save);
    return r;
}

bool tur_hamt_has_eq_ctx(Hamt *m, uint64_t hash, void *key,
                         tur_hamt_keyeq_ctx_fn eq, void *ctx) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = NULL; g_hamt_key_eq_ctx_fn = eq; g_hamt_key_eq_ctx = ctx;
    bool r = tur_hamt_has(m, hash, key);
    hamt_restore_eq_hooks(save);
    return r;
}

void *tur_hamt_get_eq_ctx(Hamt *m, uint64_t hash, void *key,
                          tur_hamt_keyeq_ctx_fn eq, void *ctx) {
    hamt_eq_hook_save save = hamt_save_eq_hooks();
    g_hamt_key_eq = NULL; g_hamt_key_eq_ctx_fn = eq; g_hamt_key_eq_ctx = ctx;
    void *r = tur_hamt_get(m, hash, key);
    hamt_restore_eq_hooks(save);
    return r;
}

/* ----------------------------------------------------------------------------
 * Ownership-aware variants (WKC2)
 *
 * Like the _eq calls but additionally install key retain/release for the
 * duration (so structural copies retain the boxed key and freed entries
 * release it) and stamp `ops` onto the resulting map so a later tur_hamt_free
 * releases its keys.  Each call consumes one reference to `key` (see header).
 * --------------------------------------------------------------------------*/

/* RAII-ish save/restore of all key hooks (plain eq + ctx eq + retain/release).
 * Installing a plain owned-eq op also clears the ctx comparator for its scope,
 * so an owned op nested inside a ctx op uses its own plain eq (prereq 2a). */
typedef struct {
    tur_hamt_keyeq_fn  eq;
    bool (*eq_ctx_fn)(int64_t, int64_t, void *);
    void *eq_ctx;
    void (*retain)(void *);
    void (*release)(void *);
} hamt_key_hook_save;

static hamt_key_hook_save hamt_install_key_hooks(tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops) {
    hamt_key_hook_save s = { g_hamt_key_eq, g_hamt_key_eq_ctx_fn, g_hamt_key_eq_ctx,
                             g_hamt_key_retain, g_hamt_key_release };
    g_hamt_key_eq        = eq;
    g_hamt_key_eq_ctx_fn = NULL;
    g_hamt_key_eq_ctx    = NULL;
    g_hamt_key_retain    = ops.retain;
    g_hamt_key_release   = ops.release;
    return s;
}

static void hamt_restore_key_hooks(hamt_key_hook_save s) {
    g_hamt_key_eq        = s.eq;
    g_hamt_key_eq_ctx_fn = s.eq_ctx_fn;
    g_hamt_key_eq_ctx    = s.eq_ctx;
    g_hamt_key_retain    = s.retain;
    g_hamt_key_release   = s.release;
}

Hamt *tur_hamt_set_eq_owned(Hamt *m, uint64_t hash, void *key, void *val,
                            tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops) {
    hamt_key_hook_save s = hamt_install_key_hooks(eq, ops);
    /* On update the passed key is not stored (the existing entry's key is
     * kept), so its incoming reference must be dropped.  Determine that up
     * front while the eq hook is installed. */
    bool existed = m ? tur_hamt_has(m, hash, key) : false;
    Hamt *r = tur_hamt_set(m, hash, key, val);
    if (existed && ops.release) ops.release(key);
    if (r) { r->key_ops = ops; r->key_ops.eq = eq; }  /* GDE3: stamp comparator */
    hamt_restore_key_hooks(s);
    return r;
}

Hamt *tur_hamt_del_eq_owned(Hamt *m, uint64_t hash, void *key,
                            tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops) {
    hamt_key_hook_save s = hamt_install_key_hooks(eq, ops);
    Hamt *r = tur_hamt_del(m, hash, key);
    if (r) { r->key_ops = ops; r->key_ops.eq = eq; }  /* GDE3: stamp comparator */
    /* The probe key is never stored; release the caller's reference.  (The
     * removed entry's own key reference was released inside node_delete.) */
    if (ops.release) ops.release(key);
    hamt_restore_key_hooks(s);
    return r;
}

bool tur_hamt_has_eq_owned(Hamt *m, uint64_t hash, void *key,
                           tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops) {
    hamt_key_hook_save s = hamt_install_key_hooks(eq, ops);
    bool r = tur_hamt_has(m, hash, key);
    if (ops.release) ops.release(key);  /* transient probe */
    hamt_restore_key_hooks(s);
    return r;
}

void *tur_hamt_get_eq_owned(Hamt *m, uint64_t hash, void *key,
                            tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops) {
    hamt_key_hook_save s = hamt_install_key_hooks(eq, ops);
    void *r = tur_hamt_get(m, hash, key);
    if (ops.release) ops.release(key);  /* transient probe */
    hamt_restore_key_hooks(s);
    return r;
}

/* Flag-driven wrappers (WKC3 + multi-word-value boxing + rc<T> values).
 * `owned` is a BITMASK:
 *
 *   bit 0 -- boxed-KEY ownership (the standard tur_hamt_box_key ops).
 *   bit 1 -- the value is a heap BOX the map owns.
 *   bit 2 -- the value is an rc<T> handle the map owns.
 *
 * Bits 1 and 2 both mean "the map owns its values" and differ only in which
 * ops do the owning; they are mutually exclusive (a value is either a by-value
 * box or an rc handle, never both).  `owned == 0` is the un-owned _eq path and
 * a legacy `owned == 1` (key only) is unchanged, so this stays
 * backward-compatible.
 *
 * Value ownership installs the caller's ops as the thread-local value hooks for
 * the duration of the mutating op (set/del) so structural copies retain and
 * dropped entries release, and stamps them on the resulting map for a later
 * tur_hamt_free.  get/has are read-only (no node copy/free), so they never
 * touch value lifetime.
 *
 * The _eq_o wrappers hardcode the box refcount and so must only ever be handed
 * bit 1; a bit-2 caller goes through _vo with its own ops. */
Hamt *tur_hamt_set_eq_vo(Hamt *m, uint64_t hash, void *key, void *val,
                         tur_hamt_keyeq_fn eq, int64_t owned,
                         void (*val_retain)(void *), void (*val_release)(void *)) {
    tur_hamt_val_ops vops = { val_retain, val_release };
    void (*svr)(void *) = g_hamt_val_retain, (*svl)(void *) = g_hamt_val_release;
    if (owned & 6) { g_hamt_val_retain = vops.retain;
                     g_hamt_val_release = vops.release; }
    Hamt *r = (owned & 1)
        ? tur_hamt_set_eq_owned(m, hash, key, val, eq, tur_hamt_box_key_ops())
        : tur_hamt_set_eq(m, hash, key, val, eq);
    if (r && (owned & 6)) {
        r->val_owned = true;
        r->val_ops   = vops;
    }
    g_hamt_val_retain = svr; g_hamt_val_release = svl;
    return r;
}

Hamt *tur_hamt_set_eq_o(Hamt *m, uint64_t hash, void *key, void *val,
                        tur_hamt_keyeq_fn eq, int64_t owned) {
    return tur_hamt_set_eq_vo(m, hash, key, val, eq, owned,
                              tur_hamt_box_retain, tur_hamt_box_release);
}

Hamt *tur_hamt_del_eq_vo(Hamt *m, uint64_t hash, void *key,
                         tur_hamt_keyeq_fn eq, int64_t owned,
                         void (*val_retain)(void *), void (*val_release)(void *)) {
    tur_hamt_val_ops vops = { val_retain, val_release };
    void (*svr)(void *) = g_hamt_val_retain, (*svl)(void *) = g_hamt_val_release;
    if (owned & 6) { g_hamt_val_retain = vops.retain;
                     g_hamt_val_release = vops.release; }
    Hamt *r = (owned & 1)
        ? tur_hamt_del_eq_owned(m, hash, key, eq, tur_hamt_box_key_ops())
        : tur_hamt_del_eq(m, hash, key, eq);
    if (r && (owned & 6)) {
        r->val_owned = true;
        r->val_ops   = vops;
    }
    g_hamt_val_retain = svr; g_hamt_val_release = svl;
    return r;
}

Hamt *tur_hamt_del_eq_o(Hamt *m, uint64_t hash, void *key,
                        tur_hamt_keyeq_fn eq, int64_t owned) {
    return tur_hamt_del_eq_vo(m, hash, key, eq, owned,
                              tur_hamt_box_retain, tur_hamt_box_release);
}

bool tur_hamt_has_eq_o(Hamt *m, uint64_t hash, void *key,
                       tur_hamt_keyeq_fn eq, int64_t owned) {
    if (owned & 1)
        return tur_hamt_has_eq_owned(m, hash, key, eq, tur_hamt_box_key_ops());
    return tur_hamt_has_eq(m, hash, key, eq);
}

void *tur_hamt_get_eq_o(Hamt *m, uint64_t hash, void *key,
                        tur_hamt_keyeq_fn eq, int64_t owned) {
    if (owned & 1)
        return tur_hamt_get_eq_owned(m, hash, key, eq, tur_hamt_box_key_ops());
    return tur_hamt_get_eq(m, hash, key, eq);
}

/* GDE3: content-correct equality using each map's stamped key comparator.
 * Reads the key comparator from a's Hamt->key_ops.eq; falls back to pointer
 * identity when the map was built with no explicit comparator (int keys). */
bool tur_hamt_eq_dynamic(int64_t a_handle, int64_t b_handle, int64_t val_cmp) {
    typedef struct { Hamt *hamt; } MapHandle;
    MapHandle *ma = (MapHandle *)(intptr_t)a_handle;
    MapHandle *mb = (MapHandle *)(intptr_t)b_handle;
    if (!ma && !mb) return true;
    if (!ma || !mb) return false;
    Hamt *a = ma->hamt;
    Hamt *b = mb->hamt;
    if (tur_hamt_count(a) != tur_hamt_count(b)) return false;
    tur_hamt_keyeq_fn keyeq = a ? a->key_ops.eq : NULL;
    HamtIter iter_buf;
    tur_hamt_iter_init(&iter_buf, a);
    uint64_t hash_out;
    void *key_out = NULL, *val_out = NULL;
    while (tur_hamt_iter_next(&iter_buf, &hash_out, &key_out, &val_out)) {
        void *val_in_b = keyeq
            ? tur_hamt_get_eq(b, hash_out, key_out, keyeq)
            : tur_hamt_get(b, hash_out, key_out);
        if (!val_in_b) { tur_hamt_iter_free(&iter_buf); return false; }
        /* CRU B-3: val_cmp is a fat closure box { thunk, env... }; fat-dispatch
         * via slot 0 so a capturing value comparator works (the Eq[Map] instance
         * and the dispatcher both box the comparator now). */
        bool vals_eq = ((bool (*)(void *, int64_t, int64_t))(intptr_t)
                            ((int64_t *)(intptr_t)val_cmp)[0])(
            (void *)(intptr_t)val_cmp,
            (int64_t)(intptr_t)val_out, (int64_t)(intptr_t)val_in_b);
        if (!vals_eq) { tur_hamt_iter_free(&iter_buf); return false; }
    }
    tur_hamt_iter_free(&iter_buf);
    return true;
}

Hamt *tur_hamt_merge(Hamt *a, Hamt *b) {
    if (!a) return tur_hamt_retain(b);
    if (!b) return tur_hamt_retain(a);

    Hamt *result = tur_hamt_retain(a);

    HamtIter iter;
    tur_hamt_iter_init(&iter, b);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        Hamt *old = result;
        result = tur_hamt_set(result, hash, key, val);
        if (old != result) {
            tur_hamt_free(old);
        }
    }

    tur_hamt_iter_free(&iter);
    return result;
}

Hamt *tur_hamt_map(Hamt *m, void *(*fn)(void *val, void *ctx), void *ctx) {
    Hamt *result = tur_hamt_new();

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        void *new_val = fn(val, ctx);
        Hamt *old = result;
        result = tur_hamt_set(result, hash, key, new_val);
        if (old != result) tur_hamt_free(old);
    }

    tur_hamt_iter_free(&iter);
    return result;
}

Hamt *tur_hamt_filter(Hamt *m, bool (*fn)(void *key, void *val, void *ctx), void *ctx) {
    Hamt *result = tur_hamt_new();

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        if (fn(key, val, ctx)) {
            Hamt *old = result;
            result = tur_hamt_set(result, hash, key, val);
            if (old != result) tur_hamt_free(old);
        }
    }

    tur_hamt_iter_free(&iter);
    return result;
}

void *tur_hamt_reduce(Hamt *m, void *(*fn)(void *acc, void *key, void *val, void *ctx), void *init, void *ctx) {
    void *acc = init;

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        acc = fn(acc, key, val, ctx);
    }

    tur_hamt_iter_free(&iter);
    return acc;
}

Hamt *tur_hamt_merge_with(Hamt *a, Hamt *b, void *(*fn)(void *va, void *vb, void *ctx), void *ctx) {
    if (!a) return tur_hamt_retain(b);
    if (!b) return tur_hamt_retain(a);

    Hamt *result = tur_hamt_retain(a);

    HamtIter iter;
    tur_hamt_iter_init(&iter, b);

    uint64_t hash;
    void *key, *val_b;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val_b)) {
        void *val_a = tur_hamt_get(result, hash, key);
        void *new_val = (val_a != NULL) ? fn(val_a, val_b, ctx) : val_b;
        Hamt *old = result;
        result = tur_hamt_set(result, hash, key, new_val);
        if (old != result) tur_hamt_free(old);
    }

    tur_hamt_iter_free(&iter);
    return result;
}


/* ============================================================================
 * Iteration Implementation
 * ============================================================================
 */

void tur_hamt_iter_init(HamtIter *iter, Hamt *m) {
    iter->map = m;
    iter->stack = NULL;
    iter->cidx = NULL;
    iter->stack_cap = 0;
    iter->stack_len = 0;
    iter->coll_entry = NULL;
    iter->done = (m == NULL || m->root == NULL);

    if (!iter->done) {
        iter->stack_cap = 16;
        iter->stack = (HamtNode **)hamt_malloc(sizeof(HamtNode *) * iter->stack_cap);
        iter->cidx  = (uint32_t *)hamt_malloc(sizeof(uint32_t) * iter->stack_cap);
        iter->stack[0] = m->root;
        iter->cidx[0]  = 0;
        iter->stack_len = 1;
    }
}

void tur_hamt_iter_free(HamtIter *iter) {
    if (iter->stack) {
        free(iter->stack);
        iter->stack = NULL;
    }
    if (iter->cidx) {
        free(iter->cidx);
        iter->cidx = NULL;
    }
    iter->stack_cap = 0;
    iter->stack_len = 0;
    iter->done = true;
}

bool tur_hamt_iter_next(HamtIter *iter, uint64_t *hash_out, void **key_out, void **val_out) {
    while (!iter->done) {
        /* Resume a collision chain from a previous call. */
        if (iter->coll_entry) {
            *hash_out = iter->coll_entry->hash;
            *key_out = iter->coll_entry->key;
            *val_out = iter->coll_entry->val;
            iter->coll_entry = iter->coll_entry->next;
            return true;
        }

        if (iter->stack_len == 0) {
            iter->done = true;
            break;
        }

        size_t top = iter->stack_len - 1;
        HamtNode *n = iter->stack[top];

        switch (n->type) {
            case HAMT_NODE_COLLISION: {
                /* Pop the collision node and start (or resume) its chain. */
                iter->stack_len--;
                iter->coll_entry = n->as.collision.entries;
                if (iter->coll_entry) {
                    *hash_out = iter->coll_entry->hash;
                    *key_out = iter->coll_entry->key;
                    *val_out = iter->coll_entry->val;
                    iter->coll_entry = iter->coll_entry->next;
                    return true;
                }
                break;
            }
            case HAMT_NODE_BITMAP: {
                uint32_t child_count = popcount32(n->as.bitmap.bitmap);
                if (iter->cidx[top] < child_count) {
                    /* Push next child of this bitmap node. */
                    HamtNode *child = n->as.bitmap.children[iter->cidx[top]++];
                    if (iter->stack_len >= iter->stack_cap) {
                        iter->stack_cap *= 2;
                        iter->stack = (HamtNode **)realloc(iter->stack,
                            sizeof(HamtNode *) * iter->stack_cap);
                        iter->cidx  = (uint32_t *)realloc(iter->cidx,
                            sizeof(uint32_t) * iter->stack_cap);
                    }
                    iter->cidx[iter->stack_len] = 0;
                    iter->stack[iter->stack_len++] = child;
                } else {
                    /* All children visited — pop this bitmap node. */
                    iter->stack_len--;
                }
                break;
            }
            case HAMT_NODE_ARRAY: {
                if (iter->cidx[top] < HAMT_SLOTS_PER_LEVEL) {
                    HamtNode *child = n->as.array.children[iter->cidx[top]++];
                    if (child) {
                        if (iter->stack_len >= iter->stack_cap) {
                            iter->stack_cap *= 2;
                            iter->stack = (HamtNode **)realloc(iter->stack,
                                sizeof(HamtNode *) * iter->stack_cap);
                            iter->cidx  = (uint32_t *)realloc(iter->cidx,
                                sizeof(uint32_t) * iter->stack_cap);
                        }
                        iter->cidx[iter->stack_len] = 0;
                        iter->stack[iter->stack_len++] = child;
                    }
                } else {
                    /* All slots visited — pop this array node. */
                    iter->stack_len--;
                }
                break;
            }
        }
    }

    return false;
}

/* ============================================================================
 * Iterator box wrapper (pure-Turmeric iteration support)
 * ============================================================================
 * The Eq[Map] / Eq[Set] instances drive iteration from pure Turmeric via a
 * TCO'd self-tail loop.  Each loop step needs to advance the iterator and read
 * the (hash, key, val) of the current entry, but the underlying tur_hamt_iter_*
 * API stores those in *_out parameters that pure-Turmeric code cannot easily
 * spell.  The "box" packs a HamtIter together with three current-position
 * slots, exposes a heap-allocated handle, and provides scalar accessors so the
 * Turmeric loop can read each field independently between advance calls.
 */

typedef struct {
    HamtIter iter;
    uint64_t cur_hash;
    void *cur_key;
    void *cur_val;
} TurHamtIterBox;

void *tur_hamt_iter_alloc(Hamt *m) {
    TurHamtIterBox *box = (TurHamtIterBox *)calloc(1, sizeof(*box));
    tur_hamt_iter_init(&box->iter, m);
    return box;
}

void tur_hamt_iter_destroy(void *box_) {
    if (!box_) return;
    TurHamtIterBox *box = (TurHamtIterBox *)box_;
    tur_hamt_iter_free(&box->iter);
    free(box);
}

bool tur_hamt_iter_advance(void *box_) {
    TurHamtIterBox *box = (TurHamtIterBox *)box_;
    return tur_hamt_iter_next(&box->iter, &box->cur_hash, &box->cur_key, &box->cur_val);
}

int64_t tur_hamt_iter_cur_hash(void *box_) {
    return (int64_t)((TurHamtIterBox *)box_)->cur_hash;
}

void *tur_hamt_iter_cur_key(void *box_) {
    return ((TurHamtIterBox *)box_)->cur_key;
}

void *tur_hamt_iter_cur_val(void *box_) {
    return ((TurHamtIterBox *)box_)->cur_val;
}

/* Read the stamped key comparator (NULL = identity).  Pure-Turmeric Eq[Map]
 * threads this into tur_hamt_get_eq when set, so content-keyed maps (e.g.
 * cstr keys) stay content-correct without leaving Turmeric. */
void *tur_hamt_keyeq(Hamt *m) {
    return m ? (void *)(intptr_t)m->key_ops.eq : NULL;
}

/* Identity-or-eq lookup: when keyeq is NULL falls back to tur_hamt_get,
 * else delegates to tur_hamt_get_eq.  Exists so the Turmeric loop body
 * stays a straight-line expression (no conditional around the lookup). */
void *tur_hamt_get_dynamic(Hamt *m, int64_t hash, void *key, void *keyeq) {
    if (keyeq)
        return tur_hamt_get_eq(m, (uint64_t)hash, key, (tur_hamt_keyeq_fn)(intptr_t)keyeq);
    return tur_hamt_get(m, (uint64_t)hash, key);
}

bool tur_hamt_has_dynamic(Hamt *m, int64_t hash, void *key, void *keyeq) {
    if (keyeq)
        return tur_hamt_has_eq(m, (uint64_t)hash, key, (tur_hamt_keyeq_fn)(intptr_t)keyeq);
    return tur_hamt_has(m, (uint64_t)hash, key);
}

/* ============================================================================
 * Debugging: Dump functions
 * ============================================================================
 */

static void dump_node(HamtNode *n, FILE *out, int indent) {
    if (!n) return;

    for (int i = 0; i < indent; i++) fprintf(out, "  ");

    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            uint32_t child_count = popcount32(bitmap);
            fprintf(out, "BitmapNode (bitmap=0x%08X, children=%u, refs=%u)\n",
                    bitmap, child_count, n->ref_count);
            for (uint32_t i = 0; i < 32; i++) {
                if (bitmap & (UINT32_C(1) << i)) {
                    uint32_t child_idx = 0;
                    uint32_t mask = 1;
                    for (uint32_t j = 0; j < i; j++, mask <<= 1) {
                        if (bitmap & mask) child_idx++;
                    }
                    for (int j = 0; j < indent + 1; j++) fprintf(out, "  ");
                    fprintf(out, "  slot %u:\n", i);
                    dump_node(n->as.bitmap.children[child_idx], out, indent + 2);
                }
            }
            break;
        }
        case HAMT_NODE_ARRAY: {
            fprintf(out, "ArrayNode (refs=%u)\n", n->ref_count);
            for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                if (n->as.array.children[i]) {
                    for (int j = 0; j < indent + 1; j++) fprintf(out, "  ");
                    fprintf(out, "  slot %u:\n", i);
                    dump_node(n->as.array.children[i], out, indent + 2);
                }
            }
            break;
        }
        case HAMT_NODE_COLLISION: {
            fprintf(out, "CollisionNode (prefix=0x%02X, refs=%u)\n",
                    n->as.collision.hash_prefix, n->ref_count);
            for (HamtEntry *e = n->as.collision.entries; e; e = e->next) {
                for (int j = 0; j < indent + 1; j++) fprintf(out, "  ");
                fprintf(out, "  {hash=0x%016llX, key=%p, val=%p}\n",
                        (unsigned long long)e->hash, e->key, e->val);
            }
            break;
        }
    }
}

/* Returns a heap-allocated string representing the HAMT as "{k1->v1, k2->v2, ...}".
 * Key and value are shown as pointer addresses. Caller must free. */
char *tur_hamt_show(Hamt *m) {
    if (!m || m->count == 0) {
        char *s = (char *)malloc(3);
        if (!s) return NULL;
        s[0] = '{'; s[1] = '}'; s[2] = '\0';
        return s;
    }
    /* Each entry: "0x%016llx->0x%016llx" = ~40 chars; plus ", " and {}.
     * Widen BEFORE multiplying: `count` is a uint32_t, and the 32-bit
     * product wrapped above ~89M entries into a buffer the loop below then
     * overran (security audit WP5, M-5). */
    size_t cap = (size_t)m->count * 48 + 4;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    buf[pos++] = '{';
    bool first = true;

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);
    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        if (!first) { buf[pos++] = ','; buf[pos++] = ' '; }
        first = false;
        int written = snprintf(buf + pos, cap - pos, "%p->%p", key, val);
        pos += (size_t)written;
    }
    tur_hamt_iter_free(&iter);
    buf[pos++] = '}';
    buf[pos] = '\0';
    return buf;
}

void tur_hamt_dump(Hamt *m, FILE *out) {
    if (!m) {
        fprintf(out, "(null)\n");
        return;
    }
    fprintf(out, "HAMT (count=%u, refs=%u)\n", m->count, m->ref_count);
    if (m->root) {
        dump_node(m->root, out, 1);
    } else {
        fprintf(out, "  (empty)\n");
    }
}

void tur_hamt_dump_dot(Hamt *m, FILE *out) {
    if (!m) {
        fprintf(out, "digraph HAMT {\n  rankdir=TB;\n}\n");
        return;
    }

    fprintf(out, "digraph HAMT {\n");
    fprintf(out, "  rankdir=TB;\n");
    fprintf(out, "  node [shape=record];\n\n");
    fprintf(out, "  root [label=\"HAMT|count=%u|refs=%u\"];\n", m->count, m->ref_count);

    if (!m->root) {
        fprintf(out, "}\n");
        return;
    }

    /* Emit nodes and edges with pointer-based IDs */
    /* We use a simple iterative worklist via the iter for leaf entries,
     * but for structural nodes we emit them inline during traversal. */

    /* Use a recursive helper via a local static to keep things simple */
    struct DotWork { HamtNode *node; };
    /* We'll emit via DFS using the stack manually. For simplicity, emit
     * all leaf entries by walking the iterator and emit structure note. */

    fprintf(out, "  n%p [label=\"", (void *)m->root);
    switch (m->root->type) {
        case HAMT_NODE_BITMAP:
            fprintf(out, "Bitmap|bitmap=0x%08X|children=%u",
                    m->root->as.bitmap.bitmap,
                    popcount32(m->root->as.bitmap.bitmap));
            break;
        case HAMT_NODE_ARRAY:
            fprintf(out, "Array|slots=32");
            break;
        case HAMT_NODE_COLLISION:
            fprintf(out, "Collision|prefix=0x%02X",
                    m->root->as.collision.hash_prefix);
            break;
    }
    fprintf(out, "\"];\n");
    fprintf(out, "  root -> n%p;\n", (void *)m->root);

    /* Emit entries as leaf nodes connected to root */
    HamtIter iter;
    tur_hamt_iter_init(&iter, m);
    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        fprintf(out, "  e%016llx [label=\"{hash=0x%016llx|key=%p|val=%p}\" shape=box];\n",
                (unsigned long long)hash, (unsigned long long)hash, key, val);
        fprintf(out, "  n%p -> e%016llx;\n", (void *)m->root, (unsigned long long)hash);
    }
    tur_hamt_iter_free(&iter);

    fprintf(out, "}\n");
}

void tur_hamt_dump_dot_stderr(Hamt *m) {
    tur_hamt_dump_dot(m, stderr);
}

/* ---------------------------------------------------------------------------
 * Transient mode — mutable batch-construction wrapper
 * ---------------------------------------------------------------------------
 *
 * A transient is a mutable view over HAMT nodes. For Phase P4 v1 we use
 * a simple strategy: the transient wraps the immutable root and delegates
 * to the existing functional set/del operations but avoids allocating a
 * new Hamt shell on every operation.  The token field guards against use
 * after tur_hamt_persistent() invalidates the transient.
 */

static uint64_t g_transient_token = 1;  /* monotonically increasing */

HamtTransient *tur_hamt_transient(Hamt *m) {
    HamtTransient *t = (HamtTransient *)malloc(sizeof(HamtTransient));
    if (!t) return NULL;

    if (m && m->root) {
        t->root = m->root;
        tur_hamt_node_retain(t->root);
        t->count = m->count;
    } else {
        t->root = NULL;
        t->count = 0;
    }
    t->token = g_transient_token++;
    return t;
}

void tur_hamt_transient_set(HamtTransient *t, uint64_t hash, void *key, void *val) {
    if (!t) return;
    if (t->token == 0) {
        fprintf(stderr, "tur: transient used after persistent! (invalidated)\n");
        abort();
    }
    /* The store-side region note, as tur_hamt_set's: a transient's nodes are
     * malloc'd and outlive any bracket the insert ran inside, and this path
     * reaches node_insert without going through tur_hamt_set.  The stdlib
     * entry takes `ptr<void>` words, so a node reached here through an
     * explicit erasure that was already noted -- this is the host-runtime
     * half, for a caller that did not come through one (security-audit WP5). */
    tur_region_note_escape(key);
    tur_region_note_escape(val);

    /* Check if key already exists in the current tree */
    bool exists = node_has(t->root, hash, key, 0);

    HamtNode *new_root = node_insert(t->root, hash, key, val, 0);
    if (new_root == t->root) return;

    /* Release old root; node_insert returned a new root with ref_count=1 */
    if (t->root) {
        tur_hamt_node_release(t->root);
    }
    t->root = new_root;
    if (!exists) {
        t->count++;
    }
}

void tur_hamt_transient_del(HamtTransient *t, uint64_t hash, void *key) {
    if (!t) return;
    if (t->token == 0) {
        fprintf(stderr, "tur: transient used after persistent! (invalidated)\n");
        abort();
    }

    if (!node_has(t->root, hash, key, 0)) return;

    HamtNode *new_root = node_delete(t->root, hash, key, 0);
    if (new_root == t->root) return;

    if (t->root) {
        tur_hamt_node_release(t->root);
    }
    t->root = new_root;
    if (new_root) {
        tur_hamt_node_retain(new_root);
    }
    t->count--;
}

Hamt *tur_hamt_persistent(HamtTransient *t) {
    if (!t) return tur_hamt_new();

    Hamt *m = hamt_alloc_empty();
    m->root = t->root;
    m->count = t->count;
    /* t already retained root; transfer ownership to m */

    /* Invalidate t so any subsequent use is detectable.  The struct is
     * deliberately left allocated (not freed) -- tur_hamt_transient_set/del
     * read t->token to abort on use-after-persistent!, which would be a
     * use-after-free if the struct were freed here. */
    t->root = NULL;
    t->count = 0;
    t->token = 0;

    return m;
}
