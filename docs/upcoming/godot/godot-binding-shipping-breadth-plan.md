## Turmeric Godot Binding -- Shipping Breadth Plan (Tier 5)

> **Status:** On hold. The plan sat in `docs/upcoming/hold/` until the
> 2026-10-16 flatten; each phase lands on its own once a demand appears.
> Checked 2026-10-18 against turmeric-godot `main`:
>
> - **T5.A is partly built, and broken as written.**
>   `.github/workflows/build.yml` ad-hoc signs the macOS library on every
>   build, and signs it for distribution and submits it to `notarytool` when
>   the `APPLE_*` secrets are set. But both steps run
>   `find examples/spike/bin -name '*.dylib'`, and the macOS build writes
>   `libturmeric-godot.macos.<target>.framework/libturmeric-godot.macos.<target>`
>   (SConstruct), which has no `.dylib` suffix, so neither step finds a file
>   to sign. The certificate-setup docs, signing the per-script AOT libraries
>   at export time, and the Gatekeeper test are not done.
> - **T5.B-T5.D** are split out into the Android, iOS and web plans; none is
>   started.
> - **T5.E** is not started.
> - **T5.F** is partly there. Its precondition, the breakpoint debugger,
>   shipped 2026-06-28. Paused-frame locals now reach Godot's debugger as
>   strings (`turi_debug_frame_locals`), but `_debug_get_globals` returns an
>   empty dictionary and there is no expandable tree for nested values.
>
> **Last Updated:** 2026-06-25
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](../../archive/godot-language-binding-plan.md).
> Long-horizon; each phase is independent and can land on its own
> schedule once a real driving demand exists.

---

## Why this exists

The previous tiers (1-4) make Turmeric a better language for writing
desktop Godot games. This plan is about *where* those games can run --
expanding the export matrix, and a few language-side niceties that
matter once a game gets shipped to real players.

None of these phases is on the critical path for v1.x adoption.
They're listed here so the option exists in writing; nothing about the
Tier 1-4 plans depends on any of this landing.

---

## Phases

### Phase T5.A -- macOS distribution code signing (~2 weeks)

Already-mentioned in v1's risk list and partially landed: the CI
workflow has an ad-hoc-sign-always skeleton with distribution signing
gated on certificate secrets. To actually ship games on macOS:

- Document the certificate setup (Apple Developer Program enrollment,
  cert export, secret storage in GitHub Actions).
- Wire notarization (`xcrun notarytool`) into the release workflow.
- Verify that per-script AOT dylibs (the `.godot/turmeric-cache/.../
  lib<pkg>.so` files) are signable at export time -- they are *not*
  signed when generated, only the bundled GDExtension is.
- Test entitlements: does Gatekeeper accept a dynamically-loaded user
  script in a notarized app? Probably yes (Godot itself ships
  GDScript-via-bytecode similarly), but verify empirically with a
  notarized paddle-pong-tur export.

Without this, end-users can't run an exported macOS game without
explicit override clicks.

### Phase T5.B -- Android

Split out into its own plan:
[godot-binding-android-plan.md](godot-binding-android-plan.md).

### Phase T5.C -- iOS

Split out into its own plan:
[godot-binding-ios-plan.md](godot-binding-ios-plan.md). Note
that iOS is the platform that makes the export-time signing story
from T5.A mandatory rather than optional.

### Phase T5.D -- Web export

Split out into its own plan:
[godot-binding-web-plan.md](godot-binding-web-plan.md).

### Phase T5.E -- Cross-language inheritance (~2-3 weeks)

A GDScript class extending a Turmeric class (or vice versa). Real but
niche -- the v1 plan explicitly carved this out.

The technical asymmetry:

- **Turmeric extends GDScript** is easy (and already works): a `.tur`
  script's `(defgodot-script ... :extends SomeGDScriptClass ...)`
  treats the parent as a Godot class like any other.
- **GDScript extends Turmeric** is harder: GDScript needs to resolve
  the Turmeric class's method table, exported properties, signals,
  and per-instance state slots at class-load time, before any
  Turmeric script has run. Requires exposing the Turmeric script's
  metadata via the same `Script::get_method_list()` / property list
  channels GDScript reads from. Most of this exists for the inspector;
  extending it to class-bind time is the work.

Worth doing if a user actually asks; defer until then.

### Phase T5.F -- Visual debugger inspector for Turmeric values (~2 weeks)

Separate from the breakpoint debugger plan ([godot-binding-debugger-plan.md](../../archive/godot-binding-debugger-plan.md)).

In Godot's debugger panel, paused-frame locals are currently shown as
`Variant` stringifications. A Turmeric Cons cell or HAMT map shows up
as `<TurmericValue at 0x...>` or similar opaque blob.

The work:

- A pretty-printer for `TuriValue` -> `String` that handles cons
  lists, vecs, HAMTs, structs, ADT constructors with structural
  formatting.
- Hook into `ScriptLanguageExtension::debug_get_globals` /
  `debug_get_stack_level_locals` to return formatted strings.
- Stretch: a tree-expander for nested structures (Godot's debugger
  supports `Dictionary` expand-arrows; the same machinery should
  surface for Turmeric containers if we shape the response right).

Most useful once the breakpoint debugger plan ships; until then, the
"paused frame" surface this would decorate doesn't exist.

---

## Risks

- **Platform certificate / signing churn.** Apple changes the
  notarization toolchain every couple of releases. Treat T5.A as
  ongoing maintenance, not a one-shot landing. iOS-specific signing
  risk is tracked in the split-out iOS plan.
- **Cross-language inheritance demand is unknown.** T5.E is the
  feature most likely to never have a real user. Don't pre-build it.

---

## Success Criteria

(Per phase, since each phase is independent.)

- T5.A: A macOS-exported paddle-pong-tur runs on a clean Mac with
  Gatekeeper enabled, no user override required.
- T5.B / T5.C / T5.D: See the per-platform split-out plans linked
  above for success criteria.
- T5.E: A GDScript class declaring `extends MyTurmericClass` resolves
  + instantiates correctly.
- T5.F: A paused breakpoint shows a cons list as `(1 2 3)` not as
  `<TurmericValue at 0x...>`.

---

## Out of Scope

- Console exports (Switch, PlayStation, Xbox). These require licensed
  SDKs and per-platform engine ports; not something a community
  language binding can really tackle.
- VR / XR specific Godot APIs. Tracked as a possible Tier 3 ALLOWLIST
  expansion when a real driver exists.
- Custom export presets / per-platform build flags. Falls under
  general Godot integration work, not language-binding work.
