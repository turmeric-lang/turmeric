# turmeric-godot: every `godot-packed-*-push` is a silent no-op

**Summary:** The nine `PackedXxxArray` push natives in `turmeric-godot` cast the
arena `Variant` to a value, `push_back` onto that *copy*, and drop it. The
Variant in the arena is never touched, so `(packed-byte-push b 222)` appends
nothing and reports no error. Every `Packed*Array` a script builds stays empty.
`godot-array-push` and `godot-dict-set` are unaffected -- Godot's `Array` and
`Dictionary` are reference types, so mutating a copy mutates the shared body.

> Status: **fixed on the `claude/godot-packed-array-push-fy8kv9` branch of
> `turmeric-godot` (2026-11-24), not yet merged.** Held open until the fix
> ships, as `godot-baked-in-prelude-fails-to-eval` was; archive it then. See
> "Fix" at the end.

**Severity:** Medium. It is silent -- no diagnostic, no crash, just an array
that is always size 0 -- and it defeats the whole T3.D `PackedXxxArray` surface
(vertex buffers, tilemap cell arrays, byte blobs), which is the reason those
natives exist.

**Repro** (macOS arm64, Godot 4.3, turmeric-godot `origin/main` @ aae82de):

`examples/spike/scripts/_pp.tur`:

```turmeric
(defn _ready []
  (let [b (packed-byte-new)]
    (packed-byte-push b 222)
    (packed-byte-push b 173)
    (godot-println "[pp] packed size after 2 pushes:")
    (godot-println (godot-num->str (packed-byte-size b)))
    (let [a (array-new)]
      (array-push-i a 7)
      (array-push-i a 9)
      (godot-println "[pp] array size after 2 pushes:")
      (godot-println (godot-num->str (array-size a))))))
```

Attach it to a Node from a `SceneTree` driver and run one frame:

```
[pp] packed size after 2 pushes:
0                                  <- expected 2
[pp] array size after 2 pushes:
2                                  <- correct, for contrast
```

## Root cause

`src/bridge/classdb_proxy.cpp`, e.g. `tg_native_godot_packed_byte_push`:

```cpp
const Variant *vp = tg_packed_handle(args[0], "(godot-packed-byte-push)");
if (!vp || vp->get_type() != Variant::PACKED_BYTE_ARRAY) return turi_nil();
PackedByteArray a = (PackedByteArray)*vp;   // by value; CoW body shared
a.push_back((uint8_t)(args[1].as_int & 0xff));
return turi_nil();                          // `a` dies here; *vp unchanged
```

`Variant::operator PackedByteArray()` in godot-cpp returns **by value**. The
returned array shares the Variant's copy-on-write body, but the refcount is now
2, so `push_back` triggers the copy and grows the local -- the arena's Variant
keeps the original, empty body. `a` is destroyed at the end of the call.

The same three lines appear in all nine families:
`packed_byte_push`, `packed_int32_push`, `packed_int64_push`,
`packed_float32_push`, `packed_float64_push`, `packed_string_push`,
`packed_vec2_push`, `packed_vec3_push`, `packed_color_push`.

Contrast `tg_native_godot_array_push`, which is correct for a different reason:
`Array` is a reference type, so the copy and the original name the same body and
`push_back` on either is visible through both.

## Fix direction

Mutate the arena's Variant, not a copy: build the modified array locally and
write it back through a non-const arena accessor, e.g.

```cpp
PackedByteArray a = (PackedByteArray)*vp;
a.push_back(...);
variant_arena_set(args[0].as_int, Variant(a));   // needs adding
```

`variant_arena_lookup` returns `const Variant *` today
(`src/bridge/variant_marshal.h`), so the fix needs a mutable accessor or a
`variant_arena_replace(handle, Variant)` helper. Nine call sites, one helper.

An alternative that avoids the write-back is to make the arena hand out the
`PackedXxxArray` by reference, but the arena stores `Variant`, so the reference
would have to come from a `Variant` internals accessor godot-cpp does not
expose. The write-back is the straightforward route.

## Note

Found while building the AOT C entry points for these natives
([godot-aot-staged-build-lacks-godot-natives.md](godot-aot-staged-build-lacks-godot-natives.md)).
The AOT path forwards to the same implementations, so it inherits the bug
exactly -- fixing it once fixes both routes.

## Fix (2026-11-24, on the branch above)

The write-back route. `variant_arena_replace(handle, Variant)` in
`src/bridge/variant_marshal.{h,cpp}` overwrites the arena slot a handle names,
and each of the nine push natives calls it after `push_back`. The comment
above the T3.D block claimed Packed* arrays share a backing store the way
`Array`/`Dictionary` do; it now says the opposite, which is the truth.

Pinned by `examples/spike/scripts/packed_array_push.{tur,gd}`: one array per
family, handed back through `set_meta`, compared element for element against a
GDScript literal. On Linux x86-64 with Godot 4.3 and turmeric `v0.44.2` the
driver reports ten failures (every array `[]`) before the change and passes
after it:

```sh
godot --headless --path examples/spike --script scripts/packed_array_push.gd
```

Two things turned up on the way:

- `packed_array_roundtrip.tur`, the T3.D smoke script, had never loaded: it
  passed the untyped `(godot-vec2 ...)` to `packed-vec2-push`, which is a
  TUR-E0001 (`expected Vec2Handle, got int`). It now uses `node/vec2`. It has
  no driver and prints rather than asserts, so nothing noticed.
- `turmeric-godot` does not compile against turmeric `main`:
  [godot-shim-does-not-build-against-turmeric-main](godot-shim-does-not-build-against-turmeric-main.md).
