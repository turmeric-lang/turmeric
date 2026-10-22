#!/usr/bin/env bash
# project-less-tools-plan PT2: `tur run <file> a b` passes `a b` to the
# program.  The words after the file that are not run's own flags are program
# arguments, then everything after `--`; run's flags (-I, --release, --engine)
# still work on either side of the file.  They used to be dropped silently.
set -u
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
F="$FIXTURE_DIR/show-args.tur"
run() {
    echo "## tur run $*" | sed -e "s|$F|F|g" -e "s|$FIXTURE_DIR|DIR|g"
    "$TUR" run "$@" 2>&1
    echo "exit $?"
}
run "$F" a b
run "$F" -- a
run "$F" a -- --release -I x
run --release "$F" a
run "$F" a --release b
run -I "$FIXTURE_DIR" "$F" a -I "$FIXTURE_DIR" b
run "$F" -x --flag
run "$F"
run --engine=interp "$F" i1 i2
run "$F" --engine interp i3
