# The Cloudflare guide's service hands `fiber-new` capturing lambdas

**RESOLVED 2026-11-07.** The guide's service starts captureless fiber bodies
and passes each one its fd in a fiber-local slot, and
`tests/check-cloudflare-guide-service.sh` (ctest
`tur_cloudflare_guide_service`) builds both copies and has the s-expression
one answer three concurrent connections. See "Resolution" at the end.

**Severity: low (a guide program that never worked).** Found 2026-11-07 while
typing `fiber-new`'s body as `(c-fn [] nil)`
([stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md) S1).

`docs/guides/cloudflare-deployment-guide.md` ("Approach A", `service.tur`, in
both the s-expression and the sweet-exp copy) starts each connection's fiber as

```turmeric
(fiber-new (fn [] : nil (handle-client client-fd)) 0)
```

and the accepter as `(fiber-new (fn [] : nil (accept-loop fd sched)) 0)`.
Both lambdas capture a local. `fiber-new` runs its body as a bare C function
pointer, so while its parameter was `ptr<void>` the closure RECORD was passed
and the fiber jumped to it as code. Since `fiber-new` takes `(c-fn [] nil)`
the program is refused at the call instead:

```
error: TUR-E0295: argument 1 of 'fiber-new' is a capturing closure, but the
parameter is a C function pointer (c-fn) and cannot carry an environment;
captures: client-fd.
```

No test compiles the guide's snippets, which is how it went unseen. The same
program also passes `fiber-new`'s `FiberHandle` to `scheduler-spawn`, whose
`f` is `ptr<void>`.

## Fix directions

- Rewrite the service so a fiber's data does not ride a capture: a
  captureless body that reads its fd from the first `fiber-resume` argument
  (`fiber-arg`, once the body can name its own handle), or a fiber-local set
  before `scheduler-spawn`.
- Or give `fiber-new` a closure-taking sibling (a fat-to-C trampoline that
  carries the env through the FiberBlock), which is S1's other design option.
- Either way, run the guide's program in a test so it cannot rot again.

## Resolution (2026-11-07)

Fix direction 1, with a fiber-local rather than `fiber-arg`.

- **The service.** `spawn-on-fd` makes a fiber from a captureless body, puts
  the fd in fiber-local slot `fd-slot` (`fiber-local-set!`) and spawns it on
  `(scheduler-current)`; `client-fiber` and `accept-fiber` read it back with
  `fiber-local-current-get`. Both copies in the guide, and a "Fiber-body
  note" under them saying why.
- **Three more ways it had rotted**, all of which the test now catches:
  `int->str` was never loaded (it is `stdlib/str-build.tur`'s; the service
  loads `stdlib/str.tur`, which also gives it `str->int` for `$PORT`, read
  with `env/get` instead of the raw-int `env/get-raw` and a
  `cstr->parse-int` that takes an `int`); the socket calls are `#fx{Unsafe
  Net}` and sat outside any `unsafe`; and `scheduler-spawn` took `ptr<void>`
  where `fiber.tur` hands out a `FiberHandle`. `scheduler-spawn` takes a
  `FiberHandle` now (`stdlib/scheduler.tur` loads `stdlib/fiber.tur`); no
  other in-tree caller existed.
- **Found on the way, and fixed: the C-unit prune dropped `main`.** Any
  program loading `stdlib/async_socket.tur` built only on the full-unit retry
  (TUR-W0071, "undefined reference to `main`"). `async-socket-accept` and
  `async-socket-recv` open one `{` in each arm of an `#ifdef _WIN32 / #else`;
  `split_chunks` (`src/compiler/jit_prune.c`) counted both, so the rest of the
  unit read as one unfinished definition that nothing reached. Only the first
  arm of a conditional moves the brace and paren depth now: `#elif` / `#else`
  put back the depth the `#if` saw, `#endif` takes what the first arm left.
  Pinned by `tests/fixtures/prune-pp-conditional-arm-braces` (its own inline C
  with two- and three-arm conditionals, and `async_socket.tur` loaded), which
  `tests/run.sh` fails on TUR-W0071; it does on the old prune.
- **Also found: `str-eq?`'s inline C** compared two pointers to distinct
  anonymous struct types, a cc warning in every program that loads
  `stdlib/str.tur`. It compares them as `const void *`.
- **Filed, not fixed:** the scheduler never frees a fiber that finished, so
  the service keeps a 1 MiB stack per connection --
  [scheduler-finished-fibers-never-freed](../reported/scheduler-finished-fibers-never-freed.md).
