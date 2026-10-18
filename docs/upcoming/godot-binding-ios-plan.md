## Turmeric Godot Binding -- iOS Support Plan

> **Status:** On hold, not started. The plan sat in `docs/upcoming/hold/`
> until the 2026-10-16 flatten; it lands when a real demand to ship iOS builds
> appears, and needs the distribution signing of T5.A in
> [godot-binding-shipping-breadth-plan.md](godot-binding-shipping-breadth-plan.md)
> to work first. Checked 2026-10-18 against turmeric-godot `main`: CI builds
> desktop targets only, and no `.gdextension` lists an `ios.*` library.
> **Last Updated:** 2026-06-26
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md).
> Split out from
> [godot-binding-shipping-breadth-plan.md](godot-binding-shipping-breadth-plan.md)
> (originally Phase T5.C).

---

## Why this exists

Expanding the Godot binding's export matrix to iOS so Turmeric Godot
games can ship to the App Store / TestFlight.

Not on the critical path for v1.x adoption; lands when a real driving
demand exists.

---

## Scope (~3-4 weeks)

Same shape as Android with extra platform constraints:

- iOS does not permit dlopen of arbitrary `.dylib` files in App Store
  builds. AOT-as-shipping-shared-libs is **not viable** on iOS.
  Practical answer: ship interpreter mode only; OR statically link
  every AOT script into the app binary at export time.
- The static-link-at-export path is non-trivial -- Godot's iOS export
  pipeline produces an Xcode project; we'd need to hook into it to add
  per-script `.o` files and a manifest the runtime statically resolves
  against.
- Recommended: ship interpreter-only on iOS for v1.x; flag the static-
  AOT path as a research follow-up.

iOS is the platform that makes the macOS distribution code-signing
story (see parent plan, Phase T5.A) mandatory rather than optional.

---

## Risks

- **Platform certificate / signing churn.** Apple changes the
  notarization toolchain every couple of releases; iOS App Store
  policies move faster. Treat this as ongoing maintenance, not a
  one-shot landing.

---

## Success Criteria

- An interpreter-mode Turmeric script runs on iOS via TestFlight.

---

## Out of Scope

- Console exports. See parent plan.
- Static-AOT-at-export-time pipeline -- tracked as a research
  follow-up, not part of the initial iOS support landing.
