/* Phase HKT-P2: Monomorphic baseline for bind-option benchmark (§8)
 *
 * This is the non-HKT equivalent of bind-option.tur, used to compute
 * the overhead ratio of dictionary passing.
 *
 * The test chains bind operations over a sequence of option values,
 * without any typeclass dispatch overhead.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct { bool is_some; int64_t value; } Option;

Option *some(int64_t x) {
    Option *o = malloc(sizeof(Option));
    o->is_some = true;
    o->value = x;
    return o;
}

void option_free(Option *o) {
    free(o);
}

// Monomorphic bind for Option
Option *option_bind(Option *ma, int64_t (*fn)(int64_t)) {
    if (!ma || !ma->is_some) return NULL;
    return (Option*)(intptr_t)fn(ma->value);
}

int64_t inc_some(int64_t x) {
    return (int64_t)(intptr_t)some(x + 1);
}

#define N 100000

int main(void) {
    Option *acc = some(0);
    
    for (int64_t i = 0; i < N; i++) {
        Option *next = (Option*)option_bind(acc, inc_some);
        if (acc) option_free(acc);
        acc = next;
    }
    
    if (acc) option_free(acc);
    
    return 0;
}
