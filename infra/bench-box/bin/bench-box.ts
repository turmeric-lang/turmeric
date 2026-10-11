#!/usr/bin/env node
import * as cdk from 'aws-cdk-lib';
import { BenchBoxStack } from '../lib/bench-box-stack';

const app = new cdk.App();

// Every knob is CDK context so `scripts/up.sh` can override it per run with
// `-c key=value`; the defaults live in cdk.json.  Read them here, once, so
// the stack gets plain typed props and a bad value fails before synth.
function ctxString(key: string): string {
  const v = app.node.tryGetContext(key);
  if (typeof v !== 'string' || v.length === 0) throw new Error(`context "${key}" must be a non-empty string`);
  return v;
}
function ctxNumber(key: string): number {
  const v = Number(app.node.tryGetContext(key));
  if (!Number.isFinite(v) || v <= 0) throw new Error(`context "${key}" must be a positive number`);
  return v;
}
const arch = ctxString('arch');
if (arch !== 'x86_64' && arch !== 'arm64') throw new Error(`context "arch" must be x86_64 or arm64, got ${arch}`);

new BenchBoxStack(app, 'TurmericBenchBox', {
  env: {
    account: process.env.CDK_DEFAULT_ACCOUNT,
    region: process.env.CDK_DEFAULT_REGION,
  },
  description: 'Disposable EC2 box for Turmeric test suites and benchmarks (infra/bench-box). cdk destroy when done.',
  instanceType: ctxString('instanceType'),
  arch,
  diskGb: ctxNumber('diskGb'),
  gitRef: ctxString('gitRef'),
  repoUrl: ctxString('repoUrl'),
  maxUptimeHours: ctxNumber('maxUptimeHours'),
  // Optional: scripts/up.sh passes the caller's public key so plain `ssh`
  // (through the SSM proxy) works.  Without it, `aws ssm start-session` still does.
  sshPublicKey: app.node.tryGetContext('sshPublicKey'),
  // Name of the IAM permissions boundary every role this stack creates must
  // carry (iam/instance-boundary.json, created by scripts/bootstrap-account.sh).
  // Empty disables it, for an account bootstrapped the plain way.
  permissionsBoundaryName: app.node.tryGetContext('permissionsBoundaryName') || undefined,
});

app.synth();
