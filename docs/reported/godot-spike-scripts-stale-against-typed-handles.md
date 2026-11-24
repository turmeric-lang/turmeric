# turmeric-godot: spike scripts no longer load, and nothing runs them

**Summary:** Three of the `examples/spike` smoke scripts fail to elaborate
against the typed-handle prelude they are meant to exercise, and a fourth
driver prints a failure and then "all assertions passed". The drivers are run
by hand only -- CI builds the extension and never launches Godot -- so all of
this went unnoticed.

**Severity:** Low. Test-suite rot, not a product fault, with one possible
exception (the `bool` export default below).

**Repro** (Linux x86-64, Godot 4.3, turmeric-godot `10956a8`; identical with
the shim built against turmeric `v0.44.2` and against `main`):

```sh
cd examples/spike
godot --headless --path . --script scripts/<name>.gd
```

| Driver | Result |
| --- | --- |
| `aggregate_export` | TUR-E0001: `node/vec2-x` arg 1: expected `Vec2Handle`, got `int` |
| `classdb_call` | TUR-E0001: `node/set-position` arg 2: expected `Vec2Handle`, got `int`; past that, TUR-E0006: operator lookup failed for `=`, first arg type `NodeHandle` |
| `generated_facade` | TUR-E0001: `node/set-position` arg 2: expected `Vec2Handle`, got `int`; past that, `node2d/set-skew` arg 1: expected `Node2DHandle`, got `int` |
| `exports` | `FAIL: default alive: expected true, got false`, then `all assertions passed` |

## Root cause

The scripts predate the `defopaque` handles: they build values with the raw
natives (`godot-vec2`, `node/self` as `:int`) and pass them to prelude
wrappers that now take `Vec2Handle`, `Node2DHandle` and so on. Two more of the
same kind were fixed in passing (`packed_array_roundtrip.tur`,
`aggregates.tur`) by switching to `node/vec2`.

`exports` is different and may be real: `(godot-export "alive" "bool" 1)`
reads back `false` before anything sets it. Either the `1` default is not a
`:bool` to the export native any more, or the default is dropped. Not
investigated. The driver's last line is unconditional, which is why it also
claims success.

## Fix directions

- Port the three scripts to the typed constructors and up-coercions
  (`handle_hierarchy.tur` shows the idiom).
- Find out why the `bool` default is lost, and make `exports.gd` exit
  non-zero and skip the success line when `fail_count` is not 0.
- Give CI a leg that downloads a headless Godot and runs the drivers, so the
  next one fails a build instead of a reader.
