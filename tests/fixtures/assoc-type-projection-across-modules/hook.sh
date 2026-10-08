#!/usr/bin/env bash
# associated-type-unusable-nullary-and-generic (half 2): a generic over an
# associated-type projection, declared in a module that holds no instance,
# called from the module that declares the instances -- both back ends.
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "== compiled"
"$TUR" run "$FIXTURE_DIR/src/main.tur" -I "$FIXTURE_DIR/src"
echo "== interpreted"
"$TUR" --interpret -I "$FIXTURE_DIR/src" "$FIXTURE_DIR/src/main.tur" 2>/dev/null
