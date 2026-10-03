#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include "buf.h"
#include "lifetimes.h"  /* Phase 13: Lifetime annotations */
#include "forms.h"      /* Phase HKT-P1: for Span */

/* Forward declarations for typeclasses (Phase 15) */
typedef struct TypeClass TypeClass;
typedef struct TypeClassInstance TypeClassInstance;

/* Forward declaration for effect rows (Phase 19) */
struct EffectRow;

/* Phase 11 / UT0: Ownership/copy traits */
typedef enum CopyKind {
    CK_UNIQUE,    /* Unique: at most one live reference (affine; replaces CK_MOVE) */
    CK_COPY,      /* Copy: bitwise duplication allowed */
    CK_UNSIZED,   /* Unsized: size unknown at compile time (e.g., slices) */
    CK_LINEAR,    /* Linear: must be used exactly once (LT0) */
    CK_MULTISHOT, /* MS1: multi-shot continuation; may be resumed any number of times */
} CopyKind;

/* c-fn-ptr-element-and-size-precision-gap fix: optional precise C spelling for
 * an integer carrier used in a (c-fn ...) FFI signature.  CNUM_DEFAULT means
 * "spell by TypeKind" (int64_t / uint64_t / ...).  The other variants make
 * type_c_name emit the precise C name so a typed c-fn parameter matches an
 * external C callback's `size_t` / `ptrdiff_t` slot exactly.  This is a
 * pure C-spelling hint: the runtime carrier stays the underlying TypeKind
 * (TY_UINT64 / TY_INT64), so a :usize is ABI-identical to a :uint64 and is
 * erased to the carrier everywhere except the precise cfnptr typedef path. */
typedef enum CNumSpelling {
    CNUM_DEFAULT = 0, /* spell by TypeKind */
    CNUM_SIZE_T,      /* size_t   (carrier TY_UINT64) */
    CNUM_PTRDIFF_T,   /* ptrdiff_t (carrier TY_INT64) */
} CNumSpelling;
/* UT0: backward-compat alias — all former CK_MOVE sites now mean CK_UNIQUE */
#define CK_MOVE CK_UNIQUE

/* ST0: Substructural type discipline.
 * Controls which structural rules (weakening, contraction) apply to a value.
 *   SK_STRUCTURAL -- default: weakening + contraction both allowed
 *   SK_AFFINE     -- no contraction: can be discarded, cannot be duplicated
 *   SK_RELEVANT   -- no weakening: must be used, can be duplicated
 *   SK_LINEAR     -- no weakening, no contraction: use exactly once
 * SK_STRUCTURAL == 0 so zero-initialised Types are structural by default. */
typedef enum SubstructKind {
    SK_STRUCTURAL = 0,  /* Default: weakening + contraction both allowed */
    SK_AFFINE,          /* No contraction: can discard, cannot duplicate */
    SK_RELEVANT,        /* No weakening: must use, can duplicate */
    SK_LINEAR,          /* No weakening, no contraction: use exactly once */
} SubstructKind;
/* Phase TP3: Arbitrary-arity kind representation.
 * Kind is an integer-backed type; the arity-5 cap is gone.
 *
 * Encoding:
 *   0       -- KIND_STAR (* -- a concrete, fully-applied type)
 *   1..N    -- arity-N arrow kind; value equals arity
 *              (KIND_ARROW=1 means * -> *, KIND_ARROW2=2 means * -> * -> *, ...)
 *   0xFFFE  -- KIND_TYPEROW (kind-level `List Type`; a row of types, e.g.
 *              the `[Pos Vel]` component row of an ECS Query -- see
 *              docs/archive/history/variadic-hkt-rows-missing.md). A sentinel, not
 *              an arrow kind: a row is a first-class kind, not a constructor
 *              you apply, so kind_apply_one is the identity on it.
 *   0xFFFF  -- KIND_ROW (effect row variable; sentinel, not an arrow kind)
 *
 * kind_for_arity(n) == (Kind)n for all n.
 * kind_apply_one(k) == k-1 for k in 1..0xFFFD, identity for STAR, TYPEROW, ROW.
 * The hkt_kind field on Type uses this encoding; sizeof(Kind)==2 verified below. */
typedef uint16_t Kind;
static_assert(sizeof(Kind) == 2, "Kind must be exactly 2 bytes");
#define KIND_STAR   ((Kind)0)       /* * -- concrete type, e.g. int, bool, vec<int> */
#define KIND_ARROW  ((Kind)1)       /* * -> * -- unary constructor, e.g. vec, option */
#define KIND_ARROW2 ((Kind)2)       /* * -> * -> * -- binary constructor, e.g. result */
#define KIND_ARROW3 ((Kind)3)       /* * -> * -> * -> * -- ternary, e.g. Tuple3 */
#define KIND_ARROW4 ((Kind)4)       /* 4-ary, e.g. Tuple4 */
#define KIND_ARROW5 ((Kind)5)       /* 5-ary, e.g. Tuple5 */
#define KIND_TYPEROW ((Kind)0xFFFE) /* [*] -- kind-level list of types (Row :: List Type) */
#define KIND_ROW    ((Kind)0xFFFF)  /* Row -- effect row variable */
/* Phase 13: Lifetime annotations */
/* Lifetimes are purely an elaborator construct - no runtime representation */

/* Phase 2 type system: function types are stored inline without recursion
 * by using a simple array of TypeKind values. Compound types (structs, 
 * generics) get added in later phases. */
typedef enum TypeKind {
    TY_UNKNOWN = 0,   /* not-yet-resolved (elaboration in progress) */
    TY_NIL,           /* unit / void; (do) with no body, (println …) result */
    TY_BOOL,
    TY_INT,           /* int64_t — default signed integer; alias for int64 */
    TY_FLOAT,         /* double — default float; alias for float64 */
    TY_CSTR,          /* const char* — string literal type for now */
    TY_PTR_VOID,      /* void* — for extern-c and raw pointers */
    TY_FN,            /* function type — requires checking as.fn */
    /* Phase 5: ref<T> — owning pointer with move semantics */
    TY_REF,           /* ref<T> — owning handle, auto-defer drop at scope end */
    /* Phase 9: rc<T> + weak<T> — reference-counted shared ownership */
    TY_RC,            /* rc<T> — reference-counted owning pointer */
    TY_WEAK,          /* weak<T> — non-owning observer for rc<T> */
    /* Phase 12: Borrow traits */
    TY_REF_IMMUT,    /* &T — immutable borrow (non-owning reference) */
    TY_REF_MUT,       /* &mut T — mutable borrow (exclusive non-owning reference) */
    /* Phase 15: Typeclasses */
    TY_TYPECLASS,    /* Typeclass type (e.g., Eq, Ord) */
    TY_TYPECLASS_INST, /* Typeclass instance type */
    /* Phase 17: Exceptions */
    TY_EXCEPTION,    /* Exception type - wraps any value for throw/catch */
    /* Phase 18: Delimited continuations */
    TY_CONT,         /* cont<T> - captured continuation that returns T */
    /* Phase B2: Cloneable continuations */
    TY_CLONEABLE_CONT, /* cloneable_cont<T> - multi-shot continuation that returns T */
    /* Phase 11: User-defined struct types */
    TY_STRUCT,       /* user-defined struct type - see as.struct_ for StructDef */
    /* Phase G0: User-defined sum types (ADTs) */
    TY_ADT,          /* user-defined sum type (ADT) — see as.adt_ for AdtDef */
    /* Phase R2: Panic - diverging/never type */
    TY_NEVER,        /* ! - diverging type (never returns; bottom type) */
    /* Phase HKT-P1: Type application (partially-applied type constructor) */
    TY_APP,          /* (type-app F A) — apply type constructor F to argument A */
    /* Phase HKT-P2: Recursive types */
    TY_REC,          /* (defrec Name [params] body) — recursive type binder */
    /* Phase N: fixed-width numeric types */
    TY_INT8,         /* int8_t */
    TY_INT16,        /* int16_t */
    TY_INT32,        /* int32_t */
    TY_INT64,        /* int64_t (alias for TY_INT) */
    TY_UINT8,        /* uint8_t */
    TY_UINT16,       /* uint16_t */
    TY_UINT32,       /* uint32_t */
    TY_UINT64,       /* uint64_t */
    TY_FLOAT32,      /* float */
    TY_FLOAT64,      /* double (alias for TY_FLOAT) */
    /* Phase X3: Set literal type — sorted int64_t array at runtime */
    TY_SET,          /* set — tur_set_t * */
    /* Phase HRT0: Higher-ranked types */
    TY_FORALL,       /* (forall [a ...] T) — universally quantified type (rank-2+) */
    TY_EXISTS,       /* (exists [a ...] T) — existentially quantified type */
    /* Phase G2: unresolved GADT type variable (skolem that escaped its arm scope) */
    TY_TYVAR,        /* free type variable — field whose type is a GADT type param not yet known */
    /* LT3: lref<T> — linear owning pointer; must be consumed exactly once */
    TY_LREF,         /* lref<T> — CK_LINEAR; silent drop is an error */
    /* IT0: Union types (-Xunion-types) */
    TY_UNION,        /* (A | B | C) — anonymous closed union type */
    /* IT2: Intersection types (-Xintersection-types) */
    TY_INTERSECTION, /* (A & B & C) — anonymous closed intersection type */
    /* IT4: Top type (gradual typing) — available with -Xunion-types or -Xintersection-types */
    TY_ANY,          /* any — top type; every type is a subtype of any */
    /* ET3: Handler type — a handler for a named algebraic effect */
    TY_HANDLER,      /* handler<Effect, ValueType, ResultType> */
    /* CT0: Contract type — { x : T | p }; a refinement-checked wrapper around T */
    TY_CONTRACT,     /* { var : base_type | predicate } */
    /* SS0a: Session type protocol constructors (-Xsessions) */
    TY_SESSION,      /* Session[P] -- linear channel endpoint carrying protocol P */
    TY_SEND,         /* Send[T, Q] -- send T then continue as Q */
    TY_RECV,         /* Recv[T, Q] -- receive T then continue as Q */
    TY_CLOSE,        /* Close -- protocol complete; must be consumed by close */
    TY_CHOOSE,       /* Choose[P, Q] -- internal choice (this endpoint picks branch) */
    TY_BRANCH,       /* Branch[P, Q] -- external choice (peer picks; this endpoint selects) */
    TY_SESSION_REC,  /* Rec[label, F] -- recursive protocol mu-type (distinct from HKT-P2 TY_REC) */
    TY_TIMEOUT,      /* SS3c: Timeout[Q, P] -- success continuation Q; timeout continuation P */
    TY_SESSION_PAIR, /* SS1: internal pair [Session[P], Session[dual(P)]] returned by make-session */
    TY_SESSION_RECV_PAIR, /* SS2: internal pair [T, Session[Q]] returned by recv */
    TY_SESSION_OFFER,     /* SS2: internal result of offer; matched with Left/Right patterns */
    /* SS5: Multi-party global protocol types (-Xsessions) */
    TY_GLOBAL,            /* Global[...] -- a multi-party global protocol (compile-time only) */
    TY_ROLE,              /* Role[G, R]  -- an endpoint of protocol G playing role R */
    /* DV0: Dynamic var reference type (-Xdynamic-vars) */
    TY_DYNVAR,       /* dynvar<T> -- internal marker for a dynamic var binding; wraps declared value type T.
                        Only held in DynVarEntry during elaboration; never appears as the type of a user expression. */
    /* GF1: Generator type */
    TY_GENERATOR,    /* generator<T> -- heap pointer to C state-machine struct; _next returns void* */
    /* SYM0 (runtime-symbols-plan): first-class interned symbol type (-Xsymbols).
     * A :Sym value is a non-null pointer to a static `struct __tur_sym` record.
     * Two keywords with the same name are pointer-identical; eq is `==` and
     * hashing reads a precomputed field. */
    TY_SYM,          /* :Sym -- interned runtime symbol (const struct __tur_sym *) */
    /* Variadic HKT rows (docs/archive/history/variadic-hkt-rows-missing.md, Layer 2):
     * a compile-time-only *row of types* -- an ordered list of element types,
     * surface-spelled `#row{T1 T2 ...}`.  hkt_kind == KIND_TYPEROW.  Used as a
     * type argument to a row-parameterised constructor (e.g. an ECS Query over
     * `#row{Pos Vel}`); never the type of a runtime value, so it erases at
     * codegen like TY_TYPECLASS / TY_GLOBAL. */
    TY_TYPEROW,
    /* Stage 1 (macro-system-direction-plan): compile-time syntax object.
     * A Syntax value wraps a reader Form*.  It exists only in the
     * interpreter (TURI_SYNTAX) and, later, in macro-time evaluation --
     * never as the type of a compiled runtime value, so it erases at
     * codegen like TY_TYPECLASS / TY_TYPEROW. */
    TY_SYNTAX,
} TypeKind;

/* SS5: Global protocol interaction tree (compile-time only, arena-allocated).
 * Forward-declared here so Type can reference them; defined in elab_internal.h. */
typedef struct GlobalInteraction GlobalInteraction;
typedef struct GlobalBranch GlobalBranch;

/* Phase G0: Constructor field descriptor for ADTs */
typedef struct CtorField {
    TypeKind kind;          /* field type kind */
    TypeKind inner_kind;    /* for ref/rc: inner type; TY_UNKNOWN otherwise */
    /* TP1: non-NULL when the field was declared as a type variable (e.g. `a` in
     * `(defdata Opt2 [a] (Yep a))`).  Carries a TY_TYVAR type node so that
     * tooling / future type-inference phases can inspect the parameter name.
     * The runtime representation is always TY_INT (int64_t carrier). */
    struct Type *full_type;
    /* CONV-S0 (struct/ADT convergence): field name for record-style variants,
     * declared as `(Circle [radius : float])`.  NULL for positional-style
     * variants (`(Just :int)`), where fields are anonymous.  When non-NULL the
     * name backs field access `(.radius v)` and by-name match binding, exactly
     * like a struct field.  Interned/NUL-terminated. */
    const char *name;
    /* structdef-retirement slice 5 A1: effect-row annotation on a `fn`-typed
     * field, e.g. `[run : fn #fx{Write}]`.  Mirrors StructField.effect_row so a
     * lowered `defstruct` (record ADT) keeps the capability-field effect
     * tracking: effect_check merges this row when a `(.run v)` call invokes the
     * stored fn.  NULL for non-fn fields or fn fields with no effect annotation. */
    struct EffectRow *effect_row;
    /* drop-glue-shallow-nested-owning-aggregate: non-NULL when this field is a
     * nested owning aggregate stored behind the int64 carrier -- a by-value
     * struct/ADT product that itself `needs_drop_glue` (transitively owns an
     * rc/ref/weak).  Such a field's `full_type` is deliberately left NULL (a
     * carrier-ADT full_type would misclassify field READS), so this dedicated
     * slot carries the inner def for the drop path: it (a) transitively flips
     * the owner's `needs_drop_glue`, and (b) tells the by-value drop/walk glue
     * to release the boxed sub-aggregate via `drop_glue_<Inner>` /
     * `walk_glue_<Inner>` instead of leaking it.  NULL for a directly-owning
     * (rc/ref/weak) field or a non-owning inline aggregate. */
    const struct AdtDef *drop_inner_def;
} CtorField;

/* Phase SZ6: Type-level size index term.
 * A `SizeTerm` is a compile-time natural-number expression over the `Size`
 * GADT: a constant (`Static n`), a size variable (a GADT type-parameter name),
 * or an `Add`/`Mul` of two sub-terms.  Size indices are *erased* in codegen
 * (zero runtime cost); they live only in the elaborator, where SZ7 compares
 * them for static size checking.  Arena-allocated. */
typedef enum SizeTermKind {
    SZT_CONST,  /* (Static n) — a literal natural number */
    SZT_VAR,    /* a size variable, e.g. `n` in (SizedVec (Add (Static 1) n) a) */
    SZT_ADD,    /* (Add a b) */
    SZT_MUL,    /* (Mul a b) */
} SizeTermKind;

typedef struct SizeTerm {
    SizeTermKind kind;
    int64_t      konst;            /* SZT_CONST */
    const char  *var;              /* SZT_VAR (interned/borrowed name) */
    struct SizeTerm *lhs, *rhs;    /* SZT_ADD / SZT_MUL */
} SizeTerm;

/* Phase G2: Skolem equality binding — one entry per GADT type parameter per arm */
#define MAX_SKOLEM_BINDINGS 8
typedef struct SkolemBinding {
    const char *name;      /* type parameter name (e.g. "a") */
    TypeKind    kind;      /* concrete resolved TypeKind (e.g. TY_INT) */
    /* TP3: full Type for ADT/struct bindings (e.g. `a → Foo`); NULL for primitives */
    struct Type *full_type;
    /* SZ6: when this parameter is a size index, the captured type-level size
     * term from the constructor's return type (e.g. `(Add (Static 1) n)`).
     * NULL for non-size bindings; additive, so existing resolution is
     * unchanged. */
    struct SizeTerm *size_index;
} SkolemBinding;

/* Phase G2: Per-arm skolem environment (stack-allocated in elab_match) */
typedef struct SkolemEnv {
    SkolemBinding bindings[MAX_SKOLEM_BINDINGS];
    uint8_t       n;
} SkolemEnv;

/* Phase G0: Constructor descriptor for ADTs */
typedef struct CtorDef {
    const char   *name;        /* constructor name (interned) */
    uint32_t      n_fields;
    CtorField    *fields;      /* arena-allocated array */
    struct AdtDef *adt;        /* back-pointer to parent ADT */
    uint32_t      tag;         /* integer discriminant tag (0-based) */
    /* Phase G1: explicit return-type annotation for defgadt constructors.
     * NULL for plain defdata constructors.  The Form type is forward-declared
     * in forms.h which is already included above. */
    const struct Form *result_type_form; /* raw parsed annotation, e.g. (Tag int) */
    /* Phase G2: raw field-type annotation forms for skolem-aware resolution.
     * field_forms[i] is the type-annotation form for field i.
     * NULL for plain defdata constructors. */
    const struct Form **field_forms;
    /* CONV-S0 (struct/ADT convergence): true when this constructor was
     * declared record-style -- `(Circle [radius : float])` -- so its fields
     * carry names (CtorField.name) and support field access / by-name match
     * binding.  False for positional-style variants (`(Just :int)`). */
    bool is_record;
} CtorDef;

/* Phase G0: ADT (sum type) descriptor */
typedef struct AdtDef {
    const char *name;            /* ADT name (interned) */
    uint32_t    n_ctors;
    CtorDef   **ctors;           /* arena-allocated pointer array */
    bool        is_copy;         /* :copy annotation */
    /* structdef-retirement slice 4: substructural annotations carried by a lowered
     * `:linear`/`:affine` defstruct, so type_adt() stamps CK_LINEAR / CK_UNIQUE
     * (+ SK_AFFINE) on the ADT type and the exactly-once / at-most-once
     * enforcement propagates from the type's copy_kind exactly as on the struct. */
    bool        is_linear;       /* :linear annotation -- exactly-once (CK_LINEAR) */
    bool        is_affine;       /* :affine annotation -- at-most-once (CK_UNIQUE) */
    bool        needs_drop_glue; /* any ctor has rc/ref/weak fields */
    /* CONV-S1 seam 3: :heap -- this record ADT's natural monomorphic ABI is a
     * typed pointer (`tur_adt_<Name>__<args> *`) to a heap-allocated header, the
     * ADT analogue of a :heap StructDef (Vec/Map/Set).  Set by defdata on `:heap`
     * (and by the defstruct->defadt lowering of a `:heap` struct). */
    bool        is_heap;
    /* Phase G1: GADT flag and type parameters */
    bool        is_gadt;         /* true for defgadt, false for defdata */
    /* SR1 (sum-representation-plan): a constructor field names this ADT, so the
     * type is self-recursive -- `(TPair :Term :Term)`, `(SBind :int :Term
     * :Subst)`, `(StCons :Subst :Stream)`.
     *
     * Recorded at declaration time because it cannot be recovered afterwards: a
     * recursive field rides the int64 carrier and its CtorField.full_type is
     * deliberately left NULL (recording a carrier-ADT full_type would
     * misclassify the field READ), so nothing downstream can tell `(SBind :int
     * :Term :Subst)` from `(SBind :int :int :int)`.
     *
     * The layout is finite either way -- the carrier field is one word -- so
     * this is not a soundness gate; adt_graph_reaches is.  It is the SR1/SR4
     * phase boundary: SR1 covers non-recursive sums, and the recursive ones are
     * SR4, which is blocked on library source that ascribes carrier-erased
     * results back to a sum type (stdlib/logic.tur). */
    bool        is_self_recursive;
    const char **type_params;    /* arena-allocated array of type param names (interned) */
    uint8_t     n_type_params;
    /* TP1/TP2: arena-alloc'd Kind array, one per type_params entry.
     * Initialised to KIND_STAR; TP4 refines based on usage in CtorField.full_type. */
    Kind       *type_param_kinds;
    /* Owning compilation unit (diag file id of the defdata/defgadt form).
     * Mirrors StructDef.origin_file_id so the orphan-instance check can credit
     * an ADT type-argument to the module that defines it. 0 = unknown. */
    uint16_t    origin_file_id;
    /* CONV-S1 (defstruct-as-defadt): true iff this AdtDef was synthesized by
     * lowering a `defstruct` (not written as a `defdata`/`defgadt`).  The
     * lowering should be invisible at the surface, so consumers that would
     * otherwise observe the ADT-ness -- runtime `type-of` (reports "struct"),
     * and the defgadt same-name duplicate check (treats it like a struct for
     * MF4 GADT-shadows-struct coexistence) -- key on this flag. */
    bool        from_struct_lowering;
    /* structdef-retirement slice 2 (CTOR-V0): a `:no-auto-ctor` def suppresses the
     * auto-bound value-namespace constructor, so `(Name ...)` is rejected ("not a
     * function") and construction goes through `make-struct`.  Mirrors
     * StructDef.no_auto_ctor for the lowered record-ADT path. */
    bool        no_auto_ctor;
    /* CONV-S1 (defstruct-as-defadt): once a struct lowers to an ADT, structs and
     * ADTs share one namespace, so a later same-name `defgadt`/`defdata` may
     * SUPERSEDE the struct-origin ADT (the GADT wins).  The superseded def is
     * left registered (its already-elaborated ctor/accessor bindings stay valid
     * for any code that referenced them before the redefinition) but is skipped
     * at C emission so its `tur_adt_<Name>` typedef does not collide with the
     * winner's.  Only ever set on a `from_struct_lowering` def. */
    bool        superseded;
    /* structdef-retirement slice 5 (defopaque migration): an opaque newtype
     * `(defopaque Name [T...] :base [:linear|:affine])`.  An opaque def has NO
     * constructors (n_ctors == 0) and NO fields; it is a named int64_t carrier
     * whose phantom type parameters are erased at codegen.  Migrated off
     * StructDef (which carried `is_opaque` before) so StructDef can be deleted;
     * an opaque value's type is `TY_ADT` with this flag set.  Every codegen path
     * that would emit a typedef / monomorph / by-value layout for an ADT must
     * skip an opaque one -- it stays the int64 carrier with its name kept only
     * for nominal identity (typeclass dispatch, REPL type tags, mangling). */
    bool        is_opaque;
    /* opaque-pointer-c-spelling (docs/archive/opaque-pointer-c-spelling-
     * gate-results.md): true iff this opaque newtype's DECLARED base type is a
     * pointer -- `(defopaque H :ptr<void>)`, `(defopaque H :ptr)`, `(defopaque
     * H :ptr<T>)`.  Before this flag the base form was parsed for POSITION only
     * (to find where the trailing attributes start) and then discarded, so the
     * declared base was not recoverable anywhere downstream.  Read only by
     * `adt_opaque_c_names_as_pointer` (types.c), which is seam-gated. */
    bool        opaque_base_is_ptr;
    /* option-niche: set by the `:non-null` attribute on a pointer-based
     * defopaque -- `(defopaque String :ptr<void> :non-null)`.  A DECLARATION by
     * the type's author that its valid values exclude the null pointer (every
     * constructor allocates; no producer returns 0), which is the soundness
     * condition for `(Option T)` niche filling.  This replaces the former
     * hard-coded String/StringBuilder rows in the eligibility allowlist: the
     * claim now lives at the definition site, next to the constructors it is a
     * claim about.  It is a claim the compiler cannot prove -- inline-C or a
     * coercing `::` can still smuggle a 0 -- so the niche `Some` ctor also
     * checks it at construction and aborts loudly rather than letting
     * `(some null)` silently read back as `(none)`.  Only legal on a pointer
     * base (elab_defopaque rejects it elsewhere); read by
     * `sr3_payload_is_nonnull_pointer` (types.c). */
    bool        opaque_non_null;
    /* sealed-opaque (GRADUATED 2026-08-17, docs/archive/sealed-opaque-plan.md):
     * set by the `:sealed` attribute on a defopaque.  `::` is a COERCING cast,
     * so an ordinary opaque can always be unwrapped to its carrier and re-wrapped
     * as a fresh value -- which bounds every guarantee built on the handle (see
     * docs/archive/frozen-region-aliasing-via-coercing-cast.md).  When this is
     * set, elab_ascribe refuses to cross the type/representation boundary
     * outside `sealed_module`. */
    bool             sealed;
    /* The module that declared this def, or NULL for a moduleless top level.
     * Interned, so compare by pointer against `e->current_module_name`.  Only
     * read for the `sealed` check today; `origin_file_id` above stays the
     * FILE-granular identity the orphan-instance check uses, which is a
     * different question (a module can span files). */
    const Symbol    *sealed_module;
    /* r7rs-programs-compile-slowly: defined inside the auto-loaded stdlib's
     * window -- a type the library unit of a split build writes too. */
    bool from_stdlib;
} AdtDef;

/* CONV-S2 (struct/ADT convergence): a single-variant, non-GADT ADT is
 * representationally a product (one named/positional payload, no choice of
 * variant), so codegen specializes it to a flat layout -- no `int tag` word in
 * the typedef, no tag store in the constructor, and no tag test in `match`.
 * Multi-variant ADTs and GADTs (whose tag may drive return-type refinement)
 * keep the tagged-union representation.  Every codegen site that would emit or
 * read the tag word for an ADT gates on this predicate so the typedef, the
 * constructors, and the match sites stay in lockstep. */
static inline bool adt_is_flat_product(const AdtDef *def) {
    return def && def->n_ctors == 1 && !def->is_gadt;
}

/* CONV-S1 seam 4: a NON-PARAMETRIC, single-variant, record-style ADT (the
 * lowered-`defstruct` shape, and a hand-written `(defdata P (P [f : T ...]))`)
 * is emitted with a FLAT, C-ABI-compatible layout -- `typedef struct
 * tur_adt_<Name> { <ty> <field>; ... } tur_adt_<Name>;` with the record's real
 * field names -- plus a `typedef tur_adt_<Name> <Name>;` surface alias, instead
 * of the tagged `union { struct { T _0; ... } <Ctor>; } as;` wrapper.  This
 * makes inline-C that reads the value by its surface type/field names
 * (`opts.name`, `sizeof(<Name>)`, `(<Name> *)p`) compile unchanged once the
 * struct lowers to an ADT -- the central inline-C-ABI graduation blocker.
 *
 * Extended to PARAMETRIC records (CONV-S1 seam 4, keystone): a parametric
 * single-variant record (the lowered `(defstruct Vec [A] ...)` / `(defstruct
 * Tuple3 [A B C] ...)`, and a hand-written `(defdata Box [A] (Box [v : A]))`)
 * now uses the same named layout -- BOTH the generic base `tur_adt_<Name>` and
 * each monomorph `tur_adt_<Name>__<args>` carry the record's real field names
 * (`{ T data; ... }`) instead of the positional `union { struct { T _0; ... }
 * <Ctor>; } as;` wrapper.  This is what makes user / stdlib inline-C that reads
 * a `(Vec A)` / `(Tuple3 ...)` by its field names (`v->len`, `t.e1`) compile
 * against the lowered monomorph.  The memory layout is byte-identical to the old
 * nested form for a single variant, so it is interchangeable in memory; only the
 * C member-access spelling differs, and every generated access site switches
 * together via adt_field_member_path (emit_core.c) -- the typedef (emit_module.c
 * base, types.c monomorph), the ctor stores, field reads, and match binds all
 * key off this one predicate. */
static inline bool adt_uses_named_layout(const AdtDef *def) {
    if (!def || def->n_ctors != 1 || def->is_gadt)
        return false;
    const CtorDef *c = def->ctors[0];
    if (!c || !c->is_record || c->n_fields == 0) return false;
    for (uint32_t i = 0; i < c->n_fields; i++)
        if (!c->fields[i].name) return false;
    return true;
}

/* CONV-S1: a single-variant, non-GADT, NON-PARAMETRIC flat product can flow
 * *by value* -- a flat `tur_adt_<Name>` C aggregate passed/returned/stored
 * directly, rather than through the int64 heap-pointer carrier.  This is the
 * representational prerequisite for lowering `defstruct` to `defadt`.
 *
 * LIVE as of CONV-S1/B3, gated to "leaf" products (every field a scalar) so the
 * recursive HKT carriers stay on the carrier path until B4.  Parametric flat
 * ADTs (stdlib `Fix`, the functor `ReF [a]`, ...) also keep the carrier ABI --
 * their concrete monomorphic by-value layout is the M7 by-value-HKT path's job.
 *
 * Defined in types.c (needs the complete `struct Type` to inspect ctor field
 * full_types, which are only forward-declared here).  See the definition and
 * docs/archive/history/struct-adt-convergence-s1-bridging-findings.md for the gate. */
bool adt_is_byvalue_product(const AdtDef *def);
/* B4 (byvalue-recursive-carrier): true when `def` is a single-variant,
 * single-field recursive carrier wrapper whose sole field is an (F Self)
 * type-application kept on the int64 carrier (Re/Expr).  Its by-value
 * representation IS its carrier int64, so it crosses the fat-closure boundary by
 * reinterpreting the carrier (no heap box, no deref).  (B4 graduated; always
 * active.) */
bool adt_is_byval_recursive_carrier_wrapper(const AdtDef *def);
/* `arg_def` and `functor_def` form a fixpoint pair (Expr/ExprF, Re/ReF): their C
 * typedefs are mutually recursive and only the carrier breaks the cycle. */
bool adt_is_fixpoint_partner_of(const AdtDef *arg_def, const AdtDef *functor_def);
/* CONV-S1 (slice 3): true when a by-value ADT product is large enough (>16
 * bytes) to use the struct-style `const tur_adt_<Name> *` pass-by-pointer ABI. */
bool adt_byval_pass_by_ptr(const AdtDef *def);
/* B4: aggregate byte size of a by-value ADT product (0 if not by-value). */
size_t adt_byval_value_size_bytes(const AdtDef *def);
/* CONV-S1 (slice 4): true when a ctor field is itself a by-value aggregate that
 * is stored INLINE (by value) in the owning by-value product, the way a struct
 * inlines a nested struct field -- as opposed to boxing it behind the int64
 * carrier.  adt_field_inline_c_name returns its inline C type name. */
bool adt_field_is_inline_byval(const CtorField *f);
const char *adt_field_inline_c_name(const CtorField *f);

/* structdef-retirement DS-D: the `StructField` / `StructDef` type descriptors
 * are deleted.  Every `defstruct` lowers to a single-variant record `AdtDef`
 * (see types below); there is no `TY_STRUCT` Type.kind, no `e->struct_defs[]`
 * registry, and no struct-headed `TY_APP`.  `TY_STRUCT` survives only as a
 * runtime any-box reflection tag (`type-of`/`cast`/`is?` report "struct" for a
 * lowered record), not as a `Type.kind`. */

/* Phase 11: canonical default copy-kind by kind (typeclass path is primary; this
 * keeps concrete move/copy semantics deterministic for elaboration/codegen). */
static inline CopyKind typekind_default_copy_kind(TypeKind k) {
    switch (k) {
        case TY_NIL:
        case TY_BOOL:
        case TY_INT:
        case TY_FLOAT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_FN:
        case TY_REF_IMMUT:
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
            return CK_COPY;
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_MUT:
        case TY_EXCEPTION:
        case TY_CONT:
        case TY_STRUCT:   /* default move; actual copy_kind set via type_struct() */
        case TY_ADT:      /* default move; actual copy_kind set via type_adt() */
        case TY_NEVER:     /* never type is move-only (no values exist) */
        case TY_REC:       /* recursive type is move-only in v1 */
            return CK_MOVE;
        /* LT3: lref<T> is always linear — exactly-once ownership */
        case TY_LREF:
            return CK_LINEAR;
        case TY_SET:       /* heap-allocated sorted array — pointer-copied in v1 */
            return CK_COPY;
        case TY_APP:       /* type application — opaque int64_t handle, copy by value */
            return CK_COPY;
        /* Phase N: fixed-width numeric types are all Copy */
        case TY_INT8:
        case TY_INT16:
        case TY_INT32:
        case TY_INT64:
        case TY_UINT8:
        case TY_UINT16:
        case TY_UINT32:
        case TY_UINT64:
        case TY_FLOAT32:
        case TY_FLOAT64:
            return CK_COPY;
        /* Phase HRT0: Quantified types — move-only (no concrete values) */
        case TY_FORALL:
        case TY_EXISTS:
            return CK_MOVE;
        /* IT0: Union types — move-only by default; actual semantics depend on members */
        case TY_UNION:
            return CK_MOVE;
        /* IT2: Intersection types — move-only by default; actual semantics depend on members */
        case TY_INTERSECTION:
            return CK_MOVE;
        /* IT4: any — top type is copy (it's an opaque container) */
        case TY_ANY:
            return CK_COPY;
        /* ET3: handler values are copyable (they are function-pointer structs) */
        case TY_HANDLER:
            return CK_COPY;
        /* CT0: contract type copies like its base type (defaults to copy for safety) */
        case TY_CONTRACT:
            return CK_COPY;
        /* SS0a: Session channels are linear (exactly-once protocol discipline).
         * Protocol descriptor types (Send, Recv, etc.) are move-only; they are
         * type-level constructs that never become runtime values by themselves. */
        case TY_SESSION:
            return CK_LINEAR;
        case TY_SEND:
        case TY_RECV:
        case TY_CLOSE:
        case TY_CHOOSE:
        case TY_BRANCH:
        case TY_SESSION_REC:
        case TY_TIMEOUT:
        case TY_SESSION_PAIR:
        case TY_SESSION_RECV_PAIR:
        case TY_SESSION_OFFER:
            return CK_MOVE;
        /* SS5: Global protocol types -- compile-time only, linear endpoints */
        case TY_GLOBAL:
            return CK_MOVE;
        case TY_ROLE:
            return CK_LINEAR;
        /* DV0: Dynamic var reference is copy (it's an opaque elaboration-time marker) */
        case TY_DYNVAR:
            return CK_COPY;
        /* GF1: Generator is a heap pointer — copy by value (pointer copy) */
        case TY_GENERATOR:
            return CK_COPY;
        /* SYM0: a symbol is an interned pointer into .rodata — freely copyable */
        case TY_SYM:
            return CK_COPY;
        /* Variadic HKT rows: a type-level row is compile-time only -- it never
         * names a runtime value, so the copy/move discipline is moot; COPY. */
        case TY_TYPEROW:
            return CK_COPY;
        /* Stage 1/2 (macro-system-direction-plan): a Syntax value is an
         * arena-resident Form* -- freely copyable, like TY_SYM.  Falling
         * into the CK_MOVE default made `(syntax-list f x x)` a
         * use-after-move error in defmacro* bodies. */
        case TY_SYNTAX:
            return CK_COPY;
        case TY_UNKNOWN:
        default:
            return CK_MOVE;
    }
}
/* MAX_FN_ARITY is NO LONGER a cap on how many parameters a function may have.
 * A function type's per-arg storage is out of line (Type.as.fn.arg_kinds /
 * arg_flags are arena pointers), FnDef/ExternC/Type arities are uint32_t, and
 * the elaboration/emission per-param buffers are arena-sized to the actual
 * parameter count -- so a defn, fn, or extern-c may declare an arbitrary number
 * of positional parameters, matching the emitted C (which has no limit of its
 * own).  This constant survives only as the default size of a few internal
 * codegen fast-path buffers (e.g. the ABI-specialization scratch), each of
 * which falls back gracefully for wider functions rather than rejecting them.
 * The house-style nudge lives in HIGH_ARITY_SOFT_LIMIT, not here. */
#define MAX_FN_ARITY 64
/* The widest function the fat-closure shim family covers: __tur_fatshim0..N
 * and __tur_poly_to_fat0..N in the emitted preamble, the ^fat auto-shim, the
 * `any` widen of a bare fn, and the dynamic call's fixed slots all stop
 * here.  Was 5 until r7rs-apply-more-than-four-arguments raised it, so a
 * Scheme procedure of up to eight parameters can be a first-class value on
 * the compiled back end. */
#define TUR_FAT_SHIM_MAX_ARITY 8

/* arbitrary-fn-arity: the historical hand-written-arity soft ceiling of 16.
 * Declaring more than this many positional params is NOT an error -- arity is
 * unbounded -- but it emits the TUR-W0041 lint nudge toward the Function Arity
 * Style Guide (prefer a defstruct options value or a `& rest :type` variadic). */
#define HIGH_ARITY_SOFT_LIMIT 16

/* forall-dict-pass-multi-constraint-hkt-plan (Task 1.1): maximum number of
 * typeclass constraints carried on a single dict-clone frame -- one leading
 * int64 dict param per constraint.  Matches the call-site `mb1_dicts[16]`
 * resolution cap in elab_call.c and the MAX_FN_ARITY leading-arg budget. */
#define MAX_FN_CONSTRAINTS 16

/* Phase 13: Maximum lifetime parameters per type */
#define MAX_TYPE_LIFETIMES 4

/* Type uses a union to store either a simple kind or function info. */
typedef struct Type {
    TypeKind kind;
    /* Phase 11: Copy/Move trait */
    CopyKind copy_kind;
    /* ST0: Substructural discipline (SK_STRUCTURAL by default) */
    SubstructKind substruct;
    /* Phase 13: Lifetime annotations */
    /* Lifetimes attached to this type (for &T, &mut T, function types with lifetime params) */
    LifetimeId lifetimes[MAX_TYPE_LIFETIMES];
    uint8_t   n_lifetimes;
    /* Phase 15: Typeclasses */
    /* For concrete types, the typeclass instances they implement (e.g., int has Eq, Show) */
    TypeClassInstance **typeclass_instances;
    uint8_t n_typeclass_instances;
    /* Phase TP3: kind annotation -- uint16_t encoding (see Kind typedef above).
     * KIND_STAR for concrete types; KIND_ARROW{N} for N-ary type constructors. */
    Kind hkt_kind;
    /* c-fn-ptr-element-and-size-precision-gap fix: CNumSpelling for integer
     * carriers (0 == CNUM_DEFAULT).  Only consulted by type_c_name in the
     * precise cfnptr typedef path; erased everywhere else. */
    uint8_t c_num_spelling;
    union {
        struct {
            /* Per-arg fast kind cache, out of line: an arena pointer of length
             * `arity` (NULL iff arity == 0), allocated from the process-lifetime
             * global type arena (see tur_fn_args_alloc).  Stored as uint8_t
             * (TypeKind has < 256 enumerators).  Out-of-line so a Type describes
             * ANY number of parameters -- there is no fixed inline ceiling -- and
             * so every Type (fn or not) stays small: the fn variant no longer
             * dominates the union with a big inline array.  Read as arg_kinds[i]
             * exactly like the former inline array (pointer indexing is
             * identical); it is immutable once built and freely shared across
             * by-value Type copies.  A handful of sites that rebuild a fn type
             * with a different arity allocate fresh storage rather than mutate a
             * shared array (see tur_fn_args_alloc call sites). */
            uint8_t *arg_kinds;
            TypeKind result_kind;
            uint32_t arity;       /* bounded only by uint32_t -- no hard cap */
            /* Future-proofing for v3 effects: effect row slot.
             * NULL in v0/v1; treated as empty effect set. */
            struct EffectRow *effect_row;
            /* Phase HRT1: full type info for polymorphic (rank-2+) parameters.
             * NULL for ordinary functions; set when any arg is a forall type.
             * Points to an arena-allocated array of n=arity Type pointers.
             * arg_full_types[i] is NULL for monomorphic args, non-NULL for poly. */
            struct Type **arg_full_types;
            struct Type  *result_full_type; /* NULL or full result type for poly results */
            /* Per-arg ownership/linearity/calling-convention markers, bit-packed
             * into one byte per parameter (arbitrary-fn-arity Phase 1).  Formerly
             * eight separate bool[MAX_FN_ARITY] arrays -- packing cuts the inline
             * per-arg footprint from 8 bytes to 1, which (with the uint8_t
             * arg_kinds above) keeps sizeof(Type) below its pre-64 value so the
             * deep by-value-Type recursion in codegen stays off the stack ceiling.
             * Read/written only through the FN_ARG_* accessors below -- never poke
             * arg_flags[i] directly at call sites.  Flag meanings:
             *   FA_LINEAR      LT2: the i-th param is ^linear
             *   FA_UNIQUE      UT0: ^unique
             *   FA_UNIQUE_MUT  UT2: ^unique ^mut
             *   FA_AFFINE      ST0: ^affine
             *   FA_RELEVANT    ST0: ^relevant
             *   FA_BORROW      LB1: ^borrow -- reads a linear/affine arg without
             *                  consuming it (see stdlib-linear-handle-borrows.md)
             *   FA_FAT         A#1: consumes its arg via the fat-closure calling
             *                  convention (thunk slot 0, env heap struct); a bare
             *                  non-capturing fn is auto-shimmed (EX_FN_TO_FAT)
             *   FA_POLY_FN     CCL: a first-class :fn poly-closure carrier
             *                  (tur_poly_fn_t {env, fn}); boxed via EX_POLY_WRAP
             * Out of line alongside arg_kinds: an arena pointer of length `arity`
             * (NULL iff arity == 0).  Allocated/owned identically. */
            uint8_t *arg_flags;
            /* A#1 (return position): true when the result type carries the
             * ^fat marker.  A bare non-capturing fn returned from this
             * function is auto-shimmed into a fat closure (EX_FN_TO_FAT) at
             * every tail/return leaf, so a fat-call consumer that reads the
             * returned value sees a valid { thunk, env } layout instead of a
             * bare function pointer.  Symmetric with arg_fat[]. */
            bool result_fat;
            /* AR6: variadic rest-param support (& rest :type) */
            bool is_variadic;                  /* true if this fn has a & rest parameter */
            TypeKind rest_kind;                /* type of the rest cons-list elements (fast-path) */
            /* Typed variadic rest: full Type for the rest element when it is a
             * user-defined type (opaque / struct / ADT / type application).
             * NULL for primitive rest (`& rest :int`, etc.), in which case the
             * fast-path TypeKind comparison on rest_kind is used. */
            struct Type *rest_full_type;
            /* closure-drop-glue (mw-compose-of): the `& rest` parameter carries a
             * `^borrow` annotation -- the callee invokes/reads each rest element
             * but does not store or return it, so a fresh uniquely-owned closure
             * passed as a rest argument does NOT escape and the CALLER may free it
             * at scope exit (per-binding-once, so the duplicate-argument aliasing
             * hazard of a callee-side per-apply free does not arise).  Set only
             * unconditionally since closure-drop-glue graduated (2026-07-22). */
            bool rest_borrow;
            /* LS4: index of the parameter whose lifetime the borrow return is
             * tied to (the returned &'a T aliases this argument's storage), or
             * -1 when the return is not a lifetime-tied borrow.  Used by the
             * inter-procedural borrow-escape check at call sites. */
            int8_t result_borrow_arg;
            /* CRU Phase 3 / Option B (first-class closure type): true when this
             * TY_FN denotes a *closure value* -- a fat box { thunk, env... } --
             * rather than a bare function reference (captureless fn / C function
             * pointer address).  A boxed TY_FN is always called through the fat
             * protocol (thunk = slot 0, env = the box) for all arities; a bare
             * TY_FN coerces to a boxed one of the same signature via the
             * EX_FN_TO_FAT auto-shim, never the reverse.  See
             * docs/archive/history/closure-first-class-type-plan.md (B-0..B-4
             * shipped: the bit is set at ~10 sites -- fn-typed struct/ADT
             * fields of arity <= 4 among them -- and read widely). */
            bool boxed;
            /* typed-c-abi-function-pointers: true when this TY_FN denotes a
             * bare C-ABI function pointer `R (*)(A...)` -- spelled `(c-fn
             * [A...] R)` in source -- rather than a Turmeric closure value.
             * A cfnptr is *never* boxed; it carries no implicit environment
             * and lowers to the concrete `R (*)(A...)` typedef (via
             * register_fn_ptr_typedef) in every position.  At the type level
             * it is distinct from an ordinary `(fn ...)`: a *capturing* closure
             * (a boxed fat box) cannot satisfy a cfnptr parameter, while a
             * *captureless* bare fn of the same signature coerces in (a
             * captureless fn IS a bare code pointer at the C ABI).  See
             * docs/archive/history/typed-c-abi-function-pointers.md. */
            bool cfnptr;
            /* sized-types-cross-param-unification: per-parameter raw type
             * annotation Form*, retained so call-site elaboration can
             * re-extract the size-index template (e.g. `(SizedVec n)`)
             * and unify shared size variables across multiple parameters.
             * NULL when no per-param Forms were recorded (e.g. extern-c,
             * thunks, or non-defn fn values).  Arena-allocated array of
             * length `arity`; individual entries may be NULL. */
            const struct Form **param_type_forms;
            /* sized-types-cross-param-unification (non-GADT extension):
             * raw declared-return-type Form, retained so a call expression
             * can infer its `size_index` from a phantom size literal in the
             * callee's return type (e.g. `(Dense (Static 2) A)`).  Mirrors
             * how CtorDef.result_type_form lets sz8_infer_ctor_size_index
             * seed a sized-GADT constructor's index; this extends the same
             * treatment to plain defn-shaped non-GADT carriers (defopaque,
             * defstruct phantom indices).  NULL when no return annotation
             * was recorded. */
            const struct Form *result_type_form;
        } fn;
        /* Phase 5: ref<T> stores the inner type T */
        struct {
            TypeKind inner;   /* The type T that ref<T> owns */
        } ref;
        /* ptr-generic-parameterised-type: typed raw pointer ptr<T>.
         * Carried on kind == TY_PTR_VOID; `inner` is the full pointee type T
         * (arena-allocated), or NULL for the legacy untyped ptr<void> spelling.
         * Folding the typed form onto TY_PTR_VOID (rather than a new TypeKind)
         * keeps every existing raw-pointer code path working unchanged while
         * letting codegen lower ptr<T> to `T *` in C. */
        struct {
            struct Type *inner; /* pointee type T, or NULL for ptr<void> */
            /* c-fn-ptr-element-and-size-precision-gap fix: true for the
             * `ptr<const-T>` spelling -- type_c_name emits `const T *` so a
             * typed c-fn parameter matches an external C callback's
             * const-qualified pointer slot (e.g. `const unsigned char *`).
             * A pure C-spelling hint; the runtime carrier is an ordinary
             * pointer, so a const ptr is ABI-identical to a non-const one. */
            bool is_const;
        } ptr;
        /* Phase 9: rc<T> and weak<T> store the inner type T */
        struct {
            TypeKind inner;   /* The type T that rc<T> or weak<T> points to */
            /* CONV-S1 (slice 2): for an `rc<Name>` whose `Name` is a
             * single-variant record ADT (a lowered struct, or a hand-written
             * record `defdata`), carries the AdtDef * so `(.field rc-of-adt)`
             * auto-derefs through the rc to the variant's named field.  NULL
             * unless inner == TY_ADT. */
            struct AdtDef *adt_def;
        } rc;
        /* Phase 12: Borrow types store the referenced type T */
        struct {
            TypeKind target;  /* The type T being referenced by &T or &mut T */
            /* saffron-dynamic-surface-pass H9: when T is a NAMED type
             * variable (`(& K)` on a generic parameter), its name -- the
             * kind alone cannot say WHICH tyvar, so a call could never bind
             * K from a borrowed argument.  NULL otherwise. */
            const char *target_tyvar;
            /* borrowed-aggregate-key-skips-the-key-check: the target's FULL
             * type when its kind alone does not name it -- `&Pt`, `&String`
             * and `&(Vec int)` are all "a borrow of an ADT/app" by kind, so
             * without this a `(& Pt)` parameter took `(& q)` for any other
             * ADT, and a `(& K)` bound to `int` took a borrowed String.
             * NULL for a scalar target (the kind is the whole type), for a
             * tyvar (see target_tyvar), and where no one recorded it; every
             * comparison falls back to the kind when either side is NULL. */
            const struct Type *target_full;
        } ref_borrow;
        /* Phase 15: Typeclass types */
        struct {
            TypeClass *typeclass;  /* For TY_TYPECLASS - the typeclass itself */
        } typeclass;
        struct {
            TypeClassInstance *instance;  /* For TY_TYPECLASS_INST - a specific instance */
        } typeclass_inst;
        /* Phase 17: Exception types */
        struct {
            TypeKind payload_type;  /* The type of the exception payload */
        } exn;
        /* Phase 18 / CC4: Continuation types. `flavor` selects the runtime that
         * (k v) application sugar resumes against (cps-transform-plan):
         *   CONT_CLONEABLE -- cloneable/call-cc* (tur_cloneable_cont_resume)
         *   CONT_ESCAPE    -- call/cc / escape   (tur_escape_resume)
         *   CONT_SERIAL    -- serial-shift       (tur_serial_cont_resume)
         *   CONT_EFFECT    -- effect handler cont (EX_RESUME / dk_invoke)  */
        struct {
            TypeKind returns;  /* ResetT: the type (k v) yields (the delimited result) */
            TypeKind arg;      /* BodyT: the resume-value type (k) expects; TY_UNKNOWN = unchecked */
            int      flavor;   /* ContFlavor; 0 = CONT_CLONEABLE (default) */
        } cont;
        /* Phase HKT-P1: Type application — (type-app F A) */
        struct {
            struct Type *fn;   /* The type constructor being applied (kind * -> * or * -> * -> *) */
            struct Type *arg;  /* The type argument (kind *) */
            /* constrained-hkt-abstract-var-requires-last-param-free: a
             * HOLE-HEADED partial application -- `(Result _ cstr)`, the shape a
             * wildcard instance head declares.  `fn` is the bare constructor and
             * `arg` is the FIXED argument; the free slot is the hole.  Applying
             * such a type to `X` places `X` at the hole index and the fixed args
             * in the remaining slots, so `(Result _ cstr)` applied to `int` is
             * `(Result int cstr)` -- which ordinary currying cannot express,
             * since a curried prefix can only ever leave the LAST slot free.
             *
             * Encoding is hole-index PLUS ONE so that 0 -- the value every
             * memset/zero-initialised Type already carries -- means "ordinary
             * application, no hole".  Never read this field directly; use
             * type_app_hole_pos() / type_app_has_hole(). */
            uint8_t hole_pos_p1;
        } app;
        /* Phase HKT-P2: Recursive type binder — (defrec Name [params] body) */
        struct {
            const char *name;  /* Interned binder name (e.g. "Fix"); compare by pointer */
            struct Type *body; /* Body type (NULL in v1 when body is not yet evaluated) */
        } rec;
        /* Phase G0: ADT types */
        struct {
            AdtDef *def;
            /* CONV-S4N: arm-local variant narrowing.  Inside a `match` arm that
             * has destructured a multi-variant ADT to one record variant, the
             * scrutinee symbol's binding carries the full ADT head but with
             * `is_narrowed` set and `narrowed_ctor_idx` naming the proven ctor
             * (an index into `def->ctors[]`).  Only `elab_with` and record-field
             * access consult these fields; every other predicate and `type_eq`
             * ignore them, so a narrowed view is interchangeable with the full
             * ADT everywhere else. */
            bool     is_narrowed;
            uint32_t narrowed_ctor_idx;
        } adt_;
        /* Phase HRT0: Universally/existentially quantified types.
         * Phase EX1b: constraint storage for `(exists [a] [(C a) ...] body)`. */
        struct {
            const char **var_names; /* bound variable names (interned, arena-allocated) */
            Kind        *var_kinds; /* kinds of bound variables (arena-allocated) */
            uint8_t      n_vars;
            struct Type *body;      /* body type (arena-allocated) */
            /* EX1b: Optional typeclass constraints on the bound variables.
             * Each entry: (typeclass, var_idx) meaning "(typeclass var_names[var_idx])".
             * n_constraints==0 means no constraints (back-compat with HRT0). */
            TypeClass  **constraint_classes; /* arena-allocated; length n_constraints */
            uint8_t     *constraint_var_idx; /* arena-allocated; length n_constraints */
            uint8_t      n_constraints;
            /* EXG6: `:linear` attribute on `(exists :linear [a] [(C a) ...] body)`.
             * When set, the existential bypasses rc_cb_alloc and the let binding
             * for a pack of this type is treated as a linear value (use-exactly-
             * once via LT1's substructural machinery). */
            bool         is_linear;
        } forall_;
        /* IT0: Union types — (A | B | C) */
        struct {
            struct Type **members;  /* arena-allocated array of member type pointers */
            uint8_t       n_members; /* number of union members (>= 2) */
        } union_;
        /* IT2: Intersection types — (A & B & C) */
        struct {
            struct Type **members;  /* arena-allocated array of member type pointers */
            uint8_t       n_members; /* number of intersection members (>= 2) */
        } intersection_;
        /* Variadic HKT rows (Layer 2): a row of element types `#row{T1 T2 ...}`.
         * Mirrors the union_/intersection_ representation: an arena-allocated
         * array of element Type pointers plus a count.  An empty row (n == 0)
         * is legal (the unit row). */
        struct {
            struct Type **elements;   /* arena-allocated array of element type pointers */
            uint8_t       n_elements; /* number of element types (>= 0) */
            /* P0 typed-field rows: when non-NULL, a parallel array of interned
             * field-name strings of length n_elements. NULL = bare positional
             * row (the existing ECS form `#row{Pos Vel}`). Typed-field rows
             * `#row{k1 : T1 k2 : T2}` populate this so two rows that differ
             * only in field names are distinct types. */
            const char  **field_names;
        } typerow_;
        /* Phase HRT/G2: Named type variable -- parameter typed with a GADT type var */
        struct {
            const char *name;  /* interned type var name (e.g. "a"), or NULL for anonymous escaped skolem */
            /* fmap-over-underdetermined-constructor-is-a-defless-shell: this
             * variable is a parameter a constructor application left OPEN --
             * the `B` of `(Ok 7.1) : (Result float B)` -- rather than one a
             * signature quantifies.  Nothing binds it; it only says "not fixed
             * here".  Copied with the Type, so it survives instantiation. */
            bool open_slot;
            /* associated-type-unusable-nullary-and-generic (half 2): an
             * UNREDUCED associated-type projection -- `(Inner A)` at a type
             * variable, which no instance can answer until the variable is
             * fixed.  It is a type variable in every other respect (its name
             * is the printed projection, so two spellings of the same
             * projection are the same variable), and a call that fixes
             * `assoc_arg` reduces it through the instance (elab_call.c,
             * call_bind_assoc_projections).  NULL for an ordinary variable. */
            const struct Symbol *assoc_of;
            const struct Type   *assoc_arg;
        } tyvar_;
        /* ET3/FH4.1: Handler type — handler<EffectRow, ValueType, ResultType> */
        struct {
            const char *effect_name;  /* FH4.1: single-effect name, or NULL for a
                                       * multi-effect (composed) handler.  Kept for
                                       * source compatibility; the authoritative
                                       * handled set is handled_row. */
            struct EffectRow *handled_row; /* FH4.1: the set of effects this handler
                                       * handles (an ERK_UNRESOLVED name-set built at
                                       * parse time; a one-element row in the single-
                                       * effect case, the union under composition).
                                       * May be NULL for legacy single-effect types. */
            TypeKind    value_kind;   /* the type of the value passed to the effect */
            TypeKind    result_kind;  /* the result type of the handle expression */
            CopyKind    cont_kind;    /* LC0: ownership discipline for the continuation k */
        } handler_;
        /* CT0: Contract type — { x : T | p } */
        struct {
            struct Type      *base_type;  /* the underlying type T */
            const char       *var_name;   /* bound variable x in { x : T | p } */
            const struct Form *predicate; /* predicate expression p (a Form*) */
        } contract_;
        /* SS0a: Session protocol type arguments (-Xsessions) */
        struct {
            struct Type *fst;   /* TY_SESSION: protocol P
                                   TY_SEND/TY_RECV: message type T
                                   TY_CHOOSE/TY_BRANCH: left branch P
                                   TY_SESSION_REC: body type */
            struct Type *snd;   /* TY_SEND/TY_RECV: continuation Q
                                   TY_CHOOSE/TY_BRANCH: right branch Q
                                   NULL for TY_SESSION, TY_CLOSE, TY_SESSION_REC */
            const char  *label; /* TY_SESSION_REC: binder label (interned) */
        } session_;
        /* SS5: Global protocol type (compile-time only) */
        struct {
            const char           *name;     /* interned protocol name */
            const char          **roles;    /* arena-allocated array of interned role names */
            int                   n_roles;
            GlobalInteraction    *body;     /* interaction tree (arena-allocated) */
        } global_;
        /* SS5: Role endpoint type -- in-progress multi-party channel endpoint */
        struct {
            struct Type          *global_type; /* TY_GLOBAL type node */
            const char           *role_name;   /* interned role name */
            GlobalInteraction    *current_step; /* current position in interaction tree */
        } role_;
        /* DV0: Dynamic var type (-Xdynamic-vars) */
        struct {
            struct Type *value_type; /* the declared element type of the dynamic var */
        } dynvar_;
        /* GF1: Generator type -- heap pointer to C state-machine struct */
        struct {
            TypeKind element_kind; /* the TypeKind of values yielded by this generator */
            /* generator-in-generic: the enclosing signature's type variable
             * when the yielded value is typed with one (element_kind is then
             * TY_TYVAR), so gen-unwrap can type its read `A`. */
            const char *element_tyvar;
        } generator_;
    } as;
} Type;

/* Per-arg flag bits packed into Type.as.fn.arg_flags[i] (see the field comment).
 * Access ONLY through the FN_ARG_* accessors -- they keep the packing an
 * implementation detail so call sites read/write named flags, not raw bits. */
enum {
    FA_LINEAR     = 1u << 0,
    FA_UNIQUE     = 1u << 1,
    FA_UNIQUE_MUT = 1u << 2,
    FA_AFFINE     = 1u << 3,
    FA_RELEVANT   = 1u << 4,
    FA_BORROW     = 1u << 5,
    FA_FAT        = 1u << 6,
    FA_POLY_FN    = 1u << 7
};

/* Read a per-arg flag.  `fn` is a `Type.as.fn` lvalue, `i` the param index.
 * Yields a bool (0/1). */
#define FN_ARG_FLAG(fn, i, bit)  (((fn).arg_flags[(i)] & (uint8_t)(bit)) != 0)

/* Set a per-arg flag to a bool value `v` (clears the bit when v is false). */
#define FN_ARG_SET(fn, i, bit, v)                                             \
    do {                                                                      \
        if (v) (fn).arg_flags[(i)] |= (uint8_t)(bit);                         \
        else   (fn).arg_flags[(i)] &= (uint8_t)~(uint8_t)(bit);               \
    } while (0)

/* Allocate `n` zeroed bytes for a fn type's out-of-line per-arg array from the
 * process-lifetime global type arena (defined in types.c).  Returns NULL for
 * n == 0.  Used by type_fn and by the few sites that rebuild a fn type at a
 * different arity; the arena is never freed (reachable, process-lifetime), so
 * the arrays outlive every by-value Type copy that shares them. */
uint8_t *tur_fn_args_alloc(uint32_t n);

/* The same process-lifetime arena, for building Types where no caller arena is
 * in scope (see constrained-hkt-abstract-var-requires-last-param-free). */
Arena *tur_type_arena(void);

/* CONV-S1 (defstruct-as-defadt): the runtime `any`-box tag for a type.  A
 * struct-origin lowered ADT boxes / casts / is?-tests as TY_STRUCT, so the
 * runtime reflection surface (type-of/cast/is?) stays transparent to the
 * lowering -- a value that was a `defstruct` still reports "struct".  The box
 * site and the cast/is? target MUST agree, so both route through this. */
static inline TypeKind any_box_tag_for_type(const Type *t) {
    if (t && t->kind == TY_ADT && t->as.adt_.def &&
        t->as.adt_.def->from_struct_lowering)
        return TY_STRUCT;
    return t ? t->kind : TY_UNKNOWN;
}

/* DV0: Dynamic var type constructor (-Xdynamic-vars).
 * Wraps the declared value type.  Only stored in DynVarEntry during elaboration;
 * user-visible expressions have the value type directly. */
Type type_dynvar(Arena *a, Type value_type);

/* Helper to check if a type has lifetime annotations */
static inline bool type_has_lifetime(Type t) {
    return t.n_lifetimes > 0;
}

/* Helper to get the first lifetime from a type */
static inline LifetimeId type_first_lifetime(Type t) {
    return t.n_lifetimes > 0 ? t.lifetimes[0] : LIFETIME_NONE;
}

/* Phase T19-B: Thread-safety marker helpers.
 *
 * type_is_send(t): true iff a value of type `t` can be transferred to another
 *   thread (ownership move across thread boundary is safe).
 * type_is_sync(t): true iff a value of type `t` can be shared across threads
 *   (immutable or internally synchronized).
 *
 * Rules (conservative defaults derived from TypeKind):
 *   - Primitives, TY_NIL, TY_BOOL, TY_INT, TY_FLOAT, TY_CSTR, TY_PTR_VOID,
 *     TY_FN, TY_TYPECLASS*, TY_EXCEPTION, TY_STRUCT (default): Send + Sync.
 *   - TY_REF, TY_RC, TY_WEAK:   neither Send nor Sync (single-threaded RC).
 *   - TY_CONT:                   neither Send nor Sync (captures C stack).
 *   - TY_REF_IMMUT, TY_REF_MUT: neither Send nor Sync (borrows; not owned).
 *
 * Struct field propagation and Arc<T>/Mutex<T>/Chan<T> Send+Sync derivation
 * are handled at call sites (T19-C) when those types are introduced.
 */
static inline bool type_is_send(Type t) {
    switch (t.kind) {
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_CONT:
        case TY_CLONEABLE_CONT:
        case TY_REF_IMMUT:
        case TY_REF_MUT:
            return false;
        case TY_NEVER:
            /* Never type has no values, so vacuously Send */
            return true;
        default:
            return true;
    }
}

static inline bool type_is_sync(Type t) {
    /* Sync implies Send; use the same conservative rule set in v1. */
    return type_is_send(t);
}

/* LT0 / UT0: Predicate helpers for ownership/linearity classification */
static inline bool ty_is_linear(Type t) {
    return t.copy_kind == CK_LINEAR;
}
/* UT0: ty_is_unique -- true iff the type is uniquely owned (at-most-one alias) */
static inline bool ty_is_unique(Type t) {
    return t.copy_kind == CK_UNIQUE;
}
static inline bool ty_is_move(Type t) {
    /* CK_UNIQUE == CK_MOVE (alias); kept for backward compat */
    return t.copy_kind == CK_UNIQUE;
}
static inline bool ty_is_copy(Type t) {
    return t.copy_kind == CK_COPY;
}
/* ST0: Substructural discipline predicates */
static inline bool ty_is_affine(Type t) {
    return t.substruct == SK_AFFINE;
}
static inline bool ty_is_relevant(Type t) {
    return t.substruct == SK_RELEVANT;
}
static inline bool ty_is_sublinear(Type t) {
    return t.substruct == SK_LINEAR;
}

/* Phase G1: type_requires_refinement -- true iff a value of this type may need
 * GADT-arm type refinement before its fields can be safely accessed.
 * A TY_ADT with is_gadt=true is rank-2 or higher in the GADT sense: matching
 * on its constructor introduces skolem equalities that refine the type
 * parameters.  Supports future tooling and the HRT rank-checking path. */
static inline bool type_requires_refinement(Type t) {
    if (t.kind != TY_ADT) return false;
    return t.as.adt_.def && t.as.adt_.def->is_gadt;
}

/* Convert TypeKind to string representation for debugging */
const char *typekind_to_string(TypeKind k);

/* ----- Phase SZ6/SZ7: type-level size index terms ----- */

/* Parse a type-position Form into a SizeTerm, or return NULL if `f` is not a
 * size expression.  Recognises `(Static n)`, `(Add a b)`, `(Mul a b)`, and a
 * bare symbol (treated as a size variable).  `is_size_var(name, ctx)` decides
 * whether a bare symbol is a size variable; pass NULL to treat every bare
 * non-numeric symbol as a variable.  Arena-allocated. */
struct Form;
SizeTerm *size_term_from_form(Arena *a, const struct Form *f,
                              bool (*is_size_var)(const char *, void *),
                              void *ctx);

/* Substitute size variable `var` with `replacement` throughout `t`, returning a
 * fresh term (or `t` unchanged when `var` does not occur). */
SizeTerm *size_term_subst(Arena *a, const SizeTerm *t,
                          const char *var, const SizeTerm *replacement);

/* Fold a closed size term (no variables) to its integer value.
 * Returns true and writes *out on success; false if the term mentions any
 * size variable (i.e. is not statically known). */
bool size_term_eval(const SizeTerm *t, int64_t *out);

/* Structural/normalised equality: true iff the two terms are provably equal by
 * constant folding + syntactic normalisation (commutative Add/Mul, identity
 * elimination).  Used by SZ7 static size checking. */
bool size_term_equal(const SizeTerm *a, const SizeTerm *b);

/* Render a size term for diagnostics, e.g. "(+ 1 n)" or "5".  Writes into buf
 * (size cap) and returns buf. */
const char *size_term_to_string(const SizeTerm *t, char *buf, size_t cap);

/* Convert type name string to TypeKind */
TypeKind typekind_from_name(const char *name);

static inline Type type_simple(TypeKind kind, CopyKind copy_kind) {
    Type t = {0};
    t.kind = kind;
    t.copy_kind = copy_kind;
    t.hkt_kind = KIND_STAR;
    return t;
}

#define TYPE_UNKNOWN  (type_simple(TY_UNKNOWN, CK_MOVE))
#define TYPE_NIL      (type_simple(TY_NIL, CK_COPY))
#define TYPE_BOOL     (type_simple(TY_BOOL, CK_COPY))
#define TYPE_INT      (type_simple(TY_INT, CK_COPY))
#define TYPE_FLOAT    (type_simple(TY_FLOAT, CK_COPY))
/* Phase N: fixed-width numeric type macros */
#define TYPE_INT8     (type_simple(TY_INT8,   CK_COPY))
#define TYPE_INT16    (type_simple(TY_INT16,  CK_COPY))
#define TYPE_INT32    (type_simple(TY_INT32,  CK_COPY))
#define TYPE_INT64    (type_simple(TY_INT64,  CK_COPY))
#define TYPE_UINT8    (type_simple(TY_UINT8,  CK_COPY))
#define TYPE_UINT16   (type_simple(TY_UINT16, CK_COPY))
#define TYPE_UINT32   (type_simple(TY_UINT32, CK_COPY))
#define TYPE_UINT64   (type_simple(TY_UINT64, CK_COPY))
#define TYPE_FLOAT32  (type_simple(TY_FLOAT32, CK_COPY))
#define TYPE_FLOAT64  (type_simple(TY_FLOAT64, CK_COPY))
#define TYPE_CSTR     (type_simple(TY_CSTR, CK_COPY))
#define TYPE_PTR_VOID (type_simple(TY_PTR_VOID, CK_COPY))
#define TYPE_NEVER    (type_simple(TY_NEVER, CK_MOVE))

/* ptr-generic-parameterised-type: typed raw pointer ptr<T>.
 * Lives on TY_PTR_VOID with a non-NULL pointee; inner == NULL is ptr<void>. */
static inline Type type_ptr(struct Type *inner) {
    Type t = {0};
    t.kind = TY_PTR_VOID;
    t.copy_kind = CK_COPY;       /* raw pointer: bitwise copyable, like ptr<void> */
    t.hkt_kind = KIND_STAR;
    t.as.ptr.inner = inner;
    return t;
}
/* SYM0: interned runtime symbol type (-Xsymbols) */
#define TYPE_SYM      (type_simple(TY_SYM, CK_COPY))

/* Stage 1 (macro-system-direction-plan): compile-time syntax object type.
 * Interpreter/macro-time only; never a compiled runtime value. */
#define TYPE_SYNTAX   (type_simple(TY_SYNTAX, CK_COPY))

/* Phase 5: ref<T> type constructor */
static inline Type type_ref(TypeKind inner) {
    Type t = {0};
    t.kind = TY_REF;
    t.copy_kind = CK_UNIQUE;  /* ref<T> is uniquely owned (UT0) */
    t.as.ref.inner = inner;
    t.n_lifetimes = 0;
    return t;
}

/* LT3: lref<T> type constructor — linear owning pointer */
static inline Type type_lref(TypeKind inner) {
    Type t = {0};
    t.kind = TY_LREF;
    t.copy_kind = CK_LINEAR;    /* lref<T> is exactly-once */
    t.substruct  = SK_LINEAR;   /* ST0: lref<T> has the linear substructural discipline */
    t.as.ref.inner = inner;
    t.n_lifetimes = 0;
    return t;
}

/* SS0a: Session type constructors (-Xsessions).
 * These take arena-allocated Type* arguments; callers own allocation.
 * Protocol descriptors (Send, Recv, etc.) are move-only type-level constructs.
 * Only TY_SESSION gets CK_LINEAR + SK_LINEAR (it is the channel endpoint). */

static inline Type type_session(struct Type *proto) {
    Type t = {0};
    t.kind = TY_SESSION;
    t.copy_kind = CK_LINEAR;
    t.substruct = SK_LINEAR;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = proto;
    return t;
}

static inline Type type_send(struct Type *msg, struct Type *cont) {
    Type t = {0};
    t.kind = TY_SEND;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = msg;
    t.as.session_.snd = cont;
    return t;
}

static inline Type type_recv(struct Type *msg, struct Type *cont) {
    Type t = {0};
    t.kind = TY_RECV;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = msg;
    t.as.session_.snd = cont;
    return t;
}

static inline Type type_close(void) {
    Type t = {0};
    t.kind = TY_CLOSE;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    return t;
}

static inline Type type_choose(struct Type *left, struct Type *right) {
    Type t = {0};
    t.kind = TY_CHOOSE;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = left;
    t.as.session_.snd = right;
    return t;
}

static inline Type type_branch(struct Type *left, struct Type *right) {
    Type t = {0};
    t.kind = TY_BRANCH;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = left;
    t.as.session_.snd = right;
    return t;
}

/* TY_SESSION_REC: recursive protocol mu-type.
 * label is an interned string (e.g. "echo"); body is the unrolled protocol. */
static inline Type type_session_rec(const char *label, struct Type *body) {
    Type t = {0};
    t.kind = TY_SESSION_REC;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.label = label;
    t.as.session_.fst = body;
    return t;
}

/* SS3c: TY_TIMEOUT -- Timeout[Q, P] protocol node.
 * fst = Q: success continuation (message was received in time).
 * snd = P: timeout continuation (no message before deadline).
 * Timeout is self-dual: both endpoints experience the same timing outcome. */
static inline Type type_timeout(struct Type *success_proto, struct Type *timeout_proto) {
    Type t = {0};
    t.kind = TY_TIMEOUT;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = success_proto;
    t.as.session_.snd = timeout_proto;
    return t;
}

/* SS1: TY_SESSION_PAIR -- internal pair returned by make-session.
 * fst = first session endpoint type (Session[P])
 * snd = second session endpoint type (Session[dual(P)])
 * Never a runtime value; always immediately destructured by vector let. */
static inline Type type_session_pair(struct Type *first_sess, struct Type *second_sess) {
    Type t = {0};
    t.kind = TY_SESSION_PAIR;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = first_sess;
    t.as.session_.snd = second_sess;
    return t;
}

/* SS2: TY_SESSION_RECV_PAIR -- internal pair returned by recv.
 * fst = received value type (e.g. int); snd = continuation session type Session[Q].
 * Never a runtime value; always immediately destructured by vector let. */
static inline Type type_session_recv_pair(struct Type *val_type, struct Type *cont_type) {
    Type t = {0};
    t.kind = TY_SESSION_RECV_PAIR;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = val_type;
    t.as.session_.snd = cont_type;
    return t;
}

/* SS2: TY_SESSION_OFFER -- internal result of offer.
 * fst = Session[P] (Left branch type); snd = Session[Q] (Right branch type).
 * Always consumed by a match expression; never a runtime value on its own. */
static inline Type type_session_offer(struct Type *left_type, struct Type *right_type) {
    Type t = {0};
    t.kind = TY_SESSION_OFFER;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = left_type;
    t.as.session_.snd = right_type;
    return t;
}

/* Phase 9: rc<T> type constructor */
static inline Type type_rc(TypeKind inner) {
    Type t = {0};
    t.kind = TY_RC;
    t.copy_kind = CK_MOVE;  /* rc<T> is move-only by default */
    t.as.rc.inner = inner;
    t.as.rc.adt_def = NULL;
    t.n_lifetimes = 0;
    return t;
}

/* CONV-S1 (slice 2): rc<ADT> with the ADT def carried alongside, so field
 * access through the rc resolves the variant layout. */
static inline Type type_rc_adt(struct AdtDef *def) {
    Type t = {0};
    t.kind = TY_RC;
    t.copy_kind = CK_MOVE;
    t.as.rc.inner = TY_ADT;
    t.as.rc.adt_def = def;
    t.n_lifetimes = 0;
    return t;
}

/* Phase 9: weak<T> type constructor */
static inline Type type_weak(TypeKind inner) {
    Type t = {0};
    t.kind = TY_WEAK;
    t.copy_kind = CK_MOVE;  /* weak<T> is move-only */
    t.as.rc.inner = inner;  /* Reuse the same field as rc */
    t.as.rc.adt_def = NULL;
    t.n_lifetimes = 0;
    return t;
}

/* weak<ADT> with the ADT def carried alongside, the weak mirror of
 * type_rc_adt.  A `weak<Name>` field over a user aggregate needs this for the
 * same reason the rc form does: without the inner def the field's type is
 * `weak<?>` while `(weak r)` over an `rc<Name>` produces `weak<ADT>`, so the
 * back-edge of a parent/child graph -- the one shape weak<T> exists for --
 * failed to type-check at the `set!`. */
static inline Type type_weak_adt(struct AdtDef *def) {
    Type t = {0};
    t.kind = TY_WEAK;
    t.copy_kind = CK_MOVE;
    t.as.rc.inner = TY_ADT;
    t.as.rc.adt_def = def;
    t.n_lifetimes = 0;
    return t;
}

/* Construct a function type from TypeKinds.  arity is unbounded (uint32_t); the
 * per-arg kind/flag arrays are allocated out of line from the global type arena
 * (tur_fn_args_alloc), so a function type can describe any number of parameters
 * -- matching the emitted C, which has no parameter-count limit of its own. */
static inline Type type_fn(const TypeKind arg_kinds[], uint32_t arity, TypeKind result_kind) {
    Type t = {0};
    t.kind = TY_FN;
    t.copy_kind = typekind_default_copy_kind(TY_FN);
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    t.as.fn.arity = arity;
    t.as.fn.arg_kinds = tur_fn_args_alloc(arity);  /* NULL when arity == 0 */
    t.as.fn.arg_flags = tur_fn_args_alloc(arity);  /* zeroed: all flags clear */
    for (uint32_t i = 0; i < arity; i++) {
        t.as.fn.arg_kinds[i] = (uint8_t)arg_kinds[i];
    }
    t.as.fn.result_kind = result_kind;
    t.as.fn.effect_row = NULL;
    t.as.fn.arg_full_types = NULL;
    t.as.fn.result_full_type = NULL;
    t.as.fn.result_fat = false;  /* A#1: must initialise or UBSan fires on bool read */
    t.as.fn.boxed = false;  /* CRU B-1: must initialise or UBSan fires on bool read */
    t.as.fn.cfnptr = false; /* typed-c-abi-function-pointers: default to a Turmeric closure type */
    t.as.fn.is_variadic = false;  /* AR6: default non-variadic */
    t.as.fn.result_borrow_arg = -1; /* LS4: no lifetime-tied borrow return by default */
    t.as.fn.rest_kind   = TY_INT; /* AR6: default rest type */
    t.as.fn.rest_full_type = NULL; /* typed-variadic: NULL = primitive rest */
    t.as.fn.rest_borrow = false; /* closure-drop-glue: ^borrow rest, default off */
    t.as.fn.param_type_forms = NULL; /* sized-types-cross-param-unification: filled by defn elab */
    t.as.fn.result_type_form = NULL; /* SZ8 non-GADT: filled by defn elab when a return ann was recorded */
    return t;
}

/* Phase 12: Borrow type constructors */
/* &T - immutable borrow */
static inline Type type_ref_immut(TypeKind target) {
    Type t = {0};
    t.kind = TY_REF_IMMUT;
    t.copy_kind = CK_COPY;  /* Borrows are copyable (they're just pointers) */
    t.as.ref_borrow.target = target;
    t.n_lifetimes = 0;
    return t;
}

/* &mut T - mutable borrow */
static inline Type type_ref_mut(TypeKind target) {
    Type t = {0};
    t.kind = TY_REF_MUT;
    t.copy_kind = CK_MOVE;  /* Mutable borrows are move-only (exclusive access) */
    t.as.ref_borrow.target = target;
    t.n_lifetimes = 0;
    return t;
}

/* borrowed-aggregate-key-skips-the-key-check: the borrow targets whose KIND
 * does not name the type, so the borrow records it in ref_borrow.target_full.
 * A scalar is named by its kind; a tyvar by ref_borrow.target_tyvar. */
static inline bool borrow_target_needs_full(TypeKind tk) {
    return tk == TY_STRUCT || tk == TY_ADT || tk == TY_APP;
}

/* Phase 13: Lifetime-annotated borrow type constructors */
/* &'a T - immutable borrow with lifetime */
static inline Type type_ref_immut_lifetime(TypeKind target, LifetimeId lifetime) {
    Type t = type_ref_immut(target);
    if (lifetime != LIFETIME_NONE) {
        t.lifetimes[0] = lifetime;
        t.n_lifetimes = 1;
    }
    return t;
}

/* &'a mut T - mutable borrow with lifetime */
static inline Type type_ref_mut_lifetime(TypeKind target, LifetimeId lifetime) {
    Type t = type_ref_mut(target);
    if (lifetime != LIFETIME_NONE) {
        t.lifetimes[0] = lifetime;
        t.n_lifetimes = 1;
    }
    return t;
}

/* Phase 15: Typeclass type constructors */
/* Create a typeclass type (e.g., Eq, Ord) */
static inline Type type_typeclass(TypeClass *tc) {
    Type t = {0};
    t.kind = TY_TYPECLASS;
    t.copy_kind = CK_COPY;  /* Typeclasses are copyable */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.typeclass.typeclass = tc;
    return t;
}

/* Create a typeclass instance type */
static inline Type type_typeclass_inst(TypeClassInstance *inst) {
    Type t = {0};
    t.kind = TY_TYPECLASS_INST;
    t.copy_kind = CK_COPY;  /* Typeclass instances are copyable */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.typeclass_inst.instance = inst;
    return t;
}

/* Phase HKT-P1: Type application constructor */
/* Create a TY_APP type representing (F A) where F is a type constructor and A is a type argument.
 * Both fn and arg are copied into newly-allocated memory on the arena.
 * The result kind is computed from fn's kind using kind_of_type_app. */
Type type_app(Arena *a, Type fn, Type arg, Span span);
/* constrained-hkt-abstract-var-requires-last-param-free: build the hole-headed
 * partial application `ctor` with `fixed` in every slot but `hole_pos`, which
 * stays free (e.g. type_app_hole(a, Result, cstr, 0) == `(Result _ cstr)`). */
Type type_app_hole(Arena *a, Type fn, Type arg, uint8_t hole_pos, Span span);
/* Does this type carry a partial-application hole?  False for every ordinary
 * application, and for every non-TY_APP type. */
static inline bool type_app_has_hole(const Type *t) {
    return t && t->kind == TY_APP && t->as.app.hole_pos_p1 != 0;
}
/* The hole's slot index.  Only meaningful when type_app_has_hole(). */
static inline uint8_t type_app_hole_pos(const Type *t) {
    return (uint8_t)(t->as.app.hole_pos_p1 - 1);
}
/* Apply a hole-headed partial application to `elem`, yielding the saturated
 * N-ary application (`(Result _ cstr)` + `int` -> `(Result int cstr)`).  When
 * `head` carries no hole this is the ordinary type_app.  Returns the saturated
 * type; arena-allocates the spine. */
Type type_app_fill_hole(Arena *a, Type head, Type elem, Span span);

/* Lift the substructural discipline (:linear / :affine) from a TY_APP's head
 * StructDef onto the application node.  Call after constructing a TY_APP whose
 * `fn` carries an opaque/struct head -- without this, the application inherits
 * the default CK_COPY and the linear-discipline checker silently treats a
 * value of type `(WriteCap T)` as plain copyable.  See
 * docs/archive/history/parametric-linear-opaque-not-enforced.md. */
void propagate_app_discipline(Type *app, const Type *fn);

/* IT0: Union type constructor.
 * Create a TY_UNION type with the given array of member types.
 * members[] is copied from the provided arena-allocated pointers.
 * Nested TY_UNION members are flattened automatically. */
Type type_union_build(Arena *a, Type **members, uint8_t n_members);

/* IT2: Intersection type constructor.
 * Create a TY_INTERSECTION type with the given array of member types.
 * members[] is copied from the provided arena-allocated pointers.
 * Nested TY_INTERSECTION members are flattened automatically. */
Type type_intersection_build(Arena *a, Type **members, uint8_t n_members);

/* Variadic HKT rows (Layer 2): row-of-types constructor.
 * Create a TY_TYPEROW from `n_elements` element type pointers; the element
 * array is copied onto the arena (the caller's `elements` array need not
 * outlive the call, though the pointed-to Types must).  hkt_kind is set to
 * KIND_TYPEROW.  Unlike unions, element order is significant and duplicates
 * are preserved -- a row is a list, not a set.  n_elements == 0 is the unit
 * row.  Rows do NOT nest-flatten (a row containing a row stays nested). */
Type type_typerow(Arena *a, Type **elements, uint8_t n_elements);
/* P0 typed-field row: parallel field_names array. NULL field_names matches
 * bare-positional type_typerow(). */
Type type_typerow_named(Arena *a, Type **elements, const char **field_names,
                       uint8_t n_elements);

/* Variadic HKT rows: order-insensitive (permutation) row equality.
 * Returns true iff `a` and `b` are both TY_TYPEROW with the same multiset of
 * element types (same length, and each element of `a` pairs with a distinct,
 * type_eq-equal element of `b`).  This backs the data-frame "column-permutation
 * is a no-op" case; ordinary `type_eq` on rows stays order-SENSITIVE. */
bool type_typerow_eq_perm(Type a, Type b);

/* Variadic HKT rows (Layer 5): row algebra -- the type-level operations the
 * ECS query / relational layers build on. All are pure and compile-time.
 *
 * row-ops-drop-field-names: every operation below FORWARDS a labeled row's
 * field_names. Dropping them would return a positional row, which compares
 * equal to any same-shaped positional row, so `#row{id : int}` would start
 * unifying with `#row{name : int}` after a round trip through the algebra.
 * Labeled rows dedup and intersect on the (name, type) PAIR, matching how they
 * unify elsewhere. Mixing a labeled operand with a bare one is rejected by the
 * caller (elab_types.c) rather than resolved here; an EMPTY row is
 * label-neutral, so `(row-union R #row{})` stays the identity either way. */
/* True if `r` is a non-empty row carrying field names. */
bool type_typerow_is_labeled(Type r);
/* First field name appearing twice in a labeled row, or NULL if all distinct.
 * concat/union can produce a duplicate that no literal could (TUR-E0291), so
 * the caller scans the folded result. */
const char *type_typerow_dup_field_name(Type r);
/* Membership: true if `row` contains an element type_eq to `elem`. */
bool type_typerow_contains(Type row, Type elem);
/* Concatenation (`++`): x ++ y, order-preserving, duplicates kept (clamped 255). */
Type type_typerow_concat(Arena *a, Type x, Type y);
/* Set-union (query join): x then y-not-in-x, order-preserving, deduplicated. */
Type type_typerow_union(Arena *a, Type x, Type y);
/* Intersection: x's elements also in y, x's order, deduplicated. */
Type type_typerow_intersect(Arena *a, Type x, Type y);
/* L6 follow-up D: canonical (sorted) copy of a row -- the opt-in surface for
 * permutation-aware equality. (row-canon #row{a b}) and (row-canon #row{b a})
 * reduce to the same TY_TYPEROW, so ordinary type_eq agrees. Compile-time only.
 * Sort key is (field_name, type_name) for a labeled row, type_name alone for a
 * positional one -- see the note in types.c on why the field name has to lead. */
Type type_typerow_canonical(Arena *a, Type x);

/* Phase 17: Exception type constructor */
/* Create an exception type wrapping a payload of the given type */
static inline Type type_exception(TypeKind payload_type) {
    Type t = {0};
    t.kind = TY_EXCEPTION;
    t.copy_kind = CK_MOVE;  /* Exceptions are move-only (one-shot) */
    t.n_lifetimes = 0;
    t.as.exn.payload_type = payload_type;
    return t;
}

/* Phase 18: Continuation type constructor */
/* Create a continuation type cont<T> that returns T */
/* CC4 continuation flavor: which runtime (k v) resumes against.
 * CONT_EFFECT (cps-backend-n6 cross-function resume): the continuation is an
 * algebraic-effect handler continuation carried as a plain int64; (k v) lowers
 * to EX_RESUME (dk_invoke), NOT a cloneable/escape/serial resume builtin.  This
 * is the flavor the shift/reset -> __Shift effect desugar attaches to a
 * cross-function resuming shift's receiver `k`, so `(k v)` inside the receiver
 * resumes the delimited continuation captured by the enclosing reset's handler. */
typedef enum {
    CONT_CLONEABLE = 0,
    CONT_ESCAPE    = 1,
    CONT_SERIAL    = 2,
    CONT_EFFECT    = 3
} ContFlavor;

static inline Type type_cont(TypeKind returns) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_CONT;
    t.copy_kind = CK_MOVE;  /* Continuations are move-only (one-shot) */
    t.n_lifetimes = 0;
    t.as.cont.returns = returns;
    t.as.cont.arg = TY_UNKNOWN;   /* resume-value type unknown/unchecked by default */
    t.as.cont.flavor = CONT_CLONEABLE;
    t.hkt_kind = KIND_STAR;
    return t;
}

/* CC4: a continuation typed with an explicit flavor (escape/cloneable/serial). */
static inline Type type_cont_flavored(TypeKind returns, ContFlavor flavor) {
    Type t = type_cont(returns);
    t.as.cont.flavor = flavor;
    return t;
}

/* slice 4 (resuming-shift plan): a fully-typed continuation Cont<BodyT,ResetT> --
 * `arg` is the resume-value type (k) expects, `returns` is what (k v) yields.
 * arg == TY_UNKNOWN leaves the resume value unchecked (backward compatible). */
static inline Type type_cont_arg_flavored(TypeKind arg, TypeKind returns,
                                          ContFlavor flavor) {
    Type t = type_cont_flavored(returns, flavor);
    t.as.cont.arg = arg;
    return t;
}

/* Map a surface type name to a continuation flavor, or -1 if not a cont name. */
static inline int cont_flavor_from_name(const char *n) {
    if (strcmp(n, "cont") == 0)        return CONT_CLONEABLE;
    if (strcmp(n, "escape-cont") == 0) return CONT_ESCAPE;
    if (strcmp(n, "serial-cont") == 0) return CONT_SERIAL;
    if (strcmp(n, "effect-cont") == 0) return CONT_EFFECT;
    /* multishot-effect-cont: a CONT_EFFECT continuation whose (k v) resumes
     * multi-shot (snapshot before each resume).  The multi-shot bit is carried in
     * the param binding's copy_kind (CK_MULTISHOT), set at the annotation sites
     * via cont_name_is_multishot; the flavor itself is still CONT_EFFECT. */
    if (strcmp(n, "multishot-effect-cont") == 0) return CONT_EFFECT;
    return -1;
}

/* True when a cont annotation name denotes a multi-shot continuation, so the
 * param binding must be CK_MULTISHOT (resume snapshots before each use). */
static inline bool cont_name_is_multishot(const char *n) {
    return strcmp(n, "multishot-effect-cont") == 0;
}

/* Phase B2: Cloneable continuation type constructor */
/* Create a cloneable continuation type cloneable_cont<T> that returns T */
static inline Type type_cloneable_cont(TypeKind returns) {
    Type t = {0};
    t.kind = TY_CLONEABLE_CONT;
    t.copy_kind = CK_COPY;  /* Cloneable continuations can be copied (multi-shot) */
    t.n_lifetimes = 0;
    t.as.cont.returns = returns;
    return t;
}

/* Phase G0: ADT type constructor */
/* Create a TY_ADT type referencing the given AdtDef.
 * copy_kind is taken from def->is_copy. */
static inline Type type_adt(AdtDef *def) {
    Type t = {0};
    t.kind = TY_ADT;
    /* structdef-retirement slice 4: a lowered `:linear`/`:affine` defstruct
     * carries its substructural copy-kind on the ADT type exactly as type_struct
     * does, so the exactly-once / at-most-once enforcement (which keys on
     * Type.copy_kind and the bindings derived from it) propagates unchanged. */
    t.copy_kind = def->is_linear ? CK_LINEAR
                : def->is_affine ? CK_UNIQUE
                : (def->is_copy ? CK_COPY : CK_MOVE);
    if (def->is_affine) t.substruct = SK_AFFINE;
    t.hkt_kind = KIND_STAR;
    t.as.adt_.def = def;
    return t;
}

/* CONV-S4N: true when `st` is (the head of) an ADT that a `match` arm has
 * narrowed to a single *record* variant -- the case `with` can reconstruct.
 * `st` must already be the unwrapped TY_ADT head (callers strip the TY_APP
 * spine first).  Bounds-checks the recorded ctor index against the def. */
static inline bool adt_is_narrowed_to_record_variant(Type st) {
    if (st.kind != TY_ADT || !st.as.adt_.def) return false;
    if (!st.as.adt_.is_narrowed) return false;
    const AdtDef *def = st.as.adt_.def;
    if (st.as.adt_.narrowed_ctor_idx >= def->n_ctors) return false;
    const CtorDef *c = def->ctors[st.as.adt_.narrowed_ctor_idx];
    return c && c->is_record;
}

/* Phase HRT/G2: Create a named type variable type for parameters like :a */
static inline Type type_tyvar_named(const char *name) {
    Type t = { .kind = TY_TYVAR, .copy_kind = CK_COPY };
    t.as.tyvar_.name = name;
    return t;
}

/* CT0: Contract type constructor — { x : T | p } */
static inline Type type_contract(Arena *a, struct Type *base, const char *var, const struct Form *pred) {
    (void)a; /* base is already arena-allocated at call sites */
    Type t = {0};
    t.kind = TY_CONTRACT;
    t.copy_kind = base ? base->copy_kind : CK_COPY;
    t.hkt_kind = KIND_STAR;
    t.as.contract_.base_type = base;
    t.as.contract_.var_name = var;
    t.as.contract_.predicate = pred;
    return t;
}

/* ET3: Handler type constructor */
static inline Type type_make_handler(const char *effect_name, TypeKind value_kind, TypeKind result_kind) {
    Type t = {0};
    t.kind = TY_HANDLER;
    t.copy_kind = CK_COPY;
    t.hkt_kind = KIND_STAR;
    t.as.handler_.effect_name = effect_name;
    t.as.handler_.value_kind = value_kind;
    t.as.handler_.result_kind = result_kind;
    return t;
}

/* SS5: Global protocol type constructor (compile-time only, no runtime representation). */
static inline Type type_global(const char *name, const char **roles, int n_roles,
                               GlobalInteraction *body) {
    Type t = {0};
    t.kind = TY_GLOBAL;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.global_.name = name;
    t.as.global_.roles = roles;
    t.as.global_.n_roles = n_roles;
    t.as.global_.body = body;
    return t;
}

/* SS5: Role endpoint type constructor -- linear channel endpoint in a global protocol. */
static inline Type type_role(struct Type *global_type, const char *role_name,
                             GlobalInteraction *current_step) {
    Type t = {0};
    t.kind = TY_ROLE;
    t.copy_kind = CK_LINEAR;
    t.substruct = SK_LINEAR;
    t.hkt_kind = KIND_STAR;
    t.as.role_.global_type = global_type;
    t.as.role_.role_name = role_name;
    t.as.role_.current_step = current_step;
    return t;
}

int          type_eq(Type a, Type b);
/* ET3-D: Subtype check. Returns true if sub is a subtype of super_. */
bool         type_is_subtype(Type sub, Type super_);
/* LT2: Check arg_linear compatibility between two function types.
 * Returns 1 if compatible (all arg_linear flags match), 0 on mismatch.
 * Non-function types always return 1. */
int          fn_type_subtype(Type actual, Type expected);
/* stdlib-int-stand-in-audit S1: arity/arg-kind/result-kind compatibility.
 * Permissive at tyvar/unknown/any slots; rejects only concrete disagreement. */
int          fn_type_structurally_compatible(Type actual, Type expected);
const char  *type_name(Type t);                   /* "int", "bool", … */
/* any-fn-tag-does-not-discriminate-signatures: the identity spelling of a
 * function type -- "(fn [int cstr] : bool)" -- rendered from its parts rather
 * than from a Type, so a caller holding only a parameter-kind vector (the
 * interpreter, reconstructing a closure's signature from its FnDef) produces
 * exactly the string `type_name` produces for the corresponding TY_FN.  That
 * string is what `emit_any_type_id` hashes into a fn payload's `any` box id, so
 * the two back ends compare the same key.  Interned; do not free.  Pass
 * arg_kinds NULL only with arity 0. */
const char  *tur_fn_type_key(const uint8_t *arg_kinds, uint32_t arity,
                             TypeKind result_kind, bool cfnptr, bool is_variadic);
const char  *type_c_name(Type t);                 /* "int64_t", "bool", … */
/* ADT-app analogues of the (removed, structdef-retirement DS-D) struct-app
 * helpers: extract an ADT-headed
 * TY_APP chain into (AdtDef*, args[], n), and substitute those args for the
 * TY_TYVAR names in a field's declared full_type.  substitute_adt_app_type
 * allocates a fresh malloc'd spine for a TY_APP result (same ownership as the
 * struct path -- free with free_struct_app_type). */
bool         type_extract_adt_app(const Type *t, struct AdtDef **out_def,
                                  Type *out_args, uint8_t *out_n);
/* The stdlib collections (Vec, Map, Set, MutableMap) whose `:heap` handle is
 * inline-C `malloc`, never region memory.  Defined in emit_expr.c; the
 * elaborator's implicit-erasure note (elab_call.c) asks it too. */
bool         region_def_is_malloc_collection(const struct AdtDef *def);
Type         substitute_adt_app_type(const Type *t,
                                     const struct AdtDef *def,
                                     const Type *args);
/* Owned variant -- deep-clones the substituted arg so the result aliases
 * nothing in args[] and is safe to free with free_struct_app_type.  Use this
 * at any call site that releases the result. */
Type         substitute_adt_app_type_owned(const Type *t,
                                           const struct AdtDef *def,
                                           const Type *args);
/* Release the malloc'd TY_APP spine nodes that substitute_adt_app_type_owned /
 * clone_struct_app_type allocate for a compound (TY_APP) result.  A no-op on a
 * leaf Type, so it is safe to call unconditionally on any substitute result. */
void         free_struct_app_type(Type t);
/* Deep-clone a Type as an OWNED spine (TY_APP nodes malloc'd, leaves by value)
 * -- release with free_struct_app_type.  A leaf is returned unchanged. */
Type         clone_struct_app_type(Type t);
/* TS4P1: ADT-app (polymorphic ADT monomorphisation) registry. */
void         type_codegen_reset_adt_apps(void);
void         type_codegen_emit_adt_apps(Buf *out);
const char  *type_register_adt_app(Type t);
char        *type_adt_app_ctor_suffix(Type t);
/* CONV-S1: stable interned C typedef name (`tur_adt_<mangled>`) for the by-value
 * representation of a non-parametric flat-product ADT.  See types.c. */
const char  *adt_byval_c_name(const AdtDef *def);
const char  *adt_heap_ptr_c_name(const AdtDef *def);
/* Parametric-by-value monomorphisation (heavy prerequisite for CONV-S1
 * graduation; see docs/archive/parametric-adt-byvalue-plan.md).  True when t is
 * a concrete monomorphisation of a single-variant non-GADT parametric flat
 * product whose every monomorphised field is by-value-able.  LIVE (P2-P4) --
 * both crossings (match/field-access and ctor-field box/unbox) are wired. */
bool         adt_app_is_byvalue_product(Type t);
/* CONV-S1 seam 4 / structdef-retirement slice 1: true when a Result/Option
 * monomorph field of resolved type `resolved` is stored as a heap pointer `T *`
 * (box-as-pointer) -- a non-parametric value-struct or by-value record-ADT field.
 * Takes precedence over the B4 wide-element int64 box. */
bool         adt_field_is_ros_pointer_box(const struct AdtDef *owner,
                                          const Type *resolved);
/* SR3 slice A (null-None): true when `ctor` is the stdlib Option's nullary
 * tag-0 `None` -- the one constructor whose CARRIER form is the null pointer
 * (the historical none-as-NULL every reader already accepts), so its carrier
 * ctors return 0 instead of mallocing a box whose only content is tag 0. */
bool         adt_ctor_is_null_none(const struct AdtDef *def,
                                   const struct CtorDef *ctor);
/* SR3 slice B (option niche, default ON since 2026-09-03; TUR_OPTION_NICHE=0
 * restores the tagged form): true when
 * `t` is an `(Option P)` monomorph carried AS its payload pointer -- 8 bytes,
 * `(none)` == NULL, no tag word.  Eligibility is an explicit allowlist of
 * payload types whose valid values exclude 0; see the definition for why
 * `:heap`-ness is not the condition and `Cons` fails it. */
bool         adt_app_is_niche_option(Type t);
/* RM3: true when `--enable=regions` is on.  Warns once per compile (TUR-W0060)
 * on first consult; see docs/archive/regions-plan.md. */
bool         regions_enabled(void);
const char  *region_free_fn(void);
/* opaque-pointer-c-spelling (GRADUATED 2026-08-28): true when `def` is an
 * opaque newtype declared over a pointer (`(defopaque String :ptr<void>)`), so
 * type_c_name spells it `void *` instead of the `int64_t` carrier word --
 * making an opaque handle distinguishable from a tagged box at the emitter's
 * `strcmp(cname, "int64_t")` sites. */
bool         adt_opaque_c_names_as_pointer(const struct AdtDef *def);
/* B4 (slice 2): true when `t` is a wide (>8 byte) by-value ADT -- one that must
 * ride a heap box when stored as a parametric carrier monomorph element. */
bool         type_is_wide_byval_adt(Type t);
bool         type_is_boxed_container_elem(Type t);

/* Increment 4 stage 2 (repr-decision-function-plan): the POSITION axis.
 * `repr_of(type, position)` states the INTENDED representation protocol --
 * what form a value of this type takes in this position under the
 * consolidated rules increments 1-3 established.  In stage 2 it is
 * consulted only by SHADOW checks (under --emit-abi-trace) that log
 * disagreements with what a site actually decided; behavior is unchanged.
 * A disagreement is either a residual seam or a hole in this spec -- both
 * are findings.  Stage 3 migrates sites to consult it for real. */
typedef enum ReprPosition {
    REPR_POS_PARAM,           /* a defn/fn parameter slot */
    REPR_POS_RESULT,          /* a fn result / control-form merge value */
    REPR_POS_LET_BIND,        /* a let/letrec binding */
    REPR_POS_CONTAINER_ELEM,  /* a Vec/Map/Set element slot */
    REPR_POS_STRUCT_FIELD,    /* a struct/ADT field slot */
    REPR_POS_CARRIER_SINK,    /* a generic (tyvar/inline-C) int64 sink */
} ReprPosition;

typedef enum ReprForm {
    REPR_SCALAR_BITS,   /* value IS the bits (int/float/bool/cstr/ptr leaf) */
    REPR_HEAP_PTR,      /* pointer to a heap object; carrier round-trip lossless */
    REPR_BYVAL_AGG,     /* a real C aggregate, by value */
    REPR_BOXED_AGG,     /* heap-boxed aggregate; slot holds the box pointer */
    REPR_CARRIER_I64,   /* the erased int64 carrier */
    REPR_FAT_HANDLE,    /* fat closure box {thunk, env...} as a void-ptr/int64 handle */
    REPR_THIN_FN,       /* bare code pointer, no environment */
} ReprForm;

ReprForm     repr_of(const Type *t, ReprPosition pos);
/* container-element-form-plan CE1: the ONE answer every Vec element site
 * consults.  CE_WORD: the value's one-word form is stored in the slot
 * directly -- scalars, cstr, :heap pointers, pointer opaques, and a niche
 * `(Option P)` (the default since 2026-09-03), whose word IS its payload
 * pointer.  CE_BOX: the slot holds a heap box pointer -- by-value aggregates
 * (16-byte Option monomorphs, wide products), exactly as before.  Everything
 * but the niche row restates prior behavior: under TUR_OPTION_NICHE=0
 * adt_app_is_niche_option is false everywhere and this is repr_of's
 * container answer verbatim, so the default path cannot move. */
typedef enum { CE_WORD = 0, CE_BOX = 1 } ContainerElemForm;
ContainerElemForm container_elem_form(Type elem);
/* Increment 4 stage 3: the binding-context sibling the param-position
 * boundary note pre-registered.  A fn-typed value's representation is decided
 * by per-BINDING flags (`is_poly_fn`, `is_fat`) that elaboration sets and that
 * the Type does not carry, so `repr_of(type, pos)` alone mislabels every fn
 * param.  This overload consults those flags first and delegates to `repr_of`
 * otherwise.  Declared here (with a forward-declared Binding) rather than in a
 * second decision function, so the campaign keeps ONE home for the question. */
struct Binding;
ReprForm     repr_of_binding(const struct Binding *b, ReprPosition pos);
const char  *repr_form_name(ReprForm f);
const char  *repr_position_name(ReprPosition pos);

/* Increment 4 stage 3 -> R3: the shadow instrument has two modes.
 *
 *   MEASUREMENT (`--emit-abi-trace`): every disagreement is one stderr line
 *   and nothing aborts, so a sweep collects the whole list.  This is how each
 *   position was calibrated.
 *
 *   ENFORCEMENT (Debug builds, no flag): a disagreement is an ICE.  Licensed
 *   only because stage 3's whole position list runs silent -- the routing
 *   plan's rule is that the ICE comes AFTER the chokepoints exist, never
 *   before.  `TUR_REPR_NO_SHADOW_ICE` downgrades it to a warning, mirroring
 *   `TUR_ABI_NO_ROUTE_ICE` on the sibling R3 assert.
 *
 * `known` marks a pinned, documented disagreement (today: the container-elem
 * TY_APP row).  A known row logs under trace and is silent otherwise -- it is
 * a work list, not a defect, and must never abort a build. */
/* The fn-PARAM routing answer.  Defined in elab_fns.c, where its two gate
 * predicates live; declared here so the repr_* family reads from one header.
 * See docs/archive/repr-coverage-census.md for why this signature exists --
 * `repr_of(type, pos)` cannot answer it, because the routing depends on the
 * binding's substructural flags as well as the annotation's shape. */
ReprForm     repr_of_fn_param(const struct Binding *b, const Type *ann);

bool         repr_shadow_active(void);
void         repr_shadow_disagree(const char *site, bool known,
                                  const char *line);
/* Parametric-by-value: app-aware siblings of adt_byval_pass_by_ptr /
 * adt_is_byvalue_product -- a concrete flat-product ADT-app (`(Pair2 int
 * float)`) is laid out as its by-value monomorph aggregate, so it shares the
 * >16-byte pass-by-pointer size gate and the by-value-product representation
 * decision (the latter covering both the non-parametric ADT and the app). */
/* Size of a by-value parametric monomorph: tag word (sums only) + widest
 * substituted variant.  0 when `t` is not a by-value monomorph.  The single
 * source for every width threshold on such a value -- pbp (> 16) and the b4box
 * closure slot (> 8). */
size_t       adt_app_byval_value_size_bytes(Type t);
/* b4box closure-slot width -- see the definition's comment for why this is
 * separate from type_is_wide_byval_adt (which also drives ADT field layout). */
bool         type_is_b4box_closure_slot(Type t);
bool         type_is_word_closure_slot(Type t);
bool         adt_app_byval_pass_by_ptr(Type t);
bool         type_is_byvalue_adt_product(Type t);
/* Phase E: Typed function-pointer typedef registry for unboxed fn struct fields. */
const char  *register_fn_ptr_typedef(const Type *fn_type);
void         type_codegen_reset_fn_ptr_typedefs(void);
void         type_codegen_emit_fn_ptr_typedefs(Buf *out);
/* Phase D: true if t is a struct type whose sizeof exceeds 16 bytes,
 * meaning it should be passed as const T* rather than by value. */
bool         type_struct_pass_by_ptr(Type t);
/* end-to-end-monomorphization: true when t is a (possibly applied) :heap struct
 * -- its monomorphic ABI is a typed pointer `T__A *`. */
bool         type_is_heap_struct(Type t);
/* seam 3: the ADT analogue -- true when t is a (possibly applied) :heap ADT
 * (the lowered form of a :heap defstruct); its ABI is `tur_adt_<Name> *`. */
bool         type_is_heap_adt(Type t);
/* True when t is a fully concrete (tyvar-free) type with a monomorphizable C
 * codegen layout -- e.g. `(Vec int)` but not `(Vec A)`.  Gates spec-minting on
 * concrete-only call sites. */
bool         type_has_concrete_codegen_layout(const Type *t);
/* True for a parametric ADT (`defdata` sum) application with all-concrete type
 * args (`(ReF bool)`, `(Cons int)`).  Distinct from the struct-app predicate
 * above; used by the M7 by-value HKT machinery for parametric SUM results. */
bool         type_app_is_concrete_adt(const Type *t);
/* The AdtDef at the head of an ADT application, or NULL if not an ADT app. */
AdtDef      *type_adt_app_def(const Type *t);
bool         type_has_open_slot(const Type *t);
/* Resolve an ADT ctor field's type against a concrete ADT-app receiver type. */
Type         adt_field_type_for_app(const Type *recv, const CtorField *field);
/* end-to-end-monomorphization: the by-value struct C name (`Vec__int`) for a
 * struct/struct-app, WITHOUT the trailing " *" the heap pointer lowering adds. */
const char  *type_struct_value_c_name(Type t);
/* SC7 (carrier-duality): true for a parametric struct with a single `:int`
 * field (e.g. `(defstruct Schema [A] (raw :int))`).  Such a phantom wrapper is
 * a transparent newtype over int64 -- one C representation everywhere, so HKT
 * dispatch can chain it (.fmap (.fmap s g) h) without rep-mixing. */
bool         type_is_transparent_int_newtype(Type t);
/* fn-value-fat-normalization stage 1: the shared param-normalization decision
 * (see types.c). Consulted by BOTH the elab call-site shim and the emit
 * invoke dispatch -- do not fork this logic. */
bool         fn_param_type_is_fat_normalized(const Type *t);
bool         fn_result_type_is_fat_normalized(const Type *t);
/* Phase HRT0: compute the rank of a type (0 = monotype, 1 = rank-1, ≥2 = higher-ranked) */
int          type_rank(const Type *t);

/* Phase HKT-P2: One-step unrolling of a TY_REC type.
 * Returns t->as.rec.body (the body with the recursive variable bound).
 * In v1, the body is not substituted; returns the raw body pointer.
 * Returns NULL if t is not TY_REC or body is not yet evaluated. */
Type *type_rec_unfold(Type *t);
void         type_print(Buf *b, Type t);

/* Phase HKT H0: Kind utilities */
bool         kind_eq(Kind a, Kind b);             /* true if a == b */
const char  *kind_to_string(Kind k);              /* "*", "* -> *", … */
Kind         kind_parse(const char *s);           /* parse "*" / "* -> *"; default KIND_STAR */

/* Phase TP3: kind helpers for n-ary type constructors (arbitrary arity).
 * kind_for_arity(n) returns (Kind)n -- no cap, any n is valid.
 *   0 -> KIND_STAR, 1 -> KIND_ARROW, 2 -> KIND_ARROW2, ... N -> arity-N kind. */
Kind         kind_for_arity(uint32_t n);
/* kind_apply_one(k) returns the kind after applying one type argument:
 * (Kind)(k-1) for any arrow kind (k >= 1, k != KIND_ROW).
 * KIND_STAR and KIND_ROW are returned unchanged (caller validates). */
Kind         kind_apply_one(Kind k);

/* Phase HKT-P2: Recursive type guards */
bool         type_is_guarded_recursive(Type t, const char *rec_name);

/* Phase 11: Copy/Move trait helpers */
/* Returns true if the type is Copy (bitwise duplication allowed) */
static inline bool type_is_copy(Type t) {
    return t.copy_kind == CK_COPY;
}

/* Returns true if the type is Move-only (ownership transfer) */
static inline bool type_is_move(Type t) {
    return t.copy_kind == CK_MOVE;
}

/* Returns true if the type is Unsized (size unknown at compile time) */
static inline bool type_is_unsized(Type t) {
    return t.copy_kind == CK_UNSIZED;
}

#endif
