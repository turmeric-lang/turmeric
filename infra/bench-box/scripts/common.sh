# Shared by the bench-box scripts.  Source, do not run.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"
OUTPUTS="$HERE/cdk-outputs.json"
STACK=TurmericBenchBox

need() { command -v "$1" >/dev/null 2>&1 || { echo "bench-box: missing '$1' -- $2" >&2; exit 2; }; }
need aws "install the AWS CLI v2 and run 'aws configure' (or set AWS_PROFILE)"
need npx "install Node.js 20+"
need jq  "install jq"

ensure_deps() { [ -d node_modules ] || npm ci --no-audit --no-fund; }

# Instance id and region from the last deploy's outputs file.
instance_id() {
  [ -f "$OUTPUTS" ] || { echo "bench-box: no $OUTPUTS -- run scripts/up.sh first" >&2; exit 2; }
  jq -r ".\"$STACK\".InstanceId" "$OUTPUTS"
}
region() { jq -r ".\"$STACK\".Region" "$OUTPUTS"; }

# ssh(1) options that tunnel through SSM Session Manager: no inbound port,
# no bastion, no public key on the box beyond the one up.sh passed.
# Needs the Session Manager plugin for the AWS CLI.
ssh_opts() {
  local id="$1" reg="$2"
  printf '%s' "-o ProxyCommand=aws ssm start-session --region $reg --target %h --document-name AWS-StartSSHSession --parameters portNumber=%p -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ServerAliveInterval=30"
}
