#!/bin/bash
# infra/bench-box/bootstrap/user-data.sh -- runs once, as root, at first boot.
#
# The __PLACEHOLDERS__ are substituted by lib/bench-box-stack.ts from CDK
# context.  Everything the box needs to run `bash tests/run.sh`, the
# benchmark harnesses and the profilers the audit plan names, then a clone
# and a Debug + Release build of the requested ref so the box is ready when
# you log in.  Progress: /var/log/bench-bootstrap.log; done marker:
# /home/ubuntu/BOOTSTRAP_DONE (or BOOTSTRAP_FAILED).
set -u
exec > >(tee -a /var/log/bench-bootstrap.log) 2>&1
echo "== bench-box bootstrap start $(date -u +%FT%TZ)"

REPO_URL='__REPO_URL__'
GIT_REF='__GIT_REF__'
MAX_UPTIME_HOURS='__MAX_UPTIME_HOURS__'
SSH_PUBKEY='__SSH_PUBKEY__'
HOME_DIR=/home/ubuntu

# -- 1. Deadman timer: stop the box MAX_UPTIME_HOURS after boot, every boot.
#    A stopped instance bills only its EBS volume; `cdk destroy` still
#    removes everything.  Disarm for a long run with:
#      sudo systemctl stop bench-deadman.timer
cat > /etc/systemd/system/bench-deadman.service <<UNIT
[Unit]
Description=Stop the bench box after its uptime budget
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'logger -t bench-deadman "uptime budget of ${MAX_UPTIME_HOURS}h spent; stopping"; /usr/sbin/shutdown -h now'
UNIT
cat > /etc/systemd/system/bench-deadman.timer <<UNIT
[Unit]
Description=Uptime budget for the bench box (${MAX_UPTIME_HOURS}h after boot)
[Timer]
OnBootSec=${MAX_UPTIME_HOURS}h
AccuracySec=1min
[Install]
WantedBy=timers.target
UNIT
systemctl daemon-reload
systemctl enable --now bench-deadman.timer

# -- 2. SSH key for `ssh` over the SSM proxy (optional).
if [ -n "$SSH_PUBKEY" ]; then
  install -d -m 700 -o ubuntu -g ubuntu "$HOME_DIR/.ssh"
  echo "$SSH_PUBKEY" >> "$HOME_DIR/.ssh/authorized_keys"
  chown ubuntu:ubuntu "$HOME_DIR/.ssh/authorized_keys"
  chmod 600 "$HOME_DIR/.ssh/authorized_keys"
fi

# -- 3. Toolchain.  Mirrors ci.yml's apt list (libedit-dev ccache gdb) plus
#    clang for the musttail fixtures, valgrind/perf/hyperfine for the audit,
#    and the Python the harnesses use.  linux-tools-aws is the perf package
#    for the AWS kernel; fall back to the generic one if it is absent.
export DEBIAN_FRONTEND=noninteractive
apt-get update || { sleep 20; apt-get update; }
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build git ca-certificates curl jq \
  gcc clang lld libedit-dev ccache gdb \
  valgrind hyperfine python3 python3-venv \
  linux-tools-common
apt-get install -y --no-install-recommends linux-tools-aws \
  || apt-get install -y --no-install-recommends "linux-tools-$(uname -r)" \
  || echo "WARN: no linux-tools package for this kernel; perf unavailable"
# Let perf sample without sudo, and keep the kernel from hiding addresses.
cat > /etc/sysctl.d/90-bench-perf.conf <<SYS
kernel.perf_event_paranoid = -1
kernel.kptr_restrict = 0
SYS
sysctl --system >/dev/null

# -- 4. Clone and build as ubuntu.  Debug (ASan, what tests/run.sh wants) and
#    Release (what every timing number wants), per CLAUDE.md.
sudo -u ubuntu -H bash <<USER
set -u
cd "$HOME_DIR"
echo "== clone $REPO_URL @ $GIT_REF"
git clone --quiet "$REPO_URL" turmeric && cd turmeric && git checkout --quiet "$GIT_REF" || { echo "clone/checkout failed"; exit 1; }
echo "== configure + build Debug"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache >/dev/null && cmake --build build -j || exit 1
echo "== configure + build Release"
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache >/dev/null && cmake --build build-release -j || exit 1
./build/tur --version && ./build-release/tur --version
USER
rc=$?

echo "== box: $(nproc) cpus, $(free -g | awk '/Mem:/{print $2}') GiB, $(lsb_release -ds), cc $(cc -dumpversion), clang $(clang --version | head -1)"
if [ $rc -eq 0 ]; then
  touch "$HOME_DIR/BOOTSTRAP_DONE"; echo "== bench-box bootstrap DONE $(date -u +%FT%TZ)"
else
  touch "$HOME_DIR/BOOTSTRAP_FAILED"; echo "== bench-box bootstrap FAILED (rc=$rc) $(date -u +%FT%TZ)"
fi
chown ubuntu:ubuntu "$HOME_DIR"/BOOTSTRAP_* 2>/dev/null || true
