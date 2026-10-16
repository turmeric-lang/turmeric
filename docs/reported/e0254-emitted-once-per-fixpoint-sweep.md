# TUR-E0254 is printed once per effect-inference sweep

**Severity:** low (diagnostic noise). One mistake is reported several times.

## Repro

```
$ tur check tests/fixtures/errors/effect-row-occurs/input.tur 2>&1 | grep -c TUR-E0254
3
```

A three-deep call chain prints it four times.

## Root cause

`src/passes/effect_check.c:collect_effects_in_expr` emits TUR-E0254 while
walking a body. That walk runs once per sweep of the inference fixpoint, so
the error is printed on every sweep until the fixpoint settles.

## Fix directions

- Record the condition during the fixpoint and emit it once, during the
  validation phase that follows; or
- dedupe by call node.
