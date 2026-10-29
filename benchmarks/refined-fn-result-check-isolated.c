/* refined-fn-result-check-isolated.c -- refined-function-types-plan F4.
 *
 * The C a call through a refined function value lowers to, with the CPS
 * machinery taken away: today every function that calls through a function
 * value is CPS-colored (cps-dk-internals-guide, "Over-coloring (R-1)"), and
 * that cost swamps the check.  This times what the check will cost once R-1 is
 * narrowed and such a loop is emitted direct.  The check is copied from
 * emitted C: `tur-contract-check-at` (an inlined compare and a cold panic)
 * followed by the `tur_panicking` test the emitter puts after every call.
 *
 *   plain    the indirect call alone
 *   result   F2's post-call result check
 *   param    F2's call-site argument check, kept
 *   wrapper  F4 option 2 for an unproved coercion: a checking wrapper in
 *            front of the real function, so two indirect calls per call
 *
 * Run by benchmarks/bench-refined-fn-result-check.sh; by hand:
 *   cc -O2 -o iso benchmarks/refined-fn-result-check-isolated.c && ./iso */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
volatile int tur_panicking;
__attribute__((noinline, cold)) static void tur_panic_at(const char *f, int l, const char *m) {
    fprintf(stderr, "panic at %s:%d: %s\n", f, l, m); tur_panicking = 1; exit(1); }
static void tur_hycontract_hycheck_hyat(bool c, const char *m, const char *f, int64_t l) {
    if (!c) tur_panic_at(f, (int)l, m); }
__attribute__((noinline)) static int64_t inc1(int64_t x) { return x + 1; }
__attribute__((noinline)) static int64_t inc2(int64_t x) { return x + 2; }
typedef int64_t (*fnp)(int64_t);
__attribute__((noinline)) static int64_t plain(fnp g, int64_t n) {
    int64_t acc = 0;
    for (int64_t i = 0; i < n; i++) acc += g(i);
    return acc; }
__attribute__((noinline)) static int64_t result(fnp g, int64_t n) {
    int64_t acc = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t r = g(i);
        tur_hycontract_hycheck_hyat(r > 0, "Result contract violated", "x.tur", 6);
        if (tur_panicking) return 0;
        acc += r; }
    return acc; }
__attribute__((noinline)) static int64_t param(fnp g, int64_t n) {
    int64_t acc = 0;
    for (int64_t i = 0; i < n; i++) {
        tur_hycontract_hycheck_hyat(i >= 0, "Contract violated by argument 1", "x.tur", 6);
        if (tur_panicking) return 0;
        acc += g(i); }
    return acc; }
static fnp wrap_inner;
__attribute__((noinline)) static int64_t wrapper_fn(int64_t x) {
    int64_t r = wrap_inner(x);
    tur_hycontract_hycheck_hyat(r > 0, "result check", "x.tur", 6);
    return r; }
static double ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; }
int main(int argc, char **argv) {
    int64_t n = argc > 1 ? atoll(argv[1]) : 1000000000;
    fnp f = argc > 2 ? inc2 : inc1;
    wrap_inner = f;
    struct { const char *nm; int64_t (*run)(fnp, int64_t); fnp g; } v[] = {
        { "plain", plain, f }, { "result", result, f }, { "param", param, f },
        { "wrapper", plain, wrapper_fn } };
    for (int k = 0; k < 4; k++) {
        double best = 1e18; int64_t s = 0;
        for (int rep = 0; rep < 5; rep++) {
            struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
            s += v[k].run(v[k].g, n);
            clock_gettime(CLOCK_MONOTONIC, &b);
            double t = ms(a, b); if (t < best) best = t; }
        printf("%-8s best %8.1f ms  %.3f ns/call  (sum %lld)\n", v[k].nm, best,
               best * 1e6 / n, (long long)s);
    }
    return 0; }
