# Bench box -- a disposable EC2 instance for suites and benchmarks

A CDK app that spins up one EC2 instance with the Turmeric toolchain
installed and the repo built, so a test-suite run or a benchmark sweep
happens on a quiet, known machine instead of a laptop. `up` before testing,
`down` after; nothing here is meant to live longer than a session.

```sh
just bench-box-up                 # or: bash infra/bench-box/scripts/up.sh
just bench-box-ssh                # a shell on the box (SSM; nothing open inbound)
just bench-box-ssh 'cd turmeric && timeout 720 bash tests/run.sh'
just bench-box-pull benchmarks/output/triangle   # rsync results back
just bench-box-down               # destroy everything
```

The audit plan's WP0 ([performance-audit-plan](../../docs/upcoming/performance-audit-plan.md))
names this box as the baseline machine: one instance type, one AMI line, one
toolchain, recorded in every table.

## What it creates

| Resource | Shape | Why |
| --- | --- | --- |
| VPC | 1 AZ, one public /24, **no NAT gateway** | a NAT is ~$32/month idle; the box has its own public IPv4 for outbound and needs nothing inbound |
| Security group | **no inbound rules**, all outbound | access is SSM Session Manager, so no port 22, no key pair, no bastion |
| IAM role | `AmazonSSMManagedInstanceCore` only | the minimum for Session Manager |
| EC2 instance | `m7i.xlarge` (4 vCPU, 16 GiB) by default, Ubuntu 24.04 (latest via SSM parameter), IMDSv2 required | the CI runners are 4-core/16 GiB boxes; the interpreter fixtures peak at ~3.5 GiB each (CLAUDE.md) |
| Root volume | 80 GiB gp3, encrypted, deleted with the instance | Debug + Release builds, ccache, ASan quarantine, fixture outputs |
| Deadman timer | systemd timer stops the box 8 h after boot | a forgotten box costs its volume (cents a day), not its vCPUs |

Thirteen CloudFormation resources, no Lambdas, no custom resources; a deploy
is about three minutes and a destroy about two.

## Cost

Approximate on-demand us-east-1 prices; check the current ones before you
lean on them.

| `BENCH_INSTANCE_TYPE` | `BENCH_ARCH` | vCPU / GiB | per hour | 3-hour session |
| --- | --- | --- | --- | --- |
| `m7i.xlarge` (default) | `x86_64` | 4 / 16 | ~$0.20 | ~$0.60 |
| `c7i.xlarge` | `x86_64` | 4 / 8 | ~$0.18 | ~$0.54 |
| `c7i.2xlarge` | `x86_64` | 8 / 16 | ~$0.36 | ~$1.08 |
| `m7g.xlarge` | `arm64` | 4 / 16 | ~$0.16 | ~$0.49 |
| `c7g.2xlarge` | `arm64` | 8 / 16 | ~$0.29 | ~$0.87 |

Plus the 80 GiB gp3 volume (~$0.009/hour, ~$6.40/month if you leave a
stopped box around) and the public IPv4 (~$0.005/hour). Avoid the burstable
`t3`/`t4g` families for anything you will time: CPU credits make the numbers
meaningless. Spot would halve the instance price but can be reclaimed
mid-benchmark; it is not wired in.

## Prerequisites (once)

- AWS CLI v2 with credentials (`aws configure` or `AWS_PROFILE`), and the
  [Session Manager plugin](https://docs.aws.amazon.com/systems-manager/latest/userguide/session-manager-working-with-install-plugin.html)
  for `ssh.sh` and `pull.sh`.
- Node.js 20+ (`npm ci` runs on first `up`).
- The account/region bootstrapped for CDK: `cd infra/bench-box && npx cdk bootstrap`.
- Your IAM user needs CloudFormation, EC2, IAM (to create the instance role),
  SSM `StartSession`, and `ssm:GetParameter` on the Canonical AMI parameter.

## Knobs

Environment variables read by `scripts/up.sh` (they become CDK context; the
defaults are in `cdk.json`):

| Variable | Default | Meaning |
| --- | --- | --- |
| `BENCH_INSTANCE_TYPE` | `m7i.xlarge` | any EC2 type; must match `BENCH_ARCH` |
| `BENCH_ARCH` | `x86_64` | `x86_64` or `arm64`; picks the Ubuntu AMI |
| `BENCH_DISK_GB` | `80` | root volume size |
| `BENCH_GIT_REF` | `main` | branch, tag or commit cloned and built at boot |
| `BENCH_MAX_UPTIME_HOURS` | `8` | deadman timer; `sudo systemctl stop bench-deadman.timer` on the box disarms it for one boot |
| `BENCH_SSH_PUBKEY_FILE` | `~/.ssh/id_ed25519.pub`, then `id_rsa.pub` | public key appended to `ubuntu`'s `authorized_keys` |

Re-running `up.sh` with different values updates the stack in place; a
changed bootstrap script or AMI replaces the instance (it is disposable).

## What the box has when `BOOTSTRAP_DONE` appears

`~/turmeric` at the requested ref with `build/` (Debug: ASan+UBSan, what
`tests/run.sh` wants) and `build-release/` (Release, `TUR_JIT=ON`, what every
timing number wants), both through ccache. gcc, clang, lld, cmake, ninja,
valgrind, `perf` (`kernel.perf_event_paranoid=-1` so no sudo), hyperfine,
gdb, python3, jq. The bootstrap log is `/var/log/bench-bootstrap.log`;
`BOOTSTRAP_FAILED` in the home directory means read it.

Not installed: the cross-language toolchains for `performance-comparison/`
(Rust, GHC, Racket, a JVM, Python is there). Add them to
`bootstrap/user-data.sh` if a sweep needs them; see
[docs/notes/benchmark-runtime-setup.md](../../docs/notes/benchmark-runtime-setup.md)
for what each column wants.

## Running things

```sh
# follow the bootstrap
just bench-box-ssh tail -f /var/log/bench-bootstrap.log

# the suite (12-minute timeout, per CLAUDE.md), detached so a dropped session does not kill it
just bench-box-ssh 'cd turmeric && nohup timeout 720 bash tests/run.sh > run.log 2>&1 &'
just bench-box-ssh 'tail -3 turmeric/run.log'

# the engine triangle on the Release build
just bench-box-ssh 'cd turmeric && TUR=./build-release/tur bash benchmarks/run-triangle.sh'

# a perf profile, demangled
just bench-box-ssh 'cd turmeric && perf record -g ./some-bin && perf script | ./build-release/tur demangle > prof.txt'
just bench-box-pull . ./box-tree          # or a subdirectory
```

Run one thing at a time on the box when you are timing it. CLAUDE.md's
overlap trap (a concurrent build or suite producing fake failures and fake
regressions) applies here exactly as on a laptop; the point of the box is
that nothing else is running on it.

## Tearing down

`just bench-box-down` destroys the stack: instance, volume, VPC, role.
Pull results first; the volume goes with it. `just bench-box-status` lists
any bench box in the account, including one another checkout deployed, so
you can confirm nothing is left running (or stopped and still holding a
volume).

## Files

```
infra/bench-box/
  bin/bench-box.ts          CDK app: reads context, builds the one stack
  lib/bench-box-stack.ts    VPC, SG, role, instance, outputs
  bootstrap/user-data.sh    first boot: deadman timer, apt, clone, builds
  scripts/up.sh             deploy + wait for SSM
  scripts/ssh.sh            shell or one command over SSM
  scripts/pull.sh           rsync a results directory back
  scripts/down.sh           destroy
  scripts/status.sh         what is running in the account
  cdk.json                  defaults for every knob
```

`cdk-outputs.json` (written by `up.sh`, read by the others) and
`cdk.context.json` are gitignored and local to the checkout that deployed.
