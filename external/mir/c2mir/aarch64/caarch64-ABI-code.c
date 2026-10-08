/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   aarch64 call ABI target specific code.
*/

typedef int target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {}

/* AAPCS64 Homogeneous Floating-point Aggregates (HFA).
   ----------------------------------------------------
   A struct/array whose members are ALL the same fundamental floating-point
   type, at most four of them, is passed and returned in the SIMD registers
   v0..v7 -- one member per register -- not in x0..x7 and not by reference,
   even when the aggregate is larger than 16 bytes (four doubles is 32).

   Without this classification every aggregate <= 16 bytes went through the
   general-purpose registers.  That is self-consistent within a single c2mir
   compilation, so it was invisible in pure-JIT code, and wrong the moment
   c2mir-generated code called (or was called by) a natively compiled
   function: one side writes x0,x1 and the other reads v0,v1.  The resulting
   value is whatever was left in the SIMD registers, so the failure is
   data-dependent -- some call sites accidentally produce the right answer.

   Vector-flavoured C APIs are exactly this shape (`struct { float x, y; }`),
   so it is not an exotic corner.

   Deliberately NOT classified as HFAs here, all of which keep the previous
   general-purpose treatment:
     - unions.  AAPCS64 does admit a union of like FP members, but the size
       does not determine the member count the way it does for a struct, and
       the shape is vanishingly rare; being conservative keeps the
       classifier's `size == n * member_size` invariant exact.
     - over-aligned aggregates, where padding breaks that same invariant.
     - long double (MIR_T_LD), which is its own register class.
   A conservative answer here is the pre-existing behaviour, never a new
   miscall: the ONLY thing that matters is that this file, mir-gen-aarch64.c
   and mir-aarch64.c agree on which aggregates are HFAs.  Keep them in step. */
#define MAX_HFA_MEMBERS 4

/* Flatten `type` into `els`, appending one MIR type per scalar leaf.  Returns
   FALSE if it does not flatten into at most MAX_HFA_MEMBERS leaves. */
static int hfa_flatten (c2m_ctx_t c2m_ctx, struct type *type, MIR_type_t *els, int *n) {
  if (scalar_type_p (type)) {
    if (*n >= MAX_HFA_MEMBERS) return FALSE;
    els[(*n)++] = get_mir_type (c2m_ctx, type);
    return TRUE;
  } else if (type->mode == TM_ARR) {
    struct arr_type *at = type->u.arr_type;
    struct expr *cexpr;
    uint64_t nel;

    if (at->size->code == N_IGNORE || !(cexpr = at->size->attr)->const_p) return FALSE;
    nel = cexpr->c.i_val;
    for (uint64_t i = 0; i < nel; i++)
      if (!hfa_flatten (c2m_ctx, at->el_type, els, n)) return FALSE;
    return TRUE;
  } else if (type->mode == TM_STRUCT) {
    for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); el != NULL;
         el = NL_NEXT (el))
      if (el->code == N_MEMBER) {
        decl_t decl = el->attr;

        if (decl->width == 0) continue;
        if (decl->width > 0) return FALSE; /* a bitfield is never an FP member */
        if (!hfa_flatten (c2m_ctx, decl->decl_spec.type, els, n)) return FALSE;
      }
    return TRUE;
  }
  return FALSE;
}

/* MIR_T_F or MIR_T_D and *n_members set when `type` is an HFA; MIR_T_UNDEF
   otherwise.  Callers rely on size == *n_members * member size, so that is
   asserted here rather than assumed downstream. */
static MIR_type_t hfa_type (c2m_ctx_t c2m_ctx, struct type *type, int *n_members) {
  MIR_type_t els[MAX_HFA_MEMBERS];
  int n = 0;
  size_t member_size;

  if (type->mode != TM_STRUCT && type->mode != TM_ARR) return MIR_T_UNDEF;
  if (!hfa_flatten (c2m_ctx, type, els, &n)) return MIR_T_UNDEF;
  if (n < 1 || n > MAX_HFA_MEMBERS) return MIR_T_UNDEF;
  if (els[0] != MIR_T_F && els[0] != MIR_T_D) return MIR_T_UNDEF;
  for (int i = 1; i < n; i++)
    if (els[i] != els[0]) return MIR_T_UNDEF;
  member_size = els[0] == MIR_T_F ? 4 : 8;
  if (type_size (c2m_ctx, type) != n * member_size) return MIR_T_UNDEF;
  if (n_members != NULL) *n_members = n;
  return els[0];
}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  /* An HFA comes back in v0..v3 whatever its size -- four doubles is 32 bytes
     and is still NOT returned by address. */
  if (hfa_type (c2m_ctx, ret_type, NULL) != MIR_T_UNDEF) return FALSE;
  return ((ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION)
          && type_size (c2m_ctx, ret_type) > 2 * 8);
}

static int reg_aggregate_size (c2m_ctx_t c2m_ctx, struct type *type) {
  size_t size;

  if (type->mode != TM_STRUCT && type->mode != TM_UNION) return -1;
  return (size = type_size (c2m_ctx, type)) <= 2 * 8 ? (int) size : -1;
}

/* Load (load_p) or store the members of HFA `type` between `var_ops[i]` and
   `mem_op + i * member size`, one FP move per member.  The generic
   gen_multiple_load_store moves whole 8-byte qwords as MIR_T_I64, which is
   wrong for an HFA twice over: a float HFA has 4-byte members, and an I64 move
   would route the value through a general-purpose register. */
static void hfa_load_store (c2m_ctx_t c2m_ctx, MIR_type_t el_type, int n_members,
                            MIR_op_t *var_ops, MIR_op_t mem_op, int load_p) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  int64_t member_size = el_type == MIR_T_F ? 4 : 8;
  MIR_insn_code_t mov = tp_mov (el_type);

  for (int i = 0; i < n_members; i++) {
    MIR_op_t slot = MIR_new_mem_op (ctx, el_type, mem_op.u.mem.disp + i * member_size,
                                    mem_op.u.mem.base, mem_op.u.mem.index, mem_op.u.mem.scale);
    MIR_append_insn (ctx, curr_func,
                     load_p ? MIR_new_insn (ctx, mov, var_ops[i], slot)
                            : MIR_new_insn (ctx, mov, slot, var_ops[i]));
  }
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  int size, n_members;
  MIR_type_t el_type;

  if ((el_type = hfa_type (c2m_ctx, ret_type, &n_members)) != MIR_T_UNDEF) {
    /* One typed result per member.  The generator's MIR_RET lowering and its
       post-call result handling already route MIR_T_F/MIR_T_D results through
       v0..v7, so declaring the results with their real types is all it takes
       to get the return direction right. */
    for (int i = 0; i < n_members; i++) VARR_PUSH (MIR_type_t, res_types, el_type);
    return;
  }
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    simple_add_res_proto (c2m_ctx, ret_type, arg_info, res_types, arg_vars);
    return;
  }
  if (size == 0) return;
  VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
  if (size > 8) VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  int size, n_members;
  MIR_type_t el_type;

  if ((el_type = hfa_type (c2m_ctx, ret_type, &n_members)) != MIR_T_UNDEF) {
    for (int i = 0; i < n_members; i++)
      VARR_PUSH (MIR_op_t, call_ops, get_new_temp (c2m_ctx, el_type).mir_op);
    return n_members;
  }
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0)
    return simple_add_call_res_op (c2m_ctx, ret_type, arg_info, call_arg_area_offset);
  if (size == 0) return -1;
  VARR_PUSH (MIR_op_t, call_ops,
             MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
  if (size > 8)
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
  return size <= 8 ? 1 : 2;
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  int size, n_members;
  MIR_type_t el_type;

  if ((el_type = hfa_type (c2m_ctx, ret_type, &n_members)) != MIR_T_UNDEF) {
    assert (res.mir_op.mode == MIR_OP_MEM);
    hfa_load_store (c2m_ctx, el_type, n_members,
                    &VARR_ADDR (MIR_op_t, call_ops)[call_ops_start + 2], res.mir_op, FALSE);
    return res;
  }
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0)
    return simple_gen_post_call_res_code (c2m_ctx, ret_type, res, call, call_ops_start);
  if (size != 0)
    gen_multiple_load_store (c2m_ctx, ret_type, &VARR_ADDR (MIR_op_t, call_ops)[call_ops_start + 2],
                             res.mir_op, FALSE);
  return res;
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  int i, size, n_members;
  MIR_type_t el_type;

  if ((el_type = hfa_type (c2m_ctx, ret_type, &n_members)) != MIR_T_UNDEF) {
    assert (res.mir_op.mode == MIR_OP_MEM && VARR_LENGTH (MIR_op_t, ret_ops) == 0);
    for (i = 0; i < n_members; i++)
      VARR_PUSH (MIR_op_t, ret_ops, get_new_temp (c2m_ctx, el_type).mir_op);
    hfa_load_store (c2m_ctx, el_type, n_members, VARR_ADDR (MIR_op_t, ret_ops), res.mir_op, TRUE);
    return;
  }
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    simple_add_ret_ops (c2m_ctx, ret_type, res);
    return;
  }
  assert (res.mir_op.mode == MIR_OP_MEM && VARR_LENGTH (MIR_op_t, ret_ops) == 0 && size <= 2 * 8);
  for (i = 0; size > 0; size -= 8, i++)
    VARR_PUSH (MIR_op_t, ret_ops, get_new_temp (c2m_ctx, MIR_T_I64).mir_op);
  gen_multiple_load_store (c2m_ctx, ret_type, VARR_ADDR (MIR_op_t, ret_ops), res.mir_op, TRUE);
}

/* MIR reserves MIR_BLK_NUM (5) block classes so a target can discriminate
   aggregates that travel differently; aarch64 used exactly one of them.  Two
   more now carry the HFA cases.  The member COUNT is not encoded: it is
   recoverable as size / member-size, which hfa_type guarantees is exact.
   mir-gen-aarch64.c and mir-aarch64.c decode these; all three must agree. */
#define MIR_T_BLK_HFA_F (MIR_T_BLK + 1) /* HFA whose members are `float`  */
#define MIR_T_BLK_HFA_D (MIR_T_BLK + 2) /* HFA whose members are `double` */

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                       struct type *arg_type MIR_UNUSED) {
  MIR_type_t el_type = hfa_type (c2m_ctx, arg_type, NULL);

  if (el_type == MIR_T_F) return MIR_T_BLK_HFA_F;
  if (el_type == MIR_T_D) return MIR_T_BLK_HFA_D;
  return MIR_T_BLK; /* everything else: one BLK is enough */
}

/* These used to delegate wholesale to the simple_* helpers, which hardcode
   MIR_T_BLK and never consult target_get_blk_type -- so the HFA classification
   above would never have reached an argument.  Aggregates now go through
   target_get_blk_type here (the same shape x86_64 and riscv64 use); everything
   that is not an aggregate still takes the simple path unchanged. */
static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;

  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    simple_add_arg_proto (c2m_ctx, name, arg_type, arg_info, arg_vars);
    return;
  }
  var.name = name;
  var.type = target_get_blk_type (c2m_ctx, arg_type);
  var.size = type_size (c2m_ctx, arg_type);
  VARR_PUSH (MIR_var_t, arg_vars, var);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;

  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    simple_add_call_arg_op (c2m_ctx, arg_type, arg_info, arg);
    return;
  }
  assert (arg.mir_op.mode == MIR_OP_MEM);
  arg = mem_to_address (c2m_ctx, arg, TRUE);
  VARR_PUSH (MIR_op_t, call_ops,
             MIR_new_mem_op (ctx, target_get_blk_type (c2m_ctx, arg_type),
                             type_size (c2m_ctx, arg_type), arg.mir_op.u.reg, 0, 1));
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx MIR_UNUSED, const char *name MIR_UNUSED,
                                  struct type *arg_type MIR_UNUSED, decl_t param_decl MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {
  return FALSE;
}
