/* sepcomp-module-def-initializers-never-run: dlopen the library
 * `tur build --shared` makes of src/, and read its defs through the exports.
 * Before the fix every def read zero.
 * sepcomp-exported-global-header-name-mismatch: the exported global is
 * `glob__counter` -- the name glob.h declares -- and user.tur's functions
 * read and write that one variable. */
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
    get_fn get = (get_fn)dlsym(h, "glob__get_hycounter");
    get_fn bump = (get_fn)dlsym(h, "glob__bump");
    get_fn read_it = (get_fn)dlsym(h, "user__read_hyit");
    typedef int64_t (*poke_fn)(int64_t);
    poke_fn poke = (poke_fn)dlsym(h, "user__poke");
    int64_t *counter = (int64_t *)dlsym(h, "glob__counter");
    if (!get || !bump || !read_it || !poke || !counter) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }
    int64_t first = get();
    int64_t bumped = bump();
    printf("counter = %lld, after bump = %lld\n", (long long)first, (long long)bumped);
    int64_t read = read_it();
    int64_t poked = poke(40);
    int64_t after = get();
    printf("user reads %lld, pokes %lld; glob reads %lld; glob__counter = %lld\n",
           (long long)read, (long long)poked, (long long)after, (long long)*counter);
    fflush(stdout);
    return 0;
}
