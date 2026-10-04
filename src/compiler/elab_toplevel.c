/* elab_toplevel.c -- top-level form dispatch and the elaborate_program entry point. */
#include "elab_internal.h"
#include "platform_fs.h"  /* realpath() on Windows */
#include "globals.h"
#include "mangle.h"   /* tur_cname_name_len */
#include "refine_discharge.h" /* RT3: final refinement discharge + stats */
#include "refine_report.h"    /* SX8a-3: --dump-refine=json obligation dump */
#include "lang_dialects.h"      /* saffron-lang-plan S6 (G7): lang_span_is_dynamic */
#include "scheme_lower.h"       /* r7rs-lang-plan R2: the Scheme core forms */

/* duplicate-ctor-names-collide-in-emitted-c: the constructor-name census lives
 * in emit_core.c; declared here because elab_toplevel.c does not include
 * emit_internal.h. */
void ctor_census_snapshot(struct AdtDef *const *defs, uint32_t n_defs);

Expr *elab_as_cast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'as' requires exactly two arguments: (as type expr)");
        return NULL;
    }
    Form *type_form = call->as.list.items[1];
    Form *expr_form = call->as.list.items[2];

    if (type_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'as' expects a type name as first argument");
        return NULL;
    }
    TypeKind target_kind = typekind_from_symbol(type_form->as.sym->name);
    if (target_kind == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "unknown type '%s' in 'as' cast", type_form->as.sym->name);
        return NULL;
    }
    if (!typekind_is_numeric(target_kind)) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'as' casts are only valid for numeric types");
        return NULL;
    }

    Expr *inner = elab_form(e, expr_form);
    if (!inner) return NULL;

    if (!typekind_is_numeric(inner->type.kind)) {
        diag_emit(DIAG_ERROR, expr_form->span,
                  "cannot cast non-numeric type '%s' with 'as'",
                  type_name(inner->type));
        return NULL;
    }

    /* No-op cast: same type */
    if (inner->type.kind == target_kind) return inner;

    Type result_type = type_simple(target_kind, CK_COPY);
    Expr *out = expr_new(e->arena, EX_CAST, result_type, call->span);
    out->as.cast_.expr = inner;
    out->as.cast_.target_kind = target_kind;
    return out;
}

/* IT4: (type-of x) — return the cstr type name of an any-typed value. */
Expr *elab_any_type_of(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "'type-of' requires exactly one argument: (type-of x)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'type-of' expects an 'any'-typed argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Type cstr_t = type_simple(TY_CSTR, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_TYPE_OF, cstr_t, call->span);
    out->as.any_type_of_.value = val;
    return out;
}

/* any-narrowing-broken-for-parametric-receivers: resolve the type target of
 * `is?` / `cast` into (kind, Type).  Shared so the two forms cannot drift --
 * they must agree on the target, because `is?` guards a narrowing that `cast`
 * then has to accept.
 *
 * The `any` box id is interned by `type_name`, which renders a TY_APP PER
 * INSTANTIATION ("(type-app Option float)") so that `(Box int)` and
 * `(Box float)` stay distinct.  A target written as a bare `Option` resolves to
 * the head ADT instead -- a different key, hence a different id -- so the test
 * compared against an id no widen ever mints.  `(is? x Option)` was silently
 * false and `(cast x Option)` panicked "any holds Option, not Option", both
 * displaying the same name because `shown` is the head name either way.
 *
 * Two changes close that.  A parenthesised target `(Option float)` now goes
 * through the shared annotation parser, so the resolved Type is the SAME TY_APP
 * the widen site interned and the ids line up.  A bare type constructor is a
 * hard error naming the arity, because it does not identify one runtime type --
 * an error the caller can act on, where the silent `false` was not.
 *
 * Returns false having emitted a diagnostic. */
static bool any_narrow_target(Elab *e, const Form *type_form, const char *who,
                              TypeKind *out_kind, Type *out_type) {
    /* A parenthesised type application, or any other compound annotation. */
    if (type_form->tag != F_SYM) {
        Type *t = fn_type_from_form(e, type_form, NULL, NULL, 0);
        if (!t) {
            /* fn_type_from_form has already reported what it could not read;
             * add the context it does not have. */
            diag_emit(DIAG_ERROR, type_form->span,
                      "'%s' could not resolve its type argument", who);
            return false;
        }
        /* any-cannot-recover-a-capturing-closure: a function TARGET names the
         * fat representation, because that is the only one an `any` holds --
         * `elab_coerce_to_any` shims a bare fn to fat at the widen.  Without
         * this the target's box id would be the bare one and no fn payload would
         * ever match.  `(-> int int)` reads as "a function of this signature";
         * whether the value carries an environment is a representation detail the
         * source spelling does not, and should not, express. */
        if (t->kind == TY_FN && !t->as.fn.cfnptr) t->as.fn.boxed = true;
        *out_type = *t;
        *out_kind = any_box_tag_for_type(t);
        return true;
    }

    TypeKind k = typekind_from_symbol(type_form->as.sym->name);
    if (k != TY_UNKNOWN) {
        *out_kind = k;
        *out_type = type_simple(k, CK_COPY);
        return true;
    }

    Type *named = elab_lookup_type_by_name(e, type_form->as.sym);
    if (!named) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "unknown type '%s' in '%s'", type_form->as.sym->name, who);
        return false;
    }

    /* A bare type CONSTRUCTOR names no single runtime type: an `any` holds one
     * instantiation, and `Option` does not say which.  Reject rather than test
     * against the head's own id, which nothing ever boxes. */
    if (named->kind == TY_ADT && named->as.adt_.def &&
        named->as.adt_.def->n_type_params > 0) {
        const AdtDef *d = named->as.adt_.def;
        diag_emit(DIAG_ERROR, type_form->span,
                  "'%s' is a type constructor taking %u type parameter%s, so it "
                  "does not name one runtime type; write it applied, e.g. "
                  "'(%s %s)'",
                  d->name, (unsigned)d->n_type_params,
                  d->n_type_params == 1 ? "" : "s", d->name,
                  (d->type_params && d->type_params[0]) ? d->type_params[0] : "T");
        diag_emit(DIAG_HELP, type_form->span,
                  "matching every instantiation of '%s' at once is not supported "
                  "yet -- test the one you expect", d->name);
        return false;
    }

    /* CONV-S1: a struct-origin lowered ADT tests/casts under its box tag, so
     * the surface stays transparent to the defstruct-as-defadt lowering. */
    *out_kind = any_box_tag_for_type(named);
    *out_type = *named;
    return true;
}

/* IT4/TY2.3: (cast x T) — checked downcast from any.  Verifies the runtime box
 * tag matches T's TypeKind and panics on mismatch (see __tur_any_cast_check),
 * then returns the inner value as T.  T may be a primitive type name, a
 * struct/ADT name (TY2.2 heap-boxed payloads unbox by dereference), or an
 * applied type constructor like `(Option float)`. */
Expr *elab_any_cast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'cast' requires exactly two arguments: (cast x T)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'cast' expects an 'any'-typed first argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Form *type_form = call->as.list.items[2];
    TypeKind target_kind;
    Type result_type;
    if (!any_narrow_target(e, type_form, "cast", &target_kind, &result_type))
        return NULL;
    (void)target_kind;   /* elab_any_unbox_to recomputes it from result_type */
    return elab_any_unbox_to(e, val, result_type, call->span);
}

/* typeclass-dispatch-on-any-receiver-emits-uncompilable-c: the checked unbox,
 * reachable from a Type rather than a source form.
 *
 * `(cast x T)` is the surface spelling; this is the same node, for callers that
 * have already resolved the target and need to bridge an `any` into a slot
 * typed by it.  The `@TypeName` witness is the one such caller: it names the
 * instance, which is exactly the information the unbox needs, so pinning an
 * instance and unboxing the receiver for it are one act.  Routing both through
 * here is what keeps the tag check on the witness path -- a wrong witness
 * panics with the ordinary `cast: any holds ...` message rather than
 * reinterpreting the payload. */
/* r7rs-lang-plan T3: the string half of the Scheme seam, in both directions.
 * A Scheme string is a `cstr` (a literal) or an `R7rsString` (one a procedure
 * newly allocated: mutable code points).  Wherever an `any` is unboxed to a
 * `cstr` -- a Scheme call passing a string to a Turmeric `cstr` parameter, or
 * Turmeric code `cast`ing a Scheme library's result -- it goes through the
 * prelude's `r7rs-str__`, which passes a cstr through, encodes an R7rsString
 * as a fresh UTF-8 copy, and is the ordinary checked cast for anything else.
 * So the Turmeric side always holds an immutable cstr, and Turmeric's `cstr`
 * is unchanged.  Gated on the Scheme prelude being in the program (no
 * `r7rs-str__` binding, no change), and never inside stdlib/r7rs/ itself,
 * whose own unboxes are the plain cast (r7rs-str__'s among them). */
static bool span_in_r7rs_stdlib(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    return f && f->path && strstr(f->path, "stdlib/r7rs/") != NULL;
}
static Expr *r7rs_string_unbox(Elab *e, Expr *val, Span span) {
    if (span_in_r7rs_stdlib(span) || span_in_r7rs_stdlib(val->span)) return NULL;
    const Symbol *nm = symtab_intern(e->st, strslice("r7rs-str__", 10));
    bool qual_err = false;
    Binding *b = elab_lookup_sym(e, nm, span, &qual_err);
    if (!b || b->type.kind != TY_FN) return NULL;
    Expr **cargs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
    cargs[0] = val;
    Expr *call = expr_new(e->arena, EX_CALL, type_simple(TY_CSTR, CK_COPY), span);
    call->as.call_.fn_binding = b;
    call->as.call_.args = cargs;
    call->as.call_.n_args = 1;
    call->as.call_.fn_expr = NULL;
    return call;
}

/* r7rs-type-errors-are-uncatchable-panics: the target of a failed cast, in
 * the words a Scheme error message uses ("car: not a pair").  NULL for a
 * target that has no Scheme name; the message then names the Turmeric type. */
static const char *scheme_type_desc(Type t) {
    switch (t.kind) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return "an exact integer";
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64: return "an inexact real";
        case TY_BOOL:  return "a boolean";
        case TY_CSTR:  return "a string";
        case TY_SYM:   return "a symbol";
        case TY_FN:    return "a procedure";
        default: break;
    }
    static const char *const names[][2] = {
        { "R7rsPair", "a pair" },             { "R7rsNull", "the empty list" },
        { "R7rsString", "a string" },         { "R7rsChar", "a character" },
        { "R7rsBytevector", "a bytevector" }, { "R7rsError", "an error object" },
        { "R7rsParam", "a parameter object" },{ "R7rsPromise", "a promise" },
        { "R7rsPort", "a port" },             { "R7rsEof", "the eof object" },
        { "R7rsRatio", "an exact rational" }, { "R7rsBig", "an exact integer" },
        { "R7rsComplex", "a complex number" },{ "R7rsValues", "multiple values" },
        { "R7rsEnvironment", "an environment" },
    };
    const char *n = type_name(t);
    if (!n) return NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(n, names[i][0]) == 0) return names[i][1];
    if (strstr(n, "Vec") != NULL) return "a vector";
    return NULL;
}

Expr *elab_any_unbox_to(Elab *e, Expr *val, Type target, Span span) {
    if (target.kind == TY_CSTR) {
        Expr *conv = r7rs_string_unbox(e, val, span);
        if (conv) return conv;
    }
    Expr *out = expr_new(e->arena, EX_ANY_CAST, target, span);
    out->as.any_cast_.value = val;
    out->as.any_cast_.target_kind = any_box_tag_for_type(&target);
    /* r7rs-type-errors-are-uncatchable-panics: in Scheme source a failed
     * cast is an R7RS error, raised; elsewhere it stays the panic. */
    if (lang_span_is_scheme(span) || lang_span_is_scheme(val->span)) {
        out->as.any_cast_.scheme_raise = true;
        out->as.any_cast_.scheme_want = scheme_type_desc(target);
        if (!out->as.any_cast_.scheme_want) {
            const char *n = type_name(target);
            if (n) {
                size_t len = strlen(n) + 4;
                char *w = (char *)arena_alloc(e->arena, len);
                snprintf(w, len, "a %s", n);
                out->as.any_cast_.scheme_want = w;
            }
        }
    }
    return out;
}

/* r7rs-type-errors-are-uncatchable-panics: name the procedure a raising cast
 * guards, as Scheme spells it -- `r7rs-car` is `car`.  A prelude-internal
 * callee (`r7rs-foo__`) names nothing; the message then says only what was
 * expected. */
void elab_any_cast_note_callee(Elab *e, Expr *cast, const Binding *callee) {
    if (!cast || cast->kind != EX_ANY_CAST || !cast->as.any_cast_.scheme_raise ||
        !callee || !callee->name)
        return;
    const char *n = callee->name->name;
    size_t len = strlen(n);
    if (len >= 2 && n[len - 2] == '_' && n[len - 1] == '_') return;
    char nbuf[256];
    const char *pub = scheme_public_name(n);
    if (!pub && strncmp(n, "r7rs-", 5) == 0) pub = n + 5;
    if (!pub) pub = scheme_source_name(n, nbuf, sizeof nbuf);
    size_t pl = strlen(pub) + 1;
    char *w = (char *)arena_alloc(e->arena, pl);
    memcpy(w, pub, pl);
    cast->as.any_cast_.scheme_who = w;
}

/* TY3: (is? x T) — runtime type test on an `any`-typed value.  Returns bool:
 * true iff x's stored box tag is T's TypeKind.  T may be a primitive, struct,
 * or ADT name.  Used directly, or as an `if` guard that narrows x to T. */
Expr *elab_is_q(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'is?' requires exactly two arguments: (is? x T)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'is?' expects an 'any'-typed first argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Form *type_form = call->as.list.items[2];
    /* type-of-cast-kind-granularity: the resolved Type is kept alongside the
     * kind -- emit turns it into the same per-monomorph box id the inject site
     * allocates, so `(is? a OtherStruct)` on an `any` holding a Point is false
     * rather than true-for-every-struct. */
    TypeKind test_kind;
    Type test_type = type_simple(TY_UNKNOWN, CK_COPY);
    if (!any_narrow_target(e, type_form, "is?", &test_kind, &test_type))
        return NULL;
    Type bool_t = type_simple(TY_BOOL, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_IS, bool_t, call->span);
    out->as.any_is_.value = val;
    out->as.any_is_.test_tag = (int64_t)test_kind;
    out->as.any_is_.test_type = test_type;
    return out;
}

/* DL1: build a synthetic (head arg0 arg1 ...) call form for a data literal. */
static Form *dl_build_call(Elab *e, Span span, const char *head,
                           Form **items, uint32_t n) {
    const Symbol *h = symtab_intern(e->st, strslice(head, (uint32_t)strlen(head)));
    Form **call_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
    call_items[0] = form_sym(e->arena, span, h);
    for (uint32_t i = 0; i < n; i++) call_items[i + 1] = items[i];
    return form_list(e->arena, span, call_items, n + 1);
}

/* saffron-lang-plan S6 (G7): wrap one data-literal element in `(:: elem any)`.
 *
 * In a Saffron file a container's element type is `any` -- that is what makes
 * `[1 "two" 7.1]` a vector rather than a type error.  `vec-of` (and `hamt-of`,
 * and `set-of`) is HOMOGENEOUS by construction: it routes every element through
 * `tur-vec-homog__`, so element 2 above is rejected against element 1's type
 * ("function 'vec-push!' arg 2: expected tyvar, got cstr").  Widening each
 * element at the LITERAL, before the macro sees it, keeps that homogeneity
 * check intact and satisfies it at `any` -- rather than teaching the macro a
 * dialect-dependent second mode.
 *
 * A uniform `any` also costs a box on `[1 2 3]`, which is the right trade for a
 * dialect where a vector's element type is not fixed: pushing a string into it
 * later is ordinary, and a `(Vec int)` that silently became one would be a
 * surprise no diagnostic covers.
 *
 * An element already typed `any` re-ascribes to `any`, which is identity. */
static Form *dl_saffron_widen_elem(Elab *e, Form *elem) {
    const Symbol *any_sym = symtab_intern(e->st, strslice("any", 3));
    Form *ty = form_sym(e->arena, elem->span, any_sym);
    Form *items[2] = { elem, ty };
    return dl_build_call(e, elem->span, "::", items, 2);
}

/* saffron-applied-class-var-result-takes-one-instances-type (found on the
 * way): does the enclosing expectation PIN the literal's instantiation -- an
 * application with an argument other than `any`, as a declared `: (Vec float)`
 * return or a `(:: [..] (Vec float))` ascription gives?  Then the literal
 * builds that instantiation, exactly as a generic constructor CALL defers to
 * the same expectation (saffron_expected_app_pins, elab_call.c).  Widening
 * anyway built a `(Vec any)` while the expectation still typed the result
 * `(Vec float)`, so `(defn f [x : float] : (Vec float) [x x])` handed its
 * caller the boxes' words as doubles -- `(vec-get (f 7.1) 1)` printed
 * 4.6e-310, compiled, even under an explicit ascription. */
static bool dl_saffron_expected_pins(const Elab *e) {
    const Type *t = e->expected_type;
    if (!t || t->kind != TY_APP) return false;
    for (const Type *cur = t; cur && cur->kind == TY_APP; cur = cur->as.app.fn)
        if (!cur->as.app.arg || cur->as.app.arg->kind != TY_ANY) return true;
    return false;
}

/* Widen every element of a data literal, or return `items` unchanged when the
 * literal is not in a Saffron file, or when an enclosing expectation pins its
 * instantiation (dl_saffron_expected_pins). */
static Form **dl_saffron_widen_elems(Elab *e, Span sp, Form **items, uint32_t n) {
    if (n == 0 || !lang_span_is_dynamic(sp) || dl_saffron_expected_pins(e)) return items;
    Form **out = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) out[i] = dl_saffron_widen_elem(e, items[i]);
    return out;
}

/* DL1: normalize a #map{...} key form to the int key the typed Map expects.
 * Int keys pass through; keyword/string keys lower to (hamt/hash-str "name")
 * so equal keyword/string keys hash identically (content equality).  The
 * reader (TUR-E0282) guarantees the key is one of these three forms. */
static Form *dl_normalize_map_key(Elab *e, Form *key) {
    if (key->tag == F_INT) return key;
    /* SYM3 (runtime-symbols-plan): a keyword key is a first-class :Sym value
     * -- pass the F_KEYWORD through so the map is keyed by Sym
     * (pointer-identity, via Hash[Sym] / MapKey[Sym]) instead of decaying to a
     * content-hashed cstr. */
    if (key->tag == F_KEYWORD) return key;
    Form *str;
    if (key->tag == F_KEYWORD) {
        str = form_str(e->arena, key->span, key->as.sym->name,
                       (uint32_t)key->as.sym->len);
    } else { /* F_STR */
        str = form_str(e->arena, key->span, key->as.s.p, key->as.s.len);
    }
    Form *items[1] = { str };
    return dl_build_call(e, key->span, "hamt/hash-str", items, 1);
}

/* inline-c-cname-module-prefix-plan (Option A): resolve __TUR_CNAME_<name>__
 * splices in an inline-C body at elaboration time.
 *
 * The bare __TUR_CNAME_ emit-time path is mangle-only -- it reproduces the
 * name-mangling scheme but cannot reproduce the *module prefix* a global
 * defined inside a named module carries in its C name (raw_name_for_binding).
 * To make the splice resolve to the callee's exact emitted C name (prefix +
 * any (export-as ...) alias), we resolve each name here through the same
 * elab_lookup_sym path ordinary calls use, attach the resolved Binding to the
 * node's captures[], and rewrite the placeholder to the corresponding
 * __TUR_CAP_N__ -- which the emitter already lowers via name_for_binding
 * (prefix + alias for free).
 *
 * Names that do NOT resolve in scope are left as __TUR_CNAME_<name>__ verbatim
 * and fall through to the emit-time mangle-only path. This preserves the
 * established escape hatch for referencing an unprefixed global in another
 * translation unit that the current module does not import (e.g. stdlib
 * carrier helpers referenced across files without an explicit import).
 *
 * Returns the (possibly rewritten) code slice; on resolution it allocates the
 * captures array from the arena and writes it back through the out params. */
static StrSlice elab_cblock_resolve_cnames(Elab *e, StrSlice code, Span span,
                                           Binding ***out_caps, uint8_t *out_n) {
    *out_caps = NULL;
    *out_n = 0;
    const char *src = code.p;
    uint32_t len = code.len;
    if (!src || len < 14) return code;

    /* First pass: does the body contain any resolvable __TUR_CNAME_ splice?
     * If not, return the slice untouched (no copy, no captures). */
    Binding *caps[255];
    uint8_t n_caps = 0;
    bool any_rewrite = false;
    for (uint32_t i = 0; i + 14 <= len; ) {
        if (memcmp(src + i, "__TUR_CNAME_", 12) != 0) { i++; continue; }
        uint32_t name_start = i + 12;
        /* tur_cname_name_len handles a <name> that itself begins with `__`. */
        uint32_t name_len = tur_cname_name_len(src, len, name_start);
        if (name_len == 0) { i = name_start; continue; }
        uint32_t j = name_start + name_len;
        const Symbol *sym = symtab_intern(e->st, strslice(src + name_start, name_len));
        bool qual_err = false;
        Binding *b = elab_lookup_sym(e, sym, span, &qual_err);
        if (b && n_caps < 255) {
            /* Dedup so a name used N times shares one capture slot. */
            bool seen = false;
            for (uint8_t k = 0; k < n_caps; k++) {
                if (caps[k] == b) { seen = true; break; }
            }
            if (!seen) caps[n_caps++] = b;
            any_rewrite = true;
        }
        i = j + 2;
    }
    if (!any_rewrite) return code;

    /* Second pass: rebuild the body, substituting resolvable splices with
     * their capture placeholder and leaving the rest byte-for-byte. */
    Buf out; buf_init(&out);
    for (uint32_t i = 0; i < len; ) {
        if (i + 14 <= len && memcmp(src + i, "__TUR_CNAME_", 12) == 0) {
            uint32_t name_start = i + 12;
            uint32_t name_len = tur_cname_name_len(src, len, name_start);
            if (name_len > 0) {
                uint32_t j = name_start + name_len;
                const Symbol *sym = symtab_intern(e->st,
                                                  strslice(src + name_start, name_len));
                bool qual_err = false;
                Binding *b = elab_lookup_sym(e, sym, span, &qual_err);
                int cap_idx = -1;
                if (b) {
                    for (uint8_t k = 0; k < n_caps; k++) {
                        if (caps[k] == b) { cap_idx = k; break; }
                    }
                }
                if (cap_idx >= 0) {
                    buf_printf(&out, "__TUR_CAP_%d__", cap_idx);
                    i = j + 2;
                    continue;
                }
                /* Unresolved: copy the splice verbatim for the emit-time
                 * mangle-only fallback. */
                buf_write(&out, src + i, (size_t)(j + 2 - i));
                i = j + 2;
                continue;
            }
        }
        buf_putc(&out, src[i++]);
    }

    char *copied = arena_strdup(e->arena, out.data, out.len);
    uint32_t new_len = (uint32_t)out.len;
    buf_free(&out);

    Binding **caps_arena = (Binding **)arena_alloc(e->arena,
                                                   n_caps * sizeof(Binding *));
    for (uint8_t k = 0; k < n_caps; k++) caps_arena[k] = caps[k];
    *out_caps = caps_arena;
    *out_n = n_caps;
    return strslice(copied, new_len);
}

Expr *elab_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:  return e_nil(e, f->span);
        case F_BOOL: {
            Expr *out = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, f->span);
            out->as.b = f->as.b;
            return out;
        }
        case F_INT: {
            /* Phase N: dispatch to fixed-width type based on suffix */
            Type lit_type;
            switch (f->lit_suffix) {
                case LIT_SUF_I8:  lit_type = TYPE_INT8;    break;
                case LIT_SUF_I16: lit_type = TYPE_INT16;   break;
                case LIT_SUF_I32: lit_type = TYPE_INT32;   break;
                case LIT_SUF_I64: lit_type = TYPE_INT64;   break;
                case LIT_SUF_U8:  lit_type = TYPE_UINT8;   break;
                case LIT_SUF_U16: lit_type = TYPE_UINT16;  break;
                case LIT_SUF_U32: lit_type = TYPE_UINT32;  break;
                case LIT_SUF_U64: lit_type = TYPE_UINT64;  break;
                case LIT_SUF_F32: lit_type = TYPE_FLOAT32; break;
                case LIT_SUF_F64: lit_type = TYPE_FLOAT64; break;
                default:          lit_type = TYPE_INT;     break;
            }
            bool is_float_suffix = (f->lit_suffix == LIT_SUF_F32 || f->lit_suffix == LIT_SUF_F64);
            ExprKind ek = is_float_suffix ? EX_FLOAT_LIT : EX_INT_LIT;
            Expr *out = expr_new(e->arena, ek, lit_type, f->span);
            if (is_float_suffix) out->as.f = (double)f->as.i;
            else                 out->as.i = f->as.i;
            return out;
        }
        case F_FLOAT: {
            /* Phase N: dispatch float suffix */
            Type lit_type;
            switch (f->lit_suffix) {
                case LIT_SUF_F32: lit_type = TYPE_FLOAT32; break;
                case LIT_SUF_F64: lit_type = TYPE_FLOAT64; break;
                default:          lit_type = TYPE_FLOAT;   break;
            }
            Expr *out = expr_new(e->arena, EX_FLOAT_LIT, lit_type, f->span);
            out->as.f = f->as.f;
            return out;
        }
        case F_STR: {
            Expr *out = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, f->span);
            out->as.s = f->as.s;
            return out;
        }
        case F_KEYWORD:
            /* SYM0 (runtime-symbols-plan): a keyword in expression position is a
             * first-class :Sym literal whose runtime value is a
             * pointer-identity-equal interned symbol.  (Its other uses are
             * syntactic: type annotations, :refer/:as, struct-field selectors,
             * ADT tags -- all consumed by earlier passes before elab_form.) */
            {
                Expr *out = expr_new(e->arena, EX_SYM_LIT, TYPE_SYM, f->span);
                out->as.sym_lit_.sym = f->as.sym;
                return out;
            }
        case F_SYM: {
            /* M1: Use elab_lookup_sym for visibility + qualified name resolution */
            bool sym_qual_err = false;
            Binding *b = elab_lookup_sym(e, f->as.sym, f->span, &sym_qual_err);
            if (!b) {
                if (sym_qual_err) return NULL; /* error already emitted */
                /* r7rs-leading-colon-identifiers: in a Scheme file `:k` is an
                 * R7RS identifier, not Turmeric's keyword.  Code written
                 * against the keyword (a map key passed through the seam)
                 * lands here; name the spelling that means what it meant. */
                if (f->as.sym->name[0] == ':' && f->as.sym->len > 1 && f->as.sym->name[1] != ':' &&
                    lang_span_is_scheme(f->span)) {
                    char msg[256], sug_text[256];
                    snprintf(msg, sizeof(msg), "unbound symbol '%s'", f->as.sym->name);
                    snprintf(sug_text, sizeof(sug_text),
                             "in #lang r7rs '%s' is an identifier, not a Turmeric keyword; "
                             "for the keyword's value (a map key, say) write the symbol '%s",
                             f->as.sym->name, f->as.sym->name + 1);
                    DiagSuggestion sug = { sug_text, NULL, "https://turmeric-lang.dev/docs/errors/TUR-E0003" };
                    diag_emit_with_suggestion(DIAG_ERROR, f->span, msg, &sug);
                    return NULL;
                }
                /* Phase 8: Enhanced unbound symbol diagnostic with suggestions */
                /* r7rs-turmeric-syntax-leaks: in user Scheme source, name the
                 * identifier as the program wrote it (the lowering respells
                 * some -- `nil` is `nil--user`), and suggest only what the
                 * program could name: not the stdlib's globals, nor the
                 * prelude's internal `r7rs-` spellings. */
                bool scheme_user = scheme_span_is_user_source(f->span);
                char shown_buf[256];
                const char *shown = scheme_user
                    ? scheme_source_name(f->as.sym->name, shown_buf, sizeof shown_buf)
                    : f->as.sym->name;
                const Symbol *best_match = NULL;
                int best_distance = 3;
                for (Scope *cur = e->scope; cur; cur = cur->parent) {
                    for (uint32_t i = 0; i < cur->n; i++) {
                        Binding *candidate = cur->bindings[i];
                        if (scheme_user && (candidate->is_from_stdlib ||
                                            elab_file_is_stdlib(candidate->span.file_id) ||
                                            strncmp(candidate->name->name, "r7rs-", 5) == 0 ||
                                            strncmp(candidate->name->name, "__", 2) == 0))
                            continue;
                        int dist = sym_levenshtein_distance(f->as.sym, candidate->name);
                        if (dist > 0 && dist < best_distance) {
                            best_distance = dist;
                            best_match = candidate->name;
                        }
                    }
                }
                if (best_match) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "unbound symbol '%s'", shown);
                    char sug_text[128];
                    snprintf(sug_text, sizeof(sug_text), "Did you mean '%s'?", best_match->name);
                    DiagSuggestion sug = {
                        sug_text,
                        NULL,
                        "https://turmeric-lang.dev/docs/errors/TUR-E0003"
                    };
                    diag_emit_with_suggestion(DIAG_ERROR, f->span, msg, &sug);
                } else {
                    diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0003_UNBOUND_SYMBOL,
                                        "unbound symbol '%s'", shown);
                }
                return NULL;
            }
            /* compiled-closure-copies-a-captured-mut: a `^mut` moved into a
             * shared heap cell is read through the cell. */
            if (b->cell_hidden_sym)
                return elab_form(e, elab_mut_cell_read_form(e, b, f->span));
            /* UT1: Check for use-after-consume of a unique binding (more specific than generic move) */
            if (b->is_unique && b->is_moved) {
                diag_emit_with_code(DIAG_ERROR, f->span,
                                    TUR_E0201_UNIQUE_COPY,
                                    "cannot copy unique value '%s' -- "
                                    "unique values may be used at most once",
                                    b->name->name);
                return NULL;
            }
            /* Phase 11: Check for use-after-move */
            if (!binding_check_not_moved(b, f->span, "binding")) {
                return NULL;
            }
            /* LT1: Check for use-after-consume of a linear binding */
            if (b->is_linear) {
                if (b->is_linear_consumed) {
                    diag_emit_with_code(DIAG_ERROR, f->span,
                                        TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                                        "linear value '%s' used after being consumed",
                                        b->name->name);
                    return NULL;
                }
                /* Mark linear binding as consumed on use */
                b->is_linear_consumed = true;
            }
            /* ST1: Substructural usage tracking.
             * ^affine: may not be used more than once (TUR_E0150).
             * ^relevant: must be used at least once; duplicates are fine. */
            if (b->is_affine && !b->is_continuation) {
                /* Continuation affine checks are handled by cont_check_double_use
                 * (LC2) to emit continuation-specific error codes instead of E0150. */
                if (b->usage_state >= USAGE_USED_ONCE) {
                    diag_emit_with_code(DIAG_ERROR, f->span,
                                        TUR_E0150_AFFINE_USED_TWICE,
                                        "affine value '%s' used more than once",
                                        b->name->name);
                    return NULL;
                }
                b->usage_state = USAGE_USED_ONCE;
            } else if (b->is_relevant && !b->is_continuation) {
                /* Continuation relevant checks are handled by the LC2 drop check. */
                b->usage_state = (b->usage_state == USAGE_UNUSED) ? USAGE_USED_ONCE
                                                                   : USAGE_USED_MANY;
            }
            /* DV1: If the binding is a dynamic var, emit a dynvar-read node.
             * The result type is the var's value type, not TY_DYNVAR. */
            if (b->is_dynvar && b->dynvar_entry) {
                Expr *out = expr_new(e->arena, EX_DYNVAR_READ,
                                     b->dynvar_entry->value_type, f->span);
                out->as.dynvar_read_.entry = b->dynvar_entry;
                return out;
            }
            Expr *out = expr_new(e->arena, EX_VAR, b->type, f->span);
            out->as.var.binding = b;
            /* Phase HRT/G2: Resolve named type variable through current GADT match arm skolem env.
             * If a binding was declared as :a (TY_TYVAR with name "a"), and we are currently
             * inside a GADT match arm that has a skolem binding for "a", use the concrete type. */
            if (b->type.kind == TY_TYVAR && b->type.as.tyvar_.name != NULL && e->g2_skolem_env) {
                TypeKind resolved = gadt_skolem_lookup(e->g2_skolem_env, b->type.as.tyvar_.name);
                if (resolved != TY_UNKNOWN) {
                    out->type = type_from_kind(resolved);
                    out->type.copy_kind = typekind_default_copy_kind(resolved);
                }
            }
            return out;
        }
        case F_VEC:
            /* DL1: in expression position (i.e. not consumed structurally by a
             * binding form), a [...] vector lowers to (vec-of ...).  Binding
             * forms (defn/fn/let/loop/...) grab their F_VEC slot before it ever
             * reaches elab_form, so reaching here means expression position. */
            {
                /* saffron-lang-plan S6 (G7): in a Saffron file the elements are
                 * widened to `any` first, so `[1 "two" 7.1]` is a `(Vec any)`
                 * of three boxes rather than a homogeneity error. */
                Form **items = dl_saffron_widen_elems(e, f->span, f->as.list.items,
                                                      f->as.list.len);
                Form *call = dl_build_call(e, f->span, "vec-of",
                                           items, f->as.list.len);
                return elab_form(e, call);
            }
        case F_MAP:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: map literals are parsed but not yet supported by elaboration");
            return NULL;
        /* DL1: data literals lower to their stdlib constructor macros. */
        case F_MAP_LITERAL: {
            uint32_t n = f->as.list.len; /* even -- validated by reader */
            /* TMS3 (typed-map-surface-plan): every #map{...} literal lowers to
             * the single typed hamt-of builder, which dispatches by (Hash K) +
             * (MapKey K).  String keys pass through raw -- map-assoc hashes and
             * compares them by content via Hash[cstr]/MapKey[cstr], so distinct
             * pointers with equal text collapse to one key (no smap-of split).
             * Keyword keys are still hash-normalized to an int; int keys pass
             * through, zero overhead. */
            bool all_str_keys = (n > 0);
            for (uint32_t i = 0; i + 1 < n; i += 2) {
                if (f->as.list.items[i]->tag != F_STR) { all_str_keys = false; break; }
            }
            bool saffron = lang_span_is_dynamic(f->span) && !dl_saffron_expected_pins(e);
            Form **kvs = (n == 0) ? NULL
                : (Form **)arena_alloc(e->arena, n * sizeof(Form *));
            for (uint32_t i = 0; i + 1 < n; i += 2) {
                /* String keys stay raw (content-keyed by map-assoc); other key
                 * literals (keywords) are hash-normalized to their int key.
                 *
                 * saffron-open-generic-result-not-grounded: in a dynamic file
                 * the KEYS widen too, raw, so every Saffron map is the one
                 * instantiation the dialect builds everywhere else --
                 * `(Map any any)`, which is also what `(map-new)` gives.  With
                 * the keys left typed, `#map{"a" 1}` was a `(Map cstr any)`,
                 * and the seam at every map accessor behind an `any` -- which
                 * can only ground the open key to `any` -- checked it against
                 * `(Map any any)` and panicked: `(map-count (mk))` for a
                 * `(defn mk [] #map{"a" 1})`.  Raw rather than normalized: a
                 * mixed literal `#map{"a" 1 :b 2}` keeps its string key a
                 * string (Hash[any] / MapKey[any] key each by its payload), so
                 * `(map-get m "a")` finds it. */
                kvs[i]     = saffron ? dl_saffron_widen_elem(e, f->as.list.items[i])
                           : all_str_keys ? f->as.list.items[i]
                                          : dl_normalize_map_key(e, f->as.list.items[i]);
                /* saffron-lang-plan S6 (G7): in a Saffron file a map's VALUES
                 * are `any`, so `#map{:a 1 :b "two"}` is not a
                 * `tur-map-homog__` error on the value side. */
                kvs[i + 1] = saffron
                    ? dl_saffron_widen_elem(e, f->as.list.items[i + 1])
                    : f->as.list.items[i + 1];
            }
            Form *call = dl_build_call(e, f->span, "hamt-of", kvs, n);
            return elab_form(e, call);
        }
        case F_SET_LITERAL: {
            /* saffron-lang-plan S6 (G7) / set-of-element-type-is-not-checked:
             * the same widen as `[...]`.  `#set{1 "two" 7.1}` used to build
             * WITHOUT it -- `set-of` had no homogeneity check, so each element
             * resolved its own Hash/MapKey and the set claimed `(Set int)`
             * while holding a cstr.  Now that `set-of` checks like `vec-of`
             * and `Hash[any]`/`MapKey[any]` exist, the literal widens to the
             * honest `(Set any)`. */
            Form **items = dl_saffron_widen_elems(e, f->span, f->as.list.items,
                                                  f->as.list.len);
            Form *call = dl_build_call(e, f->span, "set-of",
                                       items, f->as.list.len);
            return elab_form(e, call);
        }
        /* Variadic HKT rows: a #row{...} type-row is a TYPE, not a value. It is
         * only meaningful in type-annotation position (where type_expr_from_form
         * lowers it to a TY_TYPEROW). Reaching the value elaborator means it was
         * written where an expression is expected. */
        case F_ROW_LITERAL:
            diag_emit(DIAG_ERROR, f->span,
                      "#row{...} is a type-level row and can only appear in a "
                      "type annotation, not as a value expression");
            return NULL;
        case F_SET: {
            /* Phase X3: Elaborate set literal #s(e1 e2 ...) -> EX_SET_LIT */
            uint32_t n = f->as.list.len;
            Expr **items = arena_alloc(e->arena, n * sizeof(Expr *));
            for (uint32_t i = 0; i < n; i++) {
                items[i] = elab_form(e, f->as.list.items[i]);
                if (!items[i]) return NULL;
            }
            Type set_type = { .kind = TY_SET, .copy_kind = CK_COPY };
            Expr *ex = expr_new(e->arena, EX_SET_LIT, set_type, f->span);
            ex->as.set_lit_.items = items;
            ex->as.set_lit_.n = n;
            return ex;
        }
        /* Phase 6: quote form */
        case F_QUOTE: {
            /* (quote x) returns x as a literal without evaluating x */
            if (f->as.list.len != 1) {
                diag_emit(DIAG_ERROR, f->span,
                          "quote requires exactly one argument");
                return NULL;
            }
            Form *quoted = f->as.list.items[0];
            /* Quoting a bare symbol yields a first-class :Sym literal --
             * the same lowering the F_KEYWORD branch below uses for
             * `:foo`. This lets DSL helpers in defns construct AST
             * nodes without TUR-E0003 chasing the inner symbol against
             * scope. See
             * docs/archive/history/defgodot-script-macro-vec-quote-semantics.md. */
            if (quoted->tag == F_SYM) {
                Expr *out = expr_new(e->arena, EX_SYM_LIT, TYPE_SYM, f->span);
                out->as.sym_lit_.sym = quoted->as.sym;
                return out;
            }
            /* Non-symbol quoted forms (literals, lists): fall back to
             * the legacy "elaborate as expression" behaviour. Runtime
             * list-of-values construction for `(quote (a b c))` is a
             * follow-up. */
            return elab_form(e, quoted);
        }
        /* Phase 6: quasiquote forms - expand them */
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* Expand quasiquote forms first */
            {
                Form *expanded = quasiquote_expand_form(e, f);
                return elab_form(e, expanded);
            }
        case F_CBLOCK: {
            /* Phase 2: inline C code block ```c ... ``` */
            /* U6: warn if inline-C appears outside an #{Unsafe}-annotated function */
            if (g_lint_inline_c_unsafe && e->unsafe_depth == 0) {
                diag_emit_with_code(DIAG_WARNING, f->span,
                    TUR_W0036_INLINE_C_MISSING_UNSAFE,
                    "inline-C block in function not annotated #{Unsafe}; "
                    "add #{Unsafe} to the function or wrap the call site in (unsafe ...)");
            }
            /* Hoist regex.h to the file preamble when any inline-C
             * references it. Per-function `#include <regex.h>` only takes
             * effect for the first function in the TU (subsequent includes
             * are suppressed by the header guard), so without hoisting,
             * multi-function stdlib modules like stdlib/re.tur fail to
             * compile. */
            extern bool g_needs_regex_h;
            if (!g_needs_regex_h && f->as.cblock.p) {
                const char *needle = "regex.h";
                size_t nlen = 7;
                if (f->as.cblock.len >= nlen) {
                    for (uint32_t i = 0; i + nlen <= f->as.cblock.len; ++i) {
                        if (memcmp(f->as.cblock.p + i, needle, nlen) == 0) {
                            g_needs_regex_h = true;
                            break;
                        }
                    }
                }
            }
            /* WIN3-B: flag socket-using inline-C so the Winsock compat shim is
             * emitted on Windows.  "AF_INET" is present in every socket program
             * (both listen and connect set up a sockaddr_in) and nowhere else. */
            extern bool g_needs_winsock;
            if (!g_needs_winsock && f->as.cblock.p) {
                const char *needle = "AF_INET";
                size_t nlen = 7;
                if (f->as.cblock.len >= nlen) {
                    for (uint32_t i = 0; i + nlen <= f->as.cblock.len; ++i) {
                        if (memcmp(f->as.cblock.p + i, needle, nlen) == 0) {
                            g_needs_winsock = true;
                            break;
                        }
                    }
                }
            }
            /* inline-c-function-scope-include-guards fix: pre-populate the
             * hoisted-include set during elaboration so emit_module can
             * write the directives at file scope before any function body
             * is emitted. The same scan re-runs at emit time inside
             * inline_c_substitute to strip the lines from the body; dedup
             * makes that idempotent. */
            extern size_t tur_hoist_top_includes_scan(const char *body, size_t len);
            if (f->as.cblock.p) {
                (void)tur_hoist_top_includes_scan(f->as.cblock.p, f->as.cblock.len);
            }
            /* inline-c-cname-module-prefix-plan: resolve __TUR_CNAME_<name>__
             * splices into captures so module-prefixed callees get their exact
             * C name (prefix + (export-as ...) alias). Unresolved names stay as
             * the mangle-only splice for the emit-time fallback. */
            InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
            Binding **cname_caps = NULL;
            uint8_t   cname_n_caps = 0;
            ic->code = elab_cblock_resolve_cnames(e, f->as.cblock, f->span,
                                                  &cname_caps, &cname_n_caps);
            ic->return_type = TYPE_NIL; /* Will be inferred from context or default to void */
            ic->captures = cname_caps;
            ic->n_captures = cname_n_caps;
            ic->val_exprs = NULL;
            ic->n_val_exprs = 0;
            /* r7rs-programs-compile-slowly: a block the auto-loaded stdlib
             * owns, including one a stdlib file `(load ...)`s. */
            ic->from_stdlib = e->in_stdlib_load;
            
            Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, f->span);
            out->as.inline_c_.inline_c = ic;
            return out;
        }
        case F_LIST:
            if (f->as.list.len == 0) {
                diag_emit(DIAG_ERROR, f->span, "empty list ()");
                return NULL;
            }
            /* saffron-lang-plan S6 (G7): the cons-list twin of the `[...]` and
             * `#map{...}` widens.  `(list 1 "two" 7.1)` is
             * "function 'tur-list-homog__' arg 2: expected tyvar, got cstr" --
             * the same wall, from the third of the three homogeneity checks --
             * so each element is widened to `any` before the `list` macro runs.
             *
             * This one is a CALL rather than a reader literal, so the widen
             * hooks here instead of beside the data literals; the resulting
             * form goes through the ordinary macro path unchanged.
             *
             * The `scope_lookup` guard is belt-and-braces, not the thing that
             * preserves user shadowing: measured, a `(let [list f] (list 3 4))`
             * still reaches the `list` MACRO in plain Turmeric too -- macros
             * win over a same-named binding there already -- so declining here
             * changes nothing today.  It is kept so this widen is not the
             * reason a future fix to that ordering fails to take effect.
             *
             * Unlike Vec and Map, a widened cons list is only walkable through
             * an ascription: `Cons` is `(defstruct Cons :heap [A] (head A)
             * (tail :int))`, so the TAIL is an erased carrier and `.tail`
             * hands back an `:int`.  `(:: (.tail l) (Cons any))` recovers it and
             * the next `.head` reads its own tag -- pinned by
             * tests/fixtures/saffron-cons-list.  A `defdata` with `any` in BOTH
             * slots (what tests/fixtures/saffron-higher-order uses) needs no
             * ascription and stays the better idiom for a list you walk. */
            if (lang_span_is_dynamic(f->span) && f->as.list.len > 1 &&
                f->as.list.items[0]->tag == F_SYM &&
                strcmp(f->as.list.items[0]->as.sym->name, "list") == 0 &&
                !scope_lookup(e->scope, f->as.list.items[0]->as.sym)) {
                uint32_t n = f->as.list.len;
                Form **items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
                items[0] = f->as.list.items[0];
                for (uint32_t i = 1; i < n; i++)
                    items[i] = dl_saffron_widen_elem(e, f->as.list.items[i]);
                f = form_list(e->arena, f->span, items, n);
            }
            return elab_call(e, f);
        case F_TYPE_ANN:
            diag_emit(DIAG_ERROR, f->span,
                      "type annotation ': type' is only valid after a parameter name or as a return type");
            return NULL;
        /* CT0: Contract type annotations are not valid standalone expressions */
        case F_CONTRACT_TYPE:
            diag_emit(DIAG_ERROR, f->span,
                      "contract type '{ var : T | pred }' is only valid as a parameter or return type annotation");
            return NULL;
        /* RR3: Range literal variable annotation -- check for shadowing, then elaborate inner form. */
        case F_RANGE_VAR: {
            const Symbol *var_sym = f->as.list.items[0]->as.sym;
            Form *range_form = f->as.list.items[1];
            if (scope_lookup(e->scope, var_sym)) {
                diag_emit(DIAG_WARNING, f->span,
                          "#r{...}: variable '%s' shadows a binding in scope; "
                          "the name is not used in the expansion",
                          var_sym->name);
            }
            return elab_form(e, range_form);
        }
        /* INT-1: Reader conditional -- pick :tur or :turi branch based on g_interpret_mode */
        case F_READER_COND: {
            Form *tur_form = NULL, *turi_form = NULL;
            for (uint32_t i = 0; i + 1 < f->as.list.len; i += 2) {
                Form *key = f->as.list.items[i];
                Form *val = f->as.list.items[i + 1];
                if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, "tur") == 0)
                    tur_form = val;
                else if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, "turi") == 0)
                    turi_form = val;
            }
            Form *chosen = g_interpret_mode ? turi_form : tur_form;
            if (!chosen) return e_nil(e, f->span);
            return elab_form(e, chosen);
        }
    }
    return NULL;
}

/* LS2: file-scope holder for the active workspace-resolver context. Set
 * by main.c around its compile_to_c invocation; read here when Elab is
 * initialized. Stays NULL outside that scope (REPL, eval, etc.), so the
 * warning machinery is a no-op for those paths. Single-threaded usage. */
static const Ls2ResolverCtx *g_ls2_resolver_ctx;

void ls2_resolver_ctx_set(const Ls2ResolverCtx *ctx) {
    g_ls2_resolver_ctx = ctx;
}

const Ls2ResolverCtx *ls2_resolver_ctx_active(void) {
    return g_ls2_resolver_ctx;
}

/* used-attr-whole-program: file-scope holder for the active force-load list,
 * mirroring the LS2 context above.  Set by main.c around compile_to_c; read
 * after the main elaboration pass to retain unimported #[used] modules.
 * Single-threaded usage. */
static const UsedModulesCtx *g_used_modules_ctx;

void used_modules_ctx_set(const UsedModulesCtx *ctx) {
    g_used_modules_ctx = ctx;
}

const UsedModulesCtx *used_modules_ctx_active(void) {
    return g_used_modules_ctx;
}

/* Phase M: (load "path") expansion -- shared visited set + output accumulator
 * threaded through a depth-first, in-order recursive walk. */
typedef struct {
    Form         **out;        /* accumulated, fully-expanded form list */
    uint32_t       out_n, out_cap;
    int            rc;         /* -1 on any load error */
    /* Boundary tracking: the first `boundary_in` top-level INPUT forms are the
     * auto-loaded stdlib prefix. (load ...) expansion can splice in or elide
     * forms, so the stdlib region occupies a different number of OUTPUT slots
     * than its input count. `track_boundary` is set only on the outermost call
     * (whose loop index ranges over genuine top-level forms); when the loop
     * reaches input index `boundary_in`, `boundary_out` records how many output
     * forms the stdlib region produced -- the corrected stdlib_prefix. Without
     * this, the post-load stdlib boundary stays at the stale input count, and
     * the last auto-loaded defmodule's members fall past it (so the
     * stdlib macro-promotion sweep never reaches them).  See
     * docs/archive/history/autoload-defmodule-macro-not-promoted.md. */
    bool           track_boundary;
    uint32_t       boundary_in;
    uint32_t       boundary_out;
} LoadExpandCtx;

/* Normalize a (load "path") path into a stable dedup key. realpath() collapses
 * an absolute auto-load path (`/abs/.../stdlib/json.tur`) and a cwd-relative
 * user load (`stdlib/json.tur`) onto the same canonical string, so an explicit
 * re-load of an already-auto-loaded stdlib module is recognised as a duplicate
 * and skipped rather than re-spliced (which would collide with the auto-loaded
 * defns -- the "already defined by an auto-loaded stdlib module" hard error).
 * Falls back to the literal path when realpath() fails (e.g. an off-tree script
 * whose cwd-relative `stdlib/...` does not resolve until the stdlib fallback). */
static const Symbol *load_path_key(SymbolTable *st, const char *path,
                                   const char *stdlib_dir) {
    char resolved[4096];
    /* A `stdlib/<rest>` load MEANS "the stdlib's <rest>", which is the file the
     * auto-load prefix already spliced from `stdlib_dir` (`resolve_stdlib_root`).
     * Canonicalize it through `stdlib_dir` FIRST -- before the cwd-relative
     * realpath below -- so its dedup key matches the auto-load seed regardless
     * of where the build is invoked.  This is load-bearing when the two disagree:
     * running from a checkout root makes `realpath("stdlib/hamt.tur")` succeed as
     * the CWD copy while the auto-load resolved `stdlib_dir` to a *different*
     * stdlib (e.g. an installed one on `TUR_STDLIB_DIR`), so the cwd key misses
     * the seeded key and map.tur's `(load "stdlib/hamt.tur")` re-splices an
     * already-auto-loaded hamt -- a "'tur_hamt_new' is already defined" collision
     * that only reproduces off a checkout root and from a bare subprocess.
     * The stdlib dir already ends in ".../stdlib", so drop the leading
     * "stdlib/" component to avoid ".../stdlib/stdlib/...". */
    if (stdlib_dir && strncmp(path, "stdlib/", 7) == 0) {
        char alt[4096];
        int an = snprintf(alt, sizeof(alt), "%s/%s", stdlib_dir, path + 7);
        if (an > 0 && (size_t)an < sizeof(alt) && realpath(alt, resolved) != NULL)
            return intern_cstr(st, resolved);
    }
    if (realpath(path, resolved) != NULL)
        return intern_cstr(st, resolved);
    return intern_cstr(st, path);
}

/* Add a path key to the compilation-global load-visited set (idempotently). */
static void load_dedup_register(Elab *e, const Symbol *key) {
    for (uint32_t k = 0; k < e->n_load_expanded_paths; k++)
        if (e->load_expanded_paths[k] == key) return;
    if (e->n_load_expanded_paths >= e->cap_load_expanded_paths) {
        e->cap_load_expanded_paths = e->cap_load_expanded_paths
                                         ? e->cap_load_expanded_paths * 2 : 8;
        e->load_expanded_paths = (const Symbol **)realloc(
            (void *)e->load_expanded_paths,
            e->cap_load_expanded_paths * sizeof(const Symbol *));
        if (!e->load_expanded_paths) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    e->load_expanded_paths[e->n_load_expanded_paths++] = key;
}

/* security-audit-plan WP3: a capability-restricted interpreter env (no
 * TURI_CAP_IMPORT -- Env/new-sandboxed, the macro env) refuses `(load "path")`
 * in the forms it is handed, exactly as elab_load_module refuses `import`.
 * Before WP3 a sandboxed `(load "/etc/hostname")` read the file and echoed its
 * first token back in the unbound-symbol diagnostic.  Returns `f` with every
 * such load removed (top level and inside a defmodule body), NULL when `f` is
 * itself one, and sets *found.  Only the NEW forms of a turn go through this:
 * the accumulated prefix -- the host's own preload, loaded before the caps
 * were dropped -- is replayed untouched. */
static Form *sandbox_strip_loads(Elab *e, Arena *arena, Form *f, bool *found) {
    if (!f || f->tag != F_LIST || f->as.list.len < 1 ||
        f->as.list.items[0]->tag != F_SYM)
        return f;
    const Symbol *head = f->as.list.items[0]->as.sym;
    if (head == e->sym_load) {
        diag_emit(DIAG_ERROR, f->span, "load not allowed in sandboxed environment");
        *found = true;
        return NULL;
    }
    if (head != e->sym_defmodule) return f;
    uint32_t n = f->as.list.len, kept = 0;
    Form **items = (Form **)arena_alloc(arena, n * sizeof(Form *));
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        Form *it = (i == 0) ? f->as.list.items[0]
                            : sandbox_strip_loads(e, arena, f->as.list.items[i], found);
        if (it != f->as.list.items[i]) changed = true;
        if (it) items[kept++] = it;
    }
    return changed ? form_list(arena, f->span, items, kept) : f;
}

static void load_expand_emit(LoadExpandCtx *lx, Arena *arena, Form *f) {
    if (lx->out_n >= lx->out_cap) {
        lx->out_cap = lx->out_cap ? lx->out_cap * 2 : 16;
        Form **n = (Form **)arena_alloc(arena, lx->out_cap * sizeof(Form *));
        for (uint32_t k = 0; k < lx->out_n; k++) n[k] = lx->out[k];
        lx->out = n;
    }
    lx->out[lx->out_n++] = f;
}

/* Expand every top-level (load "path") in `forms` in place, depth-first and
 * in source order: a file's transitive loads are fully expanded at the point
 * they appear, BEFORE the rest of that file's forms. A path is expanded at
 * most once (the visited set), and because the walk is depth-first a module
 * that self-loads a dependency always emits that dependency ahead of its own
 * dependent forms -- so e.g. range.tur's `(load "stdlib/typeclass.tur")`
 * lands its `defclass Show` before range.tur's `Show [Bound]` instance, even
 * when a sibling file later loads typeclass.tur explicitly. (The old
 * multi-pass fixpoint deferred transitive loads a pass behind sibling
 * explicit loads, letting the later one claim the path and relocate the
 * expansion; see docs/archive/history/load-not-idempotent-typeclass.md.) */
static void load_expand_forms(LoadExpandCtx *lx, Elab *e, Arena *arena,
                              SymbolTable *st, Form *const *forms, uint32_t nforms) {
    for (uint32_t i = 0; i < nforms; i++) {
        /* Record the corrected stdlib boundary the moment the outer walk
         * crosses the last stdlib input form (before emitting any user form). */
        if (lx->track_boundary && i == lx->boundary_in)
            lx->boundary_out = lx->out_n;
        Form *f = forms[i];

        /* r7rs-lang-plan R7: a Scheme `(import (scheme time))` (or
         * process-context / file) splices in that library's file first, as a
         * `(load ...)` would -- once per compile, through the visited set. */
        {
            /* Room for every on-demand library and every SRFI in one import
             * form (r7rs-srfi-plan S1: eight silently dropped the rest). */
            const char *libs[128];
            uint32_t nl = scheme_import_library_files(f, libs, 128);
            for (uint32_t li = 0; li < nl; li++) {
                Form *ld_items[2];
                ld_items[0] = form_sym(arena, f->span, e->sym_load);
                ld_items[1] = form_str(arena, f->span, libs[li], (uint32_t)strlen(libs[li]));
                Form *ld = form_list(arena, f->span, ld_items, 2);
                Form *const one[1] = { ld };
                load_expand_forms(lx, e, arena, st, one, 1);
            }
        }

        /* Option A: descend into a (defmodule ...) body so a `(load "path")`
         * placed inside the module body splices the loaded file's forms into
         * the module's scope, exactly as a top-level load splices into the
         * compilation unit. Without this, a load nested in a defmodule body
         * survives the preprocessor and reaches elab_load, which errors. The
         * defmodule's head/name/export/import items are not load forms, so
         * they pass through this nested walk unchanged; only the `(load ...)`
         * body items expand in place. See
         * docs/archive/history/load-inside-defmodule-silently-loses-names.md. */
        if (f->tag == F_LIST && f->as.list.len >= 1 &&
            f->as.list.items[0]->tag == F_SYM &&
            f->as.list.items[0]->as.sym == e->sym_defmodule) {
            LoadExpandCtx sub = {0};
            sub.out_cap = f->as.list.len + 8;
            sub.out = (Form **)arena_alloc(arena, sub.out_cap * sizeof(Form *));
            /* Shares the compilation-global visited set on `e`, so a path
             * already spliced elsewhere is not re-spliced here. */
            load_expand_forms(&sub, e, arena, st,
                              f->as.list.items, f->as.list.len);
            if (sub.rc != 0) lx->rc = sub.rc;
            Form *expanded = form_list(arena, f->span, sub.out, sub.out_n);
            load_expand_emit(lx, arena, expanded);
            continue;
        }

        const Form *path_f = NULL;
        if (f->tag == F_LIST && f->as.list.len == 2 &&
            f->as.list.items[0]->tag == F_SYM &&
            f->as.list.items[0]->as.sym == e->sym_load &&
            f->as.list.items[1]->tag == F_STR) {
            path_f = f->as.list.items[1];
        }
        if (!path_f) { load_expand_emit(lx, arena, f); continue; }

        /* (load "path") -- read & parse */
        uint32_t plen = path_f->as.s.len;
        if (plen == 0 || plen >= 4096) {
            diag_emit(DIAG_ERROR, path_f->span,
                      "load: path must be non-empty and < 4096 chars");
            lx->rc = -1;
            continue;
        }
        char path_buf[4096];
        memcpy(path_buf, path_f->as.s.p, plen);
        path_buf[plen] = '\0';
        const Symbol *key = load_path_key(st, path_buf, e->module_stdlib_dir);
        /* The visited set is compilation-global (on the Elab) so a path the
         * entry already spliced is not re-spliced when an imported module loads
         * it too -- and vice versa. It is also seeded with the auto-loaded
         * stdlib files (see elaborate_program), so an explicit `(load
         * "stdlib/json.tur")` of a module that is already auto-loaded is a
         * no-op rather than a redefinition error. See load_expanded_paths in
         * elab_internal.h. */
        bool already = false;
        for (uint32_t k = 0; k < e->n_load_expanded_paths; k++) {
            if (e->load_expanded_paths[k] == key) { already = true; break; }
        }
        if (already) continue;  /* idempotent: a path is expanded at most once */
        /* Mark visited BEFORE recursing so a self/cyclic load is skipped. */
        load_dedup_register(e, key);

        char *src_raw = NULL;
        size_t src_len = 0;
        if (elab_read_file(path_buf, &src_raw, &src_len) != 0) {
            /* Off-tree fallback (one-off-script-print-and-annotation-ergonomics,
             * Finding 3): a freestanding `/tmp/foo.tur` that does
             * `(load "stdlib/math.tur")` cannot find the file cwd-relative when
             * run from outside the repo. `import` already falls back to the
             * resolved stdlib root (TUR_STDLIB_DIR, set absolute by main.c's
             * resolve_stdlib_root); mirror that here so the load-line the
             * "unknown function" hint suggests actually resolves off-tree.
             *
             * A "stdlib/<rest>" path is retried as "<stdlib_dir>/<rest>": the
             * resolved stdlib dir already ends in ".../stdlib", so the leading
             * "stdlib/" component is dropped to avoid ".../stdlib/stdlib/...".
             * When module_stdlib_dir is the legacy literal "stdlib" this
             * reproduces the original cwd-relative path (no regression). */
            bool recovered = false;
            const char *sdir = e->module_stdlib_dir;
            if (sdir && strncmp(path_buf, "stdlib/", 7) == 0) {
                char alt[4096];
                int an = snprintf(alt, sizeof(alt), "%s/%s", sdir, path_buf + 7);
                if (an > 0 && (size_t)an < sizeof(alt) &&
                    strcmp(alt, path_buf) != 0 &&
                    elab_read_file(alt, &src_raw, &src_len) == 0) {
                    memcpy(path_buf, alt, (size_t)an + 1);
                    plen = (uint32_t)an;
                    recovered = true;
                }
            }
            if (!recovered) {
                diag_emit(DIAG_ERROR, path_f->span, "load: cannot open '%s'", path_buf);
                lx->rc = -1;
                continue;
            }
        }
        char *src_copy = (char *)arena_alloc(arena, src_len + 1);
        memcpy(src_copy, src_raw, src_len);
        src_copy[src_len] = '\0';
        free(src_raw);
        char *path_copy = (char *)arena_alloc(arena, plen + 1);
        memcpy(path_copy, path_buf, plen + 1);
        SourceFile *sfile = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
        *sfile = (SourceFile){0};
        sfile->path = path_copy;
        sfile->src = src_copy;
        sfile->len = src_len;
        sfile->file_id = e->next_import_file_id++;
        /* load-ignores-inline-lang-directive: a loaded file's dialect was taken
         * from its EXTENSION only, so `(load "sweetlib.tur")` on a file whose
         * first line is `#lang turmeric/sweet` handed sweet source to the plain
         * reader and died on the directive itself ("unexpected character '#'").
         * Run the same detect_lang sweep the entry file gets
         * (resolve_reader_type in main.c) so the directive is both honoured and
         * stripped.  Extension still wins for the base when it selected a
         * non-default reader -- the directive is then a redundant hint. */
        {
            ReaderType ext_type = reader_type_from_extension(path_buf);
            if (ext_type == READER_UNKNOWN) ext_type = READER_TURMERIC;
            const char  *lsrc  = src_copy;
            size_t       llen  = src_len;
            const char  *bad = NULL;
            size_t       bad_len = 0;
            LangDialect dialect = LANG_TURMERIC;
            ReaderType lang_type = detect_lang_dialect(src_copy, src_len,
                                                       &lsrc, &llen,
                                                       &bad, &bad_len,
                                                       &dialect);
            if (bad) {
                if (lang_type == READER_UNKNOWN)
                    diag_emit(DIAG_ERROR, path_f->span,
                              "unknown #lang base '%.*s' -- see `tur dialects` for the valid bases (in loaded file '%s') (TUR-E0331)",
                              (int)bad_len, bad, path_buf);
                else
                    diag_emit(DIAG_ERROR, path_f->span,
                              "`#lang` takes a single base dialect; unexpected "
                              "trailing token '%.*s' in loaded file '%s' "
                              "(TUR-E0330)", (int)bad_len, bad, path_buf);
                lx->rc = -1;
                continue;
            }
            ReaderType chosen =
                (ext_type != READER_TURMERIC) ? ext_type : lang_type;
            if (!reader_type_is_implemented(chosen)) {
                diag_emit(DIAG_ERROR, path_f->span,
                          "#lang %s in loaded file '%s' is not yet implemented",
                          reader_type_name(chosen), path_buf);
                lx->rc = -1;
                continue;
            }
            /* `.scm` names the Scheme language as well as its reader. */
            {
                LangDialect ext_dialect = lang_dialect_from_extension(path_buf);
                if (ext_dialect != LANG_TURMERIC) dialect = ext_dialect;
            }
            sfile->src         = lsrc;
            sfile->len         = llen;
            sfile->reader_type = chosen;
            /* saffron-lang-plan S2/D5: a loaded file's OWN `#lang` line decides
             * its language, exactly as it decides its reader.  That is the
             * contract boundary: a Saffron program that loads a Turmeric module
             * gets Turmeric's defaults for that module's forms and Saffron's
             * for its own, because the dialect is per-SourceFile and every Form
             * carries the file it came from.
             *
             * This is also the path `tur --interpret <file>` takes for the USER
             * file -- the file-eval entry splices a `(load ...)` rather than
             * folding the source into the eval blob, so without this the
             * interpreter saw Turmeric defaults for a `#lang saffron` program
             * while the compiler saw Saffron ones. */
            sfile->lang        = dialect;
        }
        diag_register_file(sfile);
        diag_set_file_origin(sfile->file_id, path_f->span);
        /* Transitive-RM (T2): share the entry file's macro registry. */
        uint32_t lf_n = 0;
        bool had_error_before_load = diag_had_error();
        Form **lf = read_all_with_registry(arena, st, sfile, e->user_macros, &lf_n);
        if (!lf) {
            if (!had_error_before_load && diag_had_error()) {
                diag_emit(DIAG_NOTE, path_f->span, "while loading '%s'", path_buf);
            }
            lx->rc = -1;
            continue;
        }
        /* Depth-first: expand this file's own loads in place before continuing. */
        load_expand_forms(lx, e, arena, st, lf, lf_n);
    }
}

/* load-not-expanded-in-imported-or-project-modules: expand the top-level
 * (load "path") forms of an *imported* module's form list the same way
 * elaborate_program does for the entry unit. The entry preprocessor only ran
 * over the entry's own forms, so without this an imported file's top-level
 * `(load ...)` survived to elaboration and errored ("load is only valid at the
 * top level"). The visited set lives on the Elab and is shared with the entry
 * expansion, so a path loaded by both is spliced exactly once.
 *
 * Writes the fully-expanded form list to *out_forms / *out_n (arena-allocated)
 * and returns 0 on success, -1 if any load failed. */
int elab_expand_module_loads(Elab *e, Arena *arena, SymbolTable *st,
                             Form *const *forms, uint32_t nforms,
                             Form ***out_forms, uint32_t *out_n) {
    LoadExpandCtx lx = {0};
    lx.out_cap = nforms + 16;
    lx.out = (Form **)arena_alloc(arena, lx.out_cap * sizeof(Form *));
    load_expand_forms(&lx, e, arena, st, forms, nforms);
    *out_forms = lx.out;
    *out_n = lx.out_n;
    return lx.rc;
}

/* defdata-parametric-forward-decl-inference: resolve one type-argument form
 * of a compound return annotation to a Type during the Pass-1 forward-decl
 * scan.  This runs against RF0 stubs only (no registered defns, ADT kinds not
 * yet finalized), so it must NOT kind-check -- fn_type_from_form would emit a
 * spurious TUR-E0012 applying a not-yet-kinded stub.  Recognises the defn's own
 * type params (kept as named tyvars), registered ADT names, the scalar
 * primitives, and nested applications; anything else stays a named tyvar
 * filler (harmless -- the real defn pass recomputes the type in Pass 2). */
static Type fwd_shallow_type_arg(Elab *e, const Form *af,
                                 const Symbol **tps, uint8_t n_tp);

static Type *fwd_shallow_result_app(Elab *e, const Form *appform,
                                    const Symbol **tps, uint8_t n_tp) {
    if (!appform || appform->tag != F_LIST || appform->as.list.len < 1)
        return NULL;
    const Form *head = appform->as.list.items[0];
    if (head->tag != F_SYM) return NULL;
    AdtDef *def = NULL;
    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
        if (strcmp(e->adt_defs[ai]->name, head->as.sym->name) == 0) {
            def = e->adt_defs[ai];
            break;
        }
    }
    if (!def) return NULL;
    Type cur = type_adt(def);
    for (uint32_t i = 1; i < appform->as.list.len; i++) {
        Type arg = fwd_shallow_type_arg(e, appform->as.list.items[i], tps, n_tp);
        /* Build the TY_APP node by hand -- no kind_of_type_app call. */
        Type app;
        memset(&app, 0, sizeof(app));
        app.kind = TY_APP;
        app.copy_kind = CK_COPY;
        app.hkt_kind = KIND_STAR;
        app.as.app.fn = (Type *)arena_alloc(e->arena, sizeof(Type));
        *app.as.app.fn = cur;
        app.as.app.arg = (Type *)arena_alloc(e->arena, sizeof(Type));
        *app.as.app.arg = arg;
        cur = app;
    }
    Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
    *out = cur;
    return out;
}

/* r7rs-lang-plan R3: is every tyvar leaf of a shallow-resolved type one of the
 * defn's own type parameters?  The resolver spells an UNKNOWN name as a named
 * tyvar (harmless for a return, which Pass 2 recomputes), so a parameter type
 * is only committed to the forward decl when nothing in it is a guess. */
static bool fwd_type_is_closed(const Type *t, const Symbol **tps, uint8_t n_tp) {
    if (!t) return true;
    switch (t->kind) {
        case TY_TYVAR: {
            const char *nm = t->as.tyvar_.name;
            if (!nm) return false;
            for (uint8_t i = 0; i < n_tp; i++)
                if (tps[i] && strcmp(tps[i]->name, nm) == 0) return true;
            return false;
        }
        case TY_APP:
            return fwd_type_is_closed(t->as.app.fn, tps, n_tp) &&
                   fwd_type_is_closed(t->as.app.arg, tps, n_tp);
        default:
            return true;
    }
}

/* Does a shallow-resolved type name one of the defn's own type parameters? */
static bool fwd_type_mentions_tp(const Type *t, const Symbol **tps, uint8_t n_tp) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR: {
            const char *nm = t->as.tyvar_.name;
            if (!nm) return false;
            for (uint8_t i = 0; i < n_tp; i++)
                if (tps[i] && strcmp(tps[i]->name, nm) == 0) return true;
            return false;
        }
        case TY_APP:
            return fwd_type_mentions_tp(t->as.app.fn, tps, n_tp) ||
                   fwd_type_mentions_tp(t->as.app.arg, tps, n_tp);
        default:
            return false;
    }
}

static Type fwd_shallow_type_arg(Elab *e, const Form *af,
                                 const Symbol **tps, uint8_t n_tp) {
    if (af && af->tag == F_LIST) {
        Type *nested = fwd_shallow_result_app(e, af, tps, n_tp);
        if (nested) return *nested;
        return type_tyvar_named("_");
    }
    if (af && (af->tag == F_SYM || af->tag == F_KEYWORD)) {
        const char *nm = af->as.sym->name;
        for (uint8_t ti = 0; ti < n_tp; ti++) {
            if (tps[ti] && strcmp(tps[ti]->name, nm) == 0) {
                return type_tyvar_named(tps[ti]->name);
            }
        }
        if (strcmp(nm, "int") == 0)   return TYPE_INT;
        if (strcmp(nm, "bool") == 0)  return TYPE_BOOL;
        if (strcmp(nm, "cstr") == 0)  return TYPE_CSTR;
        if (strcmp(nm, "float") == 0) return TYPE_FLOAT;
        if (strcmp(nm, "void") == 0 || strcmp(nm, "nil") == 0)
            return TYPE_NIL;
        /* r7rs-lang-plan R3: `any` is a bare name that determines its type
         * completely, exactly as elab_types.c's IT4 arm builds it.  Left as
         * a named tyvar, `(Vec any)` forward-declared as `(Vec 'any)` -- an
         * OPEN application -- and the dynamic seam grounded and re-collected
         * it instead of checking against the declared instantiation. */
        if (strcmp(nm, "any") == 0)   return type_simple(TY_ANY, CK_COPY);
        for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
            if (strcmp(e->adt_defs[ai]->name, nm) == 0) {
                return type_adt(e->adt_defs[ai]);
            }
        }
        return type_tyvar_named(nm);
    }
    return type_tyvar_named("_");
}

/* r7rs-lang-plan R3: the full types of a defn's COMPOUND parameter annotations
 * for a pass-1 forward declaration, in a DYNAMIC file -- or NULL when there
 * are none to commit.
 *
 * fwd_decl_scan_params leaves `(Vec any)` as the TY_INT placeholder, and the
 * Saffron seam (elab_call.c D5/S4) reads that placeholder as a real `int`
 * target: a caller written ABOVE `(defn f [v : (Vec any)] ...)` that passes
 * an `any` got a checked unbox to int -- "cast: any holds Vec, not int" at
 * runtime, and C that handed an int64 to a `tur_adt_Vec__any *`.  Defining
 * the callee first avoided it, which is what made it look like an ordering
 * rule.  Only a fully closed application (every leaf a scalar, a registered
 * ADT or one of the defn's own type params) is committed, and the matching
 * `arg_kinds` slot becomes TY_APP; anything the shallow resolver could not
 * name stays the placeholder, so a typed file's int64 hatches are untouched.
 * Shared by the top-level pre-pass and the defmodule one (elab_module.c),
 * which is where an imported prelude's defns are forward-declared. */
Type **elab_fwd_param_full_types(Elab *e, Arena *arena, const Form *f,
                                 uint32_t name_idx, uint32_t params_idx,
                                 uint32_t param_arity, TypeKind *arg_kinds) {
    if (param_arity == 0 || !arg_kinds || !lang_span_is_dynamic(f->span) ||
        params_idx >= (uint32_t)f->as.list.len ||
        f->as.list.items[params_idx]->tag != F_VEC)
        return NULL;
    const Symbol *tp_syms[MAX_FN_ARITY];
    uint8_t n_tp = 0;
    if (params_idx > name_idx + 1 && f->as.list.items[name_idx + 1]->tag == F_VEC) {
        const Form *tpv = f->as.list.items[name_idx + 1];
        for (uint32_t ti = 0; ti < tpv->as.list.len && n_tp < MAX_FN_ARITY; ti++) {
            if (tpv->as.list.items[ti]->tag == F_SYM)
                tp_syms[n_tp++] = tpv->as.list.items[ti]->as.sym;
        }
    }
    Type **full_types = NULL;
    const Form *pv = f->as.list.items[params_idx];
    uint32_t slot = 0;
    for (uint32_t pi = 0; pi < pv->as.list.len; pi++) {
        const Form *p = pv->as.list.items[pi];
        if (p->tag == F_SYM && p->as.sym->name && p->as.sym->name[0] == '^')
            continue;
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            if (slot == 0 || slot > param_arity) continue;
            const Form *t = p;
            if (p->tag == F_TYPE_ANN && p->as.list.len >= 1)
                t = p->as.list.items[0];
            if (t->tag == F_SYM) {
                /* R10: a bare record or ADT name -- `[b : R7rsBytevector]`.
                 * fwd_decl_scan_params left it the TY_INT placeholder too,
                 * so a caller above the definition that passes an `any`
                 * got a checked unbox to int: "cast: any holds
                 * R7rsBytevector, not int" from `equal?` on two bytevectors
                 * (chibi's suite).  A registered, non-generic ADT names its
                 * type completely; commit it. */
                for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                    AdtDef *d = e->adt_defs[ai];
                    if (d->n_type_params != 0 || strcmp(d->name, t->as.sym->name) != 0)
                        continue;
                    if (!full_types) {
                        full_types = (Type **)arena_alloc(arena, param_arity * sizeof(Type *));
                        memset(full_types, 0, param_arity * sizeof(Type *));
                    }
                    Type *adt_t = (Type *)arena_alloc(arena, sizeof(Type));
                    *adt_t = type_adt(d);
                    full_types[slot - 1] = adt_t;
                    arg_kinds[slot - 1] = TY_ADT;
                    break;
                }
                continue;
            }
            if (t->tag != F_LIST) continue;
            Type *full = fwd_shallow_result_app(e, t, tp_syms, n_tp);
            if (!full || full->kind != TY_APP || !fwd_type_is_closed(full, tp_syms, n_tp))
                continue;
            if (!full_types) {
                full_types = (Type **)arena_alloc(arena, param_arity * sizeof(Type *));
                memset(full_types, 0, param_arity * sizeof(Type *));
            }
            full_types[slot - 1] = full;
            arg_kinds[slot - 1] = TY_APP;
            continue;
        }
        slot++;
    }
    return full_types;
}

/* The full result type of a defn's NAMED return annotation for the defmodule
 * pass-1 forward declaration, or NULL.  `ret_f` is the annotation as the
 * pre-pass left it: a bare symbol / keyword (`: Box`, already unwrapped from
 * its F_TYPE_ANN) or an F_TYPE_ANN around an application (`: (Result T E)`).
 *
 * r7rs-lang-plan R3 introduced this for DYNAMIC files only: a forward-declared
 * `(defn g [] : (Vec int) ...)` feeding a forward-declared `(Vec int)`
 * parameter has to say `(Vec int)` too, or the call is "expected (Vec int),
 * got int".
 *
 * forward-call-to-aggregate-result-types-as-carrier: a static file needs it
 * just as much.  The defmodule pre-pass kept every non-scalar return as the
 * TY_INT placeholder, so a caller written ABOVE `(defn good [] : (Result
 * Handle cstr) ...)` typed `(good)` as the int64 carrier and the enclosing
 * defn tripped TUR-E0709 ("declares return type '(Result Handle cstr)' but its
 * body returns int").  A bare `: Box` failed the same way.  The top-level
 * pre-pass has resolved both shapes all along (it runs after RF0 has stubbed
 * every type); a module body has no RF0, so here only what the shallow
 * resolver can name completely -- a registered ADT, a scalar, one of the
 * defn's own type parameters -- is committed.  A leaf naming a type the
 * module defines further down returns NULL, and the caller records the decl
 * as pending (elab_fwd_note_pending_result) so it is resolved once that type
 * is registered. */
Type *elab_fwd_compound_result_type(Elab *e, const Form *f, uint32_t name_idx,
                                    uint32_t params_idx, const Form *ret_f) {
    if (!ret_f) return NULL;
    const Symbol *tp_syms[MAX_FN_ARITY];
    uint8_t n_tp = 0;
    if (params_idx > name_idx + 1 && f->as.list.items[name_idx + 1]->tag == F_VEC) {
        const Form *tpv = f->as.list.items[name_idx + 1];
        for (uint32_t ti = 0; ti < tpv->as.list.len && n_tp < MAX_FN_ARITY; ti++) {
            if (tpv->as.list.items[ti]->tag == F_SYM)
                tp_syms[n_tp++] = tpv->as.list.items[ti]->as.sym;
        }
    }
    if (ret_f->tag == F_SYM || ret_f->tag == F_KEYWORD) {
        /* A bare name: a registered, non-generic ADT names its type
         * completely.  The defn's own type parameter is not a commitment
         * (the call instantiates it), and a scalar never reaches here. */
        const char *nm = ret_f->as.sym->name;
        for (uint8_t ti = 0; ti < n_tp; ti++)
            if (tp_syms[ti] && strcmp(tp_syms[ti]->name, nm) == 0) return NULL;
        for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
            AdtDef *d = e->adt_defs[ai];
            if (d->n_type_params != 0 || strcmp(d->name, nm) != 0) continue;
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_adt(d);
            return t;
        }
        return NULL;
    }
    if (ret_f->tag != F_TYPE_ANN || ret_f->as.list.len < 1) return NULL;
    const Form *app = ret_f->as.list.items[0];
    if (app->tag != F_LIST) return NULL;
    Type *full = fwd_shallow_result_app(e, app, tp_syms, n_tp);
    if (!full || full->kind != TY_APP || !fwd_type_is_closed(full, tp_syms, n_tp))
        return NULL;
    /* A GENERIC callee's result in a typed file stays the placeholder: the
     * forward decl carries no parameter types for the call to instantiate
     * `A` from, so `(some (wrap x))` above `(defn wrap [A] [x : A] : (Option
     * A) ...)` read an `(Option A)` carrier box as the by-value `(Option
     * (Option int))` the caller declared -- a wrong answer where the
     * placeholder is a compile error.  A dynamic file's forward decl carries
     * its closed parameter types (elab_fwd_param_full_types), so it keeps the
     * R3 behaviour. */
    if (!lang_span_is_dynamic(f->span) && fwd_type_mentions_tp(full, tp_syms, n_tp))
        return NULL;
    return full;
}

/* forward-call-to-aggregate-result-types-as-carrier: a defmodule forward decl
 * whose named return could not be resolved at pass 1 because a leaf is a type
 * the module defines in its own body (a module has no RF0 type pre-pass, so
 * `Box` is unregistered until its `defstruct` elaborates in pass 2).  Kept on
 * a list and retried at the start of every defn (elab_fwd_refresh_pending):
 * a type written above its first user is registered by then, which is the
 * order the language already asks of a module's types. */
typedef struct FwdPendingResult {
    Binding       *b;
    const Form    *f;
    const Form    *ret_f;
    uint32_t       name_idx;
    uint32_t       params_idx;
    struct FwdPendingResult *next;
} FwdPendingResult;

void elab_fwd_note_pending_result(Elab *e, Binding *b, const Form *f,
                                  uint32_t name_idx, uint32_t params_idx,
                                  const Form *ret_f) {
    FwdPendingResult *p =
        (FwdPendingResult *)arena_alloc(e->arena, sizeof(FwdPendingResult));
    p->b = b;
    p->f = f;
    p->ret_f = ret_f;
    p->name_idx = name_idx;
    p->params_idx = params_idx;
    p->next = (FwdPendingResult *)e->fwd_pending_results;
    e->fwd_pending_results = p;
}

void elab_fwd_refresh_pending(Elab *e) {
    FwdPendingResult **pp = (FwdPendingResult **)&e->fwd_pending_results;
    while (*pp) {
        FwdPendingResult *p = *pp;
        Binding *b = p->b;
        /* Done with once the defn itself has started: elab_defn's RR1 early
         * update gives the binding the real declared result from then on. */
        bool still_fwd = b && b->type.kind == TY_FN && !b->source_fn_def &&
                         b->type.as.fn.result_kind == TY_INT &&
                         !b->type.as.fn.result_full_type;
        if (!still_fwd) { *pp = p->next; continue; }
        Type *full = elab_fwd_compound_result_type(e, p->f, p->name_idx,
                                                   p->params_idx, p->ret_f);
        if (full) {
            b->type.as.fn.result_kind = full->kind;
            b->type.as.fn.result_full_type = full;
            *pp = p->next;
            continue;
        }
        pp = &p->next;
    }
}

/* A top-level statement USED to be fold-unsafe when its handle subtree carried a
 * `set!` (the escaping-mutable shape, effect-capture-k -- the DK had no
 * by-reference mutable capture) or a value-position nested handle (effect-nested).
 * BOTH now DK-lower:
 *  - the nested handle rides an LH_RESUME_CONT resume-frame whose borrowed `__kont`
 *    is copied into the nested handle's continuation env (CE.borrowed_kont);
 *  - the escaping mutable is lowered to a by-reference heap cell (B7): a `(set! m k)`
 *    store of a continuation deep-copies the chain into a shared cell captured by
 *    reference into the lifted handler case + continuation (emit_cps_ir.c
 *    g_byref_muts / dk_copy_range copy-on-store).
 * A synthesized d2b main whose shape the CPS subset still cannot admit is dropped
 * by the taint fixpoint and falls back to the historical direct/fiber main
 * (emit_cps_ir_try_fn returns false before the d2b wrapper) -- never a hard error.
 * So nothing is categorically fold-unsafe here now; keep the scan helper for
 * future shape-specific gating. */
static bool fold_stmt_is_risky(const Elab *e, const Form *f) {
    (void)e; (void)f;
    return false;
}

/* True when `f` contains a `(perform ...)` that is NOT lexically guarded by an
 * enclosing `handle`/`reset` and NOT inside a nested `fn`/lambda.  Such a perform
 * is a TOP-LEVEL unhandled effect: elaboration rejects it with a compile-time
 * TUR-E0008 via the `fn_body_depth == 0 && !is_effect_handled` check
 * (elab_effects.c).  Folding the statement into the synthesized main body would
 * put the perform at `fn_body_depth > 0`, suppressing that diagnostic and
 * deferring to a bare runtime abort instead (errors/effect-unhandled).  A perform
 * UNDER a handle/reset (the idiomatic B1 `(println (handle (perform E) (E ..) ..))`
 * top-level effect statement) is conservatively assumed handled, so the fold still
 * fires for it; a perform inside a nested `fn` runs at fn_body_depth > 0 either way
 * and never carried the top-level diagnostic, so it does not block the fold. */
static bool form_has_toplevel_unhandled_perform(const Elab *e, const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head && head->tag == F_SYM) {
        const Symbol *hs = head->as.sym;
        if (hs == e->sym_perform) return true;
        /* a handle/reset guards its subtree; a nested fn/lambda is its own body */
        if (hs == e->sym_handle || hs == e->sym_reset
            || hs == e->sym_fn || hs == e->sym_lambda) return false;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (form_has_toplevel_unhandled_perform(e, f->as.list.items[i])) return true;
    return false;
}

/* True when `f` contains a `def`/`define` SUBFORM -- a `def` nested inside a
 * top-level statement rather than heading a top-level form of its own.
 *
 * Such a `def` binds a GLOBAL: at file scope `(when true (def x 1))` and
 * `(if c (def x 1) (def y 2))` both mint globals that later forms can see, and
 * that is how the interpreter and the no-fold compiled path behave.  Folding
 * the statement into the synthesized main body relocates the `def` INSIDE a
 * function, where `e->scope != &e->global` makes elab_def emit "`def` here has
 * nothing to scope over: ... binds a name no later form can see" -- a claim
 * that is false about the code the user actually wrote.  So the fold turned a
 * working program into a compile error, and only when no user `main` existed:
 * adding `(defn main [] : int 0)` to the same file made the error disappear.
 *
 * This is the same hazard the `?`/`return` carve-out above guards, in the
 * opposite direction (there, folding SUPPRESSES a correct rejection; here it
 * CREATES a spurious one), so it takes the same remedy -- abort the fold and
 * leave the form at top level, where it keeps its real meaning.
 *
 * A `def` inside a nested `fn`/lambda is a function-local binding either way
 * and is genuinely rejected in both paths, so it does not block the fold.
 * See docs/archive/turi-toplevel-expr-subforms-elaborate-in-global-scope.md. */
static bool form_has_nested_def(const Elab *e, const Form *f, bool nested) {
    if (!f || f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head && head->tag == F_SYM) {
        const Symbol *hs = head->as.sym;
        if (nested && (hs == e->sym_def || hs == e->sym_define)) return true;
        /* a nested fn/lambda body is its own scope, not file scope */
        if (hs == e->sym_fn || hs == e->sym_lambda) return false;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (form_has_nested_def(e, f->as.list.items[i], true)) return true;
    return false;
}

/* Classify a `(defmacro name [params] TEMPLATE)` form: does its expansion
 * produce a STATEMENT (fold-safe) rather than a top-level definition/directive?
 *
 * A top-level macro CALL is normally ambiguous to the synthesized-main fold --
 * it may expand to a top-level `defn`/`defmodule` (stays top-level) OR to a
 * statement like `(handle ...)` (should fold into main's do-body and DK-lower).
 * The fold runs BEFORE macros are registered, so we cannot expand the call; but
 * we can inspect the macro's DEFINITION.  Return true only when the body is a
 * single template form whose direct head is a symbol that provably cannot be a
 * top-level definition: not a `def*` head, not a module directive / `do` /
 * quote / quasiquote head, and not itself another macro (whose expansion we
 * cannot see through here).  A statement-form head (`handle`, `reset`, `let`,
 * `if`, or a plain function call) qualifies.  Conservative by construction:
 * quasiquoted templates (distinct F_QUASIQUOTE tag), multi-form bodies, and
 * macro-of-macro templates all return false and keep the historical fiber path.
 * On success `*out_template` is the template form, for the risky-shape check. */
static bool macro_form_stmt_safe(const Elab *e, const Form *dm,
                                 const Symbol *const *macro_names, uint32_t n_macro,
                                 const Form **out_template) {
    *out_template = NULL;
    /* exactly `(defmacro name [params] TEMPLATE)` -- 4 items, single body form */
    if (dm->tag != F_LIST || dm->as.list.len != 4) return false;
    const Form *tmpl = dm->as.list.items[3];
    if (!tmpl || tmpl->tag != F_LIST || tmpl->as.list.len == 0) return false;
    const Form *th = tmpl->as.list.items[0];
    if (!th || th->tag != F_SYM || !th->as.sym->name) return false;
    const Symbol *ths = th->as.sym;
    const char *thn = ths->name;
    if (thn[0] == 'd' && thn[1] == 'e' && thn[2] == 'f') return false; /* def* */
    if (ths == e->sym_import || ths == e->sym_load || ths == e->sym_export
        || ths == e->sym_extern_c || ths == e->sym_defmodule
        || ths == e->sym_do || ths == e->sym_quote || ths == e->sym_quasiquote)
        return false;
    for (uint32_t m = 0; m < n_macro; m++)
        if (macro_names[m] == ths) return false;   /* macro-of-macro: opaque */
    *out_template = tmpl;
    return true;
}

/* Pass-1 forward declaration of a single top-level (defn ...) form.
 *
 * Registers the defn's name into ep->global (with the best-effort forward-decl
 * type) so a self-recursive or mutually-recursive call in the body resolves
 * even before Pass-2 elaborates the body.  Extracted from elaborate_program so
 * import_module can run the same pre-pass over the bare top-level defns a
 * (load ...) splices into an imported module -- without it those spliced defns
 * (e.g. stdlib/typeclass-show.tur's `vec-show-loop`, which the reentrant
 * string<->typeclass load chain pulls into an imported module) elaborate
 * linearly with no forward decl and a self-call reports "unknown function".
 * See docs/archive/compiled-string-return-int-conversion.md (secondary
 * blocker). */

/* typeclass-method-resolution-ignores-the-class (symptom A): does any form at
 * or after `from` register a typeclass instance?
 *
 * Used to gate the speculative defn elaboration below.  Instances register in
 * source order, so a defn body that calls a class method before ANY instance of
 * that class exists cannot resolve -- but if one appears later in the unit, the
 * body would resolve if it were elaborated after it.  This scan is the cheap,
 * purely syntactic question "is deferral worth attempting here at all", so that
 * a unit with no later `definstance` -- the overwhelming majority -- takes
 * exactly the path it took before, with no capture frame and no re-elaboration.
 *
 * Recurses (depth-bounded) because a `definstance` is usually a CHILD of a
 * `(defmodule ...)` rather than a top-level form. */
static bool tl_has_definstance_at_or_after(const Elab *e, Form *const *forms,
                                           uint32_t nforms, uint32_t from,
                                           int depth) {
    if (depth > 4) return false;
    for (uint32_t i = from; i < nforms; i++) {
        Form *f = forms[i];
        if (!f || f->tag != F_LIST || f->as.list.len == 0) continue;
        Form *h = f->as.list.items[0];
        if (h->tag == F_SYM && h->as.sym == e->sym_definstance) return true;
        if (tl_has_definstance_at_or_after(e, f->as.list.items,
                                           f->as.list.len, 1, depth + 1))
            return true;
    }
    return false;
}

/* Pass 2's state for elaborating one top-level form out of the main loop's
 * own position: a flushed defn, or a deferred one's second chance. */
typedef struct TlRetryCtx {
    Elab         *e;
    Form *const  *forms;
    uint32_t      nforms;
    Expr        **items;
    uint32_t      stdlib_prefix;
    int          *rc;
    bool         *tl_deferred;
    FwdGenOrder  *fgo;
} TlRetryCtx;

static Expr *tl_elab_form(TlRetryCtx *c, uint32_t i) {
    Elab *e = c->e;
    Form *f = c->forms[i];
    bool saved_in_stdlib = e->in_stdlib_load;
    e->in_stdlib_load = (i < c->stdlib_prefix);
    /* Statement position for the def-position check: this form, and any
     * form reachable from it through `do` chains, is a statement.  Anything
     * deeper is an expression subform.  See def_form_is_statement_position. */
    e->toplevel_stmt = f;
    e->toplevel_dynamic = lang_span_is_dynamic(f->span);   /* M10 */
    e->toplevel_scheme  = lang_span_is_scheme(f->span);    /* r7rs R2 */
    Expr *x = elab_form(e, f);
    e->toplevel_stmt = NULL;
    e->toplevel_dynamic = false;
    e->toplevel_scheme  = false;
    e->in_stdlib_load = saved_in_stdlib;
    return x;
}

/* Pass 2's main-loop treatment of forms[i] at position `pos`: speculative when
 * it is a defn and a `definstance` follows `pos` (symptom A, below). */
static void tl_elab_slot(TlRetryCtx *c, uint32_t i, uint32_t pos) {
    Elab *e = c->e;
    bool tl_may_defer = false;
    uint32_t tl_fsd_mark = e->n_file_scope_defs;
    if (c->tl_deferred) {
        Form *ff = c->forms[i];
        if (ff->tag == F_LIST && ff->as.list.len > 0) {
            Form *h = ff->as.list.items[0];
            if (h->tag == F_SYM && h->as.sym == e->sym_defn &&
                tl_has_definstance_at_or_after(e, c->forms, c->nforms, pos + 1, 0))
                tl_may_defer = true;
        }
    }
    if (tl_may_defer) diag_push_capture();
    c->items[i] = tl_elab_form(c, i);
    if (tl_may_defer) {
        uint32_t tl_cerr = diag_pop_capture();
        if (tl_cerr > 0 || !c->items[i]) {
            /* Roll back what the failed attempt registered and try again
             * at the end, with no capture frame, so a failure that is NOT
             * about instance ordering still reports its real diagnostic. */
            e->n_file_scope_defs = tl_fsd_mark;
            c->tl_deferred[i] = true;
            c->items[i] = NULL;
            return;
        }
    }
    if (!c->items[i]) *c->rc = -1;   /* keep going to surface more diagnostics */
    fwd_gen_order_done(c->fgo, i);
}

/* Elaborate, ahead of position `pos`, every waiting defn `f` names -- each
 * one's own waiting names first. */
static void tl_flush(TlRetryCtx *c, const Form *f, uint32_t pos) {
    uint32_t d;
    while ((d = fwd_gen_order_next_flush(c->fgo, f)) != UINT32_MAX) {
        fwd_gen_order_done(c->fgo, d);
        tl_flush(c, c->forms[d], pos);
        tl_elab_slot(c, d, pos);
    }
}

/* Pass 2's second chance for one deferred top-level form: no capture frame,
 * so a still-failing body reports for real. */
static void tl_retry_slot(void *vctx, uint32_t i) {
    TlRetryCtx *c = (TlRetryCtx *)vctx;
    c->items[i] = tl_elab_form(c, i);
    if (!c->items[i]) *c->rc = -1;
}

/* A speculative attempt at one deferred top-level form, under a capture
 * frame: kept when it elaborates cleanly, rolled back otherwise (the same
 * rollback as the symptom-A attempt). */
static bool tl_probe_slot(void *vctx, uint32_t i) {
    TlRetryCtx *c = (TlRetryCtx *)vctx;
    uint32_t mark = c->e->n_file_scope_defs;
    diag_push_capture();
    Expr *x = tl_elab_form(c, i);
    uint32_t cerr = diag_pop_capture();
    if (cerr == 0 && x) {
        c->items[i] = x;
        return true;
    }
    c->e->n_file_scope_defs = mark;
    return false;
}

/* forward-call-to-generic-callee-typed-as-placeholder.
 *
 * A caller elaborated before a GENERIC callee sees only the callee's pass-1
 * forward decl, which has the arity but no parameter types and no type
 * parameters (fwd_decl_scan_params records scalar kinds; `x : A` is the
 * TY_INT placeholder).  Nothing at the call can instantiate `A` from that:
 * `(wrap x)` above `(defn wrap [A] [x : A] : (Option A) ...)` was TUR-E0709,
 * a float argument "expected int, got float", and a lambda handed to a later
 * `(defn twice [A] [f : (fn [A] A) x : A] ...)` was passed bare where the
 * definition takes the fat carrier -- a SIGSEGV at run time.
 *
 * A non-generic callee with a function-typed parameter has the same hole: its
 * forward decl does not mark the parameter as the `:fn` carrier (FA_POLY_FN),
 * so a lambda argument is not wrapped into the `tur_poly_fn_t` the definition
 * takes, and cc rejects the call.  Both are "lossy" callees below.
 *
 * So Pass 2 defers such a caller until its callee has been elaborated.  A
 * defn waits when it CALLS -- `(name ...)` -- a lossy defn of the same
 * statement list that has not been reached yet, or names (anywhere) a defn
 * that is itself waiting, whose binding is the forward decl again.  A name
 * the form binds in a vector (a parameter, a let) is a local, not a call of
 * the callee.  Every other form keeps its place, and first elaborates any
 * waiting defn it names (the drivers' flush), so no form sees a forward decl
 * where it used to see the definition.  A program that never calls a lossy
 * defn above its definition therefore elaborates exactly as before.  A cycle
 * (lossy defns that call each other) is broken at its first lossy member,
 * and that member sees the forward decls every defn saw before.  Stdlib
 * forms take no part: they neither wait nor hold anything back.  A
 * dynamic-file defn is not tracked either: its forward decl carries closed
 * parameter types (elab_fwd_param_full_types).
 *
 * The scan is over the unexpanded form, so a call a macro introduces is not
 * seen (it keeps the old behaviour). */
enum { FGO_NONE = 0, FGO_LOSSY, FGO_DEFERRED, FGO_LOSSY_DEFERRED, FGO_DONE };

#define FGO_NO_SLOT UINT32_MAX

/* Is the parameter annotation `a` a function type -- `(fn [...] R)`, or the
 * bare `:fn` carrier? */
static bool fgo_ann_is_fn_type(const Elab *e, const Form *a) {
    if (a->tag == F_TYPE_ANN && a->as.list.len >= 1) a = a->as.list.items[0];
    if (a->tag == F_SYM || a->tag == F_KEYWORD) return a->as.sym == e->sym_fn;
    return a->tag == F_LIST && a->as.list.len > 0 &&
           a->as.list.items[0]->tag == F_SYM &&
           a->as.list.items[0]->as.sym == e->sym_fn;
}

/* The name of a `(defn ...)` form, or NULL.  `*lossy` says whether a caller
 * elaborated before it gets a forward decl too lossy to call it through: it
 * declares type parameters, or a function-typed parameter (the forward decl
 * has no FA_POLY_FN flag, so a bare function argument is not wrapped into the
 * `tur_poly_fn_t` the definition takes -- invalid C).  Skips the attributes
 * elab_defn accepts before the name.  Two vectors after the name are
 * `[TypeVars] [params]`, the same test the pass-1 forward decl uses. */
static const Symbol *fgo_defn_name(const Elab *e, const Form *f, bool *lossy) {
    *lossy = false;
    if (!f || f->tag != F_LIST || f->as.list.len < 3) return NULL;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM || h->as.sym != e->sym_defn) return NULL;
    uint32_t n = f->as.list.len, k = 1;
    while (k < n) {
        const Form *a = f->as.list.items[k];
        if (a->tag == F_SYM && (a->as.sym == e->sym_no_unwind_attr ||
                                a->as.sym == e->sym_used_attr ||
                                a->as.sym == e->sym_caret_reflect)) {
            k++;
        } else if (a->tag == F_SYM && a->as.sym == e->sym_caret_deprecated) {
            k++;
            if (k < n && f->as.list.items[k]->tag == F_STR) k++;
        } else if (a->tag == F_LIST && a->as.list.len == 2 &&
                   a->as.list.items[0]->tag == F_SYM &&
                   a->as.list.items[0]->as.sym == e->sym_export_as_attr) {
            k++;
        } else {
            break;
        }
    }
    if (k >= n || f->as.list.items[k]->tag != F_SYM) return NULL;
    if (n > k + 2 && f->as.list.items[k + 1]->tag == F_VEC &&
        f->as.list.items[k + 2]->tag == F_VEC) {
        *lossy = true;   /* generic */
    } else if (k + 1 < n && f->as.list.items[k + 1]->tag == F_VEC) {
        const Form *pv = f->as.list.items[k + 1];
        for (uint32_t pi = 0; pi < pv->as.list.len && !*lossy; pi++) {
            const Form *p = pv->as.list.items[pi];
            if ((p->tag == F_TYPE_ANN || p->tag == F_KEYWORD) &&
                fgo_ann_is_fn_type(e, p))
                *lossy = true;
        }
    }
    return f->as.list.items[k]->as.sym;
}

static bool fgo_form_has_list_payload(const Form *f) {
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET:
        case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN: case F_CONTRACT_TYPE: case F_READER_COND:
        case F_RANGE_VAR: case F_MAP_LITERAL: case F_SET_LITERAL:
        case F_ROW_LITERAL:
            return true;
        default:
            return false;
    }
}

static uint32_t fgo_probe(const FwdGenOrder *o, const Symbol *s) {
    uint32_t m = o->cap - 1, i = s->hash & m;
    while (o->keys[i] && o->keys[i] != s) i = (i + 1) & m;
    return i;
}

/* The slot of the tracked defn named `s`, or FGO_NO_SLOT. */
static uint32_t fgo_slot_of(const FwdGenOrder *o, const Symbol *s) {
    uint32_t k = fgo_probe(o, s);
    return o->keys[k] ? o->slot_of[k] : FGO_NO_SLOT;
}

static bool fgo_state_deferred(uint8_t st) {
    return st == FGO_DEFERRED || st == FGO_LOSSY_DEFERRED;
}

void fwd_gen_order_init(FwdGenOrder *o, const Elab *e, Form *const *forms,
                        uint32_t n) {
    memset(o, 0, sizeof *o);
    o->n = n;
    uint32_t n_defn = 0, n_lossy = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool g;
        if (!fgo_defn_name(e, forms[i], &g)) continue;
        n_defn++;
        if (g && !lang_span_is_dynamic(forms[i]->span) &&
            !elab_file_is_stdlib(forms[i]->span.file_id))
            n_lossy++;
    }
    if (n_lossy == 0) return;
    o->name   = (const Symbol **)calloc(n, sizeof *o->name);
    o->state  = (uint8_t *)calloc(n, sizeof *o->state);
    o->primed = (bool *)calloc(n, sizeof *o->primed);
    o->cap = 16;
    while (o->cap < 2 * n_defn + 2) o->cap <<= 1;
    o->keys    = (const Symbol **)calloc(o->cap, sizeof *o->keys);
    o->counts  = (uint32_t *)calloc(o->cap, sizeof *o->counts);
    o->slot_of = (uint32_t *)malloc(o->cap * sizeof *o->slot_of);
    /* Only the FIRST defn of a name is tracked, as only it gets the pass-1
     * forward decl (a later one is a redefinition); a stdlib defn's name is
     * recorded so a user redefinition of it is not tracked either. */
    for (uint32_t i = 0; i < n; i++) {
        bool g;
        const Symbol *nm = fgo_defn_name(e, forms[i], &g);
        if (!nm) continue;
        uint32_t k = fgo_probe(o, nm);
        if (o->keys[k]) continue;
        o->keys[k] = nm;
        o->slot_of[k] = FGO_NO_SLOT;
        if (elab_file_is_stdlib(forms[i]->span.file_id)) continue;
        o->name[i] = nm;
        o->slot_of[k] = i;
        if (g && !lang_span_is_dynamic(forms[i]->span)) {
            o->state[i] = FGO_LOSSY;
            o->counts[k] = 1;
        }
    }
}

/* Does `f` bind `s` in a vector -- a parameter, a let or loop binding? */
static bool fgo_vec_binds(const Form *f, const Symbol *s) {
    if (!fgo_form_has_list_payload(f)) return false;
    for (uint32_t k = 0; k < f->as.list.len; k++) {
        const Form *c = f->as.list.items[k];
        if (f->tag == F_VEC && c->tag == F_SYM && c->as.sym == s) return true;
        if (fgo_vec_binds(c, s)) return true;
    }
    return false;
}

/* A call `(name ...)` in `f` of a lossy defn not reached yet, other than
 * `self`.  `root` is the whole form, for the local-binding check. */
static bool fgo_calls_pending_lossy(const FwdGenOrder *o, const Form *f,
                                    const Symbol *self, const Form *root) {
    if (!fgo_form_has_list_payload(f)) return false;
    if (f->tag == F_LIST && f->as.list.len > 0 &&
        f->as.list.items[0]->tag == F_SYM) {
        const Symbol *h = f->as.list.items[0]->as.sym;
        uint32_t d = h == self ? FGO_NO_SLOT : fgo_slot_of(o, h);
        if (d != FGO_NO_SLOT && o->state[d] == FGO_LOSSY &&
            !fgo_vec_binds(root, h))
            return true;
    }
    for (uint32_t k = 0; k < f->as.list.len; k++)
        if (fgo_calls_pending_lossy(o, f->as.list.items[k], self, root))
            return true;
    return false;
}

/* The slot of a waiting defn `f` names anywhere, other than `self`, or
 * FGO_NO_SLOT. */
static uint32_t fgo_names_deferred(const FwdGenOrder *o, const Form *f,
                                   const Symbol *self) {
    if (f->tag == F_SYM) {
        if (f->as.sym == self) return FGO_NO_SLOT;
        uint32_t d = fgo_slot_of(o, f->as.sym);
        return (d != FGO_NO_SLOT && fgo_state_deferred(o->state[d]))
            ? d : FGO_NO_SLOT;
    }
    if (!fgo_form_has_list_payload(f)) return FGO_NO_SLOT;
    for (uint32_t k = 0; k < f->as.list.len; k++) {
        uint32_t d = fgo_names_deferred(o, f->as.list.items[k], self);
        if (d != FGO_NO_SLOT) return d;
    }
    return FGO_NO_SLOT;
}

bool fwd_gen_order_should_defer(const FwdGenOrder *o, Form *const *forms,
                                uint32_t i) {
    if (o->cap == 0 || i >= o->n || !o->name[i]) return false;
    const Symbol *self = o->name[i];
    return fgo_names_deferred(o, forms[i], self) != FGO_NO_SLOT ||
           fgo_calls_pending_lossy(o, forms[i], self, forms[i]);
}

uint32_t fwd_gen_order_next_flush(const FwdGenOrder *o, const Form *f) {
    if (o->cap == 0 || !o->any_deferred) return FGO_NO_SLOT;
    return fgo_names_deferred(o, f, NULL);
}

void fwd_gen_order_defer(FwdGenOrder *o, uint32_t i) {
    if (o->cap == 0 || i >= o->n || !o->name[i]) return;
    if (o->state[i] == FGO_LOSSY) {
        o->state[i] = FGO_LOSSY_DEFERRED;
    } else {
        o->state[i] = FGO_DEFERRED;
        o->counts[fgo_probe(o, o->name[i])]++;
    }
    o->any_deferred = true;
}

static bool fgo_is_deferred(const FwdGenOrder *o, uint32_t i) {
    return o->cap != 0 && i < o->n && fgo_state_deferred(o->state[i]);
}

void fwd_gen_order_done(FwdGenOrder *o, uint32_t i) {
    if (o->cap == 0 || i >= o->n || !o->name[i]) return;
    if (o->state[i] != FGO_NONE && o->state[i] != FGO_DONE) {
        uint32_t k = fgo_probe(o, o->name[i]);
        if (o->counts[k] > 0) o->counts[k]--;
    }
    o->state[i] = FGO_DONE;
}

void fwd_gen_order_drain(FwdGenOrder *o, Form *const *forms, bool *extra,
                         void (*retry)(void *ctx, uint32_t i),
                         bool (*probe)(void *ctx, uint32_t i), void *ctx) {
    uint32_t n = o->n;
    if (!o->any_deferred) {
        for (uint32_t i = 0; i < n; i++) {
            if (!extra || !extra[i]) continue;
            extra[i] = false;
            fwd_gen_order_done(o, i);
            retry(ctx, i);
        }
        return;
    }
    for (;;) {
        /* Sweep in source order, taking every form nothing it names is still
         * waiting for; repeat while that makes progress. */
        bool progress = false;
        for (uint32_t i = 0; i < n; i++) {
            if (!(extra && extra[i]) && !fgo_is_deferred(o, i)) continue;
            if (fwd_gen_order_should_defer(o, forms, i)) continue;
            if (extra) extra[i] = false;
            fwd_gen_order_done(o, i);
            retry(ctx, i);
            progress = true;
        }
        if (progress) continue;
        /* Stuck: what is left waits on a cycle.  First PRIME its lossy
         * members: elaborate each one speculatively.  elab_defn forwards a
         * defn's declared signature onto its binding before the body (what
         * lets a generic call itself), and that survives a rolled-back body,
         * so once every member has been tried, each sees the others' full
         * signatures instead of the pass-1 placeholder -- `ping`/`pong` with
         * an `(Option A)` result was "then=(Option A) else=int".  A member
         * whose attempt succeeds is simply done.  Each slot is primed once. */
        if (probe) {
            bool kept = false;
            for (uint32_t i = 0; i < n; i++) {
                bool lossy = o->state[i] == FGO_LOSSY_DEFERRED ||
                             (o->state[i] == FGO_LOSSY && extra && extra[i]);
                if (!lossy || o->primed[i]) continue;
                o->primed[i] = true;
                if (probe(ctx, i)) {
                    if (extra) extra[i] = false;
                    fwd_gen_order_done(o, i);
                    kept = true;
                }
            }
            if (kept) continue;
        }
        /* Break the cycle at its first lossy member, so the defns that only
         * call into the cycle still come after it; with no lossy defn left,
         * take the first form. */
        uint32_t pick = n;
        for (uint32_t i = 0; i < n && pick == n; i++)
            if (o->state[i] == FGO_LOSSY_DEFERRED ||
                (o->state[i] == FGO_LOSSY && extra && extra[i]))
                pick = i;
        for (uint32_t i = 0; i < n && pick == n; i++)
            if ((extra && extra[i]) || fgo_is_deferred(o, i)) pick = i;
        if (pick == n) return;
        if (extra) extra[pick] = false;
        fwd_gen_order_done(o, pick);
        retry(ctx, pick);
    }
}

void fwd_gen_order_free(FwdGenOrder *o) {
    free(o->name);
    free(o->state);
    free(o->primed);
    free(o->keys);
    free(o->counts);
    free(o->slot_of);
    memset(o, 0, sizeof *o);
}

/* r7rs-procedure-body-forward-reference: Pass 1 also pre-declares every
 * top-level `(def ^mut name : any init)` -- the shape the Scheme lowering
 * gives a variable a `set!` writes or a procedure above it names (R7RS 5.3.1
 * lets a body refer to any top-level variable, wherever its define stands).
 * A defn is pre-declared so mutual recursion resolves; a plain global was
 * declared where its def stood, so a body above it saw an unbound name.  The
 * declared type is what the annotation says, so nothing of the initializer
 * is needed here: the binding is `any`, mutable, global, and marked so
 * elab_def fills it in (rather than reporting a redefinition) when Pass 2
 * reaches the def.  Only this exact shape: an unannotated def's type is its
 * initializer's, which Pass 1 does not have. */
void elab_pre_declare_any_mut_def(Elab *ep, const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len < 4) return;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM || h->as.sym != ep->sym_def) return;
    uint32_t i = 1;
    bool saw_mut = false;
    while (i < f->as.list.len && f->as.list.items[i]->tag == F_SYM &&
           f->as.list.items[i]->as.sym->len > 1 && f->as.list.items[i]->as.sym->name[0] == '^') {
        const Symbol *a = f->as.list.items[i]->as.sym;
        if (a == ep->sym_caret_mut) saw_mut = true;
        i++;
        if (a == ep->sym_caret_deprecated && i < f->as.list.len && f->as.list.items[i]->tag == F_STR) i++;
    }
    if (!saw_mut || i + 3 != f->as.list.len) return;
    const Form *name_f = f->as.list.items[i], *ann = f->as.list.items[i + 1];
    if (name_f->tag != F_SYM) return;
    const Symbol *tsym = NULL;
    if (ann->tag == F_TYPE_ANN && ann->as.list.len == 1 && ann->as.list.items[0]->tag == F_SYM)
        tsym = ann->as.list.items[0]->as.sym;
    else if (ann->tag == F_KEYWORD)
        tsym = ann->as.sym;
    if (!tsym || strcmp(tsym->name, "any") != 0) return;
    if (scope_lookup(&ep->global, name_f->as.sym)) return;
    Binding *b = binding_new(ep, name_f->as.sym, type_simple(TY_ANY, CK_COPY),
                             /*is_mut=*/true, /*is_global=*/true, name_f->span);
    b->is_forward_def = true;
    scope_add(&ep->global, b);
}

/* The index of a `(defn ...)` form's name: past the bare attribute symbols
 * (#[no-unwind] / #[used]), an `(export-as "c_name")`, and the `^attr`s
 * elab_defn takes before the name (^construct, ^byval, ^deprecated
 * ["message"], ^reflect), in any order.  Every scanner that wants a defn's
 * name before elab_defn runs goes through this, so a new pre-name attribute
 * is taught in one place -- the module pre-declare scan used to skip none of
 * the `^` ones.  Unknown `^attr`s are skipped too: elab_defn reports them.
 * Returns f->as.list.len when the form has no name. */
uint32_t elab_defn_name_index(const Elab *ep, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t i = 1;
    while (i < n) {
        const Form *it = f->as.list.items[i];
        if (it->tag == F_SYM &&
            (it->as.sym == ep->sym_no_unwind_attr || it->as.sym == ep->sym_used_attr)) {
            i++;
            continue;
        }
        if (it->tag == F_LIST && it->as.list.len == 2 &&
            it->as.list.items[0]->tag == F_SYM &&
            it->as.list.items[0]->as.sym == ep->sym_export_as_attr) {
            i++;
            continue;
        }
        if (it->tag == F_SYM && it->as.sym->len > 1 && it->as.sym->name[0] == '^') {
            i++;
            if (it->as.sym == ep->sym_caret_deprecated && i < n &&
                f->as.list.items[i]->tag == F_STR)
                i++;
            continue;
        }
        break;
    }
    return i;
}

void elab_pre_declare_toplevel_defn(Elab *ep, Arena *arena, Form *f) {
        elab_pre_declare_any_mut_def(ep, f);
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM) {
                if (head->as.sym == ep->sym_defn) {
                    /* Parse defn declaration without body */
                    if (f->as.list.len >= 3) {
                        /* Skip every pre-name attribute (shared helper). */
                        uint32_t name_idx = elab_defn_name_index(ep, f);
                        if ((uint32_t)f->as.list.len <= name_idx) goto next_form;
                        Form *name_f = f->as.list.items[name_idx];
                        if (name_f->tag == F_SYM) {
                            /* Parse return type annotation if present */
                            TypeKind return_kind = TY_INT; /* default */
                            /* defdata-parametric-forward-decl-inference: full
                             * TY_APP result type for a compound parametric-ADT
                             * return (e.g. (PRes Expr)), stamped onto the
                             * forward decl so a sibling caller declared earlier
                             * sees the concrete type args. */
                            Type *fwd_result_full = NULL;
                            uint32_t params_idx_local = name_idx + 1; /* params usually here */
                            /* poly-defn-recursive-return-type-inference: a defn with
                             * explicit type parameters spells as
                             *   (defn name [TypeVars] [params] :ret body)        -- 2-vec
                             *   (defn name [TypeVars] [Constraints] [params] :ret body) -- 3-vec
                             * so the params vector is at name_idx+2 (or +3), not +1.
                             * Without this skip, ret_idx points at the params vec, the
                             * keyword/sym/type-ann probe below misses, and the forward
                             * decl falls back to TY_INT -- breaking recursive self-calls
                             * inside a poly-defn body (e.g. `: bool` typed as int).
                             * Mirrors the F_VEC detection in elab_fns.c elab_defn. */
                            if (f->as.list.len > params_idx_local + 1 &&
                                f->as.list.items[params_idx_local]->tag == F_VEC &&
                                f->as.list.items[params_idx_local + 1]->tag == F_VEC) {
                                /* type-param vec present; bump past it */
                                params_idx_local++;
                                /* optional constraint vec between TypeVars and params */
                                if (f->as.list.len > params_idx_local + 1 &&
                                    f->as.list.items[params_idx_local]->tag == F_VEC &&
                                    f->as.list.items[params_idx_local + 1]->tag == F_VEC) {
                                    params_idx_local++;
                                }
                            }
                            uint32_t ret_idx = params_idx_local + 1; /* :ret follows params */
                            /* Skip optional #{Unsafe} / effect-row annotation (F_MAP) */
                            if (f->as.list.len > ret_idx && f->as.list.items[ret_idx]->tag == F_MAP) {
                                ret_idx++;
                            }
                            /* saffron-dynamic-surface-pass H6: whether a return
                             * annotation is PRESENT, decided structurally -- a
                             * `: T` (F_TYPE_ANN) or a `:int`-style keyword that
                             * is followed by a body.  A bare symbol or list at
                             * this slot is the body of an unannotated defn. */
                            bool ret_annotated = false;
                            if (f->as.list.len > ret_idx) {
                                Form *ret_f = f->as.list.items[ret_idx];
                                ret_annotated = ret_f->tag == F_TYPE_ANN ||
                                    (ret_f->tag == F_KEYWORD &&
                                     f->as.list.len > ret_idx + 1);
                                /* Accept spaced `: T` (F_TYPE_ANN of a single
                                 * symbol/keyword) by treating the inner as a
                                 * keyword.  Compound `: (-> a b)` still routes
                                 * through the F_TYPE_ANN branch below. */
                                if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1) {
                                    Form *inner = ret_f->as.list.items[0];
                                    if (inner->tag == F_SYM || inner->tag == F_KEYWORD) {
                                        ret_f = inner;
                                    } else if (inner->tag == F_NIL) {
                                        /* `: nil` -- the bare `nil` literal in a type
                                         * position is parsed as F_NIL by the reader;
                                         * it means the nil/void return type. */
                                        return_kind = TY_NIL;
                                        ret_f = NULL;
                                    }
                                }
                                if (ret_f && (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM)) {
                                    const Symbol *kw = ret_f->as.sym;
                                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                        return_kind = TY_INT;
                                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                        return_kind = TY_BOOL;
                                    } else if ((kw->len == 5 && memcmp(kw->name, "float", 5) == 0) ||
                                               (kw->len == 7 && memcmp(kw->name, "float64", 7) == 0)) {
                                        /* proper-tail-calls T5: a `: float` defn called
                                         * before its definition -- the shape every mutual
                                         * pair has on one side -- forward-typed as the
                                         * TY_INT default, so `(if c x (g ...))` with a
                                         * float `x` was a spurious "then=float else=int".
                                         * The defmodule pre-pass (elab_module.c) and the
                                         * letrec peek (elab_forms.c) already had this
                                         * arm; the top-level twin never did.  `float32`
                                         * stays out for the reason the letrec peek gives:
                                         * its register class is policed by E0707. */
                                        return_kind = TY_FLOAT;
                                    } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                                        return_kind = TY_NIL;
                                    } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                                        /* SS3a: :nil return type must forward-declare as void,
                                         * not TY_INT, so recursive nil-returning functions
                                         * correctly infer TY_NIL for their body type. */
                                        return_kind = TY_NIL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                        return_kind = TY_CSTR;
                                    } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                                        return_kind = TY_PTR_VOID;
                                    } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                                        return_kind = TY_PTR_VOID;
                                    } else if (kw->len == 3 && memcmp(kw->name, "any", 3) == 0) {
                                        /* r7rs-lang-plan R7: an annotated `: any`
                                         * result forward-declared as the TY_INT
                                         * default, so a caller elaborated before
                                         * the callee widened the tagged result as
                                         * an int (cc: "aggregate value used where
                                         * an integer was expected"). */
                                        return_kind = TY_ANY;
                                    } else {
                                        /* bare-adt-forward-decl-inference: a non-parametric
                                         * user type name -- `: T` for a `defdata` /
                                         * `defstruct` / `defopaque` T (RF0 has already
                                         * registered the stub) -- must forward-declare with
                                         * that ADT's real result type, not the TY_INT
                                         * default.  Without this, a sibling caller declared
                                         * *earlier* in the module (mutual / forward
                                         * recursion) types the call as `int`, and a `match`
                                         * arm returning the ADT then reports a spurious
                                         * "arm types incompatible -- expected int, got adt".
                                         * (docs/archive/history/logic-port-language-gaps.md GAP 2.) */
                                        for (uint32_t ai = 0; ai < ep->n_adt_defs; ai++) {
                                            if (strcmp(ep->adt_defs[ai]->name, kw->name) == 0) {
                                                Type adt_ty = type_adt(ep->adt_defs[ai]);
                                                Type *tt = (Type *)arena_alloc(
                                                    arena, sizeof(Type));
                                                *tt = adt_ty;
                                                return_kind = adt_ty.kind;
                                                fwd_result_full = tt;
                                                break;
                                            }
                                        }
                                    }
                                } else if (ret_f && ret_f->tag == F_TYPE_ANN && ret_f->as.list.len > 0) {
                                    /* Compound return type: peek at the head symbol to
                                     * recognize Session[P] returns for pass-1 forward decls. */
                                    Form *head_f = ret_f->as.list.items[0];
                                    /* The compound annotation payload is either a bare
                                     * symbol (`Session`) or an application list
                                     * (`(PRes Expr)`); recover the type-constructor
                                     * symbol from whichever shape it is. */
                                    const Symbol *tycon_sym = NULL;
                                    if (head_f->tag == F_SYM) {
                                        tycon_sym = head_f->as.sym;
                                    } else if (head_f->tag == F_LIST &&
                                               head_f->as.list.len > 0 &&
                                               head_f->as.list.items[0]->tag == F_SYM) {
                                        tycon_sym = head_f->as.list.items[0]->as.sym;
                                    }
                                    if (tycon_sym &&
                                            strcmp(tycon_sym->name, "Session") == 0) {
                                        return_kind = TY_SESSION;
                                    } else if (tycon_sym) {
                                        /* defdata-parametric-forward-decl-inference:
                                         * a compound `(F A B)` return whose head is a
                                         * registered ADT/struct stub (RF0 has run) --
                                         * e.g. (PRes Expr), (Option Foo), (Vec T).
                                         * Build the full TY_APP so a sibling caller
                                         * declared earlier in the module resolves the
                                         * scrutinee's concrete type args (otherwise the
                                         * pattern binding falls back to the placeholder
                                         * carrier and downstream constructor arms
                                         * mismatch). Gated on a registered ADT head so
                                         * fn_type_from_form only walks pre-registered
                                         * names and cannot emit a spurious diagnostic. */
                                        bool head_is_adt = false;
                                        for (uint32_t ai = 0; ai < ep->n_adt_defs; ai++) {
                                            if (strcmp(ep->adt_defs[ai]->name,
                                                       tycon_sym->name) == 0) {
                                                head_is_adt = true;
                                                break;
                                            }
                                        }
                                        if (head_is_adt) {
                                            /* Collect the defn's own type params (poly
                                             * defn) so a return like (PRes A) keeps A as
                                             * a named tyvar rather than an unknown name. */
                                            const Symbol *tp_syms[MAX_FN_ARITY];
                                            Kind tp_kinds[MAX_FN_ARITY];
                                            uint8_t n_tp = 0;
                                            if (params_idx_local > name_idx + 1 &&
                                                (uint32_t)f->as.list.len > name_idx + 1 &&
                                                f->as.list.items[name_idx + 1]->tag == F_VEC) {
                                                Form *tpv = f->as.list.items[name_idx + 1];
                                                for (uint32_t ti = 0;
                                                     ti < tpv->as.list.len && n_tp < MAX_FN_ARITY;
                                                     ti++) {
                                                    Form *tf = tpv->as.list.items[ti];
                                                    if (tf->tag == F_SYM) {
                                                        tp_syms[n_tp] = tf->as.sym;
                                                        tp_kinds[n_tp] = KIND_STAR;
                                                        n_tp++;
                                                    }
                                                }
                                            }
                                            (void)tp_kinds;
                                            Type *ann = fwd_shallow_result_app(
                                                ep, head_f, tp_syms, n_tp);
                                            /* forward-call-to-aggregate-result-
                                             * types-as-carrier: not a result
                                             * over the defn's OWN type
                                             * parameters in a typed file.  The
                                             * forward decl has no parameter
                                             * types to instantiate them from,
                                             * so a caller above `(defn wrap [A]
                                             * [x : A] : (Option A) ...)` typed
                                             * `(some (wrap x))` with `A` unbound,
                                             * no spec was minted, and the
                                             * `(Option A)` carrier box was read
                                             * as the caller's by-value `(Option
                                             * (Option int))`: a silent wrong
                                             * answer.  The placeholder makes it
                                             * a compile error instead. */
                                            if (ann && ann->kind == TY_APP &&
                                                (lang_span_is_dynamic(f->span) ||
                                                 !fwd_type_mentions_tp(ann, tp_syms, n_tp))) {
                                                return_kind = TY_APP;
                                                fwd_result_full = ann;
                                            }
                                        }
                                    }
                                }
                            }
                            /* Count actual arity + scalar arg kinds from the
                             * params vector.  fwd_decl_scan_params skips
                             * `^`-prefixed markers (^fat/^mut/...) so the
                             * forward-declared arity is not over-stated (see
                             * docs/archive/history/pap-defmodule-fat-fn-too-many-args.md). */
                            TypeKind *arg_kinds = NULL;
                            /* `params_idx_local`, not `name_idx + 1`: for a
                             * generic defn -- `(defn f [V] [params] : R ...)`,
                             * or with a constraint vec, `[V] [(C V)] [params]`
                             * -- the vector right after the name is the TYPE
                             * parameters.  Scanning that as the value params
                             * gave the forward declaration the wrong arity (1,
                             * for `[V]`), so a caller written ABOVE the callee
                             * saw a 1-arg function, and a 2-arg call reported
                             * "returns int, which is not callable -- did you
                             * mean to pass all 1 argument(s)?".  Defining the
                             * callee first avoided it, which is what made this
                             * look like an ordering rule.  The return-type probe
                             * a few lines up already skips the type-param vec;
                             * the arity scan never did. */
                            uint32_t param_arity = (params_idx_local < (uint32_t)f->as.list.len)
                                ? fwd_decl_scan_params(arena, f->as.list.items[params_idx_local], &arg_kinds)
                                : 0;
                            /* saffron-dynamic-surface-pass H6: an UNANNOTATED
                             * Saffron return is `any` (D3), and this forward
                             * decl is what a caller elaborated EARLIER than the
                             * callee sees -- elab_defn's S4 early-forward only
                             * runs once the callee's own pass 2 starts, which is
                             * too late for `(defn user [] (+ (later) 1))` above
                             * `(defn later [] 7.1)` (cc: invalid operands to
                             * binary +) and for mutual recursion (`then=bool
                             * else=int`).  Same `main` exception as elab_defn:
                             * the zero-arity entry point stays `int`. */
                            if (!ret_annotated && lang_span_is_dynamic(f->span) &&
                                !(name_f->as.sym->len == 4 &&
                                  memcmp(name_f->as.sym->name, "main", 4) == 0 &&
                                  param_arity == 0)) {
                                return_kind = TY_ANY;
                                fwd_result_full = NULL;
                            }
                            /* r7rs-lang-plan R3: a compound parameter type in a
                             * dynamic file rides the forward decl in full -- see
                             * elab_fwd_param_full_types. */
                            Type **fwd_arg_full = elab_fwd_param_full_types(
                                ep, arena, f, name_idx, params_idx_local,
                                param_arity, arg_kinds);
                            Type fn_type = type_fn(arg_kinds, param_arity, return_kind);
                            /* r7rs-lang-plan R7: a variadic's forward decl
                             * carries the rest shape, so a caller elaborated
                             * before the definition packs its surplus
                             * arguments (it used to see a fixed arity that
                             * counted the `&` as a parameter). */
                            if (fwd_arg_full) fn_type.as.fn.arg_full_types = fwd_arg_full;
                            if (params_idx_local < (uint32_t)f->as.list.len)
                                fwd_decl_apply_variadic(ep, arena, &fn_type,
                                                        f->as.list.items[params_idx_local]);
                            fwd_arg_full = fn_type.as.fn.arg_full_types;
                            /* defdata-parametric-forward-decl-inference: carry the
                             * full compound result type on the forward decl. */
                            if (fwd_result_full) {
                                fn_type.as.fn.result_full_type = fwd_result_full;
                            }
                            if (fwd_arg_full) {
                                /* The seam indexes arg_full_types by parameter
                                 * and tolerates NULL for a slot without one. */
                                fn_type.as.fn.arg_full_types = fwd_arg_full;
                            }
                            /* MF3: if the name is already in global scope (e.g. an
                             * auto-loaded stdlib defn), do NOT pre-register a
                             * duplicate forward decl. Pass 2's elab_defn will then
                             * see the original binding (with is_from_stdlib set
                             * correctly) and either reuse it as a forward decl or
                             * emit the shadow diagnostic. */
                            if (!scope_lookup(&ep->global, name_f->as.sym)) {
                                Binding *b = binding_new(ep, name_f->as.sym, fn_type, false, true, f->span);
                                scope_add(&ep->global, b);
                            }
                        }
                    }
                    next_form:;
                }
            }
        }
}

/* TR2: session lifecycle. A session is just a heap-allocated Elab whose state
 * survives between calls; `arena`/`st` are re-pointed at each call's arena. The
 * zeroed `arena` field marks a session that has not been initialised yet. */
ElabSession *elab_session_new(void) {
    Elab *s = (Elab *)calloc(1, sizeof(Elab));
    return (ElabSession *)s;
}

void elab_session_free(ElabSession *session) {
    Elab *e = (Elab *)session;
    if (!e) return;
    if (e->arena) {   /* initialised: tear down the same state the non-session
                       * path frees at return */
        scope_free(&e->global);
        free(e->adt_defs);
        free(e->forward_type_syms);
        free(e->handled_effect_names);
        free(e->pending_reset_nodes);
        free(e->macros);
        free(e->macro_expansion_stack);
        free(e->loaded_modules);
        free(e->dynvar_entries);
        free(e->active_dynvar_bindings);
        free((void *)e->load_expanded_paths);
        free(e->file_scope_defs);
        elab_macro_env_dispose(e->macro_env);
    }
    free(e);
}

Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation,
                        bool sandboxed,
                        TypeClassEnv *out_tc_env,
                        const char **include_dirs,
                        int n_include_dirs,
                        uint32_t *out_n_file_scope_defs,
                        struct ReaderMacroRegistry *user_macros) {
    return elaborate_program_session(arena, st, forms, nforms, stdlib_prefix,
                                     module_base_dir, separate_compilation,
                                     sandboxed, out_tc_env, include_dirs,
                                     n_include_dirs, out_n_file_scope_defs,
                                     user_macros, /*session=*/NULL);
}

Expr *elaborate_program_session(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation,
                        bool sandboxed,
                        TypeClassEnv *out_tc_env,
                        const char **include_dirs,
                        int n_include_dirs,
                        uint32_t *out_n_file_scope_defs,
                        struct ReaderMacroRegistry *user_macros,
                        ElabSession *session) {
    Elab e;
    /* TR2: restore accumulated state from the session (a plain struct copy --
     * every field is POD or a malloc'd/arena pointer). `scope` is the one
     * self-referential field (it points at `global` inside the struct), so it
     * is re-anchored after the copy; a session is only ever saved at top level,
     * where scope == &global and no fn-entry scope is open. */
    Elab *sess = (Elab *)session;
    const bool sess_live = (sess && sess->arena != NULL);
    if (sess_live) {
        e = *sess;
        e.arena = arena;
        e.st    = st;
        e.scope = &e.global;
        e.fn_entry_outer_scope = NULL;
        /* Per-FILE state must NOT carry across calls: each incremental call is
         * conceptually a new file (a REPL turn, or the next preloaded stdlib
         * module). Without this reset, `has_defmodule` stays true after the
         * first defmodule-wrapped file and every later one fails with "only one
         * defmodule is allowed per file" -- which is exactly what the
         * whole-program path avoided by resetting at the stdlib_prefix/file
         * boundary. Accumulated state (scope, typeclasses, registries,
         * loaded_modules) is what we deliberately keep. */
        e.has_defmodule       = false;
        e.current_module_name = NULL;
        e.current_module      = NULL;
        /* Arena-allocated in an earlier call's arena; a module's pending
         * forward results never outlive the module anyway. */
        e.fwd_pending_results = NULL;
        /* PS4: everything defined so far belongs to earlier turns. */
        e.turn_continues_session = true;
        e.turn_start_n_globals   = e.global.n;
        e.turn_start_n_macros    = e.n_macros;
        e.turn_start_n_adt_defs  = e.n_adt_defs;
        e.turn_start_n_effects   = e.effect_env ? e.effect_env->n_effects : 0;
        e.turn_start_instances   = e.typeclass_env.instances;
        e.turn_start_n_refine_call_sites = e.n_refine_call_sites;
    } else {
        elab_init_state(&e, arena, st);
    }
    e.user_macros = user_macros;
    e.module_base_dir = module_base_dir ? module_base_dir : ".";
    /* stdlib fallback: TUR_STDLIB_DIR env var, else "stdlib" */
    {
        const char *sdir = getenv("TUR_STDLIB_DIR");
        e.module_stdlib_dir = (sdir && *sdir) ? sdir : "stdlib";
    }
    e.module_include_dirs   = include_dirs;
    e.n_module_include_dirs = n_include_dirs;
    /* LS2: pull workspace-resolution context published by main.c around
     * the compile_to_c call (NULL outside that path -- the warning code
     * then silently no-ops). */
    {
        const Ls2ResolverCtx *ctx = ls2_resolver_ctx_active();
        if (ctx && ctx->n_inc == n_include_dirs) {
            e.module_include_workspace_producer = ctx->producer_per_inc;
            e.module_include_warned             = ctx->warned_per_inc;
            e.module_consumer_declared_spices   = ctx->declared_spices;
            e.n_module_consumer_declared_spices = ctx->n_declared_spices;
        }
    }
    e.separate_compilation  = separate_compilation;
    e.sandboxed             = sandboxed;
    builtins_init(st);

    int rc = 0;

    /* Seed the load-visited set with the auto-loaded stdlib files. main.c
     * pre-splices the stdlib forms[0..stdlib_prefix) directly (not as `(load
     * ...)` forms), so they never pass through the dedup below. Registering
     * their canonical paths here makes a later explicit `(load
     * "stdlib/json.tur")` of an already-auto-loaded module (json/schema are
     * unconditionally auto-loaded since the -X reader flags became no-ops) a
     * no-op instead of a "already defined by an auto-loaded stdlib module"
     * collision. Distinct file_ids in the stdlib region map 1:1 to the
     * auto-loaded source files. */
    {
        uint16_t seen_file = (uint16_t)-1;
        for (uint32_t i = 0; i < stdlib_prefix && i < nforms; i++) {
            uint16_t fid = forms[i]->span.file_id;
            if (fid == seen_file) continue;  /* runs are contiguous per file */
            seen_file = fid;
            const SourceFile *sf = diag_source_file(fid);
            if (sf && sf->path)
                load_dedup_register(&e, load_path_key(st, sf->path, e.module_stdlib_dir));
        }
    }

    /* Phase M: (load "path") preprocessing.
     * Expand all top-level (load "path") forms in place, depth-first and in
     * source order, parsing each referenced file and splicing its (already
     * expanded) forms into the form list. A shared visited set keeps each path
     * to a single expansion and breaks cycles. Loaded forms then participate
     * in the normal two-pass elaboration. */
    {
        LoadExpandCtx lx = {0};
        lx.out_cap = nforms + 16;
        lx.out = (Form **)arena_alloc(arena, lx.out_cap * sizeof(Form *));
        lx.track_boundary = (stdlib_prefix > 0);
        lx.boundary_in    = stdlib_prefix;
        lx.boundary_out   = stdlib_prefix; /* default if the loop never crosses it */
        if (e.sandboxed) {
            bool found = false;
            uint32_t first_new = stdlib_prefix < nforms ? stdlib_prefix : nforms;
            Form **kept = (Form **)arena_alloc(arena, (nforms + 1) * sizeof(Form *));
            uint32_t nk = 0;
            for (uint32_t i = 0; i < nforms; i++) {
                Form *f = (i < first_new) ? forms[i]
                                          : sandbox_strip_loads(&e, arena, forms[i], &found);
                if (f) kept[nk++] = f;
            }
            if (found) {
                rc = -1;
                forms  = (Form *const *)kept;
                nforms = nk;
            }
        }
        load_expand_forms(&lx, &e, arena, st, forms, nforms);
        if (lx.rc != 0) rc = lx.rc;
        /* If the boundary sat at the very end (no user forms), the loop never
         * reached an index == boundary_in, so the post-expansion stdlib region
         * is everything emitted. */
        if (lx.track_boundary && stdlib_prefix >= nforms)
            lx.boundary_out = lx.out_n;
        /* Replace forms/nforms with the expanded list for the rest of
         * elaborate_program.  Cast away const since we're in our own copy. */
        forms = (Form *const *)lx.out;
        nforms = lx.out_n;
        /* Re-anchor the stdlib boundary onto the expanded form stream so the
         * stdlib promotion sweep and in_stdlib_load window line up with where
         * the auto-loaded forms actually landed. */
        if (lx.track_boundary) stdlib_prefix = lx.boundary_out;
    }

    /* r7rs-lang-plan R2: lower the Scheme core forms of every `#lang r7rs`
     * file in the (now load-expanded) program onto Turmeric's own forms.
     * Here, after load expansion and before the `main` fold and the two
     * elaboration passes, so a loaded Scheme file is lowered too and a
     * lowered top-level expression is an ordinary statement for the fold.
     * Per-form off the span's file, so stdlib and Turmeric forms are passed
     * through by pointer and the stdlib prefix keeps its index. */
    if (scheme_lower_needed(forms, nforms)) {
        uint32_t lowered_n = 0;
        forms  = (Form *const *)scheme_lower_program(arena, st, forms, nforms, &lowered_n,
                                                     elab_scheme_library_path,
                                                     elab_scheme_global_kind,
                                                     elab_scheme_stdlib_file, &e);
        nforms = lowered_n;
    }

    Expr **items = (nforms == 0) ? NULL :
        (Expr **)arena_alloc(arena, nforms * sizeof(Expr *));

    /* Phase RF0: Type pre-pass -- pre-register all top-level defstruct and defdata
     * names as forward stubs BEFORE any elaboration, so that mutually recursive type
     * definitions can reference each other regardless of declaration order. */
    for (uint32_t i = 0; i < nforms; i++) {
        Form *f = forms[i];
        /* r7rs-programs-compile-slowly: a stdlib type's stub id comes from the
         * stdlib's counter, like everything else of the stdlib's. */
        e.ids_stdlib = (i < stdlib_prefix);
        if (f->tag != F_LIST || f->as.list.len < 2) continue;
        Form *head = f->as.list.items[0];
        if (!head || head->tag != F_SYM) continue;
        bool is_defstruct = (head->as.sym == e.sym_defstruct);
        bool is_defdata   = (head->as.sym == e.sym_defdata);
        bool is_defgadt   = (head->as.sym == e.sym_defgadt);
        bool is_defopaque = (head->as.sym == e.sym_defopaque);
        if (!is_defstruct && !is_defdata && !is_defgadt && !is_defopaque) continue;
        /* range-gadt-typeclass-migration-plan A1: GADTs are registered
         * unconditionally. */
        Form *name_f = f->as.list.items[1];
        if (!name_f || name_f->tag != F_SYM) continue;
        const Symbol *type_name = name_f->as.sym;
        /* Skip if already in scope (e.g. stdlib defines a type with this name) */
        if (scope_lookup(&e.global, type_name)) continue;
        /* Pre-allocate a stub def and register a forward binding */
        elab_add_forward_type(&e, type_name);
        /* DS5: memset each stub before populating named fields so newly
         * added bool / scalar fields default to 0 -- arena_alloc returns
         * uninitialised memory and UBSan tripped on reads of
         * `is_opaque` / other bools that subsequent passes (e.g. emit
         * pass 0 in emit_module.c) consult. */
        if (is_defopaque) {
            /* structdef-retirement slice 5: an opaque newtype is now an opaque
             * AdtDef (n_ctors == 0), not a StructDef, so StructDef can be retired.
             * The stub mirrors the old struct stub: a copyable, opaque, named
             * int64 carrier whose phantom arity makes `(Name A)` annotations
             * kind-check before the full defopaque elaboration fills it in. */
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            stub->is_copy = true;
            stub->is_opaque = true;
            stub->origin_file_id = name_f->span.file_id;
            /* Parameterized opaque: an optional [A ...] vector after the name
             * makes this a type constructor.  Record the arity on the stub so
             * `(Name A)` annotations resolved before the full defopaque
             * elaboration kind-check against the right arrow kind. */
            uint8_t opaque_arity = 0;
            if (f->as.list.len >= 3 && f->as.list.items[2]->tag == F_VEC) {
                opaque_arity = (uint8_t)f->as.list.items[2]->as.list.len;
            }
            stub->n_type_params = opaque_arity;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            t.hkt_kind = kind_for_arity(opaque_arity);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defstruct) {
            /* CONV-S1 (defstruct-as-defadt): a defstruct lowers to a
             * single-variant record defadt, so pre-register an ADT stub (not a
             * struct stub) -- the later elab_defstruct rewrite to elab_defdata
             * fills this stub exactly as a real defdata would.  The lowering is
             * unconditional (every field shape lowers or is rejected at the
             * ADT field parser), so there is no struct-stub branch. */
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defgadt) {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            stub->is_gadt = true;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        }
    }

    e.ids_stdlib = false;

    /* v2 sole-effect-lowering (top-level-handle taint root): fold trailing
     * top-level STATEMENT forms into a synthesized `(defn main [] : int
     * (do <stmts> 0))` so an effect handler written at top level -- the idiomatic
     * `(println (handle ...))` with no explicit `main` -- flows through the CPS/DK
     * backend like a user main.  Without this the statements are direct/fiber-
     * emitted into a synthesized `int main()` that never reaches the CPS
     * classifier, so the top-level handle stays on the fiber, base_taints its
     * effect, and every performer of that effect is forced onto the fiber
     * (SIG-TAINT).  See docs/archive/cps-toplevel-synthesized-main-bypasses-dk.md.
     *
     * CONSERVATIVE + macro-safe: only fires when there is NO user `main` and
     * every user-region top-level form is cleanly classifiable WITHOUT expanding
     * macros -- either a definition/directive (head name starts with "def", or a
     * known module directive; stays top-level) or a plain non-macro call (folded
     * into main).  A macro call is ambiguous (it may expand to a top-level def OR
     * a statement -- see macro-emits-multiple-top-level-forms), a top-level `do`
     * progn may carry defs, and a bare atom would be dropped once the historical
     * synthesized-main path is suppressed; ANY of these in the user region aborts
     * the fold and preserves the historical behaviour exactly.  Only the entry
     * unit is transformed (never a separately-compiled module).
     *
     * This used to be GATED on --enable=cps-tramp-resume, because flag-off the
     * base CPS admissible subset was narrower and the N6.5 direct/fiber fallback
     * is retired, so a synthesized d2b `main` whose handle subtree left the
     * subset would HARD-ERROR instead of gracefully fibering.  cps-tramp-resume
     * graduated 2026-07-19, so the fold is unconditional and the historical
     * synthesized-`int main()` path it fell back to is gone. */
    /* Interpreter parity (tur-eval-prints-fn-main): the tree-walking interpreter
     * never reaches the CPS/DK backend this fold exists to feed, and folding a
     * bare top-level expression into `(defn main [] : int (do <expr> 0))` makes
     * turi_eval return the synthesized `main` closure (`#<fn main>`) instead of
     * the expression's value -- and, at the non-`main`-calling entry points
     * (`tur eval '<expr>'`, the non-interactive REPL), the statements never run
     * at all.  Under the interpreter every top-level form is evaluated directly
     * in turi_eval_impl's loop, so leaving statements top-level both runs their
     * side effects and surfaces the last expression's value.  The fold graduated
     * to default-on with `cps-tramp-resume` (2026-07-19, unconditional since it
     * graduated), which is what regressed the interpreter eval/REPL value
     * display; suppress it under g_interpret_mode to restore correct value
     * semantics while keeping the compiled-path fold. */
    if (!g_interpret_mode
        && !separate_compilation && stdlib_prefix <= nforms) {
        /* Collect every defmacro name (user + stdlib) so a macro-headed
         * top-level statement can be detected.  Also record each macro's
         * defmacro form so a statement-producing macro (template head is a
         * special form / plain call, provably not a top-level def) can be
         * FOLDED into the synthesized main instead of aborting the fold. */
        const Symbol **macro_names = NULL;
        const Form **macro_defs = NULL;
        uint32_t n_macro = 0, cap_macro = 0;
        for (uint32_t i = 0; i < nforms; i++) {
            Form *f = forms[i];
            if (!f || f->tag != F_LIST || f->as.list.len < 2) continue;
            Form *h = f->as.list.items[0];
            if (!h || h->tag != F_SYM || h->as.sym != e.sym_defmacro) continue;
            Form *nm = f->as.list.items[1];
            if (!nm || nm->tag != F_SYM) continue;
            if (n_macro == cap_macro) {
                cap_macro = cap_macro ? cap_macro * 2 : 16;
                macro_names = realloc(macro_names, cap_macro * sizeof(*macro_names));
                macro_defs  = realloc(macro_defs,  cap_macro * sizeof(*macro_defs));
            }
            macro_defs[n_macro]    = f;
            macro_names[n_macro++] = nm->as.sym;
        }
        bool have_user_main = false, ambiguous = false, any_stmt = false;
        for (uint32_t i = stdlib_prefix; i < nforms && !ambiguous; i++) {
            Form *f = forms[i];
            if (!f) continue;
            if (f->tag == F_CBLOCK) continue;             /* file-scope C stays */
            if (f->tag != F_LIST || f->as.list.len == 0) { ambiguous = true; break; }
            Form *head = f->as.list.items[0];
            if (!head || head->tag != F_SYM || !head->as.sym->name) { ambiguous = true; break; }
            const Symbol *hs = head->as.sym;
            const char *hn = hs->name;
            /* definition: any def* head stays top-level */
            if (hn[0] == 'd' && hn[1] == 'e' && hn[2] == 'f') {
                /* toplevel-def-initializers-run-before-toplevel-expressions:
                 * a `def` AFTER a statement, with an initializer that does
                 * work (a call), must run that work in source order.  The
                 * folded main cannot hold a `def`, and a top-level `def`'s
                 * initializers all run in __tur_module_def_init ahead of main.
                 * So abort the fold: on the historical path every top-level
                 * form, initializer and statement alike, is a statement of the
                 * synthesized `int main()` at its source position
                 * (emit_module.c, the EX_DEF arm).  A literal or `fn` init has
                 * nothing to order, and a `^deferred-init` def runs its
                 * initializer in the body itself (the Scheme lowering's define
                 * arm), which keeps the fold and its CPS main for a Scheme
                 * program. */
                if (hs == e.sym_def && any_stmt) {
                    uint32_t ii = f->as.list.len;
                    const Form *init = ii >= 3 ? f->as.list.items[ii - 1] : NULL;
                    bool deferred = false;
                    for (uint32_t k = 1; k < ii; k++)
                        if (f->as.list.items[k]->tag == F_SYM
                            && f->as.list.items[k]->as.sym == e.sym_caret_deferred_init) deferred = true;
                    if (!deferred && init && init->tag == F_LIST && init->as.list.len > 0) {
                        const Form *ih = init->as.list.items[0];
                        bool trivial = ih && ih->tag == F_SYM
                                       && (ih->as.sym == e.sym_fn || ih->as.sym == e.sym_lambda);
                        if (!trivial) { ambiguous = true; break; }
                    }
                }
                if (hs == e.sym_defn) {
                    uint32_t ni = 1;   /* skip ^attr / (export-as ..) prefix syms */
                    while (ni < f->as.list.len && f->as.list.items[ni]->tag == F_SYM
                           && f->as.list.items[ni]->as.sym->name
                           && f->as.list.items[ni]->as.sym->name[0] == '^') ni++;
                    if (ni < f->as.list.len && f->as.list.items[ni]->tag == F_SYM
                        && f->as.list.items[ni]->as.sym->len == 4
                        && memcmp(f->as.list.items[ni]->as.sym->name, "main", 4) == 0)
                        have_user_main = true;
                }
                continue;
            }
            /* module directives stay top-level */
            if (hs == e.sym_import || hs == e.sym_load || hs == e.sym_export
                || hs == e.sym_extern_c || hs == e.sym_defmodule) continue;
            /* macro call / do progn / quote form: ambiguous -> abort the fold */
            if (hs == e.sym_do) { ambiguous = true; break; }
            /* fn-body-only forms (`?`, `return`) are illegal at top level and are
             * rejected during elaboration by an `fn_body_depth == 0` check.  Folding
             * one into the synthesized main body would put it INSIDE a function,
             * suppressing that rejection and surfacing a misleading downstream error
             * (e.g. top-level `(? 42)` -> "requires a Result value" instead of "only
             * allowed inside a function body").  Abort the fold so the form stays
             * top-level and keeps its correct diagnostic -- such a program is a
             * compile error either way, so the historical path is exactly right. */
            if (hs == e.sym_question || hs == e.sym_return) { ambiguous = true; break; }
            /* A `def`/`define` nested inside the statement binds a global at
             * file scope; folding it into main would relocate it into a
             * function body and reject it.  Keep the form top-level. */
            if (form_has_nested_def(&e, f, false)) { ambiguous = true; break; }
            int macro_idx = -1;
            for (uint32_t m = 0; m < n_macro; m++)
                if (macro_names[m] == hs) { macro_idx = (int)m; break; }
            if (macro_idx >= 0) {
                /* A macro whose template provably expands to a STATEMENT (head is
                 * a special form / plain call, not a def / directive / do / nested
                 * macro) is fold-safe: folded into main's do-body it expands there
                 * and DK-lowers, exactly like a literal top-level handle.  Reject
                 * (keep the historical fiber path) when we cannot prove that, or
                 * when the template OR the call args carry a fold-risky shape
                 * (escaping-mutable set!-in-handle). */
                const Form *tmpl = NULL;
                if (!macro_form_stmt_safe(&e, macro_defs[macro_idx],
                                          macro_names, n_macro, &tmpl)
                    || fold_stmt_is_risky(&e, tmpl)
                    || fold_stmt_is_risky(&e, f)) {
                    ambiguous = true; break;
                }
                any_stmt = true;
                continue;
            }
            /* a plain call: a genuine top-level statement -- but abort the fold
             * if its handle subtree is a shape the DK backend miscompiles (nested
             * handle / escaping-mut set!), leaving it on the historical fiber path. */
            if (fold_stmt_is_risky(&e, f)) { ambiguous = true; break; }
            /* a top-level unhandled perform in a PLAIN-call statement must keep its
             * compile-time TUR-E0008 (elab_effects.c) rather than be folded into
             * main and deferred to a runtime abort (errors/effect-unhandled).  Only
             * applied here, in the plain-call branch: a MACRO-call statement (handled
             * above) may install a handler via its template expansion (e.g.
             * with-fail-println wraps its body in a handle), which this pre-expansion
             * lexical scan cannot see, so scanning it would wrongly abort a fold that
             * DK-lowers fine after expansion (effect-with-fail / effect-with-write). */
            if (form_has_toplevel_unhandled_perform(&e, f)) { ambiguous = true; break; }
            any_stmt = true;
        }
        free(macro_names);
        free(macro_defs);

        if (!have_user_main && !ambiguous && any_stmt) {
            /* Build `(defn main [] : int (do <stmts...> 0))`, keeping every
             * definition/directive in place and collecting the statement forms
             * in source order. */
            Span sp = (nforms > 0) ? forms[nforms - 1]->span : (Span){0,0,0,0,0,0};
            Form **new_forms = (Form **)arena_alloc(arena, (nforms + 1) * sizeof(Form *));
            uint32_t nn = 0;
            Form **stmts = (Form **)arena_alloc(arena, (nforms + 1) * sizeof(Form *));
            uint32_t n_stmts = 0;
            for (uint32_t i = 0; i < nforms; i++) {
                Form *f = forms[i];
                bool is_stmt = false;
                if (i >= stdlib_prefix && f && f->tag == F_LIST && f->as.list.len > 0
                    && f->as.list.items[0]->tag == F_SYM) {
                    const char *hn = f->as.list.items[0]->as.sym->name;
                    const Symbol *hs = f->as.list.items[0]->as.sym;
                    bool is_def = hn && hn[0]=='d' && hn[1]=='e' && hn[2]=='f';
                    bool is_dir = (hs == e.sym_import || hs == e.sym_load
                                   || hs == e.sym_export || hs == e.sym_extern_c
                                   || hs == e.sym_defmodule);
                    is_stmt = !is_def && !is_dir;
                }
                if (is_stmt) stmts[n_stmts++] = f;
                else         new_forms[nn++] = f;
            }
            /* do-body: (do stmt1 .. stmtN 0) */
            Form **do_items = (Form **)arena_alloc(arena, (n_stmts + 2) * sizeof(Form *));
            do_items[0] = form_sym(arena, sp, e.sym_do);
            for (uint32_t i = 0; i < n_stmts; i++) do_items[i + 1] = stmts[i];
            do_items[n_stmts + 1] = form_int(arena, sp, 0);
            Form *do_body = form_list(arena, sp, do_items, n_stmts + 2);
            /* (defn main [] : int <do_body>) */
            Form **defn_items = (Form **)arena_alloc(arena, 5 * sizeof(Form *));
            defn_items[0] = form_sym(arena, sp, e.sym_defn);
            defn_items[1] = form_sym(arena, sp, intern_cstr(st, "main"));
            defn_items[2] = form_vec(arena, sp, NULL, 0);
            defn_items[3] = form_type_ann(arena, sp, form_sym(arena, sp, intern_cstr(st, "int")));
            defn_items[4] = do_body;
            new_forms[nn++] = form_list(arena, sp, defn_items, 5);
            forms = (Form *const *)new_forms;
            nforms = nn;
        }
    }

    /* Phase 2: Two-pass elaboration for mutual recursion support.
     * Pass 1: Collect all top-level defn declarations and add them to scope.
     * This allows mutually recursive functions to see each other. */
    e.in_stdlib_load = (stdlib_prefix > 0);
    for (uint32_t i = 0; i < nforms; i++) {
        if (i == stdlib_prefix) e.in_stdlib_load = false;
        elab_pre_declare_toplevel_defn(&e, arena, forms[i]);
    }

    /* Phase M0+: Validate defmodule position per source file.
     *
     * "defmodule must be the first form in the file" -- where "the file"
     * is the source file that contributes the form, not the whole
     * compilation unit.  Each (load ...)-spliced file gets its own
     * scope for the check, so a defmodule-wrapped spice file loaded
     * after a flat stdlib helper is accepted.
     *
     * The check uses forms[i]->span.file_id as the file-of-origin key;
     * each loaded SourceFile is assigned a unique id at parse time
     * (see the (load ...) preprocessing loop above, line ~668).
     *
     * Continues past the first defmodule so misplaced defmodules in
     * later loaded files also surface. */
    {
        uint32_t cur_file       = (uint32_t)-1;
        uint32_t file_start_idx = stdlib_prefix;
        for (uint32_t i = stdlib_prefix; i < nforms; i++) {
            Form *f = forms[i];
            if (f->span.file_id != cur_file) {
                cur_file       = f->span.file_id;
                file_start_idx = i;
            }
            if (f->tag != F_LIST || f->as.list.len == 0) continue;
            Form *head = f->as.list.items[0];
            if (head->tag != F_SYM || head->as.sym != e.sym_defmodule) continue;
            if (i == file_start_idx) continue;
            /* If the file-start form is itself a defmodule, defer to the
             * "only one defmodule is allowed per file" check in
             * elab_module.c -- that diagnostic is more specific. */
            Form *first = forms[file_start_idx];
            if (first->tag == F_LIST && first->as.list.len > 0) {
                Form *fh = first->as.list.items[0];
                if (fh->tag == F_SYM && fh->as.sym == e.sym_defmodule) continue;
            }
            diag_emit(DIAG_ERROR, head->span,
                      "defmodule must be the first form in the file");
            diag_emit(DIAG_NOTE, forms[file_start_idx]->span,
                      "this form comes before defmodule in the same file; "
                      "move it inside the defmodule body or below it");
            rc = -1;
        }
    }
    if (rc != 0) {
        /* TR2: with a session the state is owned by the caller -- save it back
         * (so elab_session_free reclaims it exactly once) instead of freeing
         * here. The caller must discard the session after any failure: partial
         * definitions from this program may already have entered its scope. */
        if (sess) { *sess = e; return NULL; }
        scope_free(&e.global);
        free(e.adt_defs);
        free(e.forward_type_syms);
        free(e.handled_effect_names);
        free(e.pending_reset_nodes);
        free(e.macros);
        free(e.macro_expansion_stack);
        free((void *)e.load_expanded_paths);
        elab_macro_env_dispose(e.macro_env);
        return NULL;
    }

    /* Pass 2: Elaborate all forms.
     *
     * typeclass-method-resolution-ignores-the-class (symptom A): a defn whose
     * body calls a class method declared above it, but whose only instance is
     * declared BELOW it, cannot resolve -- instances register in source order.
     *
     * Reordering the pass does not fix this: instance bodies call ordinary
     * defns (`Eq [Map]`'s `eq?` calls `map-count`), so defn bodies must precede
     * instance bodies just as surely as instances must precede the defn bodies
     * that dispatch on them.  That is a cycle, and a two-sweep Pass 2 breaks
     * stdlib at map.tur on exactly it.
     *
     * So break it by TIME rather than by order, and only for the forms that
     * need it: elaborate such a defn speculatively, and if it fails, roll back
     * and re-elaborate it after every other form has been processed.  The
     * capture frame + `n_file_scope_defs` rollback is the same mechanism
     * elab_defn already uses for its bare-^fat lazy probe.
     *
     * Gated on there actually being a later `definstance`, so a unit without
     * one takes precisely the old path: no capture, no retry, no change. */
    bool *tl_deferred = (nforms > 0)
        ? (bool *)calloc(nforms, sizeof(bool)) : NULL;
    /* forward-call-to-generic-callee-typed-as-placeholder: a defn that calls a
     * generic defn not elaborated yet waits for it (fwd_gen_order_init). */
    FwdGenOrder fgo;
    fwd_gen_order_init(&fgo, &e, forms, nforms);
    TlRetryCtx tl_ctx = { &e, forms, nforms, items, stdlib_prefix, &rc,
                          tl_deferred, &fgo };
    e.in_stdlib_load = (stdlib_prefix > 0);
    for (uint32_t i = 0; i < nforms; i++) {
        if (i == stdlib_prefix) e.in_stdlib_load = false;
        if (fwd_gen_order_should_defer(&fgo, forms, i)) {
            fwd_gen_order_defer(&fgo, i);
            items[i] = NULL;
            goto tl_form_tail;
        }
        /* A waiting defn this form names is elaborated first, so the form
         * sees its definition, as it did when the defn kept its place. */
        tl_flush(&tl_ctx, forms[i], i);
        tl_elab_slot(&tl_ctx, i, i);

    tl_form_tail:
        /* Phase M7+: Each (load ...)-spliced file is conceptually its own
         * file, so reset has_defmodule at every file boundary -- not just
         * after stdlib defmodules.  Without this, a user program that
         * loads two defmodule-wrapped files in sequence would trip the
         * "one defmodule per file" check on the second file.
         *
         * The original M7 reset triggered on the defmodule form itself,
         * which works for stdlib (every auto-loaded file has a
         * defmodule).  Generalising to "any file boundary" subsumes that
         * case and additionally handles user-loaded files. */
        if (i + 1 < nforms &&
            forms[i + 1]->span.file_id != forms[i]->span.file_id) {
            e.has_defmodule = false;
        }

        /* Phase M7: Promote auto-loaded stdlib module exports back to
         * "stdlib pre-module" status (defining_module_name = NULL) so they
         * remain globally visible from user code without explicit import.
         * Triggered after the last stdlib form has been processed. */
        if (i + 1 == stdlib_prefix && stdlib_prefix > 0) {
            for (uint32_t k = 0; k < e.global.n; k++) {
                Binding *gb = e.global.bindings[k];
                if (gb->defining_module_name != NULL &&
                    gb->defining_module_name->len >= 4 &&
                    memcmp(gb->defining_module_name->name, "tur/", 4) == 0) {
                    gb->defining_module_name = NULL;
                    gb->is_from_stdlib = true;
                }
            }
            for (uint32_t k = 0; k < e.n_macros; k++) {
                MacroDef *m = e.macros[k];
                if (m->defining_module_name != NULL &&
                    m->defining_module_name->len >= 4 &&
                    memcmp(m->defining_module_name->name, "tur/", 4) == 0) {
                    m->defining_module_name = NULL;
                }
            }
        }
    }

    /* symptom A, second chance: the defns whose bodies could not resolve a class
     * method the first time round.  Every instance in the unit is registered by
     * now.  No capture frame here -- a still-failing body reports for real. */
    /* forward-call-to-generic-callee-typed-as-placeholder: the defns that
     * waited for a lossy callee join them, in dependency order
     * (fwd_gen_order_drain). */
    fwd_gen_order_drain(&fgo, forms, tl_deferred, tl_retry_slot, tl_probe_slot,
                        &tl_ctx);
    free(tl_deferred);
    tl_deferred = NULL;
    fwd_gen_order_free(&fgo);

    /* class-and-generic-in-an-instance-less-module: a defn an imported module
     * parked for want of an instance gets one last attempt now that every
     * unit is in -- for real, so a genuinely instance-less program still
     * reports TUR-E0015 against the defn that needs one. */
    if (!elab_noinst_retry(&e, true)) rc = -1;

    /* class-superclasses SC2/SC4: every defclass and definstance in the unit is
     * registered now, so resolve the superclass preambles (a superclass may be
     * declared below its subclass), reject cycles, and enforce the instance
     * obligation that keeps the entailment sound.  A no-op for a program with
     * no preamble. */
    if (!elab_typeclass_superclasses_finish(&e)) rc = -1;

    /* cps-backend-n6 cross-function resume: gated whole-program reset-wrapping.
     * Runs after the main pass so uses_crossfn_resume reflects the entire program
     * (a callee's resuming shift may be elaborated after a caller's reset).  When
     * the flag is off this is a no-op, keeping every non-using program's reset
     * codegen byte-for-byte unchanged. */
    elab_wrap_resets_for_crossfn_resume(&e);

    /* used-attr-whole-program: force-load any #[used]-bearing modules that the
     * entry reaches only via a raw mangled C symbol (no `(import)`), so their
     * defns are emitted into this single TU and the extern resolves at link
     * time.  Runs after the main pass (an already-imported module is deduped)
     * and before the file-scope prepend below (so the loaded module's
     * EX_DEFMODULE, registered via elab_register_file_def during the load, is
     * picked up).  Inert under separate compilation and when no list is set. */
    if (rc == 0) {
        const UsedModulesCtx *umc = used_modules_ctx_active();
        if (umc && umc->modules && !separate_compilation) {
            for (int i = 0; i < umc->n; i++)
                elab_force_load_module(&e, umc->modules[i]);
            if (diag_had_error()) rc = -1;
        }
    }

    /* bare-fat-result-monomorphization (Phase B): surface the deferred
     * diagnostic for any lazy bare-^fat binding that no call site specialized
     * (e.g. a float-only combinator that is defined but never called).  Must
     * run after the whole program is elaborated so all call sites have had a
     * chance to specialize.  Done before the file-scope prepend so its emitted
     * specializations (registered during the main loop) are already counted. */
    elab_sweep_bare_fat_lazy(&e);
    free(e.bare_fat_specs);
    free(e.bare_fat_lazy_bindings);

    /* Phase 3: Prepend file-scope definitions (from nested fn) */
    if (e.n_file_scope_defs > 0) {
        /* Allocate new items array with room for file-scope defs */
        Expr **new_items = (Expr **)arena_alloc(arena, 
            (nforms + e.n_file_scope_defs) * sizeof(Expr *));
        /* Copy file-scope defs first */
        for (uint32_t i = 0; i < e.n_file_scope_defs; i++) {
            new_items[i] = e.file_scope_defs[i];
        }
        /* Copy original items */
        for (uint32_t i = 0; i < nforms; i++) {
            new_items[e.n_file_scope_defs + i] = items[i];
        }
        items = new_items;
        nforms += e.n_file_scope_defs;
        /* Free the malloc'd file_scope_defs array */
        free(e.file_scope_defs);
    }

    /* TR2: report the file-scope-def count before the session reset below
     * clears it (the non-session path reads e.n_file_scope_defs at return). */
    const uint32_t n_fsd_out = (uint32_t)e.n_file_scope_defs;
    if (sess) {
        /* Everything freed above this point must be cleared, or the saved
         * session would carry dangling pointers into the next call.  These are
         * all per-call working sets, not accumulated state. */
        e.file_scope_defs   = NULL;
        e.n_file_scope_defs = 0;
        e.cap_file_scope_defs = 0;
        e.bare_fat_specs   = NULL;
        e.n_bare_fat_specs = 0;
        e.cap_bare_fat_specs = 0;
        e.bare_fat_lazy_bindings   = NULL;
        e.n_bare_fat_lazy_bindings = 0;
        e.cap_bare_fat_lazy_bindings = 0;
    }

    /* Phase M6: Check for C symbol name collisions among exported bindings.
     * Two exported bindings from different modules collide when their mangled
     * C names are identical (e.g. module "my-lib" and "my_lib" both exporting
     * "foo" would both produce "my_lib__foo"). */
    if (rc == 0) {
        uint32_t n_exp = 0;
        for (uint32_t i = 0; i < e.global.n; i++) {
            if (e.global.bindings[i]->is_exported &&
                e.global.bindings[i]->defining_module_name != NULL)
                n_exp++;
        }
        if (n_exp > 1) {
            char **mangled = (char **)malloc(n_exp * sizeof(char *));
            Binding **exp_bindings = (Binding **)malloc(n_exp * sizeof(Binding *));
            if (!mangled || !exp_bindings) { fprintf(stderr, "tur: oom\n"); abort(); }
            uint32_t idx = 0;
            for (uint32_t i = 0; i < e.global.n; i++) {
                Binding *b = e.global.bindings[i];
                if (b->is_exported && b->defining_module_name != NULL) {
                    mangled[idx] = elab_mangle_binding_name(b);
                    exp_bindings[idx] = b;
                    idx++;
                }
            }
            for (uint32_t i = 0; i < n_exp; i++) {
                for (uint32_t j = i + 1; j < n_exp; j++) {
                    if (exp_bindings[i] == exp_bindings[j]) continue;
                    if (strcmp(mangled[i], mangled[j]) == 0) {
                        diag_emit(DIAG_ERROR, exp_bindings[j]->span,
                                  "exported symbol '%s' from module '%s' mangles to "
                                  "the same C name '%s' as '%s' from module '%s'; "
                                  "rename one or use (export-as \"...\") to assign a unique C name",
                                  exp_bindings[j]->name->name,
                                  exp_bindings[j]->defining_module_name->name,
                                  mangled[j],
                                  exp_bindings[i]->name->name,
                                  exp_bindings[i]->defining_module_name->name);
                        rc = -1;
                    }
                }
            }
            for (uint32_t i = 0; i < n_exp; i++) free(mangled[i]);
            free(mangled);
            free(exp_bindings);
        }
    }

    /* method-vs-defn clash warning (TUR-W0039).
     *
     * A typeclass method and a free top-level `defn` share the same value
     * namespace.  The two now coexist (fix (1) of
     * docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md): a
     * bare `(name x ...)` dispatches to the matching instance when the
     * receiver's static type selects one, and falls back to the free defn
     * otherwise (see the `prefer_method_dispatch` gate in elab_call.c).  The
     * clash is no longer a silent footgun, but the resolution rule -- a method
     * can win over a same-named defn for some receiver types -- is still worth
     * surfacing so the author is not surprised.
     *
     * Because *overriding a stdlib class method* with a same-named user defn is
     * a documented, intentional pattern (and stdlib methods keep "defn wins"),
     * the warning fires only when the colliding class is user-defined
     * (`!tc->from_stdlib`) and the binding is genuine user code
     * (`!is_from_stdlib`).  It is a warning, not an error.  The dotted
     * `(.method ...)` form always dispatches the method regardless. */
    for (TypeClass *tc = e.typeclass_env.typeclasses; tc != NULL; tc = tc->next) {
        if (tc->from_stdlib) continue;
        for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
            const Symbol *mn = tc->methods[mi].name;
            if (!mn) continue;
            Binding *b = scope_lookup(&e.global, mn);
            if (!b || b->is_from_stdlib) continue;
            /* is_from_stdlib marks the AUTO-loaded band only; a stdlib file
             * pulled in by an explicit (load "stdlib/arrow.tur") is still
             * stdlib, and its fallback defns (arr, >>>) are deliberate. */
            if (elab_file_is_stdlib(b->span.file_id)) continue;
            diag_emit_with_code(DIAG_WARNING, b->span, TUR_W0039_METHOD_DEFN_CLASH,
                "free defn '%s' shares its name with the method '%s' of typeclass "
                "'%s'; a bare (%s ...) dispatches to the method when the receiver "
                "type has an instance, and falls back to this defn otherwise -- "
                "rename one, or use the dotted form (.%s ...) to force dispatch",
                mn->name, mn->name, tc->name->name, mn->name, mn->name);
        }
    }

    /* RT1/RT3 (refinement-types-plan): resolve the call-site crossings recorded
     * during elaboration -- deferred to here so a call to a later-defined
     * function is checked exactly like a call to an earlier-defined one -- then
     * decide any obligation not already resolved in place and print the
     * per-compile summary when TUR_REFINE_STATS=1.  A no-op for a unit with no
     * `#refine{...}` in it (nothing was collected).
     *
     * Runs BEFORE the session branch below: the obligations were collected
     * during this elaboration and have to be decided either way.  Handing the
     * session back to the caller is not a reason to leave them undischarged --
     * that would silently skip static checking for every session-based
     * elaboration. */
    /* WF2 (checked-write-frames-plan): verify every `#writes` frame against its
     * body.  Runs BEFORE the crossing resolution below, because that is where a
     * checked frame is CONSUMED -- WF3 asks "can this callee stale my
     * hypothesis?" and may only believe a frame that has already been checked.
     * Same deferral rationale as the crossings themselves: a frame's callees may
     * be defined later in the unit. */
    wf_resolve_write_frames(&e);
    wf_lint_image_globals(&e);   /* AI3.1: TUR-W0706, after every site exists */
    /* R4 slice 2: verify `#reads` frames against their elaborated bodies,
     * stamping reads_checked where every read attributes to the frame.  Emits
     * nothing but the optional --dump-read-frames dump plus slice 3's
     * EXCEEDED evidence (TUR-W0383). */
    rf_resolve_read_frames(&e);
    /* reflected-measures RF1/RF2: classify every `^reflect` site (TUR-E0384
     * on rejection) BEFORE the crossings below are resolved, because that is
     * where the encoder consumes the verdict -- it unfolds only a TOTAL
     * measure. */
    rf_resolve_reflect_sites(&e);
    /* loop-invariants-plan: a `:invariant` loop no definition analysed (a
     * top-level lambda's) is declined out loud rather than left silent. */
    li_decline_unanalyzed(&e);
    refine_resolve_call_sites(&e);
    refine_discharge_all(&e.refine_obs, arena);
    /* SX8a: the JSON obligation dump.  Emitted here rather than from the
     * discharge pass so it sees every obligation the unit produced, including
     * the crossings resolved just above.  Goes to stdout: it is the artifact
     * the caller asked for, not a diagnostic about one. */
    if (g_dump_refine_json) {
        Buf rb; buf_init(&rb);
        refine_report_json(&e.refine_obs, &rb);
        buf_to_file(&rb, stdout);
        buf_free(&rb);
    }

    /* duplicate-ctor-names-collide-in-emitted-c: the emitted constructor symbol
     * is ADT-qualified, and a constructor name owned by exactly ONE ADT also
     * keeps a bare-name alias so hand-written inline C (`ctor_Left(v)`, which
     * stdlib/either.tur documents) still resolves.  Deciding "exactly one" needs
     * every ADT in the program.
     *
     * Taken HERE, above the teardown, and not at the return: the non-session
     * path frees `e.adt_defs` a few lines down, so snapshotting after it read
     * freed memory (ASan heap-use-after-free).  The census copies the names it
     * needs, so nothing downstream holds an elaborator pointer. */
    ctor_census_snapshot(e.adt_defs, e.n_adt_defs);

    if (sess) {
        /* TR2: the accumulated state IS the session -- hand it back instead of
         * tearing it down, so the next call resolves against it. Ownership
         * transfers to the caller (elab_session_free reclaims it). */
        *sess = e;
    } else {
        scope_free(&e.global);
        free(e.adt_defs);
        free(e.forward_type_syms);
        free(e.handled_effect_names);
        free(e.pending_reset_nodes);
        free(e.macros);
        free(e.macro_expansion_stack);
        free(e.loaded_modules); /* Phase M2 */
        free(e.dynvar_entries);
        free(e.active_dynvar_bindings);
        free((void *)e.load_expanded_paths);
        elab_macro_env_dispose(e.macro_env);
    }
    if (rc != 0) return NULL;

    Expr *prog = expr_new(arena, EX_PROGRAM, TYPE_NIL,
                          nforms > 0 ? forms[0]->span : (Span){0,0,0,0,0,0});
    prog->as.program.items = items;
    prog->as.program.n = nforms;
    /* CPS-CL10: expose typeclass env to callers (e.g. cps_transform) */
    if (out_tc_env) *out_tc_env = e.typeclass_env;
    /* Tier 3: expose actual file-scope-def count so the interpreter can
     * distinguish them from (load ...)-expanded inline forms. */
    if (out_n_file_scope_defs)
        *out_n_file_scope_defs = n_fsd_out;
    return prog;
}
