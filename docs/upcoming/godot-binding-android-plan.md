## Turmeric Godot Binding -- Android Support Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-26
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md).
> Split out from
> [godot-binding-shipping-breadth-plan.md](godot-binding-shipping-breadth-plan.md)
> (originally Phase T5.B).

---

## Why this exists

The Tier 1-4 Godot binding work makes Turmeric a good language for
writing desktop Godot games. This plan covers expanding the export
matrix to Android so those games can ship to mobile players.

Not on the critical path for v1.x adoption; lands when a real driving
demand exists.

---

## Scope (~3-4 weeks)

Godot supports Android via GDExtension; the toolchain story is the
work.

- NDK build target in SConstruct (`scons platform=android arch=arm64
  target=template_debug`).
- libturi cross-compile for `aarch64-linux-android` and
  `armv7-linux-androideabi`.
- AOT path: invokes `tur build --shared` for `aarch64-linux-android`,
  requires the user's host machine has the right cross-compiler --
  practical limitation. Document that AOT-on-Android is "build on
  desktop, ship the cache" rather than "build-on-device".
- Interpreter mode is the lower-friction path on Android: libturi is
  the only binary that needs to ship, and it stays the same .so as on
  Linux except for the ABI.
- Test on a Pixel-or-similar via Godot's existing Android export
  pipeline.

---

## Success Criteria

- An Android-exported demo runs on a stock Android device with no
  developer-mode entitlements.

---

## Out of Scope

- Console exports. See parent plan.
- Per-platform export presets unrelated to the language binding.
