/* hamt.h - Persistent Hash Array Mapped Trie (Phase P1)
 *
 * A HAMT is an immutable hash map with structural sharing.
 * Uses 5-bit hash chunks (32 slots per level) for balanced trie depth.
 *
 * Memory management: reference counting (Phase P1 v1).
 * Collision handling: linked list in v1.
 * Hash function: xxHash64 (speed-optimised, non-cryptographic).
 *
 * User-facing API and semantics: docs/guides/hamt-guide.md.  The region
 * store hooks (tur_hamt_set, tur_hamt_transient_set) note every stored word;
 * see docs/guides/gc-guide.md (regions).
 */

#ifndef TUR_HAMT_H
#define TUR_HAMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declaration */
typedef struct HamtNode HamtNode;

/* GDE3: key equality comparator type (forward declaration so tur_hamt_key_ops
 * can reference it before the full typedef at line 139). */
typedef bool (*tur_hamt_keyeq_fn)(int64_t, int64_t);

/* WKC2: boxed-key ownership ops.  A "boxed" key is a heap allocation (see
 * tur_hamt_box_key) carrying a refcount so a single key can be shared across
 * structurally-shared map versions and freed exactly once.  `retain` is called
 * each time an entry referencing the key is duplicated (collision-node copy),
 * `release` each time such an entry is freed (node free / delete).  Both NULL
 * (the default) means keys are one-word/inline and the HAMT never touches their
 * lifetime -- byte-identical to the pre-WKC behavior. */
typedef struct {
    void (*retain)(void *key);
    void (*release)(void *key);
    tur_hamt_keyeq_fn eq;  /* GDE3: stamped key comparator (NULL = identity) */
} tur_hamt_key_ops;

/* Caller-supplied VALUE ownership ops.  Same contract as tur_hamt_key_ops
 * minus the comparator: `retain` runs each time an entry referencing the
 * value is duplicated, `release` each time such an entry is dropped.  Both
 * NULL means the map never touches value lifetime.  Unlike the boxed-key
 * ops these are supplied by the caller rather than hardcoded to the box
 * refcount, so an rc<T> value can pass rc_strong_increment/decrement. */
typedef struct {
    void (*retain)(void *val);
    void (*release)(void *val);
} tur_hamt_val_ops;

/* HAMT root structure. Owned via reference counting. */
typedef struct Hamt {
    HamtNode *root;      /* Root node (NULL for empty map) */
    uint32_t count;      /* Number of key/value pairs */
    uint32_t ref_count;   /* Reference count for the root struct */
    tur_hamt_key_ops key_ops;  /* WKC2: boxed-key ownership (NULL fns = none) */
    /* Multi-word-value boxing: when true, each entry's VALUE is a
     * tur_hamt_box_key box owned by the map (retained on structural copy,
     * released on entry drop / free), mirroring boxed keys.  Selected by bit 1
     * of the `owned` flag threaded through the _eq_o operations (bit 0 = key,
     * bit 1 = value).  False for the common single-word value (int/cstr/handle),
     * which rides the carrier inline and is never freed by the map. */
    bool val_owned;
    /* Caller-supplied value ownership (see tur_hamt_val_ops).  Populated
     * alongside val_owned; both NULL when the map does not own its values. */
    tur_hamt_val_ops val_ops;
} Hamt;

/* Node types for the tagged union */
typedef enum {
    HAMT_NODE_BITMAP,   /* Sparse node: <= 32 entries, bitmap tracks populated slots */
    HAMT_NODE_ARRAY,    /* Dense node: exactly 32 entries (all slots populated) */
    HAMT_NODE_COLLISION, /* Collision node: multiple keys with same hash prefix */
} HamtNodeType;

/* A key/value entry in collision nodes */
typedef struct HamtEntry {
    uint64_t hash;        /* Full 64-bit hash of the key */
    int64_t key;         /* Key word: a Turmeric carrier -- an int, a double's
                            bits, or a pointer widened to 64 bits.  Not a
                            `void *`: on wasm32 that is 4 bytes and dropped
                            the top half of a double or a large int
                            (ilp32-word-slots-truncate-64-bit-carriers, shape
                            2).  Owned by the caller unless key_ops say so. */
    int64_t val;         /* Value word, same representation and ownership. */
    struct HamtEntry *next; /* Next entry in collision chain */
} HamtEntry;

/* Bitmap node: sparse, <= 32 entries
 * Uses a 32-bit bitmap where bit i indicates slot i is populated.
 * Popcount(bitmap) gives the number of children. */
typedef struct {
    uint32_t bitmap;      /* Bitmask: bit i = 1 means slot i has a child */
    HamtNode *children[1]; /* Variable-length array of child pointers (size = popcount(bitmap)) */
} HamtNodeBitmap;

/* Array node: dense, exactly 32 entries
 * All 32 slots are populated. */
typedef struct {
    HamtNode *children[32]; /* All 32 slots populated */
} HamtNodeArray;

/* Collision node: keys with same 5-bit hash chunk
 * Stores a linked list of entries that all share the same hash prefix. */
typedef struct {
    uint32_t hash_prefix; /* The 5-bit hash chunk that collided */
    HamtEntry *entries;   /* Linked list of key/value entries */
} HamtNodeCollision;

/* Tagged union for node types */
struct HamtNode {
    HamtNodeType type;
    uint32_t ref_count;   /* Reference count for this node */
    union {
        HamtNodeBitmap bitmap;
        HamtNodeArray array;
        HamtNodeCollision collision;
    } as;
};

/* Public API - Lifecycle */

/* Create a new empty HAMT. Returns a heap-allocated Hamt with ref_count=1. */
Hamt *tur_hamt_new(void);

/* Free a HAMT. Decrements the root's ref count; if it reaches zero,
 * recursively frees all nodes. Does NOT free keys or values. */
void tur_hamt_free(Hamt *m);

/* Retain a HAMT (increment ref count). Returns the same pointer. */
Hamt *tur_hamt_retain(Hamt *m);

/* Public API - Core operations */

/* Insert or update a key/value pair. Returns a new HAMT (structural sharing).
 * hash: full 64-bit hash of the key ( caller must compute this)
 * key: key pointer (HAMT stores this pointer; caller owns lifetime)
 * val: value pointer (HAMT stores this pointer; caller owns lifetime)
 * Returns: new Hamt with ref_count=1, or same Hamt if no change.
 * Note: If key already exists, value is updated (last writer wins). */
Hamt *tur_hamt_set(Hamt *m, uint64_t hash, int64_t key, int64_t val);

/* Delete a key. Returns a new HAMT (structural sharing).
 * hash: full 64-bit hash of the key
 * key: key pointer to delete
 * Returns: new Hamt with ref_count=1, or same Hamt if key was not present. */
Hamt *tur_hamt_del(Hamt *m, uint64_t hash, int64_t key);

/* Check if a key exists.
 * hash: full 64-bit hash of the key
 * key: key pointer to check
 * Returns: true if key exists in the map */
bool tur_hamt_has(Hamt *m, uint64_t hash, int64_t key);

/* Get the value for a key.
 * hash: full 64-bit hash of the key
 * key: key pointer to lookup
 * Returns: value pointer, or NULL if not found */
int64_t tur_hamt_get(Hamt *m, uint64_t hash, int64_t key);

/* Key-equality-aware variants (TCE4).
 *
 * These behave exactly like set/del/has/get above, except keys are compared
 * with `eq` instead of pointer identity. `eq` is an opaque word interpreted
 * as `bool (*)(int64_t, int64_t)` -- e.g. a Turmeric closure handle -- and is
 * consulted only when two keys share the same 64-bit hash. Passing eq == NULL
 * is identical to the plain entry point. Use these for content-typed keys
 * (e.g. strings) where equal text may live at distinct addresses. */
/* tur_hamt_keyeq_fn is typedef'd early (before tur_hamt_key_ops) so no redecl needed. */
Hamt *tur_hamt_set_eq(Hamt *m, uint64_t hash, int64_t key, int64_t val, tur_hamt_keyeq_fn eq);
Hamt *tur_hamt_del_eq(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq);
bool  tur_hamt_has_eq(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq);
int64_t tur_hamt_get_eq(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq);

/* Content-keyed cstr convenience entry points.
 *
 * One-call set/del/has/get for NUL-terminated string keys: the key is hashed
 * by content (tur_hamt_hash_str) and compared by content on collision (an
 * internal strcmp comparator through the _eq family above).  Use these for
 * string keys instead of hash-ptr + the plain entry points -- identity
 * hashing/compare of string keys only "works" when the C compiler merges
 * identical literals, which C11 6.4.5p7 leaves unspecified, and never works
 * for keys built at runtime.  The map stores the key POINTER (no copy);
 * caller owns its lifetime, exactly like tur_hamt_set. */
Hamt *tur_hamt_set_cstr(Hamt *m, const char *key, int64_t val);
Hamt *tur_hamt_del_cstr(Hamt *m, const char *key);
bool  tur_hamt_has_cstr(Hamt *m, const char *key);
int64_t tur_hamt_get_cstr(Hamt *m, const char *key);

/* Context-carrying key-equality comparator (prereq 2a -- turi-map-set-hamt-
 * interpreter-gap.md).  Same as tur_hamt_keyeq_fn but with a trailing `void *ctx`
 * threaded through to every collision-time compare.  This is what lets a key
 * comparator that is a Turmeric *closure* (needing its captured environment +
 * the interpreter's TuriEnv*) ride along: the interpreter packs {env, closure}
 * into ctx and passes a trampoline `eq` that unpacks ctx and calls turi_call.
 * The compiled path is unaffected -- it keeps calling the no-ctx _eq family. */
typedef bool (*tur_hamt_keyeq_ctx_fn)(int64_t, int64_t, void *ctx);

/* ctx-carrying variants of the _eq family.  Passing ctx == NULL behaves exactly
 * like the no-ctx _eq entry point with one extra ignored argument, so these
 * require no codegen change and no fixture regen.  While a ctx op is in flight
 * the no-ctx comparator hook is cleared (and vice versa), so plain and ctx
 * operations nest correctly (e.g. a content-keyed map of int-keyed maps). */
Hamt *tur_hamt_set_eq_ctx(Hamt *m, uint64_t hash, int64_t key, int64_t val, tur_hamt_keyeq_ctx_fn eq, void *ctx);
Hamt *tur_hamt_del_eq_ctx(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_ctx_fn eq, void *ctx);
bool  tur_hamt_has_eq_ctx(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_ctx_fn eq, void *ctx);
int64_t tur_hamt_get_eq_ctx(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_ctx_fn eq, void *ctx);

/* Boxed-key ownership (WKC2 -- wide map-key carrier).
 *
 * A boxed key is a heap allocation holding a refcount header followed by `n`
 * bytes copied from the source key.  The map owns the box: it is retained when
 * an entry is duplicated across structural sharing and released when an entry
 * is freed, so it is freed exactly once even when several persistent versions
 * retain the entry.  Use these for keys that do not fit (or are not) a single
 * inline word -- e.g. multi-word struct/ADT keys -- where the comparator reads
 * the key bytes through the payload pointer. */

/* Allocate a boxed key holding a copy of `n` bytes from `src`.  Returns a
 * pointer to the PAYLOAD (the key bytes); the box starts with refcount 1.
 * Pass the returned pointer as the `key` to the _eq_owned operations below. */
void *tur_hamt_box_key(const void *src, size_t n);

/* Retain / release a boxed key by its payload pointer.  release frees the box
 * when the refcount reaches zero.  NULL-safe. */
void  tur_hamt_box_retain(void *boxed_key);
void  tur_hamt_box_release(void *boxed_key);

/* Convenience: the standard ops vector for tur_hamt_box_key-allocated keys. */
tur_hamt_key_ops tur_hamt_box_key_ops(void);

/* Generic content comparator for two tur_hamt_box_key-allocated payloads.  Reads
 * each payload's byte length from its box header and compares the bytes, so a
 * SINGLE comparator serves every multi-word by-value key type (struct/ADT) --
 * no per-type comparator or C type name is needed.  The signature matches
 * tur_hamt_keyeq_fn (a MapKey `mk-cmp` carrier comparator): the two int64 args
 * are the boxed-key PAYLOAD pointers threaded through the HAMT on a hash
 * collision.  POD-safe for :copy structs; padding bytes are copied verbatim by
 * tur_hamt_box_key so two structurally-equal keys built the same way compare
 * equal. */
bool tur_hamt_box_key_eq(int64_t a, int64_t b);

/* Ownership-aware variants of the _eq family.  Identical to the _eq calls,
 * except `ops` installs key retain/release for the duration of the operation
 * (so structural copies retain the box and freed entries release it) and is
 * stamped onto the resulting map so a later tur_hamt_free releases its keys.
 *
 * Each call consumes exactly ONE reference to the passed `key`:
 *   - set: on insert the reference transfers into the stored entry; on update
 *     (the key already exists by content) the passed key is not stored and its
 *     reference is released before returning.
 *   - get / has / del: the passed key is a transient probe and is released
 *     before returning (del additionally releases the removed entry's key).
 * Passing ops == {NULL,NULL} is identical to the plain _eq entry point. */
Hamt *tur_hamt_set_eq_owned(Hamt *m, uint64_t hash, int64_t key, int64_t val, tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops);
Hamt *tur_hamt_del_eq_owned(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops);
bool  tur_hamt_has_eq_owned(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops);
int64_t tur_hamt_get_eq_owned(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, tur_hamt_key_ops ops);

/* Flag-driven convenience wrappers (WKC3 -- aggregate-key lowering).
 *
 * `owned != 0` selects tur_hamt_box_key_ops() (the standard boxed-key ownership
 * for tur_hamt_box_key-allocated keys); `owned == 0` is the plain _eq path
 * (keys not owned).  These have a flat scalar/pointer ABI so the generated
 * program can declare them via (extern-c ...) without a by-value ops struct.
 * `owned` is int64_t so the extern-c `:int` prototype matches exactly. */
Hamt *tur_hamt_set_eq_o(Hamt *m, uint64_t hash, int64_t key, int64_t val, tur_hamt_keyeq_fn eq, int64_t owned);
Hamt *tur_hamt_set_eq_vo(Hamt *m, uint64_t hash, int64_t key, int64_t val, tur_hamt_keyeq_fn eq, int64_t owned,
                         void (*val_retain)(void *), void (*val_release)(void *));
Hamt *tur_hamt_del_eq_o(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, int64_t owned);
Hamt *tur_hamt_del_eq_vo(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, int64_t owned,
                         void (*val_retain)(void *), void (*val_release)(void *));
bool  tur_hamt_has_eq_o(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, int64_t owned);
int64_t tur_hamt_get_eq_o(Hamt *m, uint64_t hash, int64_t key, tur_hamt_keyeq_fn eq, int64_t owned);

/* GDE3: structural equality using each map's stamped key comparator.
 * a_handle and b_handle are opaque int64_t Map handles (as used by Turmeric).
 * val_cmp is a bool(*)(int64_t,int64_t) comparator for value equality.
 * The key comparator is read from a_handle's Hamt->key_ops.eq (NULL = identity).
 * Returns true iff both maps have the same count and every key/value pair in a
 * has a matching entry in b. */
bool tur_hamt_eq_dynamic(int64_t a_handle, int64_t b_handle, int64_t val_cmp);

/* Get the number of key/value pairs in the map. O(1). */
int64_t tur_hamt_count(Hamt *m);

/* Merge two HAMTs. b wins on collision.
 * Returns: new Hamt with ref_count=1 containing all entries from both maps.
 *         If a key exists in both, the value from b is used. */
Hamt *tur_hamt_merge(Hamt *a, Hamt *b);

/* Public API - Iteration */

/* Iterator state for in-order traversal */
typedef struct {
    Hamt *map;
    HamtNode **stack;       /* Stack of nodes to visit */
    uint32_t *cidx;         /* Per-node child indices, parallel to stack */
    size_t stack_cap;      /* Allocated capacity of stack */
    size_t stack_len;      /* Current length of stack */
    HamtEntry *coll_entry; /* Current position in collision chain */
    bool done;             /* Iteration complete */
} HamtIter;

/* Initialize an iterator. Must be paired with tur_hamt_iter_free. */
void tur_hamt_iter_init(HamtIter *iter, Hamt *m);

/* Free iterator resources. Must be called when done iterating. */
void tur_hamt_iter_free(HamtIter *iter);

/* Advance iterator to next key/value pair.
 * Returns: true if a pair was found, false if iteration is complete.
 * Output parameters (only valid if return is true):
 *   - hash_out: receives the full 64-bit hash of the key
 *   - key_out: receives the key pointer
 *   - val_out: receives the value pointer */
bool tur_hamt_iter_next(HamtIter *iter, uint64_t *hash_out, int64_t *key_out, int64_t *val_out);

/* Iterator-box wrapper: heap-allocated HamtIter + current (hash, key, val)
 * slots.  Pure-Turmeric Eq[Map] / Eq[Set] drive the loop via these accessors
 * so the iteration core stays in Turmeric while keeping the underlying iter
 * state on the C side. */
void *tur_hamt_iter_alloc(Hamt *m);
void tur_hamt_iter_destroy(void *box);
bool tur_hamt_iter_advance(void *box);
int64_t tur_hamt_iter_cur_hash(void *box);
int64_t tur_hamt_iter_cur_key(void *box);
int64_t tur_hamt_iter_cur_val(void *box);

/* Read the stamped key comparator off a HAMT root (NULL = identity).  The
 * pure-Turmeric Eq[Map] loop threads this into tur_hamt_get_dynamic for
 * content-correct equality on maps built with explicit key comparators. */
void *tur_hamt_keyeq(Hamt *m);

/* Lookup using the stamped key comparator when keyeq is non-NULL, else
 * identity.  Keeps the pure-Turmeric loop body free of a conditional. */
int64_t tur_hamt_get_dynamic(Hamt *m, int64_t hash, int64_t key, void *keyeq);

/* has? variant of get_dynamic. */
bool tur_hamt_has_dynamic(Hamt *m, int64_t hash, int64_t key, void *keyeq);

/* Public API - Debugging */

/* Dump the HAMT structure to a file for debugging.
 * Produces a human-readable representation of the node tree. */
void tur_hamt_dump(Hamt *m, FILE *out);

/* Dump the HAMT structure in DOT format for Graphviz visualization. */
void tur_hamt_dump_dot(Hamt *m, FILE *out);

/* Dump the HAMT in DOT format to stderr (convenience wrapper for Lisp bindings). */
void tur_hamt_dump_dot_stderr(Hamt *m);

/* Hash utilities - use xxHash64 as the default hash function */

/* Compute xxHash64 of a memory region. */
uint64_t tur_hamt_hash_xxh64(const void *data, size_t len);

/* Compute xxHash64 of a string (NUL-terminated). */
uint64_t tur_hamt_hash_str(const char *str);

/* Pointer hash - hash a pointer value directly. */
uint64_t tur_hamt_hash_ptr(void *ptr);

/* Higher-order operations */

/* Map: return a new HAMT with each value replaced by fn(val). */
Hamt *tur_hamt_map(Hamt *m, int64_t (*fn)(int64_t val, void *ctx), void *ctx);

/* Filter: return a new HAMT with only entries where fn(key, val, ctx) is true. */
Hamt *tur_hamt_filter(Hamt *m, bool (*fn)(int64_t key, int64_t val, void *ctx), void *ctx);

/* Reduce: fold all entries (order unspecified). Returns final accumulator. */
int64_t tur_hamt_reduce(Hamt *m, int64_t (*fn)(int64_t acc, int64_t key, int64_t val, void *ctx), int64_t init, void *ctx);

/* Merge-with: merge a and b; for duplicate keys call fn(val_a, val_b, ctx) to
 * resolve the conflict. Keys present in only one map keep their value. */
Hamt *tur_hamt_merge_with(Hamt *a, Hamt *b, int64_t (*fn)(int64_t va, int64_t vb, void *ctx), void *ctx);

/* Show: return heap-allocated string "{k->v, ...}" — caller must free. */
char *tur_hamt_show(Hamt *m);

/* Transient mode — mutable batch-construction wrapper.
 * A transient is created from an immutable map, allows in-place mutation
 * for bulk construction, then is sealed back into an immutable map.
 * A transient MUST NOT be used after calling tur_hamt_persistent(). */

typedef struct HamtTransient {
    HamtNode *root;      /* Mutable root node (may be NULL for empty) */
    uint32_t count;      /* Current entry count */
    uint64_t token;      /* Owner token — unique per transient to prevent aliasing */
} HamtTransient;

/* Fork a transient from an immutable map.
 * The original map is unchanged; the transient starts with the same data. */
HamtTransient *tur_hamt_transient(Hamt *m);

/* Mutate a transient: insert or update key with given hash. */
void tur_hamt_transient_set(HamtTransient *t, uint64_t hash, int64_t key, int64_t val);

/* Mutate a transient: delete key with given hash. */
void tur_hamt_transient_del(HamtTransient *t, uint64_t hash, int64_t key);

/* Seal transient into an immutable map.
 * Returns a new Hamt; the transient is freed and must not be used again. */
Hamt *tur_hamt_persistent(HamtTransient *t);

/* Internal allocation functions (exposed for testing) */

/* Allocate a HAMT node. Returns pointer with ref_count=1. */
HamtNode *tur_hamt_node_alloc(size_t size);

/* Retain a node (increment ref count). */
void tur_hamt_node_retain(HamtNode *n);

/* Release a node (decrement ref count; free if reaches zero). */
void tur_hamt_node_release(HamtNode *n);

#endif /* TUR_HAMT_H */
