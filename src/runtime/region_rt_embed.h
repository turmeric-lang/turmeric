/* region_rt_embed.h -- the region runtime sources, as byte arrays the
 * compiler links (generated at build time by cmake/embed_region_runtime.cmake
 * into <build>/src/generated/region_rt_embed.c).
 *
 * Why the emitter needs them: regions are on by default (RM3, graduated
 * 2026-09-05), so every emitted program references tur_region_* and every
 * static init registers tur_region_shutdown.  When libturt_runtime.a is on
 * the link line (the DEDUP-4b archive posture, rt_global_from_archive()) the
 * preamble only DECLARES those, exactly like the rc<T>/GC runtime; every
 * other consumer of the emitted C -- a project build, a --shared library, the
 * REPL spice cache, a bare `cc` of `tur emit-c` output -- gets the bodies
 * pasted into its owner TU from these arrays.  One implementation, the one
 * src/runtime/region.c is, rather than a hand-written replica that drifts.
 *
 * Each array is the file's exact bytes followed by a NUL; the _len excludes
 * the NUL.  The emitter drops the sources' local `#include "..."` lines while
 * copying, since it pastes the headers itself, in order, ahead of the bodies. */
#ifndef TUR_REGION_RT_EMBED_H
#define TUR_REGION_RT_EMBED_H

#include <stddef.h>

extern const unsigned char tur_rt_embed_region_h[];
extern const size_t        tur_rt_embed_region_h_len;
extern const unsigned char tur_rt_embed_arena_h[];
extern const size_t        tur_rt_embed_arena_h_len;
extern const unsigned char tur_rt_embed_arena_c[];
extern const size_t        tur_rt_embed_arena_c_len;
extern const unsigned char tur_rt_embed_region_c[];
extern const size_t        tur_rt_embed_region_c_len;
/* The r7rs-gc experiment's collector (src/runtime/r7gc.c), pasted into a
 * compiled `#lang r7rs` program's unit when --enable=r7rs-gc is on. */
extern const unsigned char tur_rt_embed_r7gc_c[];
extern const size_t        tur_rt_embed_r7gc_c_len;
/* The segmented stack (src/runtime/r7seg.c), pasted into a `#lang r7rs`
 * program's unit under --enable=r7rs-segmented-stack. */
extern const unsigned char tur_rt_embed_r7seg_c[];
extern const size_t        tur_rt_embed_r7seg_c_len;

#endif /* TUR_REGION_RT_EMBED_H */
