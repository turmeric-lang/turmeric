## Turmeric Godot Binding -- Web Export Plan

> **Status:** On hold, not started. The plan sat in `docs/upcoming/hold/`
> until the 2026-10-16 flatten. Still blocked on
> [`wasm-spices-plan.md`](wasm-spices-plan.md), which is only partly built:
> its `:web` manifest block and raylib's browser frame loop exist, but not the
> HTML app shell (W1). Checked 2026-10-18 against turmeric-godot `main`: CI
> builds desktop targets only, and no `.gdextension` lists a `web.*` library.
> **Last Updated:** 2026-06-26
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md).
> Split out from
> [godot-binding-shipping-breadth-plan.md](godot-binding-shipping-breadth-plan.md)
> (originally Phase T5.D).

---

## Why this exists

Expanding the Godot binding's export matrix to the web so Turmeric
Godot games can ship in a browser.

Not on the critical path for v1.x adoption; lands when a real driving
demand exists.

---

## Scope (~depends on wasm-spices)

Blocked on
[`wasm-spices-plan.md`](wasm-spices-plan.md) landing first. Godot's
web export uses its own Emscripten profile (`-sMODULARIZE`, specific
sysroot, `--js-library` hooks); ours has to agree.

Once unblocked:

- libturi compiles to WebAssembly already (turi web REPL exists).
- AOT path: also doable, since Godot's web export accepts WASM modules
  loaded via the engine's own loader. Per-script `.wasm` artifacts.
- Same "build on desktop, ship the cache" pattern as Android.
- The web REPL's existing wasm_glue.c plumbing should adapt.

The hard part isn't building turmeric WASM -- it's getting the
turmeric WASM to *interoperate cleanly* with Godot's WASM. Both run
in the same browser process but in different `Module` instances by
default.

---

## Risks

- **Wasm interop is research-heavy.** The "modules in the same
  browser process need to talk to each other" question doesn't have a
  clean off-the-shelf answer for arbitrary WASM-to-WASM. Plan accepts
  this as a research milestone, not a fixed-cost engineering task.

---

## Success Criteria

- A web-exported demo runs in Chrome/Firefox without WebAssembly
  module loading errors.

---

## Out of Scope

- Console exports. See parent plan.
- Non-Godot web targets -- general WASM support is tracked under
  `wasm-spices-plan.md`.
