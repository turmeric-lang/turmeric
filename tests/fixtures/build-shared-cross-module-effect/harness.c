/* sepcomp-cross-module-effect-unhandled: dlopen the library `tur build
 * --shared` makes of src/ and call api.tur's exports.  Each one installs a
 * handler and calls into fx/ask, a different C unit of the library, which
 * performs: before the fix that was refused at build time (and before that,
 * "unhandled effect" at run time).  run-loop performs 50000 times, so the
 * tail resume has to land on the one driver both units share. */
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#  include "platform_dl.h"
#else
#  include <dlfcn.h>
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <lib>\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    typedef int64_t (*ask_fn)(void);
    typedef double (*scale_fn)(double);
    typedef int64_t (*loop_fn)(int64_t);
    ask_fn run_ask = (ask_fn)dlsym(h, "api__run_hyask");
    scale_fn run_scale = (scale_fn)dlsym(h, "api__run_hyscale");
    loop_fn run_loop = (loop_fn)dlsym(h, "api__run_hyloop");
    if (!run_ask || !run_scale || !run_loop) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }
    printf("run-ask = %lld\n", (long long)run_ask());
    printf("run-scale(7.1) = %.1f\n", run_scale(7.1));
    printf("run-loop(50000) = %lld\n", (long long)run_loop(50000));
    fflush(stdout);
    return 0;
}
