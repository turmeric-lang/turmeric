#!/usr/bin/env python3
"""S2 split-runtime artifact generator (jit-engine-plan, findings 19.4).

Consumes the feature-complete single-file runtime preamble from
`tur emit-rt-split` and produces the committed artifacts:

  src/runtime/generated/tur_rt_split.c        -- the runtime TU: every
      file-scope static externalized (so the program half can bind it),
      the runtime thread-locals (the TLS table below) routed to the host's
      tur_tls storage (one storage; read directly where DIRECT_TLS allows,
      through its accessors otherwise), compiled by a real cc into the host.
  src/runtime/generated/tur_rt_split_decls.h  -- the declarations region:
      same preamble with function bodies dropped and statics turned into
      extern declarations.  cmd_jit splices this ahead of the program half
      instead of the full preamble when the content hash matches.
  src/runtime/generated/tur_rt_split_embed.c  -- the decls region as a C
      string plus the recorded content hash (from `tur emit-rt-split
      --hash`), linked into tur so cmd_jit needs no install-path lookup.

The transform is the validated S2 proof (tools/jit-spike/s2-split-proof.py)
promoted to a generator, with one correctness addition: a `static` prototype
whose definition is NOT in the preamble (it is per-program, below the marker
-- __tur_static_init is the case) is kept verbatim in the decls half and
dropped from the runtime TU, instead of being blanket-de-static'd into a
linkage conflict with the program half's static definition.

Usage:
  python3 tools/gen-runtime-split.py --tur ./build/tur \
      [--out src/runtime/generated]

Regenerate whenever the runtime preamble changes; commit the artifacts in
the same PR as the emitter change (same policy as fixture snapshots).
"""
import argparse
import os
import re
import subprocess
import sys

MARK = '/* ==== tur: end of fixed runtime preamble ==== */'

# The 11 host-TLS names (findings 15): the program half reaches them through
# tur_tls_* accessors (the emitted #else branch), so the runtime TU must
# route through the SAME accessors -- one storage, owned by
# src/runtime/tur_tls.c in the host process.
TLS = {
    '__stm_current_tx': ('void **', 'tur_tls_stm_current_tx_ptr'),
    'tur_handler_chain': ('void **', 'tur_tls_handler_chain_ptr'),
    'tur_panicking': ('int *', 'tur_tls_panicking_ptr'),
    'tur_root_unwind': ('int *', 'tur_tls_root_unwind_ptr'),
    'tur_cur_shift_reset': ('void **', 'tur_tls_cur_shift_reset_ptr'),
    'tur_current_fiber': ('void **', 'tur_tls_current_fiber_ptr'),
    'tur_fiber_cancelled_flag': ('bool *', 'tur_tls_fiber_cancelled_flag_ptr'),
    'tur_current_thread_state': ('void **', 'tur_tls_current_thread_state_ptr'),
    'tur_cancel_jmpbuf': ('jmp_buf *', 'tur_tls_cancel_jmpbuf_ptr'),
    'tur_cancel_jmpbuf_valid': ('int *', 'tur_tls_cancel_jmpbuf_valid_ptr'),
    'tur_current_scheduler_mt': ('void **', 'tur_tls_current_scheduler_mt_ptr'),
    'tur__rtv_': ('int64_t *', 'tur_tls_rtv_ptr'),
    # The DK runtime's per-thread state (dk-reap-list-shared-across-threads).
    # The program half's CPS entry wrappers read and write these directly,
    # and the runtime half's dk_perform / trampoline reads them, so both
    # halves must see one slot per thread.
    '__dk_reap_v': ('void **', 'tur_tls_dk_reap_v_ptr'),
    '__dk_reap_kind': ('void **', 'tur_tls_dk_reap_kind_ptr'),
    '__dk_reap_n': ('size_t *', 'tur_tls_dk_reap_n_ptr'),
    '__dk_reap_cap': ('size_t *', 'tur_tls_dk_reap_cap_ptr'),
    '__dk_entry_depth': ('int *', 'tur_tls_dk_entry_depth_ptr'),
    'g_dk_driver': ('void **', 'tur_tls_dk_driver_ptr'),
    'g_dk_pend_perform': ('void **', 'tur_tls_dk_pend_perform_ptr'),
    'g_dk_resume_chain': ('void **', 'tur_tls_dk_resume_chain_ptr'),
    'g_dk_resume_val': ('tur_dkw *', 'tur_tls_dk_resume_val_ptr'),  # int64_t on wasm32
    # The dynamic tail-call trampoline's state (emit_module.c).
    'tur_tb_desc': ('void **', 'tur_tls_tb_desc_ptr'),
    'tur_tb_armed_for': ('void **', 'tur_tls_tb_armed_for_ptr'),
    'tur_tb_root': ('void **', 'tur_tls_tb_root_ptr'),
    'tur_tb_sentinel_box': ('void **', 'tur_tls_tb_sentinel_box_ptr'),
    # The async spawn flags (async-capturing-body-env-never-freed, await-parks-
    # only-to-the-nearest-c-frame): the program half sets each just before a
    # spawn and the runtime half's tur_async_fiber* reads and clears it.  Left
    # out of this table, the runtime half kept a native thread-local of its
    # own while the hosted program wrote the accessor's slot, so under `tur
    # jit` no async body env was ever dropped and a direct-style body's root
    # depth was off by one.
    'tur_async_direct_body': ('int *', 'tur_tls_async_direct_body_ptr'),
    'tur_async_owns_env': ('int *', 'tur_tls_async_owns_env_ptr'),
    # The current panic site (tur_cur_site, panic-location-names-the-runtime-
    # not-the-call-site) is deliberately NOT here: it is emitted after the end
    # marker, program-side, where gcc/clang give it a native thread-local
    # store rather than an accessor call around every vec-get.  Only c2mir
    # reaches the host's tur_tls_cur_site_ptr, through the emitted #else.
}

# DIRECT reads of that one storage (performance-audit R-2).  An accessor call
# per access was ~25% of the cycles of a hosted-split program, up to 57% on a
# CPS loop: the emitter checks tur_panicking after every panic-capable call,
# and an opaque call there also stops gcc transforming the caller (98.7% of
# fib's instructions).  A real cc can name the host's thread-local itself, so
# wherever one compiles a half of the split, each NAME becomes the accessor's
# own body, inlined:
#
#     extern __thread int tur_tls_panicking;
#     #define tur_panicking (*((int *)&tur_tls_panicking))
#
# Still one storage -- tur_tls.c's, which the accessor returns the address of
# -- so nothing above changes, only how a cc reaches it.  The accessors stay
# for the front ends and builds that cannot use the direct form:
#
#   - c2mir (`tur jit`), which has no thread-locals at all;
#   - _WIN32, where no run has checked a PE link that names another object's
#     __thread (MinGW gcc can emulate TLS, which changes the symbol);
#   - TUR_TLS_FRESH builds (clang, Windows gcc, the sanitizers), whose compiler
#     may keep a thread-local's ADDRESS live across a call.  A fiber that
#     yields can resume on another thread, so those builds re-derive the
#     address after every call; an out-of-line accessor already does.  GCC
#     elsewhere addresses each access off %fs, the exposure the inline
#     preamble has always had.
#
# Read from src/runtime/tur_tls.c rather than tabled here, so the two cannot
# drift: each storage line and its accessor look like
#
#     __thread T tur_tls_x = 0;
#     T *tur_tls_x_ptr (void) { return &tur_tls_x; }
TUR_TLS_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         '..', 'src', 'runtime', 'tur_tls.c')
DIRECT_TLS = '!defined(_WIN32) && !defined(TUR_TLS_FRESH)'
HOSTED_TLS_GUARD = ('#if (defined(__GNUC__) || defined(__clang__)) '
                    '&& !defined(TUR_RT_SPLIT_HOSTED)')


def tls_storage(path=TUR_TLS_C):
    """accessor name -> (extern declaration of its storage, its return
    expression), read from src/runtime/tur_tls.c."""
    with open(path) as f:
        txt = f.read()
    decl = {}
    for m in re.finditer(r'^__thread\s+(?:_Alignas\(\d+\)\s+)?([^=;\[]+?)\s*'
                         r'\b(tur_tls_\w+)\s*(\[[^\]]*\])?\s*(?:=[^;]*)?;',
                         txt, re.M):
        t = m.group(1).strip()
        sep = '' if t.endswith('*') else ' '
        decl[m.group(2)] = (f'extern __thread {t}{sep}{m.group(2)}'
                            f'{m.group(3) or ""};')
    if re.search(r'^static\s+__thread', txt, re.M):
        sys.exit(f'{path}: a static __thread cannot be read directly; '
                 f'give it external linkage')
    out = {}
    for m in re.finditer(r'\b(tur_tls_\w+_ptr)\s*\(void\)\s*\{\s*return\s+'
                         r'([^;]+);\s*\}', txt):
        acc, body = m.group(1), m.group(2).strip()
        store = re.search(r'\btur_tls_\w+', body)
        if not store or store.group(0) not in decl:
            sys.exit(f'{path}: {acc} returns {body!r}, not a __thread storage')
        out[acc] = (decl[store.group(0)], body)
    return out


def direct_ref(rt, body):
    """`acc()` inlined: the accessor's return expression, cast to the return
    type the caller declares for it."""
    if re.fullmatch(r'&\w+', body):
        return f'(({rt}){body})'
    return f'(({rt})({body}))'


def direct_branch(branch, storage):
    """The accessor branch of a thread-local block, rewritten to read the
    storage directly: each `extern RT acc(void);` becomes the storage's extern
    declaration and each `acc()` in a #define becomes direct_ref.  Comments are
    dropped; anything else is a shape this was not written for, so stop."""
    out, rts = [], {}
    in_comment = False
    for l in branch:
        s = l.strip()
        if in_comment:
            in_comment = '*/' not in s
            continue
        if s.startswith('/*'):
            in_comment = '*/' not in s
            continue
        if not s:
            continue
        m = re.match(r'^extern\s+(.+?)\s*\b(tur_tls_\w+_ptr)\s*\(void\);$', s)
        if m:
            rt, acc = m.group(1).strip(), m.group(2)
            if acc not in storage:
                sys.exit(f'{acc}: no __thread storage in {TUR_TLS_C}')
            rts[acc] = rt
            out.append(storage[acc][0])
            continue
        if s.startswith('#define '):
            new = s
            for acc, rt in rts.items():
                new = re.sub(r'\b' + acc + r'\(\)',
                             lambda _m, rt=rt, acc=acc:
                             direct_ref(rt, storage[acc][1]), new)
            if new == s:
                sys.exit(f'no accessor call to inline in {s!r}')
            out.append(new)
            continue
        sys.exit(f'unexpected line in a thread-local accessor branch: {s!r}')
    return out


def direct_tls_hosted(lines, storage):
    """Give every thread-local block of the decls half a direct-read branch
    for a hosted cc build, between its native branch (not hosted) and its
    accessor branch (c2mir and the DIRECT_TLS exclusions).  -> (lines, n)"""
    out, n = [], 0
    i = 0
    while i < len(lines):
        out.append(lines[i])
        if lines[i] != HOSTED_TLS_GUARD:
            i += 1
            continue
        depth, els, end = 0, None, None
        j = i + 1
        while j < len(lines):
            s = lines[j].strip()
            if re.match(r'#\s*if', s):
                depth += 1
            elif re.match(r'#\s*endif', s):
                if depth == 0:
                    end = j
                    break
                depth -= 1
            elif depth == 0 and re.match(r'#\s*elif', s):
                sys.exit(f'line {j}: #elif in a thread-local block')
            elif depth == 0 and re.match(r'#\s*else', s):
                els = j
            j += 1
        if els is None or end is None:
            sys.exit(f'line {i}: thread-local block without #else/#endif')
        out.extend(lines[i + 1:els])
        out.append('#elif (defined(__GNUC__) || defined(__clang__)) && '
                   + DIRECT_TLS)
        out.extend(direct_branch(lines[els + 1:end], storage))
        out.extend(lines[els:end + 1])
        n += 1
        i = end + 1
    return out, n

# Functions whose BODY belongs in the decls half, emitted `static inline`, and
# which are omitted from the runtime archive entirely.
#
# The default treatment -- prototype in decls, out-of-line body in the archive
# -- is correct for linking and can be badly wrong for CODEGEN. These two are
# four-line leaf helpers the emitter calls once per defer-bearing scope, with a
# `tur_frame` (536 bytes: three fixed 32-entry arrays) as the first argument.
# With the body visible, GCC sees a frame that only ever holds one known thunk
# and eliminates the struct outright. Behind an opaque call, `&__frame_N`
# escapes, so every byte must be materialised on the stack:
#
#     build_hychain prologue, same compiler, byte-identical program C
#       inlined (monolithic preamble)   sub $0x48,%rsp     -- 72 bytes
#       out-of-line (split)             sub $0x268,%rsp    -- 616 bytes
#
# A ~6.7x stack multiplier on every scope with a defer. It overflowed a 2 MiB
# Windows stack at recursion depth ~3276 and took out
# tests/fixtures/gc-registry-growth as a bare STATUS_STACK_OVERFLOW with no
# output -- which bash reports as the thoroughly uninformative exit 127.
#
# Emitting them `static inline` into the decls half restores the small frame.
# Safe to omit from the archive because nothing there calls them: the runtime TU
# only ever DEFINED them, for the program half to link against (verified by grep
# across src/ -- the only other references are the emitter writing call sites
# into generated code).
#
# Add a name here only if it is a small leaf helper whose inlining the compiler
# demonstrably relies on. Anything with state, or anything the archive itself
# calls, must stay out-of-line.
INLINE_INTO_DECLS = {
    'tur_frame_init',
    'tur_frame_push_defer',
    # The third one, and it is the one that actually matters. Inlining only the
    # first two changed NOTHING measurable -- the prologue stayed at 0x268 --
    # because the emitter also writes `tur_frame_fire_lifo(&__frame_N)` at scope
    # exit, and one surviving opaque callee is enough to make the address
    # escape. Note it is plain `static` in the preamble, not `static inline`
    # (emit_module.c:10110), so it is easy to miss when scanning for the inline
    # ones; being file-local was enough for GCC in the monolithic build.
    'tur_frame_fire_lifo',
}

GEN_NOTE = ('AUTO-GENERATED by tools/gen-runtime-split.py from '
            '`tur emit-rt-split` -- do not edit.\n'
            ' * Regenerate (and commit, same PR) when the runtime preamble '
            'changes; cmd_jit\n'
            ' * verifies the recorded hash before trusting these artifacts, '
            'so a stale copy\n'
            ' * is never wrong -- it just forfeits the split and falls back '
            'to full-preamble\n'
            ' * emission.  See docs/archive/jit-engine-plan.md (S2) and '
            'findings 19.4/23.')


def de_static(text):
    """Remove one file-scope `static ` -- at line start OR after a leading
    __attribute__((...)) (the emitter writes both orders).  `static inline`
    loses BOTH: plain C99 `inline` at external linkage emits no standalone
    definition (found via tur_frame_init: unresolved import)."""
    return re.sub(r'^((?:__attribute__\(\([^)]*\)\)\s+)?)static\s+(inline\s+)?',
                  r'\1', text, count=1)


def code_part(line):
    """Line with a same-line trailing /* comment */ removed -- `int x = 0;
    /* note */` must still read as terminated by ';'."""
    m = re.search(r'/\*.*\*/\s*$', line)
    return line[:m.start()].rstrip() if m else line.rstrip()


def body_span(lines, i):
    """(start, end) inclusive of the {...} block opening at/after line i."""
    d, started = 0, False
    j = i
    while j < len(lines):
        d += lines[j].count('{') - lines[j].count('}')
        if '{' in lines[j]:
            started = True
        if started and d <= 0:
            return j
        j += 1
    return None


def fn_def_names(lines):
    """Names of every file-scope function DEFINED in the preamble, so a
    static prototype with no preamble definition (per-program, defined below
    the marker) can be recognized and left alone."""
    names = set()
    i = 0
    while i < len(lines):
        l = lines[i]
        if l and not l[0].isspace() and '(' in l \
           and not l.lstrip().startswith(('#', '/*', '//', '*')) \
           and not l.rstrip().endswith(';'):
            head = ' '.join(lines[i:i + 3])
            brace = head.find('{')
            if brace != -1 and (head.find(';') == -1 or head.find(';') > brace) \
               and ('=' not in head[:brace]):
                m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', head[:brace])
                if m:
                    names.add(m.group(1))
                j = body_span(lines, i)
                if j is not None:
                    i = j + 1
                    continue
        i += 1
    return names


def split(src, storage):
    """-> (impl_txt, decls_txt, program_defined_protos); storage is
    tls_storage()."""
    pre_txt, _prog = src.split(MARK, 1)
    lines = pre_txt.split('\n')
    defined = fn_def_names(lines)
    program_protos = []
    impl, decls = [], []
    i = 0
    while i < len(lines):
        l = lines[i]
        stripped = l.lstrip()
        at_col0 = l and not l[0].isspace()
        # file-scope function definition?  signature at col 0, has '(',
        # doesn't end ';', block opens within 3 lines
        is_fn = False
        if at_col0 and '(' in l and not stripped.startswith(('#', '/*', '//', '*')) \
           and not l.rstrip().endswith(';'):
            head = ' '.join(lines[i:i + 3])
            brace = head.find('{')
            if brace != -1 and (head.find(';') == -1 or head.find(';') > brace) \
               and ('=' not in head[:brace]):
                is_fn = True
        if is_fn:
            j = body_span(lines, i)
            sig_end = i
            while '{' not in lines[sig_end]:
                sig_end += 1
            sig = '\n'.join(lines[i:sig_end + 1])
            sig = sig[:sig.index('{')].rstrip()
            desig = de_static(sig)
            body = '\n'.join(lines[i:j + 1])
            # INLINE_INTO_DECLS: the whole body goes to the decls half as
            # `static inline`, and nothing goes to the archive.  See the
            # constant's comment for why -- these are the helpers whose
            # out-of-lining silently multiplied every defer-bearing stack frame
            # by ~6.7x.  Matched on the name in the signature rather than the
            # whole line, so a call to one of them inside another function
            # cannot trip it.
            fname = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', desig)
            if fname and fname.group(1) in INLINE_INTO_DECLS:
                # Body into the decls half as `static inline`, AND the
                # out-of-line copy still into the archive.  Keeping both is
                # deliberate: `static inline` has internal linkage, so the
                # program TU uses its own copy and never references the
                # archive's, while archive-internal callers keep theirs.
                # tur_frame_fire_chain calls tur_frame_fire_lifo from inside
                # the archive, so omitting the archive copy would break that
                # link -- and working out which allowlisted helper is called by
                # which other archive function is exactly the reasoning this
                # rule avoids having to do.
                decls.append('static inline ' + de_static(body))
                impl.append(de_static(body))
                i = j + 1
                continue
            impl.append(de_static(body))
            decls.append(desig + ';')
            i = j + 1
            continue
        # file-scope object with initializer or plain decl, col 0, static
        m = re.match(r'^(?:__attribute__\(\([^)]*\)\)\s+)?static\s+'
                     r'(TUR_THREAD_LOCAL\s+|_Thread_local\s+)?(.*)$', l) \
            if at_col0 else None
        if m and not stripped.startswith('static void') and ('=' in l or l.rstrip().endswith(';')):
            rest = m.group(2)
            nm = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*(=|;)', rest)
            # A FUNCTION-POINTER object is a variable, but its declarator
            # contains parens -- `static const char *(*g_x)(int64_t) = 0;` --
            # so the `'(' not in ...` test below reads it as a prototype and
            # drops it through to the verbatim path.  That is not cosmetic:
            # the decls half then carries the DEFINITION rather than an
            # `extern`, so the program half gets its own zero-initialized copy
            # and any value it stores is invisible to the runtime TU that
            # reads it.  `g_tur_any_name_ext` (the `any` box's per-type name
            # table) hit exactly that -- installed by the program, read by the
            # host, answered "unknown" under the JIT while the cc path, where
            # both halves are one TU, was correct.
            #
            # The test is anchored at the FIRST paren of the declarator, not
            # searched anywhere in the line.  A free search also matches a
            # function-pointer PARAMETER inside an ordinary prototype --
            # `int64_t tur_timer_wheel_insert(..., void (*cb)(void *), ...)`
            # -- and turns that prototype into a bogus `extern ...;;`.  At the
            # first paren, a real prototype presents its parameter list
            # (`(TurTimerWheel *w, ...`), which cannot match; only a genuine
            # fn-pointer object presents `(*name)`.  A function RETURNING a
            # fn-pointer puts a `(` right after the name, so it does not match
            # either.  An ARRAY of function pointers (`(*tbl[2])(...)`) still
            # falls through to verbatim -- add it here if one is ever needed.
            head = rest.split('=')[0]
            first_paren = head.find('(')
            fnptr = (re.match(r'\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\(',
                              head[first_paren:])
                     if first_paren >= 0 else None)
            if fnptr or (nm and '(' not in rest.split('=')[0]):
                name = fnptr.group(1) if fnptr else nm.group(1)
                j = i
                while not code_part(lines[j]).endswith(';'):
                    j += 1
                full = '\n'.join(lines[i:j + 1])
                tls = bool(m.group(1))
                if tls and name in TLS:
                    # ONE storage for thread-locals: BOTH halves route through
                    # the HOST's tur_tls accessors.  The first proof run gave
                    # the runtime its own __thread vars + accessor overrides,
                    # but dlsym(RTLD_DEFAULT) prefers the executable's exports
                    # over a preload, so the program half bound libturi's
                    # slots while runtime code used its own -- divergent
                    # state, stm-stress lost counts.  Mirror the emitted
                    # #else branch in the runtime TU instead.
                    rt, acc = TLS[name]
                    dtype = full.split(name)[0]
                    dtype = re.sub(r'^.*?(TUR_THREAD_LOCAL|_Thread_local)\s+',
                                   '', dtype).strip()
                    if acc not in storage:
                        sys.exit(f'{acc}: no __thread storage in {TUR_TLS_C}')
                    sdecl, sbody = storage[acc]

                    def tls_define(ref):
                        if rt == 'void **':
                            return f'#define {name} (*({dtype} *){ref})'
                        return f'#define {name} (*{ref})'
                    # The runtime TU is always compiled by a real cc, so it
                    # reads the storage directly wherever DIRECT_TLS allows.
                    impl.append(f'#if {DIRECT_TLS}')
                    impl.append(sdecl)
                    impl.append(tls_define(direct_ref(rt, sbody)))
                    impl.append('#else')
                    impl.append(f'extern {rt} {acc}(void);')
                    impl.append(tls_define(f'{acc}()'))
                    impl.append('#endif')
                    # decls half keeps the emitted #if/#else block verbatim
                    # (this static decl line sits inside its GNU branch).  No
                    # consumer of the decls half takes that branch -- c2mir
                    # defines neither macro, and a hosted cc build defines
                    # TUR_RT_SPLIT_HOSTED (the guard rewrite below) -- so the
                    # variable definition is dead text there.  The hosted
                    # build's direct reads go in afterwards
                    # (direct_tls_hosted).
                    decls.append(full)
                elif tls:
                    impl.append(de_static(full))
                    decls.append(full)
                else:
                    impl.append(de_static(full))
                    head_no_init = full.split('=')[0].rstrip()
                    head_no_init = de_static(head_no_init)
                    decls.append(f'extern {head_no_init};')
                i = j + 1
                continue
        # col-0 `static` PROTOTYPE (no body).
        if at_col0 and re.match(r'^(?:__attribute__\(\([^)]*\)\)\s+)?static\s', l) and '(' in l:
            j = i
            while '{' not in lines[j] and not code_part(lines[j]).endswith(';'):
                j += 1
            if code_part(lines[j]).endswith(';') and '{' not in ' '.join(lines[i:j + 1]):
                full = '\n'.join(lines[i:j + 1])
                pm = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', full)
                pname = pm.group(1) if pm else None
                if pname and pname not in defined:
                    # Defined BELOW the marker, per program (__tur_static_init
                    # is the case).  Keep the static prototype verbatim in the
                    # decls half -- the program half's static definition needs
                    # a static declaration, not an extern one -- and drop it
                    # from the runtime TU, which neither defines nor calls it.
                    program_protos.append(pname)
                    decls.append(full)
                    i = j + 1
                    continue
                # Definition lives in the runtime TU with external linkage
                # now, so a static declaration would be "declared but never
                # defined" in the program half.  De-static in both halves.
                desig = de_static(full)
                impl.append(desig)
                decls.append(desig)
                i = j + 1
                continue
        # A col-0 NON-static global DEFINITION with an initializer.  The static
        # case above becomes `extern` in the decls half; this one used to fall
        # through to "keep verbatim in both", so the symbol was defined TWICE.
        # Harmless on the JIT path (the program half is the only definition
        # c2mir compiles) and a duplicate-definition link error the moment a
        # cc-compiled program links the runtime TU -- which is exactly what
        # docs/upcoming/cc-path-preamble-split-plan.md needs it to do.
        #
        # Which half loses the definition is load-bearing, and neither obvious
        # answer works:
        #
        #   extern in the decls half -- breaks the JIT.  jit_sync_config_globals
        #     (src/jit_engine.c) locates tur_closure_headers_enabled as a MIR
        #     DATA ITEM in the program module and copies its value onto the
        #     host's.  No definition, no data item, and that handshake silently
        #     stops working while everything still builds.
        #
        #   __attribute__((weak)) in the runtime half -- breaks Windows.  On
        #     PE/COFF a weak definition inside an archive member is not pulled
        #     in to satisfy an undefined reference the way it is on ELF, so the
        #     host stopped resolving the symbol at all: `undefined reference to
        #     tur_closure_headers_enabled` from jit_engine.c and reactor.c.
        #
        # So the decls half carries BOTH spellings behind a guard the consumer
        # picks.  The JIT compiles it undefined and keeps the definition it
        # needs; a cc-compiled program that links the runtime TU defines
        # TUR_RT_SPLIT_HOSTED and takes the extern.
        if (at_col0 and '(' not in l and '=' in l
                and code_part(l).endswith(';')
                and not re.match(r'^\s*(?:static|extern|typedef|#)\b', l)
                and re.match(r'^[A-Za-z_][A-Za-z0-9_ \t]*[\s*]\*?[A-Za-z_][A-Za-z0-9_]*\s*=', l)):
            impl.append(l)
            decls.append('#ifdef TUR_RT_SPLIT_HOSTED')
            decls.append(f'extern {l.split("=")[0].rstrip()};')
            decls.append('#else')
            decls.append(l)
            decls.append('#endif')
            i += 1
            continue
        # The THREAD-LOCAL selector.  Each thread-local in the TLS table is emitted as
        #
        #     #if defined(__GNUC__) || defined(__clang__)
        #     static TUR_THREAD_LOCAL T x = ...;      <- native TLS, private
        #     #else
        #     extern T *tur_tls_x_ptr(void);          <- the HOST's storage
        #     #define x (*tur_tls_x_ptr())
        #     #endif
        #
        # and the decls half was kept verbatim on the assumption -- true until
        # now -- that "the decls half is only ever compiled by c2mir", which
        # defines neither macro and so takes the accessor branch.
        #
        # A hosted cc build breaks that assumption: gcc defines __GNUC__, takes
        # the FIRST branch, and the program gets its own private thread-locals
        # while the runtime keeps using the host accessors.  They never agree,
        # and the symptom is not a link error -- it is tur_current_fiber and
        # tur_panicking reading zero forever, which took out every async, fiber,
        # panic and catch-unwind fixture at once.
        #
        # So the decls half must take the accessor branch whenever the program
        # links the runtime, exactly as c2mir does.
        #
        # Rewritten only when the guard actually introduces a thread-local: the
        # same guard also selects __atomic_load_n over a shim, and gcc really
        # does have those.
        if (at_col0
                and l.strip() == '#if defined(__GNUC__) || defined(__clang__)'
                and i + 1 < len(lines)
                and ('TUR_THREAD_LOCAL' in lines[i + 1]
                     or '_Thread_local' in lines[i + 1])):
            impl.append(l)
            decls.append('#if (defined(__GNUC__) || defined(__clang__)) '
                         '&& !defined(TUR_RT_SPLIT_HOSTED)')
            i += 1
            continue
        # A PROJECT header (quoted include), dropped from the decls half for a
        # hosted consumer.
        #
        # This is the one place the split is genuinely LOSSY, and it is worth
        # being precise about why.  The preamble is not actually fixed across
        # programs: the emitter picks EITHER `#include "hamt.h"` (for a program
        # that uses hamt directly) OR loose `extern void *tur_hamt_new();`
        # declarations from stdlib's extern-c -- never both.  The decls region
        # is generated from ONE canonical emission, so it freezes whichever
        # choice that program made and hands it to every other program.
        #
        # c2mir does not care: it accepts the loose declaration alongside
        # hamt.h's `Hamt *tur_hamt_new(void)`.  gcc calls that a conflicting
        # type and stops.  Since the loose form is what the majority of
        # programs emit, the include is what gives way -- a program that
        # depends on the header instead is left with implicit declarations and
        # fails loudly at compile time, which is the safe direction.
        #
        # Properly fixing this means making the decls region carry both shapes,
        # or making the emitted extern-c declarations agree with the header.
        # See docs/upcoming/cc-path-preamble-split-plan.md.
        if at_col0 and re.match(r'^#include\s+"', l):
            impl.append(l)
            decls.append('#ifndef TUR_RT_SPLIT_HOSTED')
            decls.append(l)
            decls.append('#endif')
            i += 1
            continue
        # everything else (includes, macros, types, non-static decls,
        # comments): both halves keep it verbatim
        impl.append(l)
        decls.append(l)
        i += 1
    impl_txt = '\n'.join(impl) + '\n'
    decls, n_direct = direct_tls_hosted('\n'.join(decls).split('\n'), storage)
    if n_direct == 0:
        sys.exit('no thread-local block in the decls half took a direct-read '
                 'branch; did the guard rewrite change?')
    decls_txt = ('\n'.join(decls) +
                 '\n/* ==== S2: host-resident runtime; declarations only ==== */\n')
    return impl_txt, decls_txt, program_protos


def c_string_lines(text):
    """text -> list of C string-literal lines, one per source line."""
    out = []
    for line in text.split('\n'):
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append(f'"{esc}\\n"')
    # the final split('\n') element after a trailing newline is '', which
    # would append one spurious blank line; harmless but drop it for fidelity
    if text.endswith('\n'):
        out.pop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tur', default='./build/tur')
    ap.add_argument('--out', default='src/runtime/generated')
    args = ap.parse_args()

    src = subprocess.run([args.tur, 'emit-rt-split'], check=True,
                         capture_output=True, text=True).stdout
    hash_hex = subprocess.run([args.tur, 'emit-rt-split', '--hash'], check=True,
                              capture_output=True, text=True).stdout.strip()
    if MARK not in src:
        sys.exit('no preamble marker in emit-rt-split output')
    if not re.fullmatch(r'[0-9a-f]{16}', hash_hex):
        sys.exit(f'unexpected hash output: {hash_hex!r}')

    impl, decls, program_protos = split(src, tls_storage())
    print(f'preamble: {src.count(chr(10))} lines   '
          f'runtime TU: {impl.count(chr(10))} lines   '
          f'decls: {decls.count(chr(10))} lines   hash: {hash_hex}')
    print(f'program-defined prototypes kept static: {program_protos}')

    os.makedirs(args.out, exist_ok=True)

    # newline='\n' on every write below: these are committed artifacts, and the
    # default text mode would translate to CRLF when regenerating on Windows,
    # turning a content-free regen into a 3000-line whole-file diff.
    with open(os.path.join(args.out, 'tur_rt_split.c'), 'w', newline='\n') as f:
        f.write(f'/* {GEN_NOTE}\n'
                f' *\n'
                f' * The Turmeric runtime as one TU: the emitted preamble '
                f'(every gate on) with\n'
                f' * statics externalized and thread-locals routed to the '
                f'host tur_tls\n'
                f' * storage.  source hash {hash_hex}. */\n')
        f.write(impl)

    with open(os.path.join(args.out, 'tur_rt_split_decls.h'), 'w', newline='\n') as f:
        f.write(f'/* {GEN_NOTE}\n'
                f' *\n'
                f' * Declarations-only image of the runtime preamble: what '
                f'cmd_jit splices\n'
                f' * ahead of the program half when the split is engaged.  '
                f'Kept as a file for\n'
                f' * review/diffing; the copy tur actually uses is the '
                f'embedded string in\n'
                f' * tur_rt_split_embed.c.  source hash {hash_hex}. */\n')
        f.write(decls)

    with open(os.path.join(args.out, 'tur_rt_split_embed.c'), 'w', newline='\n') as f:
        f.write(f'/* {GEN_NOTE} */\n')
        f.write('#include <stdint.h>\n#include <stddef.h>\n\n')
        f.write('/* xxHash64 of the exact `tur emit-rt-split` text these '
                'artifacts were\n * generated from (hash spelling: '
                'tur_hamt_hash_xxh64, same call cmd_jit\n * makes at JIT '
                'time). */\n')
        f.write(f'const uint64_t tur_rt_split_hash = 0x{hash_hex}ull;\n\n')
        f.write('const char tur_rt_split_decls[] =\n')
        for line in c_string_lines(decls):
            f.write(f'  {line}\n')
        f.write(';\n\n')
        f.write('const size_t tur_rt_split_decls_len = '
                'sizeof(tur_rt_split_decls) - 1;\n')

    print(f'wrote {args.out}/tur_rt_split.c, tur_rt_split_decls.h, '
          f'tur_rt_split_embed.c')


if __name__ == '__main__':
    main()
