# A spice suite that does not link libturi runs without LeakSanitizer

**Severity:** low-medium (test coverage gap, silent). A sanitized Debug `tur`
adds `-fsanitize=address,undefined` to a program it builds only when that
program links `libturi`. Spices CI runs every spice with a Debug `tur` from
`main`, and that looks like "every spice suite is leak-checked". In fact only
the suites that pull in libturi are, for example through `stdlib/reactor.tur`.
A leak in any other suite passes CI with no diagnostic.

Found 2026-10-10 while implementing NG-C of
[nng-async-plan](../upcoming/spices/nng-async-plan.md).

## Repro

In turmeric-spices, on a branch that has `spices/nng/src/nng/ctx.tur`
(NG-C), delete the `nng_msg_free(m);` that follows `out[len] = '\0';` in
`ctx-recv-str`. Then:

```sh
cd spices/nng
TUR=<turmeric>/build/tur            # a Debug (ASan) build, as spices CI uses
mkdir -p tests/ctx-only && cp tests/nng/ctx_test.tur tests/ctx-only/
"$TUR" test tests/ctx-only          # 10/10 ok, exit 0 -- the leak is not seen
TUR_CC_FLAGS="-fsanitize=address,undefined" "$TUR" test tests/ctx-only
                                    # LeakSanitizer: 904 bytes in 10 allocations
```

`tests/nng/aio_test.tur`, which imports `reactor`, catches the same kind of
mutation with no flag: removing the untaken-message drop in
`c/nng/turnng_aio.c` fails it.

## Root cause

`resolve_autolink_flags` sets `needs_asan` only when the autolink line names
`-lturi` and an `nm` scan finds ASan symbols in that `libturi.a`
(`src/main.c:2989`). `link_command_run` adds the sanitizer flags only when
`needs_asan` is set (`src/main.c:3158`). That autodetect exists so a sanitized
libturi links at all; it was never meant to choose which programs get
checked. The archived reports
[catch-unwind-panic-payload-leaks](../archive/catch-unwind-panic-payload-leaks.md)
and [closure-drop-glue](../archive/closure-drop-glue.md) hit the same gap on
the fixture side. This report is about spices CI, where nothing documents it.

## Forcing it on today

`TUR_CC_FLAGS=-fsanitize=address,undefined` over the whole nng suite, on
2026-10-10, gave 6 of 8 suites clean. The two that failed fail on leaks in the
tests or another spice, not in nng:

- `pubsub_test`: the `send-str` Results its helpers drop in statement position
  ([carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)).
- `msgpack_test`: a `cons` cell from msgpack's derived `encode-mp`.

Other spices were not measured.

## Fix directions

1. **Spices CI sets `TUR_CC_FLAGS=-fsanitize=address,undefined`** for
   `tur test` (scripts/run-shard.sh). This is the cheapest option and covers
   every suite. It needs the existing test-side leaks fixed or marked first;
   measure every spice before turning it on.
2. **A `tur` knob for "sanitize what I build"**, for example on when `tur`
   itself is sanitized, or opted in by environment. This would cover the
   fixture side too. The cost is every emitted program paying ASan when only
   CI wants it.
3. **Per spice:** a `build.tur` key or a `tur-test-flags`-style directive that
   adds the cc flags. This is the smallest blast radius, but each spice
   remembers on its own, which is how a gap like this one opens.

## Update 2026-10-10: the httpd stack is now covered

tourist-on-stdlib-httpd-plan H2 moved turmeric-spices' `httpd` spice onto
`stdlib/httpd.tur`, which links libturi.  Every suite that imports it --
`httpd`, `tourist`, `tourist-session`, `tourist-ws`, `ws-server` -- now runs
under ASan/UBSan with leak detection on a Debug `tur`, with no flag.  The
switch found only test-side leaks, fixed on the same branch:

- the `http` client spice had no way to free a response at all; it gained
  `http-result-free` and `request-free`, and the httpd tests use them;
- `json`'s derived encoders leaked two cons cells per field per encode
  (fixed in `__json-obj-build` / `__json-arr-build`);
- handler strings, routes (`route-free` now drops the route's handler) and
  reader-thread arguments the tests never freed.

One suppression remains, scoped to `httpd/tests/json_codec_test.tur`'s JSON
handler: a derived `DecodeJson`'s `(ok struct)` box, the dictionary-dispatched
residue of [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md).
The other spices are unchanged by this; fix direction 1 still stands for them.
