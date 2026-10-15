/* LD_PRELOAD: count memcpy/memmove calls by caller and size; report at exit. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NSLOT 4096
static struct { void *ra; char fn; size_t n_small, n_big, bytes; } slots[NSLOT];
static long total[2];

static void note(char fn, void *ra, size_t n) {
  total[fn == 'v']++;
  size_t h = ((uintptr_t)ra >> 2) % NSLOT;
  for (size_t i = 0; i < NSLOT; i++) {
    size_t k = (h + i) % NSLOT;
    if (slots[k].ra == ra && slots[k].fn == fn) { goto hit; }
    if (slots[k].ra == NULL) { slots[k].ra = ra; slots[k].fn = fn; goto hit; }
    continue;
  hit:
    if (n <= 16) slots[k].n_small++; else slots[k].n_big++;
    slots[k].bytes += n;
    return;
  }
}

static void *(*real_memcpy)(void *, const void *, size_t);
static void *(*real_memmove)(void *, const void *, size_t);

/* Before dlsym has run, fall back to a byte loop (dlsym itself may memcpy). */
static void *slow_copy(void *d, const void *s, size_t n) {
  unsigned char *dd = d; const unsigned char *ss = s;
  if (dd < ss) for (size_t i = 0; i < n; i++) dd[i] = ss[i];
  else for (size_t i = n; i > 0; i--) dd[i - 1] = ss[i - 1];
  return d;
}
static int resolving;
static void resolve(void) {
  if (resolving) return;
  resolving = 1;
  real_memcpy = dlsym(RTLD_NEXT, "memcpy");
  real_memmove = dlsym(RTLD_NEXT, "memmove");
  resolving = 0;
}

void *memcpy(void *d, const void *s, size_t n) {
  if (!real_memcpy) { resolve(); if (!real_memcpy) return slow_copy(d, s, n); }
  note('c', __builtin_return_address(0), n);
  return real_memcpy(d, s, n);
}
void *memmove(void *d, const void *s, size_t n) {
  if (!real_memmove) { resolve(); if (!real_memmove) return slow_copy(d, s, n); }
  note('v', __builtin_return_address(0), n);
  return real_memmove(d, s, n);
}

__attribute__((destructor)) static void report(void) {
  char path[64];
  snprintf(path, sizeof path, "/tmp/memcount.%d", (int)getpid());
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "total memcpy=%ld memmove=%ld\n", total[0], total[1]);
  for (int pass = 0; pass < 12; pass++) {       /* top 12 by call count */
    size_t best = (size_t)-1, bc = 0;
    for (size_t k = 0; k < NSLOT; k++) {
      size_t c = slots[k].n_small + slots[k].n_big;
      if (slots[k].ra && c > bc) { bc = c; best = k; }
    }
    if (best == (size_t)-1) break;
    fprintf(f, "%c ra=%p calls=%zu small(<=16B)=%zu big=%zu bytes=%zu\n", slots[best].fn,
            slots[best].ra, bc, slots[best].n_small, slots[best].n_big, slots[best].bytes);
    slots[best].n_small = slots[best].n_big = 0;
  }
  FILE *m = fopen("/proc/self/maps", "r");
  char line[512];
  while (m && fgets(line, sizeof line, m)) if (strstr(line, "x")) fputs(line, f);
  if (m) fclose(m);
  fclose(f);
}
