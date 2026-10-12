#!/usr/bin/env bash
# Open a shell on the box, or run one command there, over SSM.
#   scripts/ssh.sh
#   scripts/ssh.sh 'cd turmeric && timeout 720 bash tests/run.sh'
source "$(dirname "$0")/common.sh"
id="$(instance_id)"; reg="$(region)"
need session-manager-plugin "install the AWS Session Manager plugin"
# shellcheck disable=SC2046
exec ssh $(ssh_opts "$id" "$reg") "ubuntu@$id" "$@"
