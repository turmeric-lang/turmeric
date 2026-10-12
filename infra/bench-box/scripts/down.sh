#!/usr/bin/env bash
# Tear the bench box down: instance, volume, VPC, role -- everything.
# Pull anything you want to keep first (scripts/pull.sh); the volume goes too.
source "$(dirname "$0")/common.sh"
ensure_deps
npx cdk destroy "$STACK" --force
rm -f "$OUTPUTS"
echo "bench-box: gone."
