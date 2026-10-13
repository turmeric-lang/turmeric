# Bench box -- a disposable EC2 instance for suites and benchmarks

A CDK app that spins up one EC2 instance with the Turmeric toolchain
installed and the repo built (`just bench-box-up` / `bench-box-down`).

The full notes -- what the stack creates, cost, the one-time account
bootstrap, credentials and permissions, knobs, and how to run things on the
box -- live in
[docs/design/aws-bench-box-design.md](../../docs/design/aws-bench-box-design.md).
No current plan uses the box; the performance audit's baseline is a local
machine ([performance-audit-plan](../../docs/upcoming/performance-audit-plan.md),
section 8).
