# Security policy

## Reporting a vulnerability

**Report privately, through GitHub:**
<https://github.com/turmeric-lang/turmeric/security/advisories/new>

That form is private to the maintainers until an advisory is published. Please
use it rather than a public issue for anything that looks exploitable.

Include, as far as you have it:

- what you pointed Turmeric at, and which subcommand (`tur build`, `tur check`,
  `tur run --list`, `tur repl`, the LSP, `tvm`, the web playground);
- a minimal reproducer -- a `.tur` file, a `build.tur`, a byte string, a request;
- what happened versus what you expected;
- the `tur --version` output and your platform.

Turmeric is maintained by one person. The commitment is:

| | |
| --- | --- |
| Acknowledgement | within 7 days |
| Assessment, with a severity and a plan | within 14 days |
| Fix or a public advisory with a workaround | negotiated in the thread |

No fix deadline is promised, because one maintainer cannot honestly promise
one. What is promised is that you will not be left without an answer, and that
you will be credited in the advisory unless you ask otherwise.

There is no bug bounty.

## What is in scope

Turmeric makes different promises to different inputs, and a report is graded
against the promise for the input it uses. Those promises are written down in
the [Security Guide](docs/guides/security-guide.md) -- read its table before
filing, because it is the difference between a bug and a non-bug.

In scope, briefly:

- **Memory corruption in a compiled program's stdlib readers** given malformed
  input -- the serial/continuation decoder, image files, JSON, `httpd` requests.
- **Capability escapes from a sandboxed interpreter** (`Env/new-sandboxed`,
  the compile-time macro environment).
- **Code or shell execution from a project tree you have only opened** --
  `tur check`, `tur run --list`, and the language server.
- **Supply chain:** the installer, release assets, `tur fetch`, `tur.lock`
  verification, the GitHub Actions workflows.
- **Injection in the compiler driver** -- anything that reaches a shell.
- **The web playground** at turmeric-lang.com, beyond the browser's own sandbox.

## What is not in scope

- **`tur build` on a project tree you do not trust.** Building a Turmeric
  project runs that project's code: inline C, compile-time macros, its
  Justfile, its `:link-flags`. This is the same position Cargo takes on
  `build.rs` and `.cargo/config.toml`, and `make` on a Makefile. See the guide.
- A program doing to its own machine what its own author told it to do.
- Denial of service against your own build (a macro that loops, a huge file).
- Anything requiring write access to this repository already.
- The Godot bindings (a separate repository).

## Known gaps

Several promises in the guide are **not yet kept**, and the guide marks each
one where it stands, with the defect named. Reporting one of those again is
welcome but not news; the guide is the current state of the audit.
