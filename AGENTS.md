# Turmeric 2 -- Agent Guide

This repository already carries a detailed root guide in `CLAUDE.md`. Treat
that file as the authoritative project instruction set and apply it in full.

Critical rules called out here so agents that only load `AGENTS.md` still get
the highest-risk constraints:

- Read CLI arguments only via `*args*` or helpers from `stdlib/args.tur`.
  Do not reintroduce `parse-first-arg`, `parse-arg`, or raw inline-C access
  to `g_tur_args`.
- Use `just`, not `make`. Common targets are `just build`, `just test`,
  `just release`, `just docs`, `just wasm`, and `just web-dev`.
- Spice implementations live in the sibling repository `../turmeric-spices`.
  Never create or scaffold `./spices/` in this repo.
- Use `;;;` docstrings for documented Turmeric definitions and keep fixture
  files ASCII-only.
- At most one PR open against `turmeric-lang/mir` at a time, and no scratch
  branches pushed to it: its CI runs on every push and several PRs in a row
  starve all other CI for hours. Batch MIR fixes into one PR (see `CLAUDE.md`).
- Prefer sweet-expression style in new `.tur.sweet` files, following the
  formatting and indentation rules documented in `CLAUDE.md`.

If an instruction here conflicts with `CLAUDE.md`, follow `CLAUDE.md`.
