#!/usr/bin/env python3
"""gen-symtab.py -- wasm-jit-plan 5.3: the import table for the wasm engine.

Usage: gen-symtab.py DECLS_H NM_OUTPUT OUT_C [EXCLUDE]
                     [--header INCLUDE=PATH ...] [--accessors PATH ...]

EXCLUDE names identifiers to leave out, whitespace-separated: the names the
hosted declarations turn into macros, and any the compiler then reports as
undeclared (build-module.sh feeds both back and retries).

--header adds a runtime header the program half includes beside the
declarations region (runtime/hamt.h, for `#include "hamt.h"`): its identifiers
join the candidates and the table TU includes it as INCLUDE.

--accessors adds the one-line accessor definitions of a runtime file
(`TYPE *tur_tls_NAME_ptr (void) { ... }` in runtime/tur_tls.c).  The emitter
declares some of those straight into the program text rather than the region,
so the table declares them itself, from the definitions.

--wasm-objects LIST OUT2_C covers the rest of the resident runtime: every
function and data object the wasm objects named in LIST (one path per line)
define and the main table does not already carry.  Their C types are not at
hand, but the objects record each function's wasm signature, and a declaration
with the same wasm-level signature (i32 as int, i64 as long long, f32 as
float, f64 as double) is exactly what wasm-ld checks: a mismatched one links
at -O0 and traps at -O2.  OUT2_C includes no header, so these declarations
cannot clash with the real ones the main table uses.  The main table's lookup
falls back to it.

A plain Emscripten module has no dlsym, so the JIT'd program's imports are
resolved from a table.  Rows come from two places:

  1. every identifier declared in the runtime's declarations region
     (src/runtime/generated/tur_rt_split_decls.h) that the linked objects
     define (llvm-nm --defined-only over them), and
  2. a fixed list of libc/libm names the emitted C and the stdlib's inline C
     reach for, declared through the system headers.

A name the program imports that is in neither fails at MIR_link with the
name, which is the loud failure this design wants.
"""
import re, sys

args = sys.argv[1:]
headers, accessor_files = [], []
wasm_objects = None
positional = []
i = 0
while i < len(args):
    if args[i] == '--header':
        headers.append(args[i + 1].split('=', 1)); i += 2
    elif args[i] == '--accessors':
        accessor_files.append(args[i + 1]); i += 2
    elif args[i] == '--wasm-objects':
        wasm_objects = (args[i + 1], args[i + 2]); i += 3
    else:
        positional.append(args[i]); i += 1
decls_h, nm_out, out_c = positional[0:3]
macros = set(open(positional[3]).read().split()) if len(positional) > 3 else set()

def identifiers(path):
    text = open(path).read()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//.*', '', text)
    return set(re.findall(r'\b([A-Za-z_]\w*)\b', text))

# every identifier in the region and the extra headers: functions AND objects
# (g_tur_args, the registries); nm keeps only the ones the link defines, the
# macro list drops the ones the hosted arm turns into accessors
declared = identifiers(decls_h)
for _inc, path in headers:
    declared |= identifiers(path)
# accessor definitions: (declaration text, name)
ACCESSOR = re.compile(r'^([A-Za-z_][\w \t\*]*?[\s\*])(tur_tls_\w+_ptr)\s*\(void\)\s*\{', re.M)
accessors = []
for path in accessor_files:
    for m in ACCESSOR.finditer(open(path).read()):
        accessors.append(('extern %s%s(void);' % (m.group(1).strip() + (' ' if not m.group(1).strip().endswith('*') else ''), m.group(2)), m.group(2)))
defined = set()
for line in open(nm_out):
    parts = line.split()
    if len(parts) >= 3 and parts[1] in 'TDBRWC':
        defined.add(parts[2])
runtime = sorted((declared & defined) - macros)
accessor_names = sorted({n for _d, n in accessors if n in defined} - set(runtime))

LIBC = """malloc calloc realloc free aligned_alloc posix_memalign
printf fprintf sprintf snprintf vprintf vfprintf vsnprintf puts fputs fputc putc putchar fwrite fread
fflush fopen fclose fgets fgetc getc ungetc fseek ftell rewind ferror feof fileno setvbuf perror
memcpy memmove memset memcmp memchr strlen strnlen strcmp strncmp strcpy strncpy strcat strncat
strchr strrchr strstr strdup strndup strtol strtoll strtoul strtoull strtod strtof atoi atol atoll atof
strerror strspn strcspn strpbrk strtok
abs labs llabs exit _exit abort atexit getenv setenv unsetenv qsort bsearch rand srand
longjmp
sqrt pow floor ceil fabs round trunc fmod sin cos tan asin acos atan atan2 exp log log2 log10 exp2 expm1 log1p
sinh cosh tanh hypot cbrt ldexp frexp modf copysign nan fmin fmax
sqrtf powf floorf ceilf fabsf roundf fmodf sinf cosf tanf expf logf
time clock gettimeofday clock_gettime nanosleep usleep sleep localtime gmtime mktime strftime difftime
pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_unlock pthread_mutex_trylock
pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_signal pthread_cond_broadcast pthread_cond_timedwait
pthread_self pthread_equal pthread_join pthread_detach pthread_exit pthread_key_create pthread_getspecific pthread_setspecific
pthread_once pthread_attr_init pthread_attr_destroy pthread_attr_setstacksize
isalpha isdigit isalnum isspace isupper islower toupper tolower isprint ispunct isxdigit
write read close open lseek unlink getpid isatty dup dup2 pipe
__errno_location""".split()
# Data objects: inline C writes to stdout/stderr directly (fprintf (stdout,
# ...)), and an import of data resolves to the object's address like any other.
LIBC += """stdout stderr stdin""".split()
# Found missing by the first corpus run through the module (2026-10-28).
LIBC += """strcasecmp strncasecmp remove mkstemp localtime_r gmtime_r fsync clearerr
tmpfile fdopen ftruncate getline getdelim realpath getcwd access stat lstat fstat mkdir
rmdir opendir readdir closedir chdir readlink""".split()

with open(out_c, 'w') as o:
    o.write('/* AUTO-GENERATED by tools/wasm-jit/gen-symtab.py -- do not edit. */\n')
    o.write('#define _GNU_SOURCE 1\n')
    for h in ['stdio.h', 'stdlib.h', 'string.h', 'math.h', 'time.h', 'pthread.h', 'ctype.h',
              'unistd.h', 'fcntl.h', 'errno.h', 'setjmp.h', 'sys/time.h', 'stdint.h',
              'strings.h', 'sys/stat.h', 'dirent.h']:
        o.write('#include <%s>\n' % h)
    # The declarations region, read the way the JIT'd program reads it: as
    # the hosted runtime's externs.
    o.write('#define TUR_RT_SPLIT_HOSTED 1\n#include "runtime/generated/tur_rt_split_decls.h"\n')
    for inc, _path in headers:
        o.write('#include "%s"\n' % inc)
    for decl, name in accessors:
        if name in accessor_names:
            o.write(decl + '\n')
    # Declared in the decls region only on the no-builtin-atomics arm, which
    # c2mir takes and clang does not; the table must still carry them.
    o.write('extern uint64_t tur_atomic_load_u64(const volatile uint64_t *);\n'
            'extern void     tur_atomic_store_u64(volatile uint64_t *, uint64_t);\n'
            'extern uint64_t tur_atomic_add_fetch_u64(volatile uint64_t *, uint64_t);\n'
            'extern void    *tur_atomic_load_ptr(void *const volatile *);\n'
            'extern void     tur_atomic_store_ptr(void *volatile *, void *);\n'
            'extern int      tur_atomic_load_int(const volatile int *);\n'
            'extern int      tur_atomic_cas_int(volatile int *, int *, int);\n')
    o.write('typedef struct { const char *name; void *addr; } tur_wasm_sym;\n')
    o.write('static const tur_wasm_sym tur_wasm_symtab[] = {\n')
    for n in runtime + accessor_names:
        o.write('  {"%s", (void *) &%s},\n' % (n, n))
    for n in LIBC:
        o.write('  {"%s", (void *) &%s},\n' % (n, n))
    o.write('};\n')
    if wasm_objects:
        o.write('void *tur_wasm_jit_symtab_lookup_rt (const char *);\n')
    o.write('void *tur_wasm_jit_symtab_lookup (const char *name) {\n'
            '  for (size_t i = 0; i < sizeof tur_wasm_symtab / sizeof tur_wasm_symtab[0]; i++)\n'
            '    if (strcmp (tur_wasm_symtab[i].name, name) == 0) return tur_wasm_symtab[i].addr;\n'
            + ('  return tur_wasm_jit_symtab_lookup_rt (name);\n' if wasm_objects else '  return NULL;\n')
            + '}\n')
# --- the rest of the runtime, typed from the wasm objects --------------------
def leb(b, i):
    r = shift = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << shift; shift += 7
        if not c & 0x80:
            return r, i

def wasm_symbols(path):
    """{name: ('func', params, results) | ('data',)} for the global symbols
    the object defines (not local, not thread-local)."""
    b = open(path, 'rb').read()
    if b[:4] != b'\0asm':
        return {}
    types, func_types, n_imported_funcs, syms = [], [], 0, {}
    i = 8
    while i < len(b):
        sid = b[i]; i += 1
        size, i = leb(b, i)
        end = i + size
        if sid == 1:                                     # type
            n, j = leb(b, i)
            for _ in range(n):
                j += 1                                   # 0x60
                np_, j = leb(b, j); params = list(b[j:j + np_]); j += np_
                nr, j = leb(b, j); results = list(b[j:j + nr]); j += nr
                types.append((params, results))
        elif sid == 2:                                   # import
            n, j = leb(b, i)
            for _ in range(n):
                for _s in range(2):
                    ln, j = leb(b, j); j += ln
                kind = b[j]; j += 1
                if kind == 0:
                    _t, j = leb(b, j); n_imported_funcs += 1
                elif kind == 1:
                    j += 1; fl, j = leb(b, j); _m, j = leb(b, j)
                    if fl & 1: _x, j = leb(b, j)
                elif kind == 2:
                    fl, j = leb(b, j); _m, j = leb(b, j)
                    if fl & 1: _x, j = leb(b, j)
                elif kind == 3:
                    j += 2
                elif kind == 4:
                    j += 1; _t, j = leb(b, j)
        elif sid == 3:                                   # function
            n, j = leb(b, i)
            for _ in range(n):
                t, j = leb(b, j); func_types.append(t)
        elif sid == 0:                                   # custom
            ln, j = leb(b, i); name = b[j:j + ln].decode(); j += ln
            if name == 'linking':
                _ver, j = leb(b, j)
                while j < end:
                    sub = b[j]; j += 1
                    ssz, j = leb(b, j); send = j + ssz
                    if sub == 8:                         # symbol table
                        n, k = leb(b, j)
                        for _ in range(n):
                            kind = b[k]; k += 1
                            flags, k = leb(b, k)
                            undefined = flags & 0x10
                            if kind in (0, 2, 4, 5):     # function, global, tag, table
                                idx, k = leb(b, k)
                                nm = None
                                if not undefined or flags & 0x40:
                                    ln2, k = leb(b, k); nm = b[k:k + ln2].decode(); k += ln2
                                if kind == 0 and nm and not undefined and not flags & 2 \
                                        and idx >= n_imported_funcs:
                                    tp = types[func_types[idx - n_imported_funcs]]
                                    syms[nm] = ('func', tp[0], tp[1])
                            elif kind == 1:              # data
                                ln2, k = leb(b, k); nm = b[k:k + ln2].decode(); k += ln2
                                if not undefined:
                                    _seg, k = leb(b, k); _off, k = leb(b, k); _sz, k = leb(b, k)
                                    if not flags & 2 and not flags & 0x100:
                                        syms[nm] = ('data',)
                            elif kind == 3:              # section
                                _idx, k = leb(b, k)
                    j = send
        i = end
    return syms

CTYPE = {0x7f: 'int', 0x7e: 'long long', 0x7d: 'float', 0x7c: 'double'}
IDENT = re.compile(r'^[A-Za-z_]\w*$')
second = []
if wasm_objects:
    list_file, out2_c = wasm_objects
    taken = set(runtime) | set(accessor_names) | set(LIBC) | macros
    seen = {}
    for path in open(list_file).read().split():
        for nm, info in wasm_symbols(path).items():
            if nm in taken or nm in seen or not IDENT.match(nm) or nm.startswith('__'):
                continue
            if info[0] == 'func':
                params, results = info[1], info[2]
                if len(results) > 1 or any(t not in CTYPE for t in params + results):
                    continue
            seen[nm] = info
    with open(out2_c, 'w') as o:
        o.write('/* AUTO-GENERATED by tools/wasm-jit/gen-symtab.py -- do not edit.\n'
                ' * The resident runtime\'s remaining symbols, declared at their wasm\n'
                ' * signatures (see --wasm-objects in the generator). */\n')
        for nm in sorted(seen):
            info = seen[nm]
            if info[0] == 'data':
                o.write('extern char %s;\n' % nm)
            else:
                ret = CTYPE[info[2][0]] if info[2] else 'void'
                ps = ', '.join(CTYPE[t] for t in info[1]) or 'void'
                o.write('extern %s %s(%s);\n' % (ret, nm, ps))
        o.write('int strcmp(const char *, const char *);\n')
        o.write('static const struct { const char *name; void *addr; } tur_wasm_symtab_rt[] = {\n')
        for nm in sorted(seen):
            o.write('  {"%s", (void *) &%s},\n' % (nm, nm))
        o.write('};\n')
        o.write('void *tur_wasm_jit_symtab_lookup_rt (const char *name) {\n'
                '  for (unsigned i = 0; i < sizeof tur_wasm_symtab_rt / sizeof tur_wasm_symtab_rt[0]; i++)\n'
                '    if (strcmp (tur_wasm_symtab_rt[i].name, name) == 0) return tur_wasm_symtab_rt[i].addr;\n'
                '  return (void *) 0;\n}\n')
    second = sorted(seen)

print('symtab: %d runtime + %d accessor + %d libc rows (%d macro names excluded); %d typed from the wasm objects'
      % (len(runtime), len(accessor_names), len(LIBC), len((declared & defined) & macros), len(second)))
