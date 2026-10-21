/* fuzz_link_stub.c -- the one driver symbol tur_core needs.
 *
 * lsp/lsp.c references tur_collect_symbols, which is defined in the driver
 * (src/driver/pipeline.c).  A harness that links every tur_core object
 * without the driver resolves it here,
 * as the unit tests do (tests/unit/pkg_hash.c). */
#include "lsp/lsp_sym.h"

int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out);

int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)source_path; (void)logical_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}
