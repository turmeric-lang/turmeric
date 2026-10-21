/*
 * justrun.h -- tur run: Justfile-compatible task runner (RN0-RN7).
 *
 * Public API: a single entry point dispatched from cmd_run_main when `tur run`
 * is invoked without a .tur file argument.
 */

#ifndef TUR_JUSTRUN_H
#define TUR_JUSTRUN_H

/*
 * cmd_justrun -- execute Justfile recipes from the command line.
 *
 * argc/argv are the full program argv (argv[0] = "tur", argv[1] = "run"
 * or its "just" synonym).
 *
 * Exit codes:
 *   0   recipe succeeded
 *   1   recipe ran but exited non-zero (propagated)
 *   2   CLI / parse / unsupported-feature error
 *   127 Justfile not found or unreadable
 */
int cmd_justrun(int argc, char **argv);

/* justrun_finds_justfile -- true when an explicit --justfile arg or the
 * upward search from cwd yields a Justfile.  Lets the driver's `tur run`
 * dispatcher fall back to the classic project-run path for a BARE
 * invocation in a Justfile-less build.tur project (engine-selection-plan
 * E3) instead of the historical hard 127. */
int justrun_finds_justfile(void);

/* usage_justrun -- print tur run help to stderr and return 0. */
int usage_justrun(void);

/*
 * justrun_write_template -- write the standard spice Justfile template.
 *
 * dir        directory to write Justfile into (must exist)
 * spice_name spice name to substitute for {{ spice_name }}; if NULL,
 *            derived from the directory basename
 * force      if 0, refuse to overwrite an existing Justfile
 *
 * Returns 0 on success, 1 on error.
 */
int justrun_write_template(const char *dir, const char *spice_name, int force);

#endif /* TUR_JUSTRUN_H */
