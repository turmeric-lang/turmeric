/* front_link_test.c -- the driver's front half compiles Turmeric to C on its own.
 *
 * tur_front (wasm-jit-plan P2) is five driver files -- pipeline, preamble_split,
 * cc, paths, spice -- that a host with no command line links to turn source
 * into C: the browser JIT is the first such host.  This program is linked from
 * tur_front and tur_core and nothing else.  So the test is mostly the link:
 * when one of those files grows a reference to cli.c, a cmd_*.c, or anything
 * else that pulls the command table in, this target stops linking, and the
 * build says which symbol did it.
 *
 * At run time it compiles two fixtures and checks the C that comes back: a
 * plain program, and one whose inline-C body has to reach the output verbatim.
 *
 * usage: tur_front_link_unit <stdlib dir> <plain .tur> <inline-C .tur>
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "driver/driver.h"
#include "lsp/lsp_sym.h"
#include "platform_fs.h"  /* setenv on Windows */

/* tur_core's LSP server (lsp/lsp.c) calls tur_collect_symbols, which every
 * host supplies itself: tur from driver/lsp_backend.c, the browser module
 * from web/wasm_lsp.c.  tur_front deliberately carries neither, so this test
 * supplies the stub the other tur_core unit tests use. */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)source_path; (void)logical_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static int failed = 0;

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}

static void compile_one(const char *path, const char *needle, const char *what) {
    Buf out;
    buf_init(&out);
    int rc = compile_to_c(path, &out, NULL, 0, NULL, 0);
    char label[256];
    snprintf(label, sizeof(label), "%s: compile_to_c returns 0", what);
    check(rc == 0, label);
    snprintf(label, sizeof(label), "%s: the C defines main", what);
    check(out.data && strstr(out.data, "int main(") != NULL, label);
    if (needle) {
        snprintf(label, sizeof(label), "%s: the inline-C body is in the C", what);
        check(out.data && strstr(out.data, needle) != NULL, label);
    }
    buf_free(&out);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <stdlib dir> <plain .tur> <inline-C .tur>\n", argv[0]);
        return 2;
    }
    setenv("TUR_STDLIB_DIR", argv[1], 1);
    setenv("TUR_NO_GLOBAL_SPICES", "1", 1);
    g_main_argc = argc;
    g_main_argv = (const char *const *)argv;

    compile_one(argv[2], NULL, "plain program");
    compile_one(argv[3], "(int)x.e", "inline-C program");

    printf("%s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}
