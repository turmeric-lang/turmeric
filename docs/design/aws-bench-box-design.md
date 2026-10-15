# Bench box -- a disposable EC2 instance for suites and benchmarks

**Status:** Reference -- not used by any current plan
**Last updated:** 2026-10-13

> Moved here from `infra/bench-box/README.md` on 2026-10-13, together with
> the AWS notes from the [performance-audit-plan](../archive/performance-audit-plan.md)
> (last section below), when the audit's baseline moved to the author's
> local machine. The CDK app itself is unchanged and still lives in
> `infra/bench-box/`; every relative path below (`scripts/`, `iam/`,
> `bootstrap/`, `cdk.json`) is relative to that directory.

A CDK app that spins up one EC2 instance with the Turmeric toolchain
installed and the repo built, so a test-suite run or a benchmark sweep
happens on a quiet, known machine instead of a laptop. `up` before testing,
`down` after; nothing here is meant to live longer than a session.

```sh
tur run bench-box-up                 # or: bash infra/bench-box/scripts/up.sh
tur run bench-box-ssh                # a shell on the box (SSM; nothing open inbound)
tur run bench-box-ssh 'cd turmeric && timeout 720 bash tests/run.sh'
tur run bench-box-pull benchmarks/output/triangle   # rsync results back
tur run bench-box-down               # destroy everything
```

The performance audit named this box as its baseline machine from
2026-10-11 to 2026-10-13; see the last section for what the plan said and
why it no longer does.

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
- The account set up once by `scripts/bootstrap-account.sh`, run by a human
  with admin credentials. The next section says what it does and why; the
  short version is that the CDK app deploys through roles it creates, so the
  credentials that run `up`/`down` day to day never need EC2 or IAM
  permissions of their own.

## Credentials and permissions

**Recommendation in one paragraph.** Never give anything a key with EC2 or
IAM permissions. Bootstrap the account once, as yourself, with
`scripts/bootstrap-account.sh`: it installs a *scoped* CloudFormation
execution policy (`iam/cfn-exec-policy.json`) under a CDK qualifier of its
own, so CloudFormation, not the caller, holds the power to create instances,
and it can create them only in one region, only of allowlisted types, and
with an instance role that must carry a permissions boundary limiting it to
the SSM agent (`iam/instance-boundary.json`). The thing that runs
`up`/`ssh`/`pull`/`down` then needs only `iam/operator-policy.json`: assume
the three CDK roles, describe instances, open SSM sessions to boxes tagged
`Purpose=bench-box`, stop/start those boxes. For a human, attach that as an
IAM Identity Center permission set and use short-lived SSO credentials. For a
Claude Code cloud environment or a CI job, which cannot do SSO, create the
dedicated user `turmeric-bench-operator` with that one policy and one access
key (`--create-user`), store the key in the consumer's secret store, and put
a tagged monthly budget behind it (`--budget you@example.com`).

### What each policy lets happen

| Policy | Attached to | Can | Cannot |
| --- | --- | --- | --- |
| `TurmericBenchBoxOperator` (`iam/operator-policy.json`) | the human's permission set, or the `turmeric-bench-operator` user | assume `cdk-benchbox-{deploy,file-publishing,lookup}-role` (which is how `cdk deploy`/`destroy` work); `ec2:Describe*`; `ssm:StartSession` to instances tagged `Purpose=bench-box` with the shell, SSH and port-forward documents; stop/start/reboot those instances; end its own sessions | call `ec2:RunInstances`, `TerminateInstances`, or any `iam:*` directly; open a session to an untagged instance; act outside `${REGION}` (except STS, which is global) |
| `TurmericBenchBoxCfnExec` (`iam/cfn-exec-policy.json`) | the CDK CloudFormation execution role (`cdk-benchbox-cfn-exec-role-*`) | `ec2:*` in `${REGION}`; read the Canonical AMI SSM parameters; create/delete roles and instance profiles named `TurmericBenchBox-*`, and only with the boundary attached; pass such a role to EC2 | run an instance type outside the allowlist (`Deny` on `ec2:RunInstances`, edit the list in the file); create NAT gateways, VPNs, transit gateways, fleets, reservations, dedicated hosts; create or modify any role not named `TurmericBenchBox-*`; remove or swap a boundary; anything outside EC2/IAM/that one SSM path |
| `TurmericBenchBoxInstanceBoundary` (`iam/instance-boundary.json`) | every role the stack creates (the stack applies it via `permissionsBoundaryName`) | the SSM agent's calls (`ssm:UpdateInstanceInformation`, `ssmmessages:*Channel`, `ec2messages:*`) | anything else, whatever policy a future stack change attaches |

The blast radius of a leaked operator key is therefore: deploy or destroy
*this* stack's shape in one region (an allowlisted EC2 instance with an
SSM-only role), and shell into a box that already has the tag. It cannot
read S3, touch other stacks, mint credentials, or run a GPU fleet. The
remaining cost exposure is bounded by the instance-type allowlist times the
budget alert.

### Why a separate CDK qualifier

Plain `cdk bootstrap` creates a `CDKToolkit` stack whose execution role has
`AdministratorAccess`, and every CDK app in the account shares it. Changing
its policy to scope this app would scope every other app too. The bootstrap
script uses `--qualifier benchbox --toolkit-stack-name CDKToolkit-benchbox`,
so the bench box gets its own roles and bucket (`cdk-benchbox-*`) and leaves
an existing toolkit alone; `cdk.json` sets
`@aws-cdk/core:bootstrapQualifier` to match. Nothing else in the account is
affected.

### Setting it up

```sh
cd infra/bench-box
AWS_PROFILE=admin AWS_REGION=us-east-1 scripts/bootstrap-account.sh \
    --create-user --budget you@example.com --budget-usd 25
```

Pick the one region the box may live in; it is baked into every policy. The
script is idempotent. `--create-user` prints the access key **once**, to the
terminal; store it directly into the consumer and nowhere else. For a Claude
Code cloud environment that means the environment's settings (the cloud
environment menu in the session's title bar, then Edit), as the three
variables `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_DEFAULT_REGION`;
a new session picks them up. The environment's network access also has to
reach AWS: `*.amazonaws.com` under Allowed domains, or a broader level
(`sts`, `cloudformation`, `ec2`, `ssm`, `ssmmessages`, `iam` and the
`cdk-benchbox-assets-*` S3 bucket are the endpoints the scripts touch).

The budget filters on the `Purpose=bench-box` cost-allocation tag, which has
to be activated once in the Billing console (Cost allocation tags) and
starts filtering about a day later; until then it reads zero.

### Hygiene

- **Rotate the operator key** every 90 days or when a session that held it
  ends badly: `aws iam create-access-key`, update the consumer, then
  `aws iam delete-access-key` on the old id. There is never a reason for the
  user to hold two keys for longer than that swap.
- **Kill switch:** `aws iam attach-user-policy --user-name
  turmeric-bench-operator --policy-arn arn:aws:iam::aws:policy/AWSDenyAll`
  stops the key cold without deleting anything; detach to restore. The
  budget's 100% alert is the cue.
- **Audit:** every deploy, session and stop/start lands in CloudTrail under
  the operator principal; `scripts/status.sh` answers the cheaper question
  of whether anything is running right now.
- **Do not widen the operator policy to make a script work.** If `up.sh`
  fails with `AccessDenied`, the missing permission belongs either in the
  CloudFormation execution policy (a resource the stack creates) or is a sign
  that a role name or tag drifted from the patterns above. Fix the pattern.

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
[docs/notes/benchmark-runtime-setup.md](../notes/benchmark-runtime-setup.md)
for what each column wants.

## Running things

```sh
# follow the bootstrap
tur run bench-box-ssh tail -f /var/log/bench-bootstrap.log

# the suite (12-minute timeout, per CLAUDE.md), detached so a dropped session does not kill it
tur run bench-box-ssh 'cd turmeric && nohup timeout 720 bash tests/run.sh > run.log 2>&1 &'
tur run bench-box-ssh 'tail -3 turmeric/run.log'

# the engine triangle on the Release build
tur run bench-box-ssh 'cd turmeric && TUR=./build-release/tur bash benchmarks/run-triangle.sh'

# a perf profile, demangled
tur run bench-box-ssh 'cd turmeric && perf record -g ./some-bin && perf script | ./build-release/tur demangle > prof.txt'
tur run bench-box-pull . ./box-tree          # or a subdirectory
```

Run one thing at a time on the box when you are timing it. CLAUDE.md's
overlap trap (a concurrent build or suite producing fake failures and fake
regressions) applies here exactly as on a laptop; the point of the box is
that nothing else is running on it.

## Tearing down

`tur run bench-box-down` destroys the stack: instance, volume, VPC, role.
Pull results first; the volume goes with it. `tur run bench-box-status` lists
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
  scripts/bootstrap-account.sh   once, as admin: policies, scoped CDK bootstrap, operator user, budget
  iam/operator-policy.json       what up/ssh/pull/down need (and nothing more)
  iam/cfn-exec-policy.json       what CloudFormation may create for this stack
  iam/instance-boundary.json     ceiling on any role the stack creates
  cdk.json                  defaults for every knob
```

`cdk-outputs.json` (written by `up.sh`, read by the others) and
`cdk.context.json` are gitignored and local to the checkout that deployed.

## As the performance audit's baseline (2026-10-11 to 2026-10-13)

The AWS notes that were in
[performance-audit-plan](../archive/performance-audit-plan.md) before its
baseline moved to the author's local Intel NUC8i7HVK under WSL2 (that plan's
section 8, decision 1). Kept here as the plan stated them.

- **Decision 1 (2026-10-11): baseline instance type `m7i.xlarge`** (x86-64,
  4 vCPU, 16 GiB), the bench box default. It matched the CI runners' shape so
  suite timings stayed comparable, and `perf` and callgrind are mature there.
  The nightly's arm64 could be a column; it was not the reference.
- **WP0's machine bullet.** The box is `infra/bench-box/`
  (`tur run bench-box-up` / `bench-box-down`): a disposable EC2 instance,
  `m7i.xlarge` on Ubuntu 24.04 by default, with the toolchain, `perf`,
  valgrind and hyperfine installed and Debug + Release builds made at boot.
  Record its instance type, AMI, cc version and glibc in the artifact README;
  every later table carries that header or it is not a result. An `arm64`
  column (`BENCH_ARCH=arm64`, e.g. `m7g.xlarge`) is one env var away when WP4
  or WP6 wants one.
- **Decision 4 (2026-10-11), the AWS half.** CI performance tracking stays on
  hosted runners: there is no budget for a standing dedicated runner, and the
  bench box is for short-lived project work like the audit, not a standing
  runner. (The hosted-runner decision itself still stands in the plan.)
- **The one outstanding author action** the plan listed before WP0 could
  start: the AWS account bootstrap (`scripts/bootstrap-account.sh`, run once
  with admin credentials, choosing the one region). The audit no longer
  needs it.

What the local machine gives up relative to this box, should either matter
later: a Claude Code cloud session could drive the bench box over SSM with
an operator key, but cannot reach a desktop; the box matched the CI
runners' 4 vCPU / 16 GiB shape; and an arm64 column was one env var away.
