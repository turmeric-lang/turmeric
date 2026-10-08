#!/usr/bin/env bash
# class-and-generic-in-an-instance-less-module: src/box/cls.tur declares a
# class and constrained generics over it and holds no instance -- the layout a
# spice takes, with the vocabulary in one module and the instances beside the
# types (src/main.tur, one of them declared AFTER the defn that uses it).
# box/mid imports the generics and defines another over them.  The imported
# module is elaborated whole at the import, before any importer instance
# exists, so every generic reported TUR-E0015 "declares no 'Box' instance at
# all" for a program that declares two.  They are parked now and retried once
# an instance registers.  Both back ends, then the instance-less program,
# which must still report the error.  See
# docs/reported/associated-type-unusable-nullary-and-generic.md.
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "== compiled"
"$TUR" run "$FIXTURE_DIR/src/main.tur" -I "$FIXTURE_DIR/src"
echo "== interpreted"
"$TUR" --interpret -I "$FIXTURE_DIR/src" "$FIXTURE_DIR/src/main.tur" 2>/dev/null
echo "== no instance anywhere"
if "$TUR" check "$FIXTURE_DIR/src/noinst.tur" -I "$FIXTURE_DIR/src" > "$1/noinst.log" 2>&1; then
    echo "unexpected: an instance-less program checked clean"
    exit 1
fi
grep -c "TUR-E0015.*declares no 'Box' instance at all" "$1/noinst.log"
