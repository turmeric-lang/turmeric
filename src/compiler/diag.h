#ifndef TUR_DIAG_H
#define TUR_DIAG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "compiler/forms.h"

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

/* Forward declaration for Buf (defined in buf.h) */
struct Buf;

/* Error codes for diagnostics (Phase 8: diagnostics polish) */
typedef enum DiagCode {
    DIAG_CODE_NONE = 0,
    /* Type errors */
    TUR_E0001_TYPE_MISMATCH,
    TUR_E0002_ARITY_MISMATCH,
    TUR_E0003_UNBOUND_SYMBOL,
    /* Scope errors */
    TUR_E0004_INVALID_SCOPE,
    TUR_E0005_USE_AFTER_MOVE,
    /* Operator errors */
    TUR_E0006_OPERATOR_LOOKUP_FAILED,
    /* Capture errors */
    TUR_E0007_CAPTURE_ERROR,
    /* Effect-row mismatch (P19-2) */
    TUR_E0009_EFFECT_ROW_MISMATCH,
    /* Thread safety (T19-B) */
    TUR_E0010_NOT_SEND,   /* type cannot be sent across thread boundaries */
    TUR_E0011_NOT_SYNC,   /* type cannot be shared across thread boundaries */
    /* Kind mismatch (Phase HKT H1) */
    TUR_E0012_KIND_MISMATCH, /* type constructor kind does not match expected kind */
    /* Orphan instance (Phase HKT H4) */
    TUR_E0013_ORPHAN_INSTANCE, /* typeclass instance defined outside typeclass/type origin file */
    /* Parameterized typeclass constraints (Phase PTC2) */
    TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED, /* constraint cannot be satisfied for type */
    /* Phase B2: Cloneable continuation checks (CPS-CL7) */
    TUR_E0014_NOT_CLONE,                          /* captured binding does not implement Clone */
    TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,      /* cloneable-shift used outside cloneable-reset */
    /* Phase T25: Continuation escape into async scope */
    TUR_E0017_CONT_ESCAPE_ASYNC,                  /* effect handler continuation captured by async block */
    /* Phase 21: Serializable continuations */
    TUR_E0018_NOT_SERIALIZABLE,                   /* captured binding does not implement Serializable */
    TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,         /* serial-shift used outside serial-reset boundary */
    /* Phase D0: ambiguous typeclass method dispatch (erased receiver type) */
    TUR_E0020_AMBIGUOUS_DISPATCH,                 /* .method on int64_t receiver matches multiple instances */
    /* ER5: module effect visibility (PR5-1) */
    TUR_E0021_PRIVATE_EFFECT,                     /* effect is private to its defining module */
    /* CF6 (control-flow-completeness-plan): async Send-across-await soundness */
    TUR_E0022_AWAIT_LIVE_NOT_SEND,                /* non-Send binding in scope at await in async body */
    /* Phase B: mixed-width numeric arithmetic (no implicit coercion) */
    TUR_E0023_BIND_VOID_EXPRESSION,               /* `let` binding whose init has type :void */

    TUR_E0024_READS_FRAME_INVALID,                /* malformed or duplicated `#reads` frame */
    /* duplicate-instance-silently-drops-a-user-definstance: a user definstance
     * for a (class, type) the stdlib already covers is rejected, not dropped */
    TUR_E0025_DUPLICATE_INSTANCE,                 /* instance already defined for this class and type */
    /* effect-row honesty W1: a #fx{...} names an effect no defeffect declares */
    TUR_E0026_UNKNOWN_EFFECT_IN_ROW,

    TUR_E0042_MIXED_WIDTH_ARITH, /* distinct numeric kinds cannot be combined without (as ...) */
    /* ER1: strict-effects warnings */
    TUR_W0030_STRICT_EFFECTS_UNANNOTATED,  /* unannotated fn has non-empty inferred row (--strict-effects) */
    TUR_W0031_EFFECT_OVER_ANNOTATED,       /* declared effect never performed */
    TUR_W0032_ROW_VAR_ALWAYS_CONCRETE,    /* row variable is always concrete; suggest replacing with concrete row */
    TUR_W0033_UNREACHABLE_HANDLER,        /* handler clause for Foo is unreachable -- body never performs Foo */
    /* ET2: effect polymorphism warnings */
    TUR_W0034_ROW_VAR_GENERALISED,        /* row variable auto-generalised; consider explicit forall [e] (--strict-effects) */
    /* GATE / ET2: effect row type errors */
    TUR_E0254_INFINITE_EFFECT_ROW,     /* occurs check: binding effect row variable would produce an infinite row */
    /* SZ7 (sized-types-completion-plan): static size checking (-Xsized-types) */
    TUR_E0260_SIZED_TYPE_MISMATCH,     /* two statically-known size indices are not equal/compatible */
    /* ET3: handler typing errors */
    TUR_E0251_HANDLER_OVERLAP,            /* composed handlers handle overlapping effects */
    TUR_E0252_HANDLER_RESULT_MISMATCH,   /* handler clause result type does not match handle expression type */
    /* ET4: effect scope errors */
    TUR_E0250_ROW_VAR_ESCAPES_SCOPE,    /* forall [e] row variable used outside its quantifier scope */
    TUR_E0253_EFFECT_NOT_IN_SCOPE,      /* perform site uses an effect not declared or in scope */
    /* LT1: Linear type errors (-Xlinear) */
    TUR_E0100_LINEAR_DROPPED,      /* linear value dropped without being consumed */
    TUR_E0101_LINEAR_USE_AFTER_CONSUME, /* linear value used after being moved/consumed */
    TUR_E0102_LINEAR_COPY,         /* cannot copy a linear value */
    TUR_E0103_LINEAR_IN_RC,        /* cannot wrap a linear value in rc<T> */
    TUR_E0104_LINEAR_BRANCH_MISMATCH, /* linear value consumed in one branch but not another */
    /* TY4: Lifetime / borrow-escape errors */
    TUR_E0105_BORROW_ESCAPES_SCOPE,   /* a borrow outlives the value it points to */
    TUR_E0106_CYCLIC_LIFETIME,        /* lifetime outlives-constraints form a cycle */
    TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER, /* handler case drops an owning field of a captured by-value local (double-drop vs scope auto-drop) */
    TUR_E0108_REF_FIELD_MOVED_OUT_OF_BORROW, /* a by-value struct with an owning ref field returned out of a borrow / global / element */
    /* UT1: Uniqueness type errors (-Xunique-types) */
    TUR_E0200_UNIQUE_ALIASED,      /* value is not unique -- aliased by another binding */
    TUR_E0201_UNIQUE_COPY,         /* cannot copy a unique value (use after consume) */
    TUR_E0202_UNIQUE_IN_RC,        /* cannot wrap a unique value in rc<T> */
    /* ST0: Substructural type errors (-Xsubstructural) */
    TUR_E0150_AFFINE_USED_TWICE,   /* affine value used more than once */
    TUR_E0151_RELEVANT_DROPPED,    /* relevant value dropped without being used */
    /* CONV-S6: product-shape (struct/record-variant) construction errors */
    TUR_E0292_MISSING_FIELD,    /* construction omits a required field */
    TUR_E0293_DUPLICATE_FIELD,  /* construction names the same field twice */
    TUR_E0294_UNKNOWN_FIELD,    /* construction names a field the type lacks */
    TUR_E0296_WITH_NOT_COPY,    /* `with` used on a move-only (non-:copy) type */
    TUR_E0297_WITH_UNKNOWN_FIELD, /* `with` override names a field the type lacks */
    TUR_E0298_WITH_DUPLICATE_FIELD, /* `with` overrides the same field twice */
    TUR_E0299_MIXED_POS_KW,     /* construction mixes positional and keyword args */
    /* GAP 3 (byvalue-adt-int-cast-plan): `::` between a by-value aggregate
     * (non-recursive ADT/struct product) and a one-word carrier (:int/:ptr<void>)
     * has no sound lowering -- the aggregate has no int64 handle to reinterpret. */
    TUR_E0295_BYVALUE_CARRIER_CAST,
    /* IT1: Union type errors (-Xunion-types) */
    TUR_E0300_UNION_TYPE_MISMATCH,   /* value type not a member of union type */
    TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH, /* match on union type missing arm for one or more members */
    /* sealed-opaque (graduated 0.34.0): `::` between a `:sealed` defopaque and
     * its representation type, outside the module that declared it. */
    TUR_E0302_SEALED_OPAQUE_CAST,
    /* option-niche: a literal 0 ascribed into a `:non-null` defopaque -- a
     * statically provable violation of the declaration the niche's soundness
     * rests on.  The runtime Some-ctor check covers what elaboration cannot
     * see (inline-C, computed values); a violation it CAN see errors here. */
    TUR_E0303_NON_NULL_OPAQUE_ZERO,
    /* IT3: Intersection type errors (-Xintersection-types) */
    TUR_E0350_INTERSECTION_UNSATISFIABLE,   /* no value can satisfy all intersection members */
    TUR_E0351_INTERSECTION_MEMBER_MISMATCH, /* value doesn't satisfy an intersection member */
    /* U6: inline-C outside Unsafe annotation */
    TUR_W0036_INLINE_C_MISSING_UNSAFE, /* inline-C block in function not annotated #{Unsafe} */
    /* Phase C: narrow-width param in inline-C body */
    TUR_W0037_INLINE_C_NARROW_PARAM,   /* defn param has narrow numeric type in inline-C body */
    /* Phase R6b: --lint-panic panic call site outside the allow-list */
    TUR_W0038_LINT_PANIC_SITE,
    /* A free top-level defn shares its name with a user-defined typeclass
     * method, silently shadowing the method at every bare call site.
     * See docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md. */
    TUR_W0039_METHOD_DEFN_CLASH,
    /* Eval-mode unknown call head deferred to runtime-dispatch -- the name is
     * not bound at elaboration time and not in the typed-native registry, so
     * it will fail at runtime if no native is registered for it before the
     * call runs.  Likely a typo.  See
     * docs/archive/history/eval-mode-unknown-call-deferred-to-runtime.md. */
    TUR_W0040_EVAL_UNKNOWN_CALL_RUNTIME_DISPATCH,
    /* arbitrary-fn-arity Phase 6: a defn/fn declares more positional parameters
     * than the historical soft ceiling of 16.  Not an error -- the mechanical
     * bound is now MAX_FN_ARITY (64) -- but a lint nudge toward the arity style
     * guide (a defstruct options value or a `& rest :type` variadic). */
    TUR_W0041_HIGH_ARITY,
    /* A `defn`/`defmacro` names a reserved special form (`return`, `match`,
     * `handle`, ...).  Head-position dispatch in elab_call matches those names
     * by symbol identity *before* any binding or macro lookup, so the
     * definition is accepted but every bare `(name ...)` call site elaborates
     * as the special form and the definition is unreachable by its bare name.
     * See docs/archive/history/defn-shadows-return-special-form.md. */
    TUR_W0042_SHADOWS_SPECIAL_FORM,
    /* compiled-async-fiber-deadlocks-on-a-session-op, fix direction 3: an
     * `async` body captures a session endpoint or spells a session op.
     * Compiled `async` runs its body on the spawning thread and a session
     * op blocks that thread until the peer arrives, so unless the peer is
     * on another OS thread the program hangs with no further diagnostic.
     * Not emitted under --interpret, whose rendezvous is cooperative. */
    TUR_W0043_SESSION_OP_IN_ASYNC,
    /* MS2: Multi-shot continuation capture analysis */
    TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,       /* ^multishot handler captures a unique/linear value */
    TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,  /* ^multishot annotation outside a handler continuation */
    TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC,     /* resume of ^multishot k inside atomically block */
    /* CT0: Contract type errors */
    TUR_E0400_CONTRACT_VIOLATED,   /* contract check failed: predicate is false */
    TUR_E0401_POSTCOND_VIOLATED,   /* postcondition failed: predicate is false for result */
    /* SS0b-SS1: Session type errors (-Xsessions) */
    TUR_E0210_SESSION_NOT_DUAL,         /* session endpoints are not dual protocols */
    TUR_E0211_SESSION_DROPPED,          /* session channel dropped before protocol completion */
    TUR_E0212_SESSION_PROTO_MISMATCH,   /* channel operation not valid for current session protocol */
    /* SS5: Global protocol type errors (-Xsessions) */
    TUR_E0220_GLOBAL_NOT_PROJECTABLE,   /* global protocol G is not projectable onto role R at step */
    TUR_E0221_ROLE_NOT_DECLARED,        /* role R is not declared in global protocol G */
    TUR_E0222_ROLE_IMPL_MISMATCH,       /* role R impl does not match projected local type */
    TUR_E0223_GLOBAL_NOT_WELLFORMED,    /* global protocol G is not well-formed: reason */
    /* DV0-DV1: Dynamic var errors (-Xdynamic-vars) */
    TUR_E0600_DYNVAR_SET_NOT_DYNAMIC,    /* set! or binding target is not a dynamic var */
    TUR_E0601_DYNVAR_SET_NO_BINDING,     /* set! on dynamic var with no active binding frame */
    TUR_E0602_DYNVAR_TYPE_MISMATCH,      /* override value type does not match defdynamic type */
    TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE, /* dynamic var declared with a substructural type */
    TUR_E0604_DYNVAR_NOT_TOPLEVEL,       /* defdynamic used outside module toplevel */
    TUR_E0605_DYNVAR_SET_IN_ATOMIC,      /* set! on dynamic var inside an atomically block */
    /* DV0: Dynamic var naming warning */
    TUR_W0600_DYNVAR_NO_EARMUFFS,        /* defdynamic name does not use *earmuffs* convention */
    /* CF3/CF4 (control-flow-completeness-plan): gated / unsupported control-flow.
     * E07xx band reserved in Phase CF0; see docs/control-flow-completeness-plan.md. */
    TUR_E0700_CALLCC_GATED,              /* RETIRED (call-cc-completion CC5): call/cc is now real + ungated on the CPS substrate. Code reserved, no longer emitted. */
    TUR_E0701_ESCAPE_GATED,             /* RETIRED (call-cc-completion CC5): escape is now real + ungated on the CPS substrate. Code reserved, no longer emitted. */
    /* CF5 (control-flow-completeness-plan): always-on generator limitation diagnostics */
    TUR_E0702_YIELD_IN_MATCH_ARM,        /* yield/yield* inside a match arm (1.0 limitation) */
    TUR_E0703_YIELD_IN_RECURSIVE_GEN,    /* yield/yield* inside a recursive generator (1.0 limitation) */
    TUR_E0704_HANDLER_COMPOSE_UNIMPL,    /* first-class handler composition (compose-handlers) not yet implemented (gated) */
    /* poly-defn-shares-inner-closure-body-across-monomorphizations: a generic
     * defn that returns an (fn ...) whose result type is one of the defn's type
     * parameters emits a single shared inner closure body (integer thunk ABI);
     * a floating-point specialization dispatches through the wrong register and
     * silently miscompiles.  Rejected until per-A inner-body specialization lands. */
    TUR_E0705_POLY_CLOSURE_RESULT_TYVAR,
    /* serial-shift-unsupported-context-miscompile: a serial-shift whose
     * delimited context falls outside the DK-lowering grammar (collect_ctx)
     * cannot be reified into a marshalable continuation.  Rejected at codegen
     * instead of silently lowering to a 0 placeholder / __builtin_trap(). */
    TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
    /* float-register-class-returns: a function/instance-method whose declared
     * return type and body land in DIFFERENT register classes -- a float
     * (xmm) on one side and a concrete non-float (int64 GP register: int,
     * cstr, bool, opaque/struct/ADT handle) on the other.  Unlike the
     * same-register-class carrier bridges the ABI deliberately tolerates,
     * a float-vs-non-float result is a genuine xmm0-vs-rax miscompile. */
    TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
    /* pointer-vs-scalar-returns: a function/instance-method whose declared
     * return type is concretely `cstr` (a `const char*`) but whose body yields
     * a concrete integer-family scalar (int / bool / intN / uintN).  cstr and
     * the integer family all ride the int64 GP register, so the carrier ABI
     * cannot see the swap -- but a bare integer is never a valid string
     * pointer, so committing to `cstr` and returning an integer is a genuine
     * type-erasure bug, not a tolerable carrier bridge.  Only this commit
     * direction is rejected; the reverse (an integer carrier returning a cstr
     * handle) stays a legitimate bridge. */
    TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
    /* carrier-aware-return-unification Phase 2: a genuinely COMMITTED function
     * (a monomorphic, non-`#{Unsafe}` `defn` -- one that does not participate in
     * the int64 carrier ABI) declares a concrete integer-family return but its
     * body yields a concrete `cstr` (a `const char*` string pointer).  This is
     * the REVERSE of the TUR-E0708 commit direction: a string pointer is never a
     * valid integer.  It is tolerated for generic / `#{Unsafe}` / typeclass code
     * (the carrier-handle bridge), but in a committed monomorphic function there
     * is no carrier to bridge, so it is a real type-erasure bug. */
    TUR_E0709_RETURN_TYPE_MISMATCH,
    /* cloneable-shift-unsupported-context-miscompile (D6a): a cloneable-shift
     * whose delimited context falls outside the native build_cloneable grammar
     * cannot be reified into a multi-shot continuation.  The legacy setjmp
     * fallback silently dropped the context (lowered the continuation as the
     * identity), so `(+ (compute) (cloneable-shift ...))` printed a wrong number.
     * Rejected at codegen instead, mirroring the serial TUR-E0706 fix. */
    TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE,
    /* defmodule-bare-toplevel-forms-silently-dropped: a non-definition form as
     * a direct child of (defmodule ...).  The emitter works off the globally
     * registered definitions and never reads the module body list, so such a
     * form was fully elaborated -- type errors inside it reported normally --
     * and then dropped with no diagnostic.  109 (describe ...) blocks across 12
     * spices never ran, and 8 of them passed CI vacuously.  There is no
     * module-level side-effect position today, so this is rejected rather than
     * emitted; `defer` is the one non-definition form that is legal here. */
    TUR_E0711_MODULE_TOPLEVEL_EXPR,
    /* emit-value-dispatch-unbounded-recursion: the emitter's expression walk
     * (emit_value -> emit_value_dispatch -> emit_builtin -> ...) is plain
     * structural recursion, so a deeply nested expression exhausted the C
     * stack -- a SIGSEGV with no source attribution, and on the documented
     * Debug+ASan bootstrap build it took only ~50 levels because ASan inflates
     * the frame roughly 40x.  Bounded now, so an expression too deep to compile
     * gets a diagnostic instead of a crash.  `tur check` runs the emitter too,
     * so this covers the LSP path as well. */
    TUR_E0712_EXPR_NESTING_TOO_DEEP,
    /* nested-defn-accepted-outer-returns-zero: a function body whose LAST form
     * is a definition.  Nested `defn` is a real feature (Phase B3 -- it lifts to
     * file scope and is callable by name), so the definition is not the problem;
     * being in TAIL position is.  A definition yields no value, and codegen fell
     * back to `return 0;` for the enclosing function -- it ran, exited 0, and
     * returned the wrong answer with no diagnostic at any stage.  In practice
     * the cause is always a missing close paren, which makes the following
     * definitions parse as nested ones. */
    TUR_E0713_DEFINITION_IN_TAIL_POSITION,
    /* container-element-form-plan CE1: a niche-represented `(Option P)`
     * element (the default representation since 2026-09-03) is stored into a Vec whose element
     * type is still ERASED at this site (a raw-`:int` or unresolved
     * receiver).  The slot convention is per element monomorph -- a word for
     * a niche element, a box otherwise -- and an erased store cannot know
     * which, so it would put a second convention into the same vec that a
     * concrete reader cannot tell apart.  Refuses loudly instead of guessing. */
    TUR_E0714_NICHE_ELEMENT_ERASED_STORE,
    /* erased-closure-param-over-niche-vec-slot-reads-box: the READ side of
     * E0714's store.  `vec-eq?` hands its comparator raw Vec slot words, and a
     * niche `(Option P)` element rides its slot as the payload pointer rather
     * than a carrier box.  A comparator written inline is marked so the
     * ascription back to the element type reinterprets the word; a NAMED one
     * cannot be, because the same function may also be called with genuine
     * boxes.  Erased parameters there have no decidable convention. */
    TUR_E0715_NICHE_ELEMENT_ERASED_COMPARATOR,
    /* Deprecation band (TUR-D####): syntax accepted for backward
     * compatibility but slated for removal.  Emitted as DIAG_WARNING;
     * promoted to DIAG_ERROR under --Werror=deprecated. */
    /* fn-type-bare-identifier-plan Phase 3: a leading colon on a type
     * inside a (fn ...) type expression is redundant -- position alone
     * marks the param/result slots as types.  Drop the colon:
     * (fn [:int] :int) -> (fn [int] int). */
    TUR_D0001_FN_TYPE_COLON,
    /* fx-row-syntax-rename-plan Phase 2: bare `#{...}` as an effect row is
     * deprecated; prefer `#fx{...}`.  Emitted from elaboration when an
     * F_MAP whose provenance is PROV_FX_LEGACY is consumed as an effect row. */
    TUR_D0002_FX_ROW_LEGACY_HASH,
    /* fx-row-syntax-rename-plan Phase 2: `@{...}` as an effect row is
     * deprecated; prefer `#fx{...}`.  Emitted from elaboration when an
     * F_MAP whose provenance is PROV_FX_AT_LEGACY is consumed as an
     * effect row.  Note: this does not affect bare `@x` deref sugar. */
    TUR_D0003_FX_ROW_LEGACY_AT,
    /* effect-row-honesty-plan W0: `(match #fx{NonExhaustive} x ...)` is
     * deprecated; the marker is the attribute `^non-exhaustive`. */
    TUR_D0004_NONEXHAUSTIVE_FX_MARKER,
    /* XF (experimental-flag-mechanism-plan): the `--enable=<name>` surface.
     * E0310 fires at CLI/manifest parse on an unknown experiment name;
     * W0060/W0061 fire once per compile at the first use site of an enabled
     * prototype/beta experiment.  W0060/W0061 are emitted directly to stderr
     * by experiment_warn_if_used (mirroring TUR-W0050), not through diag_emit;
     * they are registered here only so `tur explain` can describe them. */
    TUR_E0310_UNKNOWN_EXPERIMENT,
    /* engine-selection-plan E1: build.tur's `:engine` key carries a value
     * outside {"cc","jit","interp"}.  A hard error, unlike unknown manifest
     * KEYS (silently ignored for forward compatibility): a typo'd engine
     * silently running under cc is the exact failure the key exists to
     * prevent. */
    TUR_E0311_UNKNOWN_ENGINE,
    /* saffron-lang-plan D7: a `#lang saffron` file used a feature whose proof
     * READS AN INFERRED TYPE -- the one thing Saffron makes `any`.
     *
     * Today that is exactly `with-region`.  The emitter's static walk over the
     * bracket's result type hits `any`, takes the `default: return true` arm
     * of `region_type_reaches_node` ("can reach a node"), and therefore emits
     * a plain `tur_region_pop` -- retire, do not rewind -- where the same
     * program in Turmeric emits `tur_region_pop_checked`.  So the bracket is
     * SAFE and reclaims nothing: the cost is paid and the saving never
     * arrives.  A feature that silently does nothing is worse than one that
     * says it is unavailable.
     *
     * D7's list was longer, and MEASUREMENT CUT IT TO ONE.  GADTs, sessions,
     * linearity and borrows all check identically in a Saffron file, because
     * their proofs read ANNOTATIONS (which D2 keeps legal) or walk uses and
     * scopes, neither of which `any` touches: skolem escape, TUR-E0211,
     * TUR-E0101 and the borrow-aliasing conflict all still fire.  They are
     * pinned by tests/fixtures/saffron-static-guarantees-still-hold so a later
     * reading of the plan's original list does not take them away.
     *
     * File-level, not program-level: a Turmeric module in the same project
     * keeps regions in full.  The two dialects link; they just do not each get
     * the other's guarantees. */
    TUR_E0312_SAFFRON_STATIC_ONLY,
    TUR_W0060_EXPERIMENTAL_PROTOTYPE,
    TUR_W0061_EXPERIMENTAL_BETA,
    /* RT3 (refinement-types-plan): static discharge of `#refine{...}`
     * predicates.  Emitted unconditionally since refinement types graduated in
     * v0.33.0; there is no longer an experiment gate.  E0371/W0372 are the two
     * verdict-carrying codes: the runtime contract check survives in both
     * cases, so neither is a miscompile -- W0372 is "we could not prove it",
     * E0371 is "we found a counterexample".  Under --strict-refine both are
     * hard errors. */
    TUR_E0370_REFINE_ILL_TYPED,   /* refinement predicate is ill-typed */
    TUR_E0371_REFINE_NOT_PROVED,  /* counterexample found; predicate is not entailed */
    TUR_W0372_REFINE_UNKNOWN,     /* no backend decided it; runtime check kept */
    TUR_W0373_REFINE_NONLINEAR,   /* nonlinear subterm treated as uninterpreted */
    TUR_E0374_REFINE_INSTANCE_STRONGER, /* instance method demands more than its class signature */
    TUR_E0375_REFINE_EFFECTFUL,   /* refinement predicate mentions effects */
    TUR_E0376_REFINE_TYPE_PARAM,  /* refinement on a type parameter (unsupported) */
    TUR_W0377_REFINE_INSTANCE_LENIENCY, /* call allowed only because the resolved
                                         * instance demands less than its class */
    TUR_E0378_REFINE_IN_FN_TYPE,  /* refinement written inside a (fn ...) type */
    TUR_I0379_REFINE_ORACLE_MISMATCH, /* RETIRED (Z3 oracle retirement, 0.32.5): the
                                       * dev-only oracle whose disagreements this
                                       * reported is gone. Code reserved, no longer
                                       * emitted. */
    /* A refinement written in TYPE-ARGUMENT position -- `(Box #refine{...})`.
     * The contract is peeled to its base type so the payload behaves like the
     * ordinary value it is; the predicate is NOT enforced on the payload.
     * Warned rather than dropped silently: an annotation that quietly does
     * nothing is how a reader ends up believing a container's contents are
     * checked when they are not.  See
     * docs/archive/contract-type-arg-not-peeled-to-base.md. */
    TUR_W0380_REFINE_TYPE_ARG_UNENFORCED,
    /* WF1/WF2 (checked-write-frames-plan): the `#writes` write-frame annotation.
     * E0381 -- the annotation itself is malformed, or names something that is
     *          not a parameter of this function.  A frame that does not resolve
     *          cannot be checked against anything, so it is an error rather
     *          than a silently ignored decoration.
     * E0382 -- the body WRITES outside the frame it declared.  This is the WF2
     *          checked tier: a declared frame the body exceeds is an error, not
     *          a silent widening, for the same reason `#reads` is -- downstream
     *          code is entitled to believe the declaration. */
    TUR_E0381_WRITES_FRAME_INVALID,
    TUR_E0382_WRITES_FRAME_EXCEEDED,
    /* mutable-globals-plan section 12.3, shipped warning-first per section
     * 13.1: a `#reads <param>` frame is TRUSTED, and its one consumer grants
     * congruence -- so a frame that omits mutable state the body reads buys a
     * proof it has not earned (the caller-side crossing check is elided on a
     * predicate that may be false).  This warns when the body DEMONSTRABLY
     * reads a mutable global: positive evidence only, so an inline-C body --
     * which is every measure that predates this -- stays silent.  Gateless
     * because it reports a live trust-boundary fact and changes no behavior;
     * escalating warn -> refuse-the-override is a later, gated step. */
    TUR_W0383_READS_FRAME_OMITS_MUTABLE,
    /* reflected-measures (docs/upcoming/reflected-measures-plan.md), behind
     * `--enable=reflected-measures`.  The plan reserved E0383/W0384; W0383
     * had been taken by `#reads` by land time, so the pair sits one up.
     * E0384 -- a `^reflect` function failed the TOTALITY gate: it is not
     *          proven pure, a self-call does not pass a strict structural
     *          subterm of the corresponding parameter, it is mutually
     *          recursive, or a `match` in it is not proven exhaustive.  A
     *          hard error on the definition, never a silent downgrade: an
     *          unfolded equation of a non-total function is an inconsistent
     *          hypothesis, which discharges EVERY obligation in the unit.
     * W0385 -- the unfolding fuel ran out while encoding an obligation that
     *          then stayed Unknown; the runtime check is kept (the W0372
     *          principle).  --strict-refine promotes it. */
    TUR_E0384_REFLECT_NOT_TOTAL,
    TUR_W0385_REFLECT_FUEL_EXHAUSTED,
    /* class-superclasses (docs/archive/typeclass-superclasses-plan.md), the
     * `defclass` constraint preamble `[(Super var)...]`:
     * E0390 -- the preamble itself: empty, malformed (an element that is not
     *          `(Class var...)`), naming a variable that is not one of the
     *          class's type params, or placed after the `|` fundep clause
     *          instead of before it.
     * E0391 -- a superclass name does not resolve to a defined typeclass, or
     *          its parameter count / kinds do not fit the arguments given.
     * E0392 -- the superclass graph has a cycle (a class may not, directly
     *          or transitively, list itself).
     * E0393 -- the instance obligation (plan Half B): `(definstance C [T])`
     *          where a superclass `S` of `C` has no instance applying to `T`.
     *          Without this the entailment (Half A) would license a method
     *          call for which no instance need exist. */
    TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
    TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED,
    TUR_E0392_CLASS_SUPERCLASS_CYCLE,
    TUR_E0393_CLASS_SUPERCLASS_INSTANCE_MISSING,
    /* exports-map-syntax-tighten-plan: `:exports` in build.tur got an
     * effect-row literal (`#fx{...}` or `@{...}`) instead of a map literal
     * (`#map{...}`) or a legacy bare `#{...}` map or a path vector. */
    TUR_E0620_EXPORTS_FX_ROW,
    /* `:tur-version` in build.tur (no-compiler-version-constraint-in-manifest):
     * E0621 -- the running compiler is BELOW the declared floor, so the spice's
     *          source genuinely will not work.  Hard error.
     * E0622 -- the range itself is malformed; a typo must not silently become a
     *          different constraint.  Hard error.
     * W0623 -- the running compiler is ABOVE the declared ceiling.  Only means
     *          "never tested against this compiler", which is usually fine, so
     *          it warns: a hard ceiling would make every release break every
     *          spice until each author bumped a number. */
    TUR_E0621_TUR_VERSION_BELOW_FLOOR,
    TUR_E0622_TUR_VERSION_MALFORMED,
    TUR_W0623_TUR_VERSION_ABOVE_CEILING,
    /* examples-have-no-suite-coverage (section 2): a whole-program build with
     * no `main` and no top-level statements synthesizes an EMPTY main -- the
     * program builds, runs, and does nothing.  A top-level function whose name
     * is a near-miss of `main` (`-main`, `main-`, `Main`, `_main`) is the
     * tell that an entry point was intended; warn at its definition. */
    TUR_W0624_NO_ENTRY_POINT_NEAR_MISS,
    /* application-image-dumps-plan AI3.1: the `init` root of a
     * with-image-cache-after-init expansion writes a top-level global that no
     * `defimage-global` declared.  init runs only on a cold start and the
     * image carries the continuation, not the heap, so the write is silently
     * absent after a warm start. */
    TUR_W0706_IMAGE_GLOBAL_UNREGISTERED,
    /* sweet-dollar-inside-brackets-is-a-silent-symbol: the sweet-exp
     * preprocessor rewrites `$ <rest>` to `(<rest>)` only where the
     * indentation layer is live -- `bd == 0` in sweet_emit_content.  Inside
     * `(...)`, `[...]` or `{...}` it declines, and the token then reached the
     * reader as an ordinary symbol named `$`, silently changing the form's
     * shape with no error.  A bare `$` in a sweet-exp file can only have
     * arrived that way, so the reader rejects it. */
    TUR_E0332_SWEET_DOLLAR_IN_BRACKETS,
    /* proper-tail-calls T1 (docs/archive/proper-tail-calls-plan.md, T-D1):
     * a call annotated `^tailcall` that the emitter did NOT place in tail
     * position.  The annotation exists so that "is this actually a tail
     * call?" -- today answerable only by reading the emitted C, and
     * answerable in production only by a SIGSEGV at an unpredictable depth --
     * becomes a compile-time conversation.  The message always names the
     * specific reason the call could not be one. */
    TUR_E0716_TAILCALL_NOT_TAIL,
} DiagCode;

typedef enum DiagLevel {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE,
    DIAG_HELP,
} DiagLevel;

#define DIAG_CONTEXT_LINES 2  /* Number of context lines before/after the error line */

/* Reader types for #lang dispatch (Phase S1) */
typedef enum ReaderType {
    READER_UNKNOWN = -1,   /* Unknown/invalid #lang directive */
    READER_TURMERIC,       /* Default s-expression reader */
    READER_CURLY_INFIX,    /* Turmeric + curly-infix (SRFI-105) */
    READER_NEOTERIC,       /* Turmeric + neoteric notation */
    READER_SWEET,          /* Full sweet-expressions */
    /* r7rs-lang-plan R1: the Scheme reader.  A VARIANT of the s-expression
     * reader (one flag on `Reader`, beside neoteric_enabled), not a second
     * reader: `#t`/`#f`, `#\c` with the R7RS names and `#\x<hex>`, `#(...)`,
     * `#u8(...)`, `,`/`,@` as unquote (comma is whitespace in every Turmeric
     * reader), dotted pairs, `|sym|`, the `#x`/`#o`/`#b`/`#d`/`#e`/`#i`
     * numeric prefixes and the Scheme string escapes.  Only `#lang r7rs`
     * selects it (D1). */
    READER_R7RS,
    /* r7rs-sweet-base-dialect-missing: `#lang r7rs/sweet`, SRFI-110 over
     * Scheme's lexical syntax -- the sweet-exp preprocessor (indentation, `$`)
     * with its Scheme lexemes, then the Scheme reader with neoteric `f(x)` on.
     * Scheme's second reader, and its last: `r7rs/neoteric` and
     * `r7rs/curly-infix` are not bases (curly-infix is on under both). */
    READER_R7RS_SWEET,
} ReaderType;

/* saffron-lang-plan D1: the LANGUAGE axis of a `#lang` line, orthogonal to the
 * reader axis above.
 *
 * `#lang <base>[/<dialect>]` resolves to a PAIR now, not a single enum.  The
 * reader axis says how the text is parsed (s-expr, curly-infix, neoteric,
 * sweet); this one says which language the parsed forms are elaborated as.
 * They are independent: a Saffron file may be written in any of the four
 * surface syntaxes, so `saffron`, `saffron/sweet`, `saffron/neoteric` and
 * `saffron/curly-infix` are all spellable.
 *
 * Deliberately NOT folded into ReaderType.  Doing so would double that enum and
 * leave every `switch (reader_type)` in the tree obliged to remember that half
 * its cases mean the same reader -- see the plan's D1 for the full argument. */
typedef enum LangDialect {
    LANG_TURMERIC = 0,   /* the default; every existing file */
    LANG_SAFFRON,        /* dynamically typed dialect (`#lang saffron`; stable since 0.46.0) */
    /* r7rs-lang-plan D1: R7RS-small Scheme.  Rides the same dynamic substrate
     * as Saffron (LangTraits.dynamic) under a Scheme reader (READER_R7RS);
     * experiment-gated, and the `#lang` line is itself the enable (D11). */
    LANG_R7RS,
} LangDialect;

/* Canonical name of a dialect, for diagnostics and `tur dialects`.
 * The sibling of reader_type_name. */
const char *lang_dialect_name(LangDialect d);

/* Source map for syntax-transforming readers (currently sweet-exp).
 * Each run says "starting at xform_offset in the transformed text,
 * `length` bytes were copied verbatim from orig_offset of the original
 * source."  The runs are kept sorted by xform_offset; bytes that fall
 * in the gaps between runs are reader-inserted (`(`, `)`, etc.) and
 * have no original counterpart. */
typedef struct SweetMapRun {
    uint32_t xform_offset;
    uint32_t orig_offset;
    uint32_t length;
} SweetMapRun;

typedef struct SweetMap {
    SweetMapRun *runs;
    size_t       n_runs;
    size_t       cap_runs;
} SweetMap;

/* Translate a byte offset in the transformed text to the corresponding
 * byte offset in the original source.  When the input offset falls in
 * an inserted gap it returns the end of the preceding run (the closest
 * real position).  Safe to call with map == NULL — returns xform_off. */
size_t sweet_map_translate_offset(const SweetMap *map, size_t xform_off);

typedef struct SourceFile {
    const char *path;
    /* Directory to resolve in-source relative paths (e.g. the
     * `#use-reader-macros "..."` directive) against, used when `path` itself
     * carries no directory component -- notably the `--interpret`/eval blob
     * whose path is the synthetic "<eval>".  NULL means fall back to dirname
     * of `path` (the normal compiled-file case). */
    const char *base_dir;
    const char *src;     /* full source text (transformed if xform_map set) */
    size_t      len;
    uint16_t    file_id;
    ReaderType  reader_type;  /* Phase S1: for enabling syntax features */
    /* saffron-lang-plan S1: which LANGUAGE this file is elaborated as, parsed
     * from the same `#lang` token as reader_type (D1).  LANG_TURMERIC for every
     * file without a `#lang saffron...` line, which is what a `{0}`/memset
     * SourceFile starts as -- so an unwired construction site keeps today's
     * behaviour rather than silently opting into a dialect. */
    LangDialect lang;
    /* Sweet-exp transformation support: when xform_map is non-NULL, src
     * is the preprocessed s-expression text and orig_src/orig_len point
     * to the user's original source.  Diagnostics render snippets from
     * orig_src and translate span offsets via xform_map. */
    const char     *orig_src;
    size_t          orig_len;
    const SweetMap *xform_map;
    /* Bytes of the file on disk that `src` starts past -- the `#lang` line
     * the reader strips before handing the body over.
     *
     * Line numbering survives that strip (the directive's own newline is left
     * in place, so line 1 is simply empty), which is why nothing needed this
     * until something started reading span *offsets*. Those are relative to
     * `src`, so an editor that seeks to one lands `head_offset` bytes early --
     * on a `#lang turmeric` file as much as a sweet-exp one. Zero for a file
     * with no directive. */
    size_t          head_offset;
} SourceFile;

/* Detect a `#lang` directive (Phase S0).  `out_rest`/`out_rest_len` advance
 * past the directive line, so the reader never sees its leading '#'; they are
 * left pointing at `src` when there is no directive.
 *
 * `#lang` takes a single base dialect and nothing else.  A trailing token
 * after the base name is not legal: `*out_bad`/`*out_bad_len` point at the
 * first one (into `src`) and the caller reports TUR-E0330.  The one exception
 * is a RETIRED token from the decommissioned layer axis, which is accepted,
 * warned once (TUR-W0064) and ignored for one minor line -- see reader.c.
 *
 * An unrecognised BASE comes back through the same out-param, with
 * READER_UNKNOWN returned; that is how a caller tells the two apart.  The
 * token loop consumes to end-of-line either way, so nothing ever leaks into
 * the body.  Any out-param may be NULL. */
ReaderType detect_lang(const char *src, size_t len,
                       const char **out_rest, size_t *out_rest_len,
                       const char **out_bad, size_t *out_bad_len);

/* saffron-lang-plan S1: detect_lang plus the LANGUAGE axis (D1).
 * `out_dialect` receives the dialect the base token named; every non-Saffron
 * spelling yields LANG_TURMERIC.  detect_lang is this with
 * `out_dialect == NULL`, which is why the callers that do not care about the
 * dialect need no change -- only the paths that elaborate a file thread it
 * through to SourceFile.lang. */
ReaderType detect_lang_dialect(const char *src, size_t len,
                               const char **out_rest, size_t *out_rest_len,
                               const char **out_bad, size_t *out_bad_len,
                               LangDialect *out_dialect);

/* Apply a file's dialect, at the point the reader has decided what it is.
 *
 * saffron GRADUATED at 0.46.0, so no dialect is gated: there is nothing to
 * enable, nothing to warn about, and a project manifest can no longer refuse
 * one.  What remains is recording that this build contains a dynamically
 * typed TU (g_opt_dynamic_any -- keyed on the language's LangTraits.dynamic
 * bit, not its name), which the emitter reads to decide whether to emit the
 * `any` registries -- see lang_dialects.c.
 *
 * Still returns bool, and callers still check it, because that is the shape a
 * future gated dialect needs; today it cannot fail. */
bool lang_dialect_apply(LangDialect d, const char *path);

/* Get reader type from file extension (Phase S0); `.scm` is the Scheme reader. */
ReaderType reader_type_from_extension(const char *path);

/* The language an extension selects on its own: LANG_R7RS for `.scm`, else
 * LANG_TURMERIC ("the directive decides").  Every site that pairs
 * reader_type_from_extension with detect_lang_dialect applies this after
 * the directive, so a `.scm` file elaborates as Scheme with or without a
 * `#lang r7rs` line. */
LangDialect lang_dialect_from_extension(const char *path);

/* Get reader type name as string (Phase S0) */
const char *reader_type_name(ReaderType type);

/* Check if a reader type is implemented (Phase S0) */
bool reader_type_is_implemented(ReaderType type);

/* Underline style for diagnostics */
typedef enum UnderlineStyle {
    UNDERLINE_PRIMARY,   /* ^^^ for primary span */
    UNDERLINE_SECONDARY, /* ~~~ for secondary/related spans */
    UNDERLINE_GAP,       /* - for gaps between spans */
} UnderlineStyle;

/* A diagnostic note with its own span */
typedef struct DiagNote {
    DiagLevel level;
    Span span;
    const char *message;
} DiagNote;

/* A suggestion with optional replacement text */
typedef struct DiagSuggestion {
    const char *text;            /* Suggested fix text */
    const char *replacement;     /* Optional: text to replace with */
    const char *doc_url;        /* Optional: documentation URL */
} DiagSuggestion;

/* Snippet rendering options */
typedef struct SnippetOpts {
    bool show_line_numbers;
    uint32_t context_lines;    /* lines of context before/after */
    UnderlineStyle primary_style;
    UnderlineStyle secondary_style;
} SnippetOpts;

/* Default snippet options */
#define SNIPPET_OPTS_DEFAULT ((SnippetOpts){.show_line_numbers = true, .context_lines = DIAG_CONTEXT_LINES, .primary_style = UNDERLINE_PRIMARY, .secondary_style = UNDERLINE_SECONDARY})

/* Initialize diagnostics - call once at program start */
void diag_init(bool use_color);

/* Check if colors are enabled */
bool diag_use_color(void);

/* Check if stderr is a TTY (for auto-color) */
bool stderr_is_tty(void);

void diag_register_file(const SourceFile *file);
/* Record that `file_id` was pulled into the compile by the form at `origin`
 * (the `load` string or the `import` form, a span in the file that names it).
 * Call after diag_register_file, which clears it.  The LSP follows these
 * links up to the open document, so an error in a file loaded by a loaded
 * file is drawn on the document's own `load` line.  A span with line 0 (no
 * source form, e.g. a forced import) records nothing. */
void diag_set_file_origin(uint16_t file_id, Span origin);
/* A fresh file id for a SourceFile a pass reads on its own, outside the
 * elaborator's import/load counter (an R7RS `include`): handed out from the
 * top of the range downwards, so the two never meet. */
uint16_t diag_alloc_file_id(void);

/* Return the filesystem path registered for file_id, or NULL. */
const char *diag_file_path(uint16_t file_id);
/* The source-file registry's capacity: file ids run [0, DIAG_MAX_FILES).
 * Raised from a hard-coded 64 at r7rs-lang-plan R7, when import/load ids
 * stopped reusing the compiled driver's auto-loaded band (~40 files) and a
 * procedural macro's compile-time evaluation ran out of ids. */
#define DIAG_MAX_FILES 512
const SourceFile *diag_source_file(uint16_t file_id);
/* r7rs-lang-plan R7: the compiled driver records where its auto-loaded stdlib
 * file ids end, so import/load ids start past them instead of overwriting one. */
void     diag_note_autoload_file_ids(uint16_t end);
uint16_t diag_autoload_file_ids_end(void);

/* Translate a span into the coordinates of the file the user is editing.
 *
 * A sweet-exp file reaches the elaborator as transformed s-expression text,
 * so every Span on the tree indexes THAT buffer, not the one on disk.
 * Diagnostics have always translated back (render_snippet_ex); anything else
 * that reports a position -- the LSP's symbol and scope tables above all --
 * has to make the same trip, or an editor lands the caret on a byte that is
 * not where the user's name lives. A file with no transformation returns the
 * span unchanged, so callers can apply this unconditionally. */
Span diag_translate_span(Span span);

/* Core diagnostic emission */
void diag_emit(DiagLevel level, Span span, const char *fmt, ...) TUR_PRINTF_FMT(3, 4);
void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap)
    TUR_PRINTF_FMT(3, 0);

/* Enhanced diagnostics with code and notes (Phase 8) */
void diag_emit_with_code(DiagLevel level, Span span, DiagCode code, const char *fmt, ...)
    TUR_PRINTF_FMT(4, 5);
void diag_emit_with_notes(DiagLevel level, Span span, const char *message,
                          DiagNote *notes, size_t note_count);
void diag_emit_with_suggestion(DiagLevel level, Span span, const char *message,
                               const DiagSuggestion *suggestion);

/* Multi-span diagnostics for complex errors */
void diag_emit_multi_span(DiagLevel level, const char *message,
                         Span primary_span, const char *primary_label,
                         Span *secondary_spans, const char **secondary_labels,
                         size_t secondary_count);

bool diag_had_error(void);

/* Re-mark the error flag after a nested evaluation's diag_reset cleared it
 * (macro-time evaluation inside a compile; see diag.c). */
void diag_force_had_error(void);

/* Monotonic count of errors actually SHOWN to the user (captured/speculative
 * errors excluded).  Compare across a window to learn whether it surfaced an
 * error; never reset, so only differences are meaningful. */
uint64_t diag_error_serial(void);
void diag_reset(void);

/* Save / restore the registered-SourceFile table across a diag_reset().
 *
 * diag_reset() clears the file registry, which is right for a compiler driver
 * looping over files: the SourceFiles live in a per-file arena that is about to
 * go away, so keeping them registered would leave dangling pointers.
 *
 * It is wrong for the interpreter's incremental path.  There, turn N-1 splices
 * `(load ...)`ed files in and registers each one; turn N reuses the Forms it
 * already parsed, so those loads never re-run and the files are never
 * re-registered -- but the reused Forms still carry their file ids.  Every
 * later diag_file_path() on one of those ids misses, and everything downstream
 * that needs to know WHICH FILE a form came from degrades: the DAP debugger
 * reports a frame as `?:19` with no `source` object, and file-scoped
 * breakpoints stop matching.
 *
 * The interpreter's eval arenas are retained for the life of the env, so its
 * SourceFile pointers stay valid across turns and restoring them is sound.
 * A caller that frees per-turn arenas must NOT use these.
 *
 * `diag_files_save` writes up to `cap` entries and returns how many slots the
 * table holds (indices are file ids, and NULL slots are preserved so ids stay
 * stable).  `diag_files_restore` re-registers every non-NULL entry EXCEPT id 0,
 * which the caller has just re-registered with the current turn's blob.
 * See docs/archive/incremental-elab-loses-span-file-provenance.md. */
size_t diag_files_save(const SourceFile **out, size_t cap);
void   diag_files_restore(const SourceFile **in, size_t n);
/* Put the registry back EXACTLY as `diag_files_save` found it: every slot the
 * snapshot does not hold is cleared, not kept.  For a nested evaluation whose
 * own files live in arenas that can be freed before the next save -- the
 * macro-time env (src/turi/macro_env.c) -- so they cannot outlive it. */
void   diag_files_replace(const SourceFile **in, size_t n);
/* Slots the registry can hold, so callers can size their snapshot buffer. */
size_t diag_files_capacity(void);

/* Speculative-elaboration capture (bare-fat-result-monomorphization-plan).
 * While a capture frame is active, every diag_emit* call is suppressed
 * (nothing is rendered to stderr) and DIAG_ERROR emissions are counted into
 * the innermost frame.  diag_pop_capture() restores had_error_ to its value
 * at the matching push and returns how many errors were suppressed in the
 * frame.  This lets the elaborator *try* to elaborate a bare-^fat body at the
 * default int result kind, detect that it does not typecheck, and defer it to
 * per-call-site specialization -- without leaking spurious diagnostics. Frames
 * nest (bounded depth); a caller that wants the real diagnostics simply
 * re-runs the elaboration with no capture frame active. */
void     diag_push_capture(void);
uint32_t diag_pop_capture(void);

/* Snippet rendering */
void diag_render_snippet(const SourceFile *f, Span span, const SnippetOpts *opts);

/* Get error code string for display */
const char *diag_code_to_string(DiagCode code);

/* JSON diagnostics support (Phase 8) */
typedef struct JsonDiag {
    const char *severity;   /* "error", "warning", "note", "help" */
    const char *code;       /* error code like "TUR-E0001" */
    const char *message;
    const char *file;
    uint32_t line;
    uint32_t col;
    uint32_t end_line;
    uint32_t end_col;
} JsonDiag;

/* Enable/disable JSON output mode */
void diag_set_json_output(bool enabled);

/* Emit a diagnostic in JSON format */
void diag_emit_json(DiagLevel level, Span span, DiagCode code, const char *message);

/* LSP collection mode: buffer diagnostics for batch JSON output.
 * diag_lsp_begin resets the internal list and activates collection.
 * diag_lsp_flush writes {"diagnostics":[...]} (0-based line/col) to out.
 * diag_lsp_end discards the list and deactivates collection. */
void diag_lsp_begin(void);
void diag_lsp_flush(FILE *out);

/* Write just the diagnostics JSON array [...] into buf (no outer wrapper). */
void diag_lsp_flush_array(struct Buf *buf);

/* lsp-publishes-other-files-diagnostics-under-one-uri: where a diagnostic that
 * belongs to ANOTHER file should be drawn in the document being published.
 * Given the foreign file's path, set the 0-based anchor range in the document
 * (typically the `load` / `import` that pulled the file in) and that file's
 * `file://` URI, and set `*anchored` when an anchor was found.  Return false
 * to leave the URI out of the related location.  A file the document does not
 * name directly is retried through the origin chain (diag_set_file_origin):
 * first the document's `load` / `import` of the outermost file on the chain,
 * then that form's recorded span. */
typedef bool (*DiagLspRelocateFn)(void *ctx, const char *foreign_path,
                                  uint32_t *line0, uint32_t *col_start0,
                                  uint32_t *col_end0, bool *anchored,
                                  char *uri_out, size_t uri_cap);

/* As diag_lsp_flush_array, for a publish addressed to `doc_path`: an entry
 * from any other file is moved onto the document at the range `relocate`
 * gives, its message prefixed with the real `path:line:col`, and the real
 * location attached as LSP `relatedInformation`.  Every entry still carries
 * its `"file"` key. */
void diag_lsp_flush_array_for(struct Buf *buf, const char *doc_path,
                              DiagLspRelocateFn relocate, void *ctx);

void diag_lsp_end(void);

/* Replace all occurrences of `from_path` with `to_path` in buffered entries.
 * Call after diag_lsp_begin + compilation to fix up temp file paths. */
void diag_lsp_remap_path(const char *from_path, const char *to_path);

/* ---------------------------------------------------------------------------
 * Diagnostic sink (embed API -- libturi-per-embed-env-and-peripherals Gap 3)
 *
 * When a sink is installed, every diagnostic is delivered to it as a
 * structured single-line record BEFORE it would otherwise be rendered to
 * stderr, and the stderr render is suppressed.  This lets a libturi embedder
 * (e.g. the Godot binding) route each script's compile errors to its own
 * output channel attributed to that script, instead of a global stderr
 * firehose.  Pass fn == NULL to remove the sink and restore stderr rendering.
 *
 * `code` is the diagnostic's "TUR-E0001"-style string, or "" when none.
 * `file` is the registered source path, or "" when unknown.  `line`/`col_*`
 * are 1-based (0 when unavailable).  Multi-part diagnostics (notes,
 * suggestions, secondary spans) deliver the primary record first, then one
 * record per subordinate part at its own level.
 *
 * The sink coexists with the speculative-elaboration capture frames (a
 * captured error is still swallowed, not delivered).  LSP-collection and JSON
 * modes take precedence over the sink when active. */
typedef void (*DiagSinkFn)(DiagLevel level, const char *code, const char *file,
                           uint32_t line, uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud);

/* Install (or, with NULL, clear) the diagnostic sink. */
void diag_set_sink(DiagSinkFn fn, void *ud);

/* Return the currently-installed sink (NULL if none) and, when out_ud is
 * non-NULL, its user-data pointer.  Lets a caller save/restore the sink. */
DiagSinkFn diag_get_sink(void **out_ud);

/* Phase HKT-P5: Look up a long-form explanation for a diagnostic code.
 * If an explanation exists, writes it to `out` and returns true.
 * If no explanation is registered for `code`, returns false without writing.
 * `code_str` must be a string like "TUR-E0012" (as returned by
 * diag_code_to_string); `code` is the corresponding DiagCode enum value. */
bool diag_explain(DiagCode code, FILE *out);

/* Phase HKT-P5: Parse a TUR-E#### string into its DiagCode.
 * Returns DIAG_CODE_NONE if the string is not recognised. */
DiagCode diag_code_from_string(const char *s);

/* Phase HKT-P5: Return true if `s` looks like a diagnostic code string
 * of the form "TUR-E" (or W or D) followed by one or more decimal digits. */
bool diag_looks_like_code(const char *s);

#endif
