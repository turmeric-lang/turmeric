# The Cloudflare guide's service hands `fiber-new` capturing lambdas

**Severity: low (a guide program that never worked).** Found 2026-11-07 while
typing `fiber-new`'s body as `(c-fn [] nil)`
([stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md) S1).

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
