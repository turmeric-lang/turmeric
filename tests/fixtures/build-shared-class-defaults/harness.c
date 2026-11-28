/* build-shared-class-defaults: dlopen the library `tur build --shared` makes
 * of src/ and call through instance methods whose bodies are class defaults
 * compiled into the instance's module.  Before the fixes the library did not
 * build: inst.h prototyped a carrier-returning instance method as the struct
 * pointer inst.c defines as int64_t, and a defaulted body's first call named
 * its temp with the previous method's result type. */
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
    typedef int64_t (*get_fn)(void);
    get_fn r_labels = (get_fn)dlsym(h, "cd__inst__r_hylabel_hycount");
    get_fn s_labels = (get_fn)dlsym(h, "cd__inst__s_hylabel_hycount");
    get_fn s_marks  = (get_fn)dlsym(h, "cd__inst__s_hymarks_hysum");
    if (!r_labels || !s_labels || !s_marks) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }
    printf("R labels = %lld, S labels = %lld, S marks = %lld\n",
           (long long)r_labels(), (long long)s_labels(), (long long)s_marks());
    return 0;
}
