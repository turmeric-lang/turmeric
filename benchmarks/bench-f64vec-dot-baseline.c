/* bench-f64vec-dot-baseline.c -- bench-f64vec-dot.tur's workload in C.
 *
 * Two dot products over the same arrays: the eight-lane order f64vec-dot uses
 * (so the totals match the Turmeric program's exactly), and a plain
 * left-to-right loop, which gcc does not vectorize without -ffast-math.
 *
 *   cc -O3 -std=c99 bench-f64vec-dot-baseline.c -o base && ./base lanes
 *   ./base plain
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N 4096
#define REPS 200000

static double a[N], b[N];

static double dot_lanes(const double *y, const double *x, int64_t n) {
    double s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int64_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) { double m = y[i + k] * x[i + k]; s[k] = s[k] + m; }
    for (; i < n; i++) { double m = y[i] * x[i]; s[i & 7] = s[i & 7] + m; }
    return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
}

static double dot_plain(const double *y, const double *x, int64_t n) {
    double s = 0;
    for (int64_t i = 0; i < n; i++) s = s + y[i] * x[i];
    return s;
}

int main(int argc, char **argv) {
    int plain = argc > 1 && strcmp(argv[1], "plain") == 0;
    for (int i = 0; i < N; i++) {
        a[i] = 7.1 + (double)(i % 13);
        b[i] = 3.25 - (double)(i % 7);
    }
    double acc = 0;
    for (int k = 0; k < REPS; k++) acc = acc + (plain ? dot_plain(a, b, N) : dot_lanes(a, b, N));
    printf("%g\n", acc);
    return 0;
}
