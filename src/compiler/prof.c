/* prof.c -- `--timings`: the compiler's own zone timer.  See prof.h.
 *
 * profiling-plan C0+C1.  The one invariant worth the bookkeeping: time is
 * charged only at transitions, always to the zone that was on top since the
 * previous transition, starting from the moment the timer started.  The
 * charges telescope, so the phases sum to the wall clock exactly -- the table
 * is a partition, not a pile of overlapping inclusive timers summing to 300%.
 */

#include "prof.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>   /* QueryPerformanceCounter */
#endif

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif

/* Sanitizer detection, for the "not representative" note (profiling-plan
 * 7.1).  GCC spells it __SANITIZE_ADDRESS__; clang answers __has_feature. */
#if defined(__has_feature)
#  define PROF_HAS_FEATURE(x) __has_feature(x)
#else
#  define PROF_HAS_FEATURE(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    PROF_HAS_FEATURE(address_sanitizer) || PROF_HAS_FEATURE(thread_sanitizer)
#  define PROF_SANITIZED 1
#else
#  define PROF_SANITIZED 0
#endif

ProfZone g_prof_top = PROF_OTHER;

static const char *const k_zone_names[PROF_ZONE_N] = {
    [PROF_READ]         = "read",
    [PROF_SWEET]        = "sweet",
    [PROF_MODULE_LOAD]  = "module-load",
    [PROF_MACRO_EXPAND] = "macro-expand",
    [PROF_ELAB]         = "elaborate",
    [PROF_TYPECLASS]    = "typeclass",
    [PROF_KIND]         = "kind-check",
    [PROF_EFFECT_LOWER] = "effect-lower",
    [PROF_EFFECT_ROW]   = "effect-row",
    [PROF_CPS]          = "cps",
    [PROF_BORROW]       = "borrow-check",
    [PROF_REFINE]       = "refine",
    [PROF_MONO]         = "mono",
    [PROF_EMIT]         = "emit",
    [PROF_CC]           = "cc",
    [PROF_JIT]          = "jit",
    [PROF_RUN]          = "run",
    [PROF_OTHER]        = "other",
};

const char *prof_zone_name(ProfZone z) {
    return ((unsigned)z < (unsigned)PROF_ZONE_N && k_zone_names[z]) ? k_zone_names[z] : "?";
}

/* The zone stack, run-length encoded: re-entering the zone on top bumps its
 * count instead of pushing, so the depth is the number of zone CHANGES on the
 * path (a handful), not the elaborator's recursion depth (hundreds).  Entry 0
 * is PROF_OTHER and is never popped.  Past the cap every further enter/exit
 * is counted in s_overflow and charges nothing -- time keeps flowing to the
 * zone that was on top -- so LIFO stays intact; the report says it happened. */
#define PROF_STACK_CAP 1024
static uint8_t  s_zone[PROF_STACK_CAP];
static uint32_t s_count[PROF_STACK_CAP];
static int      s_sp;
static uint32_t s_overflow;
static bool     s_overflowed;
static bool     s_unbalanced;

static uint64_t s_self_ns[PROF_ZONE_N];
static uint64_t s_start_ns;
static uint64_t s_last_ns;
static uint64_t s_clock_reads;

static bool        s_started;
static bool        s_finished;
static time_t      s_started_utc;
static char       *s_command;
static const char *s_out_path;

#define PROF_MAX_NOTES 8
static char *s_notes[PROF_MAX_NOTES];
static int   s_n_notes;

/* A monotonic clock in nanoseconds.  POSIX: clock_gettime(CLOCK_MONOTONIC),
 * the idiom src/jit_engine.c already uses (macOS has it too).  Windows:
 * QueryPerformanceCounter, which is what MinGW's clock_gettime wraps anyway,
 * without depending on winpthreads' <pthread_time.h> being pulled in. */
#ifdef _WIN32
static uint64_t prof_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    s_clock_reads++;
    uint64_t f = (uint64_t)freq.QuadPart, t = (uint64_t)c.QuadPart;
    return (t / f) * 1000000000ull + (t % f) * 1000000000ull / f;
}
#else
static uint64_t prof_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_clock_reads++;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/* Charge [s_last_ns, now) to the zone on top and move the mark. */
static void prof_charge_top(void) {
    uint64_t now = prof_now_ns();
    s_self_ns[s_zone[s_sp - 1]] += now - s_last_ns;
    s_last_ns = now;
}

void prof_enter_slow(ProfZone z) {
    if (s_overflow) { s_overflow++; return; }
    if (s_zone[s_sp - 1] == (uint8_t)z) { s_count[s_sp - 1]++; return; }
    if (s_sp == PROF_STACK_CAP) { s_overflow = 1; s_overflowed = true; return; }
    prof_charge_top();
    s_zone[s_sp]  = (uint8_t)z;
    s_count[s_sp] = 1;
    s_sp++;
    g_prof_top = z;
}

void prof_exit_slow(ProfZone z) {
    if (s_overflow) { s_overflow--; return; }
    if (s_sp <= 1) { s_unbalanced = true; return; }
    if (s_zone[s_sp - 1] != (uint8_t)z) s_unbalanced = true;
    if (--s_count[s_sp - 1] > 0) return;
    prof_charge_top();
    s_sp--;
    g_prof_top = (ProfZone)s_zone[s_sp - 1];
}

void prof_note(const char *note) {
    if (!note || s_n_notes >= PROF_MAX_NOTES) return;
    size_t n = strlen(note);
    char *c = (char *)malloc(n + 1);
    if (!c) return;
    memcpy(c, note, n + 1);
    s_notes[s_n_notes++] = c;
}

static void prof_atexit(void) { prof_finish(); }

void prof_start(int argc, char **argv, const char *out_path) {
    if (s_started) return;
    s_started = true;
    s_out_path = out_path;

    /* "tur <args...>", as typed. */
    size_t len = 4;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    s_command = (char *)malloc(len + 1);
    if (s_command) {
        size_t at = 0;
        memcpy(s_command, "tur", 3);
        at = 3;
        for (int i = 1; i < argc; i++) {
            size_t n = strlen(argv[i]);
            s_command[at++] = ' ';
            memcpy(s_command + at, argv[i], n);
            at += n;
        }
        s_command[at] = '\0';
    }

    s_started_utc = time(NULL);
    s_zone[0]  = (uint8_t)PROF_OTHER;
    s_count[0] = 1;
    s_sp       = 1;
    g_prof_top = PROF_OTHER;
    s_start_ns = s_last_ns = prof_now_ns();
    g_timings     = true;
    g_timings_out = out_path;
    atexit(prof_atexit);
}

/* A JSON string body: quotes, backslashes and control bytes escaped. */
static void prof_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static double prof_ms(uint64_t ns) { return (double)ns / 1.0e6; }

static double prof_pct(uint64_t ns, uint64_t wall) {
    return wall ? 100.0 * (double)ns / (double)wall : 0.0;
}

static void prof_write_json(FILE *f, uint64_t wall) {
    char when[32] = "";
    struct tm *tm = gmtime(&s_started_utc);
    if (tm) strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", tm);

    fputs("{\n  \"schema\": \"tur-profile/1\",\n  \"kind\": \"compiler\",\n", f);
    fputs("  \"tur_version\": ", f); prof_json_str(f, TUR_VERSION); fputs(",\n", f);
    fputs("  \"command\": ", f); prof_json_str(f, s_command); fputs(",\n", f);
    fputs("  \"started_utc\": ", f); prof_json_str(f, when); fputs(",\n", f);
    fprintf(f, "  \"wall_ms\": %.4f,\n", prof_ms(wall));
    fputs("  \"notes\": [", f);
    for (int i = 0; i < s_n_notes; i++) {
        fputs(i ? ",\n    " : "\n    ", f);
        prof_json_str(f, s_notes[i]);
    }
    fputs(s_n_notes ? "\n  ],\n" : "],\n", f);
    fprintf(f, "  \"clock_reads\": %llu,\n", (unsigned long long)s_clock_reads);
    fputs("  \"phases\": [\n", f);
    for (int z = 0; z < PROF_ZONE_N; z++) {
        fputs("    {\"name\": ", f);
        prof_json_str(f, k_zone_names[z]);
        fprintf(f, ", \"self_ms\": %.4f, \"self_pct\": %.4f}%s\n",
                prof_ms(s_self_ns[z]), prof_pct(s_self_ns[z], wall),
                z + 1 < PROF_ZONE_N ? "," : "");
    }
    fputs("  ]\n}\n", f);
}

static void prof_print_table(FILE *f, uint64_t wall) {
    fprintf(f, "tur --timings: %s\n", s_command ? s_command : "tur");
    for (int i = 0; i < s_n_notes; i++) fprintf(f, "  note: %s\n", s_notes[i]);
    fprintf(f, "  %-14s %11s %8s\n", "phase", "self ms", "self %");
    for (int z = 0; z < PROF_ZONE_N; z++) {
        if (s_self_ns[z] == 0) continue;
        fprintf(f, "  %-14s %11.3f %8.1f\n", k_zone_names[z],
                prof_ms(s_self_ns[z]), prof_pct(s_self_ns[z], wall));
    }
    fprintf(f, "  %-14s %11.3f %8.1f\n", "total (wall)", prof_ms(wall), wall ? 100.0 : 0.0);
    fprintf(f, "  (exclusive time: a phase's own time, nested phases excluded; "
               "%llu clock reads)\n", (unsigned long long)s_clock_reads);
}

void prof_finish(void) {
    if (!s_started || s_finished) return;
    s_finished = true;
    /* Close the books before anything else costs time. */
    prof_charge_top();
    uint64_t wall = s_last_ns - s_start_ns;
    g_timings = false;

    if (PROF_SANITIZED)
        prof_note("this tur is sanitized (ASan/UBSan/TSan): timings from a "
                  "sanitized tur are not representative -- profile a Release build");
#ifndef NDEBUG
    prof_note("this tur has assertions on (a Debug build): every core pass also "
              "re-verifies kind info, which is charged to that pass");
#endif
    if (s_self_ns[PROF_RUN] > 0)
        prof_note("phase 'run' is the compiled program's own execution, not compile time");
    if (s_overflowed)
        prof_note("the zone stack overflowed; time past the cap went to the zone below it");
    if (s_unbalanced)
        prof_note("the zone stack was unbalanced (a prof_exit did not match its "
                  "prof_enter); some time may be charged to a neighboring phase");
    if (s_sp != 1) {
        char msg[160];
        snprintf(msg, sizeof msg, "the process exited from inside phase '%s'; "
                 "it was charged up to the report",
                 prof_zone_name((ProfZone)s_zone[s_sp - 1]));
        prof_note(msg);
    }

    prof_print_table(stderr, wall);
    fflush(stderr);

    if (s_out_path && *s_out_path) {
        /* `/dev/stdout` and `-` reuse the process's own stdout so the JSON
         * lands after anything already buffered there (and works where there
         * is no /dev, i.e. a native Windows tur.exe). */
        bool to_stdout = strcmp(s_out_path, "-") == 0 ||
                         strcmp(s_out_path, "/dev/stdout") == 0;
        bool to_stderr = strcmp(s_out_path, "/dev/stderr") == 0;
        FILE *f = to_stdout ? stdout : to_stderr ? stderr : fopen(s_out_path, "w");
        if (!f) {
            fprintf(stderr, "tur: --timings: cannot write '%s': %s\n",
                    s_out_path, strerror(errno));
        } else {
            prof_write_json(f, wall);
            if (f == stdout || f == stderr) fflush(f);
            else fclose(f);
        }
    }
}
