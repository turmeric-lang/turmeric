/* driver.h -- what the compiler driver's files share (main-c-split-plan).
 *
 * The `tur` executable's driver lives in src/driver/ beside main.c.  This
 * header declares the helpers and the state that cross between those files;
 * commands.h declares the `cmd_*` / `usage_*` entry points the dispatcher
 * calls.  A helper that only its own file uses stays `static` there: `tur`
 * exports its symbols for the JIT, so every name declared here is visible to
 * JIT-compiled programs.
 *
 * Sectioned by defining file, one `== driver/<file>.c ==` heading comment
 * each; tests/check-driver-headers.py fails when a prototype sits under a
 * file that does not define it.  Each global is defined in the file that owns
 * its logic and declared `extern` in that file's section. */
#ifndef TUR_DRIVER_H
#define TUR_DRIVER_H

#endif /* TUR_DRIVER_H */
