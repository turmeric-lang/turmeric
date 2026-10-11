#!/usr/bin/env bash
# Copy results back from the box with rsync over SSM.
#   scripts/pull.sh <remote path under ~/turmeric> [local dir]
#   scripts/pull.sh performance-comparison/results ./results-from-box
#   scripts/pull.sh benchmarks/output/triangle
source "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || { echo "usage: scripts/pull.sh <remote path under ~/turmeric> [local dir]" >&2; exit 2; }
id="$(instance_id)"; reg="$(region)"
need rsync "install rsync"
need session-manager-plugin "install the AWS Session Manager plugin"
src="$1"; dst="${2:-./bench-box-results/$(basename "$src")}"
mkdir -p "$dst"
rsync -az --info=progress2 -e "ssh $(ssh_opts "$id" "$reg")" "ubuntu@$id:turmeric/$src/" "$dst/"
echo "bench-box: pulled turmeric/$src -> $dst"
