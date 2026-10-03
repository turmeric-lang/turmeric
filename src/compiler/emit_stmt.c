/* emit_stmt.c -- statement-position C emission (emit_stmt and friends). */
#include "emit_internal.h"
#include "mangle.h"

void emit_while_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* while (1) { cond; if (!cond) break; body; } — we re-emit the condition
     * inside the loop because it may itself produce statements. */
    indent_buf(body, ctx->indent);
    buf_puts(body, "while (1) {\n");
    ctx->indent += 4;
    char *cond = emit_value(ctx, body, e->as.while_.cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (!(%s)) break;\n", cond);
    free(cond);
    emit_stmt(ctx, body, e->as.while_.body);
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
}

void emit_set_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *bn = name_for_binding(ctx, e->as.set_.target);
    char *v = emit_value(ctx, body, e->as.set_.value);

    /* G4a: a write to an `^atomic` global is a sequentially-consistent store.
     * Emitted before the rc-release path below because an atomic global is a
     * scalar by construction (elab_def rejects anything wider), so it never
     * owns a refcounted value there is an old reference to release. */
    /* G4b: a write to a `^thread-local` goes through its setter. */
    if (e->as.set_.target && e->as.set_.target->is_thread_local) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "__tur_tl_set_%s(%s);\n", bn, v);
        free(bn); free(v);
        return;
    }
    if (e->as.set_.target && e->as.set_.target->is_atomic) {
        const Binding *tb = e->as.set_.target;
        indent_buf(body, ctx->indent);
        if (tb->type.kind == TY_FLOAT) {
            buf_printf(body,
                "TUR_ATOMIC_STORE_U64((volatile uint64_t *)&%s, __tur_f64_to_bits(%s), __ATOMIC_SEQ_CST);\n",
                bn, v);
        } else if (tb->type.kind == TY_PTR_VOID || tb->type.kind == TY_CSTR) {
            buf_printf(body,
                "TUR_ATOMIC_STORE_PTR((void *volatile *)&%s, (void *)(%s), __ATOMIC_SEQ_CST);\n",
                bn, v);
        } else {
            buf_printf(body,
                "TUR_ATOMIC_STORE_U64((volatile uint64_t *)&%s, (uint64_t)(%s), __ATOMIC_SEQ_CST);\n",
                bn, v);
        }
        free(bn); free(v);
        return;
    }

    /* B7b: a `^mut` the CPS backend promoted to a SHARED HEAP CELL (because a
     * lifted body -- a handler clause, emitted as its own C function -- touches
     * it) binds the cell POINTER, so the STORE derefs.  This is the assignment-
     * side counterpart of the read deref in atom_var, and it must live here
     * rather than in `name_for_binding`, which also spells the declaration.
     * Rewriting `bn` once covers every store path below, including a `set!`
     * nested inside a DELEGATED composite (a `while` in a clause), which never
     * reaches the CPS emitter's own statement lowering. */
    if (emit_binding_is_byref_cell(e->as.set_.target)) {
        size_t n = strlen(bn) + 8;
        char *deref = (char *)malloc(n);
        if (!deref) { fprintf(stderr, "tur: oom\n"); abort(); }
        snprintf(deref, n, "(*%s)", bn);
        free(bn);
        bn = deref;
    }

    /* set-bang-rc-release: release the value being overwritten.  Only when the
     * elaborator proved this binding owns a continuous strong reference -- see
     * elab_set_rc_release for why a hand-managed binding must NOT come here.
     *
     * The spill to a temp is load-bearing, not a readability nicety: `v` is an
     * expression string that may still READ the binding being assigned.
     * `(set! h (.next h))` lowers to `((tur_adt_Node *)h->value)->next` inline,
     * so releasing first could free `h` and then dereference it.  Materializing
     * `v` first makes the order [evaluate new] -> [release old] -> [store]
     * regardless of the value's shape, which is also what keeps
     * `(set! h (rc/of ... (rc/clone h) ...))` correct: the clone's +1 is taken
     * while the old value is still alive, so the release drops it to the count
     * the new structure holds rather than to zero. */
    if (e->as.set_.release_old) {
        const char *tcn =
            type_c_name(emit_resolve_type(ctx, e->as.set_.target->type));
        char *tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = (%s)(%s);\n", tcn, tmp, tcn, v);
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (%s) { rc_strong_decrement(%s); rc_free_queue_drain(); }\n",
                   bn, bn);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", bn, tmp);
        emit_region_note_lvalue(body, ctx->indent, tcn, bn);
        free(tmp); free(bn); free(v);
        return;
    }

    /* global-def-store-misses-int-ptr-bridge: a `set!` whose target's carrier
     * and value temp straddle int64/pointer gets the same bridge the `let`
     * binder applies.  The target's RECORDED emitted C type wins over its
     * type's c-name when the side table has it (a local declared by-value or
     * as a spec pointer); a global falls back to the c-name. */
    {
        const char *tgt_c = NULL;
        if (emit_str_is_bare_ident(bn)) tgt_c = emit_localvar_lookup_ctype(bn);
        if (!tgt_c && e->as.set_.target)
            tgt_c = type_c_name(emit_resolve_type(ctx, e->as.set_.target->type));
        char *bv = emit_store_int_ptr_bridge(ctx, tgt_c, v, e->as.set_.value);
        if (bv) { free(v); v = bv; }
        /* global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c (repro 1): the value
         * is a pass-by-pointer binder -- a match arm binds a recursive-field
         * link as `const T *` (the SR4 borrow) -- and the target is the
         * aggregate by value, so `g = t` assigns a pointer to a struct.  Deref,
         * exactly as the `let` binder does for the same initializer shape
         * (`T g = *t;`).  A pointer-represented target (its C name carries a
         * `*`) keeps the bare alias. */
        /* (repro 2) a store into a thin function-pointer global casts the
         * int64 carrier the value emits as to the pointer type the global is
         * declared with -- the same cast its initializer gets. */
        {
            const char *fr = NULL; char *fa = NULL;
            if (e->as.set_.target && e->as.set_.target->is_global &&
                emit_global_def_thin_fnptr(e->as.set_.target, &fr, &fa)) {
                size_t n = strlen(v) + strlen(fr) + strlen(fa) + 32;
                char *cv = (char *)malloc(n);
                if (!cv) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(cv, n, "(%s (*)(%s))(intptr_t)(%s)", fr, fa, v);
                free(v); v = cv;
                free(fa);
            }
        }
        if (tgt_c && !strchr(tgt_c, '*') &&
            emit_expr_is_pbp_param(ctx, e->as.set_.value)) {
            size_t n = strlen(v) + 4;
            char *dv = (char *)malloc(n);
            if (!dv) { fprintf(stderr, "tur: oom\n"); abort(); }
            snprintf(dv, n, "(*%s)", v);
            free(v);
            v = dv;
        }
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", bn, v);
    /* region-lock-hardening: a `set!` target that is a global / module-level
     * def, or a heap cell a lifted body shares, is memory that outlives any
     * bracket the store runs inside -- note the word.  A plain local is a
     * stack slot that dies with its frame: nothing to note, and a hot loop's
     * counter stays a bare assignment. */
    if (e->as.set_.target &&
        (e->as.set_.target->is_global || emit_binding_is_byref_cell(e->as.set_.target)))
        emit_region_note_lvalue(body, ctx->indent,
                                type_c_name(emit_resolve_type(ctx, e->as.set_.target->type)), bn);
    free(bn); free(v);
}

/* Phase 12: emit (set! (@ r) value) - mutation through mutable borrow */
void emit_set_deref_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *ref = emit_value(ctx, body, e->as.set_deref_.ref);
    char *val = emit_value(ctx, body, e->as.set_deref_.value);
    const char *inner_type_c = type_c_name(e->as.set_deref_.value->type);
    indent_buf(body, ctx->indent);
    buf_printf(body, "*((%s *)%s) = %s;\n", inner_type_c, ref, val);
    /* region-lock-hardening: a ref cell is heap memory. */
    {
        Buf lv; buf_init(&lv);
        buf_printf(&lv, "(*((%s *)%s))", inner_type_c, ref);
        buf_putc(&lv, '\0');
        emit_region_note_lvalue(body, ctx->indent, inner_type_c, lv.data);
        buf_free(&lv);
    }
    free(ref); free(val);
}

/* Phase DS3: emit (set! (.field s) v) - struct field write.
 * For rc-typed fields, releases the prior pointer before storing the new
 * one (the new value carries its own +1).  For rc<Struct> receivers,
 * accesses the field through the rc-block's value pointer. */
void emit_set_field_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    uint32_t fi = e->as.set_field_.field_idx;
    char *rv = emit_value(ctx, body, e->as.set_field_.receiver);
    char *vv = emit_value(ctx, body, e->as.set_field_.value);

    /* CONV-S1 seam 4: a lowered record-ADT receiver -- write the field through
     * the ADT member path (`.as.<Ctor>._N` for the positional layout, `.value`
     * for a flat-named record).  A :heap receiver is a typed pointer (`->`); a
     * by-value receiver is the aggregate lvalue (`.`); the abstract carrier base
     * casts the int64 to the monomorph pointer first.
     * structdef-retirement DS-C: every set-field receiver is a lowered record
     * ADT now (set_field_.def is always NULL); the former StructDef write path
     * that followed is dead and has been removed. */
    {
        const AdtDef *adt = e->as.set_field_.adt_def;
        const CtorDef *ctor = e->as.set_field_.adt_ctor;
        char *mp = adt_field_member_path(adt, ctor, fi);
        Buf lhs; buf_init(&lhs);
        Type recv_rty = emit_resolve_type(ctx, e->as.set_field_.receiver->type);
        const char *recv_cn = type_c_name(recv_rty);
        bool recv_is_ptr = recv_cn && strchr(recv_cn, '*') != NULL;
        if (e->as.set_field_.receiver_is_rc) {
            char *madt = mangle_adt_name(adt->name);
            buf_printf(&lhs, "((tur_adt_%s *)((RcControlBlock *)(%s))->value)->%s",
                       madt, rv, mp);
            free(madt);
        } else if (type_is_heap_adt(recv_rty)) {
            if (recv_is_ptr)
                buf_printf(&lhs, "(%s)->%s", rv, mp);
            else if (!recv_cn || strcmp(recv_cn, "int64_t") == 0) {
                /* r7rs-lang-plan R8: a receiver that arrives as the int64
                 * carrier word (a heap record with a pointer-opaque field is
                 * passed that way) names no struct, and `((int64_t *)p)->f`
                 * is not C.  Cast to the record's own cell, as the field
                 * READ path does (emit_expr.c, `tur_adt_%s`). */
                char *madt = mangle_adt_name(adt->name);
                buf_printf(&lhs, "((tur_adt_%s *)(intptr_t)(%s))->%s", madt, rv, mp);
                free(madt);
            } else
                buf_printf(&lhs, "((%s *)(intptr_t)(%s))->%s",
                           type_c_name(recv_rty), rv, mp);
        } else {
            buf_printf(&lhs, "(%s).%s", rv, mp);
        }
        buf_putc(&lhs, '\0');
        /* Mirror the struct path's ownership transfer for the old field value:
         * an rc field's prior pointer is decremented (the new value carries its
         * own +1), a weak field's is weak-decremented, a ref/lref field freed.
         * Guarded by `if (lhs)` so a NULL sentinel (e.g. the construction-time
         * placeholder) is a no-op. */
        TypeKind afk = ctor->fields[fi].kind;
        bool releases_old = (afk == TY_RC || afk == TY_WEAK ||
                             afk == TY_REF || afk == TY_LREF);
        /* set-bang-rc-release: spill the incoming value before releasing the old
         * one.  `vv` is an expression string that can still READ the very field
         * being written -- `(set! (.next a) (.next a))` lowers to the same
         * `a->value->next` path on both sides, so releasing first could free the
         * block and then read it.  Same ordering device as emit_set_stmt. */
        if (releases_old) {
            const CtorField *cf = &ctor->fields[fi];
            const char *fcn = cf->full_type ? type_c_name(*cf->full_type)
                                            : type_c_name(emit_type_from_kind(afk));
            char *vtmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = (%s)(%s);\n", fcn, vtmp, fcn, vv);
            free(vv);
            vv = vtmp;
        }
        if (afk == TY_RC) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s) { rc_strong_decrement(%s); rc_free_queue_drain(); }\n",
                       lhs.data, lhs.data);
        } else if (afk == TY_WEAK) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s) rc_weak_decrement(%s);\n", lhs.data, lhs.data);
        } else if (afk == TY_REF || afk == TY_LREF) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s) free(%s);\n", lhs.data, lhs.data);
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", lhs.data, vv);
        /* region-lock-hardening: the receiver is a heap box or an outer
         * aggregate; either can outlive the bracket the store runs in. */
        {
            const CtorField *cf = &ctor->fields[fi];
            const char *fcn = cf->full_type ? type_c_name(*cf->full_type)
                                            : type_c_name(emit_type_from_kind(afk));
            emit_region_note_lvalue(body, ctx->indent, fcn, lhs.data);
        }
        buf_free(&lhs);
        free(rv); free(vv); free(mp);
        return;
    }
    /* structdef-retirement DS-C: unreachable -- a set-field always has a lowered
     * record-ADT receiver.  Free and return rather than deref a NULL def. */
    free(rv); free(vv);
}


/* D8 piece 2: does this method parameter ride the int64 CARRIER in the dict
 * slot (and therefore need a per-instance deref wrapper)?
 *
 * Three sites have to agree exactly -- the slot's declared type, whether a
 * wrapper is emitted, and the wrapper's own parameter list.  They did not on
 * the first attempt: the slot skipped `pass_by_ptr` only when the body was NOT
 * inline-C, while the wrapper skipped it unconditionally, so an inline-C method
 * with a pass-by-ptr aggregate got a carrier-shaped SLOT and no wrapper --
 * `-Wincompatible-pointer-types`, caught by run.sh's pointer/integer ratchet on
 * `constrained-generic-inline-c-receiver-dispatch`. One predicate now. */
static bool dict_slot_param_is_carrier(EmitCtx *ctx, const FnDef *mi,
                                       uint32_t j) {
    if (!mi || !mi->param_types) return false;
    if (mi->params && mi->params[j]->is_poly_fn) return false;
    bool body_is_inline_c = (mi->body && mi->body->kind == EX_INLINE_C);
    Type pt = mi->param_types[j];
    if (!mi->closure && !body_is_inline_c && type_struct_pass_by_ptr(pt))
        return false;                      /* spelled `const T *`, not carrier */
    return emit_type_is_byvalue_adt(ctx, pt);
}

/* dict-slot-classvar-scalar-param: a parameter the CLASS declares as its
 * bare class variable (`(sh [x : a])`) is erased to the int64 word at every
 * dict-passing dispatch site -- the site cannot know the instance -- while the
 * instance impl takes it at its own C type: `bool`, `double`, `const char *`.
 * The raw impl in the slot was then called through `(ret (*)(int64_t))`: an
 * fnsan trap for a bool or a pointer, and for a `double` a SILENT WRONG ANSWER
 * (the word went in an integer register, the impl read a float register:
 * `Sh float`'s `(sh 7.1)` answered as if for garbage).  Such a slot holds a
 * per-instance wrapper that takes the word and converts it (bits for a float
 * kind), exactly as D8 piece 2 already does for a by-value aggregate.  The
 * dispatch site (emit_call_name) asks this same question. */
bool dict_slot_param_is_word_scalar(const TypeClass *tc, int slot,
                                    const FnDef *mi, uint32_t j) {
    if (!tc || slot < 0 || slot >= tc->n_methods || !mi || !mi->param_types)
        return false;
    const TypeClassMethod *m = &tc->methods[slot];
    if (!m->param_types || j >= m->n_params || j >= mi->n_params) return false;
    /* An UNANNOTATED class parameter (`(show [x] : cstr)`) is the class
     * variable too: param_types defaults it to int, and param_explicit_type
     * says the user wrote nothing (the instance rewrite substitutes the class
     * variable into it, elab_definstance). */
    bool classvar = m->param_types[j].kind == TY_TYVAR ||
                    (m->param_explicit_type && !m->param_explicit_type[j]);
    if (!classvar) return false;
    if (mi->params && mi->params[j] && mi->params[j]->is_poly_fn) return false;
    const char *c = type_c_name(mi->param_types[j]);
    if (!c || !*c) return false;
    size_t L = strlen(c);
    if (c[L - 1] == '*') return true;
    return strcmp(c, "double") == 0 || strcmp(c, "float") == 0 ||
           strcmp(c, "bool") == 0 || strcmp(c, "int8_t") == 0 ||
           strcmp(c, "int16_t") == 0 || strcmp(c, "int32_t") == 0 ||
           strcmp(c, "uint8_t") == 0 || strcmp(c, "uint16_t") == 0 ||
           strcmp(c, "uint32_t") == 0;
}

/* dict-slot-classvar-scalar-result: the RESULT twin.  A method whose class
 * result is the bare class variable (`(cid [x : a] : a)`) is read as the word
 * at every dict-passing dispatch site, while the instance impl returns its own
 * C type.  A `double` read through `(double (*)(int64_t))` by the
 * representative instance's spelling and then handed on as the carrier was
 * VALUE-converted (`(int64_t)(intptr_t)` of a double: a rank-2 identity at
 * 7.1 answered 3.45846e-323).  Such a slot's wrapper returns the word -- bits
 * for a float kind -- and the dispatch site reads `int64_t`.  Returns the
 * impl's C result spelling through `*impl_rc` when it holds. */
static bool dsr_mentions_tyvar(const Type *t, const char *name, int depth) {
    if (!t || depth > 32) return false;
    if (t->kind == TY_TYVAR)
        return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
    if (t->kind == TY_APP)
        return dsr_mentions_tyvar(t->as.app.fn, name, depth + 1) ||
               dsr_mentions_tyvar(t->as.app.arg, name, depth + 1);
    return false;
}
bool dict_slot_result_is_word_scalar(const TypeClass *tc, int slot,
                                     const FnDef *mi, const char **impl_rc) {
    if (!tc || slot < 0 || slot >= tc->n_methods || !mi || !mi->binding ||
        mi->binding->type.kind != TY_FN)
        return false;
    const Type *crt = &tc->methods[slot].return_type;
    /* class-var-applied-result: a kind-* class whose result mentions its
     * variable inside an application -- `(co [x : a] : (Option a))`.  Each
     * instance's impl returns its OWN by-value monomorph (`tur_adt_Option__float`,
     * `tur_adt_Option__cstr`), and a dispatch site cast to the representative's
     * struct read another instance's payload from the wrong register.  The
     * carrier spells an open `(Option A)` as a word pointing at the aggregate,
     * so the slot hands back exactly that: the impl's result, boxed. */
    bool cv_app = crt->kind == TY_APP && tc->n_type_params >= 1 &&
                  tc->type_params && tc->type_params[0] &&
                  !(tc->type_param_kinds && tc->type_param_kinds[0] != KIND_STAR) &&
                  dsr_mentions_tyvar(crt, tc->type_params[0]->name, 0);
    if (crt->kind != TY_TYVAR && !cv_app) return false;
    const Type *rft = mi->binding->type.as.fn.result_full_type;
    Type rt = rft ? *rft : emit_type_from_kind(mi->binding->type.as.fn.result_kind);
    const char *c = type_c_name(rt);
    if (!c || !*c) return false;
    size_t L = strlen(c);
    /* A by-value aggregate result (`tur_adt_W`) rides the word as a heap box,
     * the convention the carrier call's result unbox already expects. */
    bool agg = strncmp(c, "tur_adt_", 8) == 0 && c[L - 1] != '*' &&
               !type_is_heap_adt(rt) && !type_is_heap_struct(rt);
    if (cv_app) {
        /* Only the by-value aggregate: a :heap app is already a pointer word,
         * and an impl whose result rides the carrier returns int64_t now. */
        bool ok_app = agg && !type_uses_carrier_abi(rt);
        if (ok_app && impl_rc) *impl_rc = c;
        return ok_app;
    }
    bool ok = agg || (c[L - 1] == '*') ||
              strcmp(c, "double") == 0 || strcmp(c, "float") == 0 ||
              strcmp(c, "bool") == 0 || strcmp(c, "int8_t") == 0 ||
              strcmp(c, "int16_t") == 0 || strcmp(c, "int32_t") == 0 ||
              strcmp(c, "uint8_t") == 0 || strcmp(c, "uint16_t") == 0 ||
              strcmp(c, "uint32_t") == 0;
    if (ok && impl_rc) *impl_rc = c;
    return ok;
}

/* saffron-applied-class-var-result-takes-one-instances-type: the C return type
 * of an instance-method impl, spelled the way emit_fns.c spells its signature
 * (emit_inst_result_rides_carrier is that one decision, consulted, not
 * re-derived).  A declared result that rides the carrier ABI returns `int64_t`
 * there (the
 * dict's uniform slot shape, "Direction (1)"), so the dict slot and the
 * by-value wrapper must say `int64_t` too.  They used type_c_name, which agreed
 * only while such a result was OPEN -- `(Vec a)` -- and so lowered to the
 * carrier anyway; with the class variable substituted, `Twice [float]`'s
 * `(Vec float)` slot read `tur_adt_Vec__float *` against an `int64_t` impl
 * (-Wincompatible-pointer-types, -Wint-conversion). */
static const char *dict_slot_ret_c_name(EmitCtx *ctx, const FnDef *mi, Type ret) {
    if (mi && mi->binding && mi->binding->type.kind == TY_FN &&
        emit_inst_result_rides_carrier(ctx, mi, mi->binding->type.as.fn.result_full_type))
        return "int64_t";
    return type_c_name(ret);
}

/* saffron-lang-plan S9 (D8 piece 4): the per-instance DYNAMIC dispatch table.
 *
 * Why not just point the registry row at the dict singleton?  Because the dict's
 * slots are not uniformly shaped.  Piece 2 made a BY-VALUE receiver's slot
 * carrier-shaped, but a primitive one still holds the raw impl -- `Show bool`'s
 * slot is `__inst_Show_show_bool(bool)` and a float instance's is `(double)`.
 * A dynamic site has one cast for every instance, so calling those through a
 * single `(ret (*)(int64_t))` would pass a double in the wrong register class
 * and read a bool from a 64-bit word: exactly the silent float truncation P8
 * fixed on the mode-B path, reintroduced one layer up.
 *
 * So each dispatchable instance gets its OWN table of shims with a uniform
 * `(int64_t) -> ret` signature, and the row points there.  Mode B keeps the
 * dict it already has, untouched.
 *
 * v0 covers a one-parameter method with a concrete result -- `show`, `hash`,
 * `display`, `debug`.  A method with extra `a`-typed parameters (`eq [x : a
 * y : a]`) or an `a`-typed RESULT (`clone : a -> a`) needs the call site to box
 * and unbox more than the receiver, and gets a NULL slot, which the dispatch
 * site reports rather than calls.  That is a stated limit, not a silent one. */
void emit_instance_dyn_table(EmitCtx *ctx, TypeClassInstance *inst,
                             const char *dict_name, const char *type_suffix,
                             int64_t tag) {
    TypeClass *tc = inst->typeclass;
    char sanitized[128];
    char table[192];
    snprintf(table, sizeof(table), "__dyn%s", dict_name);

    /* D8 Q3: a witness-backed slot (parametric receiver).  The witness is an
     * ordinary defn taking the by-value/heap receiver and `any` extras and
     * returning `any`; the shim's only job is the receiver word -> receiver
     * value conversion, spelled the way the witness's own C signature wants it
     * (struct params may be passed by pointer). */
    Type wrecv;
    bool have_wrecv = emit_instance_dispatch_recv_type(ctx, inst, &wrecv);
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        FnDef *w = (inst->dyn_witness && have_wrecv) ? inst->dyn_witness[i] : NULL;
        if (!w || !w->binding) continue;
        tur_mangle_ident(tc->methods[i].name->name, sanitized, sizeof(sanitized));
        char *wc = raw_name_for_binding(w->binding);
        Type rr = emit_resolve_type(ctx, wrecv);
        const char *rcn = emit_type_c_name(ctx, rr);
        uint32_t nx = w->n_params > 0 ? w->n_params - 1 : 0;
        /* Almost every witness returns `any`; S9's witness for a one-parameter
         * concrete-result method on a constrained parametric head returns the
         * declared result, so its slot matches the direct shims beside it. */
        const char *wret = "tur_tagged_t";
        if (w->binding->type.kind == TY_FN) {
            const Type *wrt = w->binding->type.as.fn.result_full_type;
            Type wr = wrt ? *wrt : emit_type_from_kind(w->binding->type.as.fn.result_kind);
            if (wr.kind != TY_ANY && wr.kind != TY_UNKNOWN && wr.kind != TY_TYVAR)
                wret = type_c_name(wr);
        }
        buf_printf(ctx->file, "static %s __dynshim_%s_%s%s(int64_t __r",
                   wret, tc->name->name, sanitized, type_suffix);
        for (uint32_t k = 0; k < nx; k++) buf_printf(ctx->file, ", tur_tagged_t __a%u", k + 1);
        buf_puts(ctx->file, ") {\n    return ");
        buf_printf(ctx->file, "%s(", wc);
        if (type_struct_pass_by_ptr(rr))
            buf_printf(ctx->file, "(const %s *)(intptr_t)__r", rcn);
        else if (emit_type_is_byvalue_adt(ctx, wrecv))
            buf_printf(ctx->file, "*(%s *)(intptr_t)__r", rcn);
        else if (rr.kind == TY_FLOAT)
            /* M9: a float receiver's word is the double's bit pattern (the
             * inverse of what the widen wrote), as the direct shim below
             * already spells it. */
            buf_puts(ctx->file, "((union { double d; int64_t i; }){.i = __r}).d");
        else if (rr.kind == TY_FLOAT32)
            /* A float32 rides `any` as its promoted double (emit_any_type_id);
             * `(float)(intptr_t)__r` read the bit pattern as a count. */
            buf_puts(ctx->file,
                     "TUR_AS(float, ((union { double d; int64_t i; }){.i = __r}).d)");
        else
            buf_printf(ctx->file, "(%s)(intptr_t)__r", rcn);
        for (uint32_t k = 0; k < nx; k++) buf_printf(ctx->file, ", __a%u", k + 1);
        buf_puts(ctx->file, ");\n}\n");
        free(wc);
    }

    for (uint8_t i = 0; i < tc->n_methods; i++) {
        FnDef *mi = inst->method_impls[i];
        if (inst->dyn_witness && inst->dyn_witness[i]) continue;   /* witness-backed above */
        if (!mi || !mi->param_types || mi->n_params != 1) continue;
        Type ret;
        if (mi->binding && mi->binding->type.kind == TY_FN) {
            Type *rft = mi->binding->type.as.fn.result_full_type;
            ret = rft ? *rft : emit_type_from_kind(mi->binding->type.as.fn.result_kind);
        } else if (mi->body) {
            ret = mi->body->type;
        } else continue;
        if (ret.kind == TY_TYVAR || ret.kind == TY_UNKNOWN) continue;

        Type pt = mi->param_types[0];
        tur_mangle_ident(tc->methods[i].name->name, sanitized, sizeof(sanitized));
        const char *pcn = type_c_name(pt);

        /* How the carrier word becomes the impl's declared parameter.  Each arm
         * is the exact inverse of what the WIDEN wrote into the box, which is
         * why the float arm bit-reinterprets rather than converts: the box holds
         * the double's bit pattern, so `(double)__r` would read 4614...LL as a
         * count and hand the instance a wrong number, not a rounded one. */
        char conv[320];
        if (dict_slot_param_is_carrier(ctx, mi, 0)) {
            snprintf(conv, sizeof(conv), "*(%s *)(intptr_t)__r", pcn);
        } else if (emit_type_is_byvalue_adt(ctx, pt) && type_struct_pass_by_ptr(pt)) {
            /* saffron-dynamic-dispatch-on-payload-adt-emits-bad-c: a by-value
             * ADT the C convention passes as `const T *` is exactly the case
             * `dict_slot_param_is_carrier` DECLINES (its pass-by-ptr arm), so
             * before this it fell through to the scalar `(T)__r` at the bottom
             * and the shim did not compile -- `conversion to non-scalar type
             * requested`, on a class whose interpreted answer was correct.
             *
             * The split is by PAYLOAD, which is why an all-nullary `defdata`
             * always worked: that one rides the int64 carrier, so the scalar
             * cast was right for it and wrong for every ADT carrying anything.
             *
             * The widen boxed the value, so `__r` is already the address of the
             * ADT -- the same three-way choice the WITNESS path above makes,
             * which is where the correct spelling for each arm was established. */
            snprintf(conv, sizeof(conv), "(const %s *)(intptr_t)__r", pcn);
        } else if (pt.kind == TY_FLOAT) {
            snprintf(conv, sizeof(conv),
                     "((union { double d; int64_t i; }){.i = __r}).d");
        } else if (strchr(pcn, '*') != NULL) {
            snprintf(conv, sizeof(conv), "(%s)(intptr_t)__r", pcn);
        } else if (pt.kind == TY_FLOAT32) {
            /* The box holds the float32 as its promoted double, under the
             * float tag (emit_any_type_id): read the double, narrow. */
            snprintf(conv, sizeof(conv),
                     "TUR_AS(float, ((union { double d; int64_t i; }){.i = __r}).d)");
        } else {
            snprintf(conv, sizeof(conv), "(%s)__r", pcn);
        }

        buf_printf(ctx->file, "static %s __dynshim_%s_%s%s(int64_t __r) {\n",
                   type_c_name(ret), tc->name->name, sanitized, type_suffix);
        buf_puts(ctx->file, "    return ");
        if (mi->binding && mi->binding->name)
            buf_printf(ctx->file, "%s(%s);\n}\n", mi->binding->name->name, conv);
        else
            buf_printf(ctx->file, "__inst_%s_%s%s(%s);\n}\n", tc->name->name,
                       sanitized, type_suffix, conv);
    }

    /* The table itself: one slot per class method, in slot order, so the
     * dispatch site can index it with the method index the elaborator resolved.
     * A method this instance cannot serve dynamically is a NULL slot. */
    buf_printf(ctx->file, "static const void *%s[] = {\n", table);
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        FnDef *mi = inst->method_impls[i];
        bool have = (inst->dyn_witness && have_wrecv && inst->dyn_witness[i] &&
                     inst->dyn_witness[i]->binding);
        if (!have && mi && mi->param_types && mi->n_params == 1) {
            Type ret = (mi->binding && mi->binding->type.kind == TY_FN)
                ? (mi->binding->type.as.fn.result_full_type
                       ? *mi->binding->type.as.fn.result_full_type
                       : emit_type_from_kind(mi->binding->type.as.fn.result_kind))
                : (mi->body ? mi->body->type : emit_type_from_kind(TY_UNKNOWN));
            const char *pcn0 = type_c_name(mi->param_types[0]);
            have = (ret.kind != TY_TYVAR && ret.kind != TY_UNKNOWN &&
                    !(mi->param_types[0].kind == TY_FLOAT32 &&
                      !dict_slot_param_is_carrier(ctx, mi, 0) &&
                      strchr(pcn0, '*') == NULL));
        }
        if (have) {
            tur_mangle_ident(tc->methods[i].name->name, sanitized, sizeof(sanitized));
            buf_printf(ctx->file, "    (const void *)__dynshim_%s_%s%s,\n",
                       tc->name->name, sanitized, type_suffix);
        } else {
            buf_puts(ctx->file, "    0,\n");
        }
    }
    buf_puts(ctx->file, "};\n\n");

    emit_note_instance_row(ctx, tc->name->name, tag, table);
}

void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Debugger Phase 4 (--debug): anchor each statement to its source line so
     * native stepping advances line-by-line through the `.tur` file.  No-op
     * unless --debug; deduped against the previous directive on this stream. */
    emit_line_directive(ctx, body, e->span);
    /* Run e for side effects, discard value. */
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT: case EX_SYM_LIT: case EX_VAR:
        case EX_DEFAULT_OF:    /* M2b: pure zero-initializer, no side effects */
        case EX_UNION_INJECT: /* IT4: pure struct literal, no stmt-level side effects */
        case EX_ANY_TYPE_OF:  /* IT4: pure read, no stmt-level side effects */
        case EX_ANY_CAST:     /* IT4: pure unbox, no stmt-level side effects */
        case EX_ANY_IS:       /* TY3: pure tag test, no stmt-level side effects */
        case EX_TYPECLASS_DEF:
        case EX_DEFMODULE: /* Phase M0: module metadata — nothing to emit */
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_CONS_LIST:    /* AR8: cons-list expr -- allocation side-effects handled in emit_value */
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
        case EX_PANIC_PAYLOAD_DOWNS:
        case EX_EXISTS_PACK: /* Phase HRT2: pure boxing, no stmt-level side effects */
        case EX_DEFGADT:     /* Phase G1: ADT definition — handled in Pass 0 */
        case EX_CPS_CONT_APP: /* CPS2: continuation application — emit via emit_value if side-effecting */
            /* No side effects — emit nothing. */
            return;
        /* poly-call-in-statement-position-dropped: these four are pure WRAPPER
         * nodes -- the wrapper itself has no effect, but the expression inside
         * it may.  They used to sit in the emit-nothing group above, which
         * silently deleted the wrapped expression, effects and all.  The bug's
         * signature asymmetry came straight from here: a discarded parametric
         * call instantiated at non-int has its carrier result wrapped in
         * `(reinterpret int -> T <call>)` by elaboration, while an int
         * instantiation needs no wrapper -- so only int calls survived
         * statement position.  A discarded `(:: (call) :T)` ascription and a
         * cast wrapping a call were the same hole.  Delegate to the inner
         * expression: emit ITS statement form (running its effects, and its
         * own discard handling -- e.g. the catch-unwind box free), and let the
         * pure wrapper vanish, which was the only part of the old behavior
         * that was right. */
        case EX_REINTERPRET: emit_stmt(ctx, body, e->as.reinterpret_.expr); return;
        case EX_CAST:        emit_stmt(ctx, body, e->as.cast_.expr);        return;
        case EX_ASCRIBE:     emit_stmt(ctx, body, e->as.ascribe_.inner);    return;
        case EX_POLY_WRAP:   emit_stmt(ctx, body, e->as.poly_wrap_.inner);  return;
        case EX_EXISTS_OPEN: { /* Phase HRT2: run open body for side effects */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_EXISTS_DISPATCH: { /* witness-indirected call; materialize for effects */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_FN_TO_FAT: { /* A#1: only ever an argument; if discarded, still
                              * materialize the shim so codegen stays valid. */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_POLY_TO_FAT: { /* SC7: like EX_FN_TO_FAT, normally an argument;
                                * materialize if discarded so codegen stays valid. */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_PANIC_WITH: {
            /* Diverging - emit the panic */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_CATCH_UNWIND:
        case EX_CATCH_PANIC_OF: {
            /* These produce values but also have side effects (setting up handlers).
             * In statement position the result box is discarded, so free it here
             * (catch-unwind-result-box-leak): an unfreed box -- and the caught
             * panic payload an err box owns -- would leak per catch site, unbounded
             * inside a loop.  Safe because a discarded statement-position value
             * provably does not escape. */
            char *v = emit_value(ctx, body, e);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_result_box_free((int64_t)(intptr_t)%s);\n", v);
            free(v);
            return;
        }
        case EX_CALLCC: {   /* call-cc-completion: run for effect, discard value */
            uint32_t pd_mark[3];
            emit_pending_drops_mark(ctx, pd_mark);
            char *v = emit_value(ctx, body, e);
            free(v);
            emit_pending_drops_drain(ctx, body, pd_mark);
            return;
        }
        case EX_WHILE: emit_while_stmt(ctx, body, e); return;
        case EX_SET:      emit_set_stmt(ctx, body, e);       return;
        case EX_SET_DEREF: emit_set_deref_stmt(ctx, body, e); return;
        case EX_SET_FIELD: emit_set_field_stmt(ctx, body, e); return;
        case EX_DO: {
            /* v1 lowering: use same frame-based approach as emit_do_value */
            uint32_t n = e->as.do_.n;
            
            /* Check if we have any defers in this scope */
            bool has_defers = false;
            for (uint32_t i = 0; i < n; i++) {
                if (e->as.do_.items[i]->kind == EX_DEFER) {
                    has_defers = true;
                    break;
                }
            }

            /* Also check for return/throw/panic in any item (for emitted_diverging tracking) */
            for (uint32_t i = 0; i < n; i++) {
                if (expr_contains_return_or_throw(e->as.do_.items[i])) {
                    /* Will use emitted_diverging to track */
                    break;
                }
            }

            if (!has_defers) {
                /* No defers - emit all items */
                for (uint32_t i = 0; i < n; i++) {
                    emit_stmt(ctx, body, e->as.do_.items[i]);
                }
                return;
            }

            /* Has defers - use tur_frame */
            const char *saved_frame = ctx->frame_var;
            char *frame_var = fresh_frame(ctx);
            ctx->frame_var = frame_var;
            emit_frame_note_parent(frame_var, saved_frame);

            /* Emit frame declaration and init */
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_frame %s;\n", frame_var);
            indent_buf(body, ctx->indent);
            if (saved_frame) {
                buf_printf(body, "tur_frame_init(&%s, &%s);\n", frame_var, saved_frame);
            } else {
                buf_printf(body, "tur_frame_init(&%s, NULL);\n", frame_var);
            }

            /* Emit non-defer items and register defer thunks */
            bool emitted_diverging = false;  /* Track if we've emitted a return/throw/panic */
            for (uint32_t i = 0; i < n; i++) {
                const Expr *it = e->as.do_.items[i];
                
                /* If we've already emitted a diverging expression, skip remaining items */
                if (emitted_diverging) {
                    continue;
                }
                
                if (it->kind == EX_DEFER) {
                    emit_frame_push_defer(ctx, body, frame_var, it);
                } else {
                    emit_stmt(ctx, body, it);
                    /* Check if this statement is a panic (which fires defers via tur_frame_fire_chain) */
                    if (it->kind == EX_PANIC || it->kind == EX_PANIC_WITH) {
                        emitted_diverging = true;
                    }
                }
            }

            /* Fire all defers LIFO at scope exit */
            /* But if a diverging expression was emitted, it already fired defers */
            if (!emitted_diverging) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);
            }

            /* Restore frame state */
            ctx->frame_var = saved_frame;
            free(frame_var);
            return;
        }
        case EX_LET:
        case EX_LETREC: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_IF: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_BUILTIN: {
            char *v = emit_value(ctx, body, e);
            /* If the builtin produced a value (non-nil), emit it as a
             * void-cast expression statement. For nil-typed (println) this
             * was already a stmt. */
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_DEF: case EX_PROGRAM:
            /* shouldn't reach here at expr level */
            return;
        case EX_DEFDATA:
            /* Phase G0/G1: ADT definition — emitted in Pass 0, nothing to do here */
            return;
        case EX_MATCH: {
            /* Phase G0: emit match in stmt position — discard result */
            char *v = emit_value(ctx, body, e);
            if (v && e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        /* Phase 2 */
        case EX_FN_DEF:
            /* shouldn't reach here in stmt position */
            fprintf(stderr, "tur: emit: EX_FN_DEF in stmt position\n");
            abort();
            return;
        /* saffron-lang-plan S5: all three dynamic nodes are emitted for effect,
         * and none of them may be discarded.
         *
         * S3/S4 listed the operator and the field read with the pure forms
         * above, on the reasoning that `(+ x 1)` alone on a line is dead code.
         * That was true only while nothing was emitted for them.  It is wrong
         * now on both counts: `println` is a dynamic OPERATOR, so discarding one
         * would drop the output; and every dynamic node can PANIC on the type
         * that actually arrives -- `(+ x 1)` where x is a cstr, `(.f v)` where v
         * has no such field -- which the interpreter reports because it
         * evaluates statement-position expressions.  Dropping the emission would
         * make the compiled program silently accept what the interpreter
         * rejects.  They are not pure; they were only unemitted.
         *
         * A separate arm from EX_CALL rather than a shared one: the block below
         * reads `e->as.call_.fn_binding`, and a dyn-call node's union carries
         * `dyn_call_` there instead -- reading it would interpret the callee
         * expression pointer as a Binding. */
        case EX_DYN_OP:
        case EX_DYN_FIELD:
        /* saffron-lang-plan S9: a dispatched method call is a call -- it runs an
         * instance body, so statement position must still emit it. */
        case EX_DYN_METHOD:
        case EX_DYN_CALL: {
            uint32_t pd_mark[3];
            emit_pending_drops_mark(ctx, pd_mark);
            char *v = emit_value(ctx, body, e);
            if (v && v[0]) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            emit_pending_drops_drain(ctx, body, pd_mark);
            return;
        }
        case EX_CALL: {
            /* G1 (carrier<->concrete crossing audit): `tur-list-homog__` is the
             * compile-time-only element-homogeneity assertion the `(list ...)`
             * macro chains over adjacent elements. Its inline-C body is a no-op
             * (`(void)a;(void)b;`) and the type unification that actually enforces
             * homogeneity (and emits TUR-E0001 on a mismatch) already ran during
             * elaboration -- so the emitted runtime call is dead. Its single C
             * signature takes the int64 carrier; a scalar / heap-pointer element
             * coerces into that carrier fine (existing snapshots keep the call),
             * but a by-value aggregate element (e.g. Option__int) cannot, which is
             * the only case that mismatched. Skip emitting the dead call there;
             * the element is still constructed once by `list-build__`. */
            {
                const Binding *cb = e->as.call_.fn_binding;
                if (cb && cb->name && cb->name->name &&
                    strcmp(cb->name->name, "tur-list-homog__") == 0) {
                    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                        Type at = emit_resolve_type(ctx, e->as.call_.args[i]->type);
                        if (type_uses_carrier_abi(at) && !type_is_heap_struct(at))
                            return;
                    }
                }
            }
            /* Emit as expression statement.  Bracketed by the pending-drop
             * mark/drain: a VOID callee never materializes in emit_value, so a
             * fresh sum-box argument's free (stamped sum_box_drop_after) would
             * otherwise be orphaned here -- see emit_pending_drops_mark. */
            uint32_t pd_mark[3];
            emit_pending_drops_mark(ctx, pd_mark);
            char *v = emit_value(ctx, body, e);
            indent_buf(body, ctx->indent);
            if (e->type.kind == TY_NIL) {
                buf_printf(body, "%s;\n", v);
            } else {
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            emit_pending_drops_drain(ctx, body, pd_mark);
            return;
        }
        case EX_FN:
            fprintf(stderr, "tur: emit: EX_FN not yet implemented in emit_stmt\n");
            abort();
            return;
        case EX_EXTERN_C:
            /* extern-c declarations emit nothing in statement position (they're file-scope) */
            return;
        /* Phase R2: Panic */
        case EX_PANIC: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 18: Delimited continuations.
         * A `reset` in statement position (its value discarded, e.g. a non-final
         * `do` item) is lowered through emit_value -- which owns the DK machine
         * lowering (emit_cps_reset) -- and its result is discarded.  This mirrors
         * EX_SERIAL_RESET below.  Emitting a bare __builtin_trap() here was a
         * silent-compile/runtime-SIGILL bug (do-discarded-reset-shift-crash):
         * it fired whenever a `(do (reset (shift ...)) <tail>)` landed on the
         * direct/fiber emitter (e.g. a colored function evicted from the CPS
         * backend by an owning-field aggregate return). */
        case EX_RESET: {
            char *v = emit_value(ctx, body, e);
            indent_buf(body, ctx->indent);
            buf_printf(body, "(void)(%s);\n", v);
            free(v);
            return;
        }
        case EX_SHIFT:
        case EX_SHIFT0:
            /* A bare shift in statement position (no enclosing reset to reify a
             * continuation into) is not lowerable; emit a placeholder trap. */
            buf_puts(body, "__builtin_trap();");
            return;
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            /* For now, emit a placeholder - full impl deferred */
            buf_puts(body, "__builtin_trap();");
            return;
        /* Phase 21: Serializable continuations. A serial-reset in statement
         * position is routed through emit_value so a lowerable (or shift-free)
         * reset still works; its value is discarded. */
        case EX_SERIAL_RESET: {
            char *v = emit_value(ctx, body, e);
            indent_buf(body, ctx->indent);
            buf_printf(body, "(void)(%s);\n", v);
            free(v);
            return;
        }
        /* serial-shift-unsupported-context-miscompile: reaching a serial-shift
         * in statement position means an enclosing serial-reset could not lower
         * its delimited context (e.g. an unsupported `do` shape).  This used to
         * emit __builtin_trap() -- a silent compile, runtime crash.  Reject it
         * with a real diagnostic instead. */
        case EX_SERIAL_SHIFT:
            diag_emit_with_code(DIAG_ERROR, e->span,
                                TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
                                "serial-shift context is not capturable\n"
                                "  = note: the delimited context falls outside "
                                "the DK lowering grammar, so the continuation "
                                "cannot be reified\n"
                                "  = help: restructure into a supported shape; "
                                "run `tur explain TUR-E0706` for details");
            buf_puts(body, "__builtin_trap(); /* serial-shift: rejected (TUR-E0706) */");
            return;
        case EX_INSTANCE_DEF: {
            /* Phase 15: Emit dictionary struct and global singleton */
            TypeClassInstance *inst = e->as.instance_def_.instance;
            TypeClass *tc = inst->typeclass;

            /* Phase 5 carrier-bridge deletion -- dead-instance elimination: skip
             * the whole dict (struct + singleton) for a DEAD instance, in
             * lockstep with emit_abi_fn_skip_generic skipping its method bodies
             * / carrier bases.  An instance whose methods are never directly
             * called AND whose dict singleton is never referenced (EX_DICT
             * dispatch or an existential witness table -- both noted as liveness
             * in emit_abi_scan_expr) is pure dead code: for HKT instances its
             * `ap`/`fmap`/`bind` carrier bases carry the ubiquitous
             * `carrier->concrete (Option (fn ...))` crossing; for ground
             * (kind-*) instances the dict slot would reference a body that the
             * body-skip dropped.  Both halves key off emit_instance_is_live so
             * the dict initializer never references a skipped base/body (and
             * vice versa). */
            {
                /* Skip the dict (struct + singleton) for a DEAD instance, in
                 * lockstep with emit_abi_fn_skip_generic (emit_module.c)
                 * skipping its method bodies.  Both HKT (M7) and ground
                 * (kind-*) instances key off the same emit_instance_is_live,
                 * so the dict initializer never references a skipped
                 * __inst_<Class>_<method>_<T> body and a dead instance
                 * contributes neither dict nor body.  Ground coverage closes
                 * the json Decode `__inst_Decode_decode_int undeclared`
                 * miscompile (a ground dict was previously emitted
                 * unconditionally while its body could be dropped).  Ground
                 * instances like Eq are unaffected unless genuinely dead (no
                 * method directly called). */
                if (!emit_instance_is_live(ctx, inst))
                    break;  /* skip emitting the dead instance's dict */
            }

            /* Generate dictionary struct name: dict_<TypeClass>_<typeargs>
             * Mirrors the type_suffix logic in elab_definstance so that
             * the struct name matches the method impl names already emitted. */
            char dict_name[128];
            char type_suffix[320] = "";  /* wide enough for the longest mangled component (<=259) */
            for (uint8_t i = 0; i < inst->n_type_args; i++) {
                if (i == 0) strcat(type_suffix, "_");
                const char *component = "T";
                switch (inst->type_args[i].kind) {
                    case TY_INT:      component = "int";      break;
                    case TY_BOOL:     component = "bool";     break;
                    case TY_CSTR:     component = "cstr";     break;
                    case TY_NIL:      component = "nil";      break;
                    case TY_PTR_VOID: component = "ptr_void"; break;
                    case TY_INT8:     component = "int8";     break;
                    case TY_INT16:    component = "int16";    break;
                    case TY_INT32:    component = "int32";    break;
                    case TY_UINT8:    component = "uint8";    break;
                    case TY_UINT16:   component = "uint16";   break;
                    case TY_UINT32:   component = "uint32";   break;
                    case TY_UINT64:   component = "uint64";   break;
                    case TY_FLOAT:    component = "float";    break;
                    case TY_FLOAT32:  component = "float32";  break;
                    case TY_FLOAT64:  component = "float64";  break;
                    case TY_SYM:      component = "Sym";      break;
                    /* structdef-retirement DS-C: the former TY_STRUCT instance-head
                     * arm is dead -- a named struct head is now a TY_ADT (below)
                     * and an unknown head is a TY_TYVAR (below); no instance
                     * type-arg is ever TY_STRUCT. */
                    case TY_TYVAR:
                        /* structdef-retirement slice 5 B2 (P3): an unresolved
                         * instance head (unknown name like "option"/"vec") is
                         * now a named TY_TYVAR carrying its source symbol in
                         * type_arg_syms.  Name the dict struct by it, mirroring
                         * TY_STRUCT -- else two such instances both emit
                         * dict_<C>_T and collide (ODR). */
                        if (inst->type_arg_syms && inst->type_arg_syms[i]) {
                            component = inst->type_arg_syms[i]->name;
                        } else if (inst->type_args[i].as.tyvar_.name) {
                            component = inst->type_args[i].as.tyvar_.name;
                        }
                        break;
                    case TY_ADT:
                        /* CONV-S1 (defstruct-as-defadt): a record-ADT head names its
                         * dict struct/singleton by the constructor, mirroring
                         * TY_STRUCT / emit_dict_name -- otherwise two ADT-headed
                         * instances both emit dict_<C>_T and collide (ODR). */
                        if (inst->type_arg_syms && inst->type_arg_syms[i]) {
                            component = inst->type_arg_syms[i]->name;
                        } else if (inst->type_args[i].as.adt_.def &&
                                   inst->type_args[i].as.adt_.def->name) {
                            component = inst->type_args[i].as.adt_.def->name;
                        }
                        break;
                    case TY_APP: {
                        /* Phase HKT §3: partial type application — encode as "ctor_arg"
                         * e.g. (result int) → "result_int" → dict_Functor_result_int */
                        const char *fn_part  = "T";
                        const char *arg_part = "T";
                        if (inst->type_arg_syms && inst->type_arg_syms[i])
                            fn_part = inst->type_arg_syms[i]->name;
                        if (inst->type_args[i].as.app.arg) {
                            const char *n = type_name(*inst->type_args[i].as.app.arg);
                            if (n) arg_part = n;
                        }
                        char mfn[128], marg[128], app_comp[260];
                        tur_mangle_ident(fn_part, mfn, sizeof(mfn));
                        tur_mangle_ident(arg_part, marg, sizeof(marg));
                        snprintf(app_comp, sizeof(app_comp), "%s_%s", mfn, marg);
                        strncat(type_suffix, app_comp, sizeof(type_suffix) - strlen(type_suffix) - 1);
                        continue;
                    }
                    default: break;
                }
                char comp_buf[128];
                tur_mangle_ident(component, comp_buf, sizeof(comp_buf));
                strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
            }
            snprintf(dict_name, sizeof(dict_name), "dict_%s%s", tc->name->name, type_suffix);
            
            /* Helper to sanitize method name for C identifiers */
            char sanitized_method_name[64];
            
            /* Emit the dictionary struct to file scope */
            buf_printf(ctx->file, "typedef struct %s {\n", dict_name);
            for (uint8_t i = 0; i < tc->n_methods; i++) {
                const TypeClassMethod *method = &tc->methods[i];
                FnDef *method_impl = inst->method_impls[i];
                
                /* Mangle method name into a C field name. Routed through the
                 * shared mangler so sigil pairs (`>>>`/`<<<`) get distinct
                 * fields instead of colliding on `___`. */
                char sanitized_method_name[64];
                tur_mangle_ident(method->name->name, sanitized_method_name,
                                 sizeof(sanitized_method_name));

                /* Build function pointer type */
                buf_puts(ctx->file, "    ");
                
                /* Return type: mirror the priority used by emit_fns.c.
                 *
                 * 1. emit_carrier_return_override: body returns a struct by
                 *    value (e.g. HasSchema decode → User) → use the struct type.
                 * 2. binding result_full_type: poly result with a known full type.
                 * 3. binding result_kind: handles inline-C (TY_NIL body), bare
                 *    fn-reference bodies (TY_FN), and bool-typed methods where
                 *    body literal 1 has kind TY_INT.
                 * 4. body->type fallback when no binding is available. */
                Type ret_type;
                /* structdef-retirement DS-C: emit_carrier_return_override is dead
                 * (a method body is never TY_STRUCT); the `carrier_override.kind
                 * == TY_STRUCT` branch is removed. */
                if (method_impl->binding
                           && method_impl->binding->type.kind == TY_FN) {
                    Type *rft = method_impl->binding->type.as.fn.result_full_type;
                    ret_type = rft ? *rft
                                   : emit_type_from_kind(
                                         method_impl->binding->type.as.fn.result_kind);
                } else {
                    ret_type = method_impl->body->type;
                }
                const char *ret_c_name = dict_slot_ret_c_name(ctx, method_impl, ret_type);
                /* instance-method-closure-return: a method whose declared
                 * return is a concrete function value carries it as a thin
                 * fn-ptr typedef (non-capturing body) or the int64_t closure
                 * carrier (fat box) -- never type_c_name's *result* type,
                 * which mis-lowers non-int result kinds (e.g. :float -> double)
                 * and breaks the impl-body return.  Must agree with the impl
                 * signature in emit_fns.c / emit_module.c. */
                if (ret_type.kind == TY_FN) {
                    const char *carrier =
                        emit_inst_fn_return_carrier(method_impl, &ret_type);
                    if (carrier) ret_c_name = carrier;
                }
                if (dict_slot_result_is_word_scalar(tc, (int)i, method_impl, NULL))
                    ret_c_name = "int64_t";
                buf_printf(ctx->file, "%s", ret_c_name);
                buf_puts(ctx->file, " (*");
                buf_printf(ctx->file, "%s", sanitized_method_name);
                buf_puts(ctx->file, ")(");
                
                /* Parameter types — Phase HRT3: use tur_poly_fn_t for poly fn params.
                 * Mirror emit_fns.c: pass_by_ptr structs use const T* in the slot typedef
                 * so it matches the actual by-pointer function signature. */
                bool body_is_inline_c = (method_impl->body
                                         && method_impl->body->kind == EX_INLINE_C);
                for (uint32_t j = 0; j < method_impl->n_params; j++) {
                    if (j > 0) buf_puts(ctx->file, ", ");
                    if (method_impl->params && method_impl->params[j]->is_poly_fn) {
                        buf_puts(ctx->file, "tur_poly_fn_t");
                    } else {
                        Type pt = method_impl->param_types[j];
                        if (!method_impl->closure && !body_is_inline_c
                            && type_struct_pass_by_ptr(pt)) {
                            buf_printf(ctx->file, "const %s *", type_c_name(pt));
                        } else if (dict_slot_param_is_carrier(ctx, method_impl, j) ||
                                   dict_slot_param_is_word_scalar(tc, (int)i, method_impl, j)) {
                            /* D8 piece 2 (forall-dict-byvalue-receiver): a
                             * BY-VALUE aggregate parameter is spelled as the
                             * CARRIER here, and the slot is filled with a
                             * per-instance wrapper that derefs it (below).
                             *
                             * Safe because this dict slot is ONLY ever read
                             * through the mode-B `(void **)dict[slot]` pun --
                             * measured: in a rank-2 program the singleton is
                             * written at init and read only through that cast,
                             * and a program with only STATIC dispatch emits no
                             * dict at all.  So making the slot carrier-shaped
                             * cannot disturb a typed caller; there is none. */
                            buf_puts(ctx->file, "int64_t");
                        } else {
                            buf_printf(ctx->file, "%s", type_c_name(pt));
                        }
                    }
                }
                buf_puts(ctx->file, ");\n");
            }
            buf_printf(ctx->file, "} %s;\n\n", dict_name);
            
            /* D8 piece 2: per-instance CARRIER WRAPPERS for any method that
             * takes a by-value aggregate.
             *
             * The mode-B clone reads every slot as a carrier-shaped function
             * pointer, and the CALLER already boxes a by-value argument into
             * the carrier (a malloc'd pointer -- see the poly call site).  What
             * was missing was the other end: the slot held the raw instance
             * function, whose parameter is the struct BY VALUE, so the pun
             * passed a pointer where a struct was expected.  That was
             * `incompatible type for argument 1` from cc, and is why the shape
             * was guarded rather than supported.
             *
             * One wrapper per (instance, method) makes the pun honest -- it
             * takes the carrier, derefs it, and calls the impl -- so the slot
             * and the cast finally agree. */
            for (uint8_t i = 0; i < tc->n_methods; i++) {
                FnDef *mi = inst->method_impls[i];
                if (!mi) continue;
                /* A zero-parameter method (`default-of [] : a`) has no
                 * param_types, but its class-variable RESULT still needs the
                 * word-returning wrapper the slot fill below points at. */
                if (!mi->param_types &&
                    !dict_slot_result_is_word_scalar(tc, (int)i, mi, NULL))
                    continue;
                const char *res_impl_c = NULL;
                bool res_word = dict_slot_result_is_word_scalar(tc, (int)i, mi, &res_impl_c);
                bool needs_wrap = res_word;
                for (uint32_t j = 0; j < mi->n_params; j++)
                    if (dict_slot_param_is_carrier(ctx, mi, j) ||
                        dict_slot_param_is_word_scalar(tc, (int)i, mi, j)) {
                        needs_wrap = true; break;
                    }
                if (!needs_wrap) continue;
                tur_mangle_ident(tc->methods[i].name->name, sanitized_method_name,
                                 sizeof(sanitized_method_name));
                Type wret;
                if (mi->binding && mi->binding->type.kind == TY_FN) {
                    Type *rft = mi->binding->type.as.fn.result_full_type;
                    wret = rft ? *rft
                               : emit_type_from_kind(mi->binding->type.as.fn.result_kind);
                } else {
                    wret = mi->body->type;
                }
                buf_printf(ctx->file, "static %s __dictwrap_%s_%s%s(",
                           res_word ? "int64_t" : dict_slot_ret_c_name(ctx, mi, wret),
                           tc->name->name, sanitized_method_name, type_suffix);
                bool wrap_body_is_inline_c = (mi->body
                                              && mi->body->kind == EX_INLINE_C);
                for (uint32_t j = 0; j < mi->n_params; j++) {
                    if (j) buf_puts(ctx->file, ", ");
                    if (mi->params && mi->params[j]->is_poly_fn)
                        buf_printf(ctx->file, "tur_poly_fn_t __a%u", j);
                    else if (dict_slot_param_is_carrier(ctx, mi, j) ||
                             dict_slot_param_is_word_scalar(tc, (int)i, mi, j))
                        buf_printf(ctx->file, "int64_t __a%u", j);
                    else if (!mi->closure && !wrap_body_is_inline_c
                             && type_struct_pass_by_ptr(mi->param_types[j]))
                        /* Same predicate the slot typedef above uses, and the
                         * same one the impl itself was emitted with: a
                         * pass-by-ptr aggregate is spelled `const T *`.  The
                         * wrapper used to spell it `T` here, so for a method
                         * whose RESULT forced a wrapper (a by-value ADT, which
                         * returns through the word-boxing tail below) but whose
                         * PARAMS are ordinary pass-by-ptr structs, the wrapper
                         * took the struct by value and handed it to an impl
                         * expecting a pointer -- and the slot it filled was
                         * typed `const T *` either way.  cc saw both halves:
                         * `incompatible type for argument 1` on the call and
                         * `-Wincompatible-function-pointer-types` on the slot
                         * fill.  The argument loop below already passes
                         * `__a%u` straight through, which is what a pointer
                         * parameter wants. */
                        buf_printf(ctx->file, "const %s * __a%u",
                                   type_c_name(mi->param_types[j]), j);
                    else
                        buf_printf(ctx->file, "%s __a%u",
                                   type_c_name(mi->param_types[j]), j);
                }
                if (mi->n_params == 0) buf_puts(ctx->file, "void");
                buf_puts(ctx->file, ") {\n    ");
                if (res_word) buf_printf(ctx->file, "%s __r = ", res_impl_c);
                else          buf_puts(ctx->file, "return ");
                if (mi->binding && mi->binding->name)
                    buf_printf(ctx->file, "%s(", mi->binding->name->name);
                else
                    buf_printf(ctx->file, "__inst_%s_%s%s(", tc->name->name,
                               sanitized_method_name, type_suffix);
                for (uint32_t j = 0; j < mi->n_params; j++) {
                    if (j) buf_puts(ctx->file, ", ");
                    if (dict_slot_param_is_carrier(ctx, mi, j))
                        buf_printf(ctx->file, "*(%s *)(intptr_t)__a%u",
                                   type_c_name(mi->param_types[j]), j);
                    else if (dict_slot_param_is_word_scalar(tc, (int)i, mi, j)) {
                        /* The word carries a float kind as its bits (the
                         * carrier convention, emit_word_slot_bits); a pointer
                         * or a narrower integer is the word itself. */
                        const char *pc = type_c_name(mi->param_types[j]);
                        size_t pL = strlen(pc);
                        if (strcmp(pc, "double") == 0)
                            buf_printf(ctx->file,
                                "((union { int64_t i; double d; }){ .i = __a%u }).d", j);
                        else if (strcmp(pc, "float") == 0)
                            buf_printf(ctx->file,
                                "((union { uint32_t u; float f; }){ .u = (uint32_t)__a%u }).f", j);
                        else if (pc[pL - 1] == '*')
                            buf_printf(ctx->file, "(%s)(intptr_t)__a%u", pc, j);
                        else
                            buf_printf(ctx->file, "(%s)__a%u", pc, j);
                    }
                    else
                        buf_printf(ctx->file, "__a%u", j);
                }
                if (res_word) {
                    size_t rL = strlen(res_impl_c);
                    buf_puts(ctx->file, ");\n");
                    if (strcmp(res_impl_c, "double") == 0)
                        buf_puts(ctx->file, "    return ((union { double d; int64_t i; }){ .d = __r }).i;\n}\n");
                    else if (strcmp(res_impl_c, "float") == 0)
                        buf_puts(ctx->file, "    return (int64_t)((union { float f; uint32_t u; }){ .f = __r }).u;\n}\n");
                    else if (res_impl_c[rL - 1] == '*')
                        buf_puts(ctx->file, "    return (int64_t)(intptr_t)__r;\n}\n");
                    else if (strncmp(res_impl_c, "tur_adt_", 8) == 0)
                        buf_printf(ctx->file,
                            "    %s *__b = (%s *)malloc(sizeof(%s));\n"
                            "    *__b = __r;\n"
                            "    TUR_REGION_NOTE_WORDS(__b, sizeof *__b);\n"
                            "    return (int64_t)(intptr_t)__b;\n}\n",
                            res_impl_c, res_impl_c, res_impl_c);
                    else
                        buf_puts(ctx->file, "    return (int64_t)__r;\n}\n");
                } else {
                    buf_puts(ctx->file, ");\n}\n");
                }
            }

            /* Emit the global singleton dictionary to file scope */
            buf_printf(ctx->file, "static %s %s_singleton = {\n", dict_name, dict_name);
            for (uint8_t i = 0; i < tc->n_methods; i++) {
                const TypeClassMethod *method = &tc->methods[i];
                
                /* Mangle method name into the C field name (must match the
                 * struct field spelling emitted above). */
                tur_mangle_ident(method->name->name, sanitized_method_name,
                                 sizeof(sanitized_method_name));

                buf_puts(ctx->file, "    ");
                buf_printf(ctx->file, ".%s = ", sanitized_method_name);
                /* Phase HKT H3: Use the binding name directly — it already encodes
                 * the correct type-arg suffix (e.g. _option, _vec) as computed in
                 * elab_definstance, so we avoid a second, potentially wrong suffix. */
                FnDef *method_impl_ref = inst->method_impls[i];
                bool slot_wrapped =
                    dict_slot_result_is_word_scalar(tc, (int)i, method_impl_ref, NULL);
                if (!slot_wrapped && method_impl_ref && method_impl_ref->param_types) {
                    for (uint32_t j = 0; j < method_impl_ref->n_params; j++)
                        if (dict_slot_param_is_carrier(ctx, method_impl_ref, j) ||
                            dict_slot_param_is_word_scalar(tc, (int)i, method_impl_ref, j)) {
                            slot_wrapped = true; break;
                        }
                }
                if (slot_wrapped) {
                    /* D8 piece 2: the carrier wrapper, not the raw impl. */
                    buf_printf(ctx->file, "__dictwrap_%s_%s%s", tc->name->name,
                               sanitized_method_name, type_suffix);
                } else if (method_impl_ref && method_impl_ref->binding) {
                    buf_printf(ctx->file, "%s", method_impl_ref->binding->name->name);
                } else {
                    buf_printf(ctx->file, "__inst_%s_%s", tc->name->name, sanitized_method_name);
                    buf_puts(ctx->file, type_suffix);
                }
                buf_puts(ctx->file, ",\n");
            }
            buf_printf(ctx->file, "};\n\n");

            /* saffron-lang-plan S9 (D8 piece 3c): record the registry row here,
             * where the singleton has just been written, rather than rebuilding
             * the mangled name later from the instance list.  The row can then
             * only ever name a symbol this TU actually emitted -- the failure
             * mode of the reverted first attempt, which built the table from
             * the instance list in the preamble and referenced singletons that
             * dead-instance elimination had dropped (or that had not been
             * emitted yet). */
            {
                int64_t __disp_tag = 0;
                if (emit_instance_dispatch_tag(ctx, inst, &__disp_tag))
                    emit_instance_dyn_table(ctx, inst, dict_name, type_suffix,
                                            __disp_tag);
            }
            return;
        }
        /* Phase H §1: dictionary passing — EX_DICT is a pure value node; no statement to emit */
        case EX_DICT:
            return;
        case EX_INLINE_C: {
            /* Emit the raw C code inline as a statement (with __TUR_CAP_N__ /
             * __TUR_VAL_N__ substitution).  Full inline-C function bodies already
             * end with ';' or '}'; session ops need a semicolon appended.
             * Strip trailing whitespace and block comments, then check last byte. */
            InlineC *ic = e->as.inline_c_.inline_c;
            char *subst = inline_c_substitute(ctx, body, ic);
            indent_buf(body, ctx->indent);
            buf_puts(body, subst);
            /* Find last significant byte in the substituted string. */
            size_t l = strlen(subst);
            bool strip_again;
            do {
                strip_again = false;
                while (l > 0 && (subst[l-1] == ' ' || subst[l-1] == '\t'
                                 || subst[l-1] == '\n' || subst[l-1] == '\r')) {
                    l--; strip_again = true;
                }
                if (l >= 2 && subst[l-1] == '/' && subst[l-2] == '*') {
                    size_t j = l - 2;
                    bool found = false;
                    while (j > 0) {
                        j--;
                        if (subst[j] == '/' && subst[j+1] == '*') {
                            l = j; strip_again = true; found = true; break;
                        }
                    }
                    if (!found) { l = 0; break; }
                }
            } while (strip_again);
            bool needs_semi = l > 0 && subst[l-1] != ';' && subst[l-1] != '}';
            free(subst);
            if (needs_semi) { buf_putc(body, ';'); }
            buf_putc(body, '\n');
            return;
        }
        /* Phase 3 */
        case EX_CLOSURE: {
            /* Emit closure as expression and discard value */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        /* Phase 4: a bare defer that escapes its containing EX_DO (e.g. the
         * sole body of a let or defn) degenerates to "fires at scope end"
         * trivially — emit the body inline. */
        case EX_DEFER: {
            emit_stmt(ctx, body, e->as.defer_.body);
            return;
        }
        /* Phase 3/4: return */
        case EX_RETURN: {
            /* GF1: Inside a generator _next function, (return) terminates the generator */
            if (ctx->gen_var_name) {
                ctx->return_emitted = true;
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s->__state = -1;\n", ctx->gen_var_name);
                indent_buf(body, ctx->indent);
                buf_puts(body, "return NULL;\n");
                return;
            }

            /* (return) or (return expr) - emit full return statement with defer firing */

            /* Set flag to indicate return has been emitted */
            ctx->return_emitted = true;

            /* S1b/dynvar early-exit: pop every dynamic binding still in scope,
             * innermost first -- the order the cleanup attribute would use.
             * Without this the JIT (where c2mir drops that attribute) leaves
             * the dynvar key pointing into this frame after it returns. */
            for (uint32_t di = ctx->n_dynvar_guards; di-- > 0; ) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "_dynvar_pop_%s(&%s);\n",
                           ctx->dynvar_guard_names[di], ctx->dynvar_guard_ptrs[di]);
            }

            /* Fire all defers in the frame chain if we're in a scope with defers */
            if (ctx->frame_var) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_fire_chain(&%s);\n", ctx->frame_var);
            }

            /* Emit the return statement.
             *
             * any-struct-box-leak-per-widen: an `any` local owned by an
             * enclosing scope is released here, because the trailing free at
             * that scope's end is about to be jumped past.  The drop must come
             * AFTER the return value is computed -- `(return (reads a))` reads
             * the box -- so the value is hoisted to a temp first when there is
             * anything to drop.  Zero drops keeps the emitted form byte-identical
             * to before. */
            if (ctx->n_any_scope_drops > 0 && e->as.return_.value) {
                char *val = emit_value(ctx, body, e->as.return_.value);
                char *rv = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                /* Name the type rather than reach for __auto_type: this frame's
                 * C return type is exactly what the temp holds, and c2mir does
                 * not take __auto_type -- a JIT run silently falls back to cc
                 * for the whole fixture when it appears.  __auto_type stays as
                 * the last resort for the shapes that do not record a return
                 * ctype, matching the panic-signal hoist next door. */
                buf_printf(body, "%s %s = (%s);\n",
                           ctx->current_fn_ret_ctype ? ctx->current_fn_ret_ctype
                                                     : "__auto_type",
                           rv, val);
                free(val);
                emit_any_scope_drops(ctx, body);
                indent_buf(body, ctx->indent);
                buf_printf(body, "return %s;\n", rv);
                free(rv);
                return;
            }
            if (ctx->n_any_scope_drops > 0) emit_any_scope_drops(ctx, body);
            indent_buf(body, ctx->indent);
            if (e->as.return_.value) {
                char *val = emit_value(ctx, body, e->as.return_.value);
                buf_printf(body, "return %s;\n", val);
                free(val);
            } else {
                buf_puts(body, "return;\n");
            }
            return;
        }
        /* GF1: yield inside a generator _next function */
        case EX_YIELD: {
            uint32_t yid    = e->as.yield_.yield_id;
            const char *gv  = ctx->gen_var_name;
            char *yval      = emit_value(ctx, body, e->as.yield_.value);
            /* Save state for resumption */
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->__state = %u;\n", gv, yid);
            /* Allocate SOME(value) and return it */
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "int64_t *__opt = (int64_t *)malloc(sizeof(int64_t));\n");
            /* generator-yield-payload-is-int64-only: the slot holds the
             * value's BITS -- `(int64_t)` on a double was a value conversion
             * (7.25 -> 7); a pointer is cast, a bool widened.  The typed
             * `gen-unwrap` reinterprets the same way on the way out. */
            /* Inside a spec the value is the clone's concrete `A`: take its
             * bits at the RESOLVED type (a tyvar-typed double went in by value
             * conversion), and box a by-value aggregate the way a container
             * element is boxed -- `gen-unwrap` bridges the word back. */
            Type yt = emit_resolve_type(ctx, e->as.yield_.value->type);
            const char *ytc = emit_type_c_name(ctx, yt);
            if (ytc && strncmp(ytc, "tur_adt_", 8) == 0 && !strchr(ytc, '*') &&
                !type_is_heap_adt(yt) && !type_is_heap_struct(yt)) {
                char *boxed = emit_carrier_bridge_escaping(ctx, body, strdup(yval),
                                                           CK_CONCRETE, CK_CARRIER, yt);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*__opt = %s;\n", boxed);
                free(boxed);
            } else {
                indent_buf(body, ctx->indent);
                char *ybits = emit_word_slot_bits(&yt, yval);
                buf_printf(body, "*__opt = %s;\n", ybits);
                free(ybits);
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "return (void *)__opt;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            free(yval);
            /* Resume label -- execution continues here on next _next call */
            buf_printf(body, "__yield_%u:;\n", yid - 1);
            return;
        }
        /* Phase 5 */
        case EX_REF: {
            /* (ref expr) as statement - emit and discard */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_DEREF: {
            /* (@ expr) as statement - emit and discard */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        /* Phase 9: rc<T> + weak<T> as statements */
        case EX_RC_OF:
        case EX_RC_CLONE:
        case EX_RC_PTR:
        case EX_RC_COUNT:
        case EX_RC_FROM_REF:
        case EX_REF_FROM_RC:
        case EX_WEAK:
        case EX_WEAK_UPGRADE:
        case EX_WEAK_PRED:
        case EX_REF_PRED:
        case EX_CONT_PRED:
        /* Phase T21-F: async/await as statement - emit and discard */
        /* Phase SEL1: select as statement - emit and discard */
        case EX_ASYNC:
        case EX_AWAIT:
        case EX_SELECT: {
            /* Emit as value expression, discard result */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_RC_DROP: {
            /* rc/drop as statement - emit and discard (already returns nil) */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 12: Borrow traits as statements */
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT: {
            /* Borrow as statement - emit and discard (borrows are pointer operations) */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 20: Software Transactional Memory */
        case EX_STM:
        case EX_ATOMICALLY:
        case EX_RETRY:
        case EX_CHECK:
        case EX_OR_ELSE:
        case EX_TVAR_NEW:
        case EX_TVAR_READ:
        case EX_TVAR_WRITE:
        case EX_TVAR_MODIFY:
        case EX_TVAR_SWAP:
        case EX_TVAR_CAS: {
            /* STM expressions as statements - emit and discard result */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions are compile-time only */
            return;
        /* DV2: Dynamic vars */
        case EX_DEFDYNAMIC:
            /* Root init is handled by emit_module.c Pass 2 directly; nothing to do here. */
            return;
        case EX_DYNVAR_READ:
            /* Reading a var as a statement discards the value; skip. */
            return;
        case EX_DYNVAR_BINDING:
        case EX_DYNVAR_SET: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* GF1: Generator expressions as statements -- emit and discard */
        case EX_GEN:
        case EX_GEN_NEXT:
        case EX_GEN_DONE:
        case EX_GEN_UNWRAP:
        case EX_PERFORM:
        case EX_HANDLE:
            /* defopaque-struct-payload-fails-through-unsafe-helper: a pure
             * `Unsafe` handle in statement position (its result discarded)
             * emits its body as a statement so the body's side effects still
             * run, without forcing the value through a typed temp (whose
             * carrier-vs-struct type we cannot recover here).  The value
             * position is handled in emit_effects_handle. */
            /* `e->kind == EX_HANDLE` is NOT redundant: this block is shared
             * with EX_GEN / EX_GEN_NEXT / EX_GEN_DONE / EX_PERFORM above, and
             * `as` is a union.  Without the test, a `(perform ...)` in
             * statement position reinterpreted its PerformExpr as a
             * HandleExpr and read `is_unsafe_marker` out of unrelated bytes --
             * UBSan: "load of value 190, which is not a valid value for type
             * '_Bool'".  A non-zero byte there sent the emitter down the
             * pure-Unsafe path and made it emit `handle->body`, a pointer read
             * from the wrong union member. */
            if (e->kind == EX_HANDLE && e->as.handle_.handle &&
                emit_handle_is_pure_unsafe(e->as.handle_.handle)) {
                emit_stmt(ctx, body, e->as.handle_.handle->body);
                return;
            }
            /* fallthrough */
        case EX_HANDLER_LIT:
        case EX_WITH_HANDLER:
        case EX_COMPOSE_HANDLERS:
        case EX_RESUME:
        case EX_DISCONTINUE:
        case EX_MAKE_STRUCT:
        case EX_GET_FIELD:
        case EX_SET_LIT: {
            /* These should be lowered by effect_lower/CPS passes.
             * Fallback: emit via emit_value and discard result.
             * Do not recurse into emit_stmt with the same node. */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
    }
}
