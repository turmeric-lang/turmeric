/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   wasm32 call ABI target specific code.

   The wasm32 C ABI has no register-passed aggregates: every struct/union
   argument is passed indirectly and every struct/union return uses an
   sret pointer.  That is exactly what the generic simple_* helpers in
   c2mir.c implement, so this target delegates to them throughout.  */

typedef int target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  return simple_return_by_addr_p (c2m_ctx, ret_type);
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  simple_add_res_proto (c2m_ctx, ret_type, arg_info, res_types, arg_vars);
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  return simple_add_call_res_op (c2m_ctx, ret_type, arg_info, call_arg_area_offset);
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call, size_t call_ops_start) {
  return simple_gen_post_call_res_code (c2m_ctx, ret_type, res, call, call_ops_start);
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  simple_add_ret_ops (c2m_ctx, ret_type, res);
}

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx, struct type *arg_type) {
  return simple_target_get_blk_type (c2m_ctx, arg_type);
}

static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  simple_add_arg_proto (c2m_ctx, name, arg_type, arg_info, arg_vars);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  simple_add_call_arg_op (c2m_ctx, arg_type, arg_info, arg);
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  decl_t param_decl, target_arg_info_t *arg_info) {
  return simple_gen_gather_arg (c2m_ctx, name, arg_type, param_decl, arg_info);
}
