/* benchmarks/audit/rusage-run.c -- run one command and report its rusage.
 *
 * Usage: rusage-run <timeout-seconds> <stdin-file|-> <stdout-file> <argv...>
 *
 * stdin is the named file, or /dev/null for "-"; stderr goes to /dev/null.
 *
 * Prints one line to its own stdout:
 *     <status> <wall_ns> <user_us> <sys_us> <maxrss_kib>
 * where <status> is the child's exit code, or "timeout", or "signal:<n>".
 *
 * Why a C launcher and not Python's subprocess: on Linux, exec records the
 * replaced address space's high-water RSS into the process's ru_maxrss, and
 * a child forked from Python starts as a copy of Python.  Every row measured
 * that way reads at least the runner's own ~16 MB, whatever the program did.
 * Forked from this binary, the floor is this binary's ~1 MB -- the same reason
 * GNU time is a small C program.  run-corpus.py compiles this at start. */
#define _POSIX_C_SOURCE 200809L
/* wait4() and the BSD rusage fields (ru_maxrss) are outside POSIX: glibc
 * shows them under _DEFAULT_SOURCE, Darwin under _DARWIN_C_SOURCE.  Without
 * the latter, _POSIX_C_SOURCE alone hides both on macOS and this file fails
 * to compile -- which kept every nightly bench job from producing a row. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t timed_out;
static pid_t child;

static void on_alarm(int sig) {
    (void)sig;
    timed_out = 1;
    if (child > 0) kill(child, SIGKILL);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: rusage-run <timeout-s> <stdin-file|-> <stdout-file> <argv...>\n");
        return 2;
    }
    const char *in_path = strcmp(argv[2], "-") == 0 ? "/dev/null" : argv[2];
    unsigned timeout = (unsigned)strtoul(argv[1], NULL, 10);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    child = fork();
    if (child < 0) { perror("fork"); return 2; }
    if (child == 0) {
        int in = open(in_path, O_RDONLY);
        int out = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int err = open("/dev/null", O_WRONLY);
        if (in < 0 || out < 0 || err < 0) _exit(126);
        dup2(in, 0); dup2(out, 1); dup2(err, 2);
        execvp(argv[4], argv + 4);
        _exit(127);
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(timeout);
    int status;
    struct rusage ru;
    while (wait4(child, &status, 0, &ru) < 0) {
        if (errno != EINTR) { perror("wait4"); return 2; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    alarm(0);
    long long wall_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL
                      + (t1.tv_nsec - t0.tv_nsec);
    long long user_us = (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec;
    long long sys_us  = (long long)ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec;
#ifdef __APPLE__
    long long rss_kib = (long long)ru.ru_maxrss / 1024;   /* bytes on macOS */
#else
    long long rss_kib = (long long)ru.ru_maxrss;          /* KiB on Linux */
#endif
    if (timed_out)
        printf("timeout");
    else if (WIFSIGNALED(status))
        printf("signal:%d", WTERMSIG(status));
    else
        printf("%d", WEXITSTATUS(status));
    printf(" %lld %lld %lld %lld\n", wall_ns, user_us, sys_us, rss_kib);
    return 0;
}
