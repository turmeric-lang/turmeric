#!/usr/bin/env bash
# Spin the bench box up.  Idempotent: re-running updates the stack in place.
#
#   scripts/up.sh                       # defaults from cdk.json
#   BENCH_INSTANCE_TYPE=c7i.2xlarge scripts/up.sh
#   BENCH_ARCH=arm64 BENCH_INSTANCE_TYPE=m7g.xlarge scripts/up.sh
#   BENCH_GIT_REF=claude/my-branch scripts/up.sh
#   BENCH_MAX_UPTIME_HOURS=4 scripts/up.sh
#
# Passes your SSH public key (first of ~/.ssh/id_ed25519.pub, id_rsa.pub, or
# $BENCH_SSH_PUBKEY_FILE) so `scripts/ssh.sh` works; without one you still
# have `aws ssm start-session`.
source "$(dirname "$0")/common.sh"
ensure_deps

ctx=()
[ -n "${BENCH_INSTANCE_TYPE:-}" ]    && ctx+=(-c "instanceType=$BENCH_INSTANCE_TYPE")
[ -n "${BENCH_ARCH:-}" ]             && ctx+=(-c "arch=$BENCH_ARCH")
[ -n "${BENCH_DISK_GB:-}" ]          && ctx+=(-c "diskGb=$BENCH_DISK_GB")
[ -n "${BENCH_GIT_REF:-}" ]          && ctx+=(-c "gitRef=$BENCH_GIT_REF")
[ -n "${BENCH_MAX_UPTIME_HOURS:-}" ] && ctx+=(-c "maxUptimeHours=$BENCH_MAX_UPTIME_HOURS")

pub=""
for f in "${BENCH_SSH_PUBKEY_FILE:-}" "$HOME/.ssh/id_ed25519.pub" "$HOME/.ssh/id_rsa.pub"; do
  [ -n "$f" ] && [ -f "$f" ] && { pub="$(head -1 "$f")"; break; }
done
if [ -n "$pub" ]; then ctx+=(-c "sshPublicKey=$pub"); else
  echo "bench-box: no SSH public key found; ssh.sh/pull.sh will not work, 'aws ssm start-session' will" >&2
fi

npx cdk deploy "$STACK" --require-approval never --outputs-file "$OUTPUTS" "${ctx[@]}"

id="$(instance_id)"; reg="$(region)"
echo
echo "bench-box: $id ($(jq -r ".\"$STACK\".InstanceType" "$OUTPUTS"), $reg). Waiting for SSM agent..."
for _ in $(seq 60); do
  st="$(aws ssm describe-instance-information --region "$reg" \
        --filters "Key=InstanceIds,Values=$id" \
        --query 'InstanceInformationList[0].PingStatus' --output text 2>/dev/null || true)"
  [ "$st" = "Online" ] && break
  sleep 5
done
echo "bench-box: SSM $st. Bootstrap (apt + clone + Debug and Release builds) runs for several more minutes."
echo "  follow it:   scripts/ssh.sh tail -f /var/log/bench-bootstrap.log"
echo "  ready when:  scripts/ssh.sh test -f BOOTSTRAP_DONE"
echo "  shell:       scripts/ssh.sh            (or: $(jq -r ".\"$STACK\".SessionCommand" "$OUTPUTS"))"
echo "  tear down:   scripts/down.sh"
