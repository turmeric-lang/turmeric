/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   On wasm32 a va_list is simply a pointer into the variadic argument
   buffer that the caller built (see _MIR_get_ff_call in mir-wasm.c).  */

static char stdarg_str[]
  = "#ifndef __STDARG_H\n"
    "#define __STDARG_H\n"
    "\n"
    "typedef void *va_list[1];\n"
    "\n"
    "#define va_start(ap, param) __builtin_va_start (ap)\n"
    "#define va_arg(ap, type) __builtin_va_arg(ap, (type *) 0)\n"
    "#define va_end(ap) 0\n"
    "#define va_copy(dest, src) ((dest)[0] = (src)[0])\n"
    "\n"
    "#ifndef __GNUC_VA_LIST\n"
    "#define __GNUC_VA_LIST 1\n"
    "#endif\n"
    "typedef va_list __gnuc_va_list;\n"
    "#endif /* #ifndef __STDARG_H */\n";
