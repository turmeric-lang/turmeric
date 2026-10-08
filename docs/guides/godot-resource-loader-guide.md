---
title: Wiring a ResourceFormatLoader for the Turmeric/Godot Binding
category: Integration / Godot
description: Why and how the turmeric-godot GDExtension has to register its own ResourceFormatLoader so .tur files load through Godot's resource pipeline
---

# Wiring a ResourceFormatLoader for the Turmeric/Godot Binding

A Turmeric script attached to a node, dragged into the editor, or loaded with
`load("res://path/to/foo.tur")` only works if Godot's *resource pipeline* knows
how to turn the file on disk into a `Script` object. Registering a
`ScriptLanguageExtension` does **not** do that on its own. This guide explains
the gap and walks through the loader (and saver) you need to add.

This guide is specific to the `turmeric-godot` binding repo. It lives in this
repo's `docs/guides/` because the v1 Godot-binding plan
([`docs/archive/godot-language-binding-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/godot-language-binding-plan.md))
treats it as part of the G2 deliverable, and the same gotcha will hit anyone
building a future GDExtension scripting binding from this codebase.

---

## The Gap

Inside a GDExtension, registering a language singleton looks like:

```cpp
Engine::get_singleton()->register_script_language(turmeric_language_singleton);
```

After that call you can `ClassDB.instantiate("TurmericScript")` in GDScript,
set `source_code`, call `reload()`, and the language's hooks fire. **But this
does not register a resource loader.** The first time anything tries:

```gdscript
var s = load("res://scripts/hello.tur")
```

Godot's `ResourceLoader` walks its list of registered
`ResourceFormatLoader`s, finds none that recognise the `.tur` extension, and
fails with:

```
ERROR: No loader found for resource: res://scripts/hello.tur (expected type: )
   at: _load (core/io/resource_loader.cpp:291)
```

The Godot editor hits the same path when you click "Attach Script..." on a
node and pick an existing `.tur` file, and when scenes are deserialised that
reference a `.tur` script via `script = ExtResource("...")`.

So: **`ScriptLanguageExtension` covers script *behaviour*; `ResourceFormatLoader`
covers script *loading*. They are two separate registrations and a language
binding owns both.**

The mirror of this is the saver: Godot's editor uses
`ResourceFormatSaver` to write a script back to disk when you Save As, Move,
or refactor the script's path. Without one, you can load `.tur` files but
not save them from the editor.

---

## What to Implement

### 1. `TurmericResourceLoader : ResourceFormatLoaderExtension`

Minimum viable overrides:

```cpp
class TurmericResourceLoader : public ResourceFormatLoaderExtension {
    GDCLASS(TurmericResourceLoader, ResourceFormatLoaderExtension)
protected:
    static void _bind_methods() {}
public:
    PackedStringArray _get_recognized_extensions() const override {
        PackedStringArray a;
        a.push_back("tur");
        return a;
    }

    bool _handles_type(const StringName &p_type) const override {
        // We produce Script (specifically TurmericScript) instances.
        return p_type == StringName("Script") ||
               p_type == StringName("TurmericScript");
    }

    String _get_resource_type(const String &p_path) const override {
        if (p_path.get_extension().to_lower() == "tur") {
            return String("TurmericScript");
        }
        return String();
    }

    Variant _load(const String &p_path,
                  const String &p_original_path,
                  bool p_use_sub_threads,
                  int32_t p_cache_mode) const override {
        Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
        if (f.is_null()) {
            return Variant();
        }
        String src = f->get_as_text();
        Ref<TurmericScript> script;
        script.instantiate();
        script->set_source_code(src);
        script->set_path(p_path);
        Error err = script->reload();
        if (err != OK) {
            // The reload() call already pushed a diagnostic via
            // UtilityFunctions::printerr; return an empty Variant so the
            // ResourceLoader records the failure.
            return Variant();
        }
        return script;
    }
};
```

A few details worth knowing:

- `set_path()` on the script is what makes the editor's inspector show the
  on-disk location and what wires up the "Open in External Editor" action.
- `p_cache_mode` is one of `CACHE_MODE_IGNORE` / `CACHE_MODE_REUSE` /
  `CACHE_MODE_REPLACE` / `CACHE_MODE_IGNORE_DEEP`. For G2, ignore the value
  and always do a fresh load; revisit when hot-reload lands.
- `_handles_type` returning true for `"Script"` is what lets generic code
  like `load(path) as Script` work without naming `TurmericScript` directly.

### 2. `TurmericResourceSaver : ResourceFormatSaverExtension`

Minimum viable overrides:

```cpp
class TurmericResourceSaver : public ResourceFormatSaverExtension {
    GDCLASS(TurmericResourceSaver, ResourceFormatSaverExtension)
protected:
    static void _bind_methods() {}
public:
    Error _save(const Ref<Resource> &p_resource,
                const String &p_path, uint32_t p_flags) override {
        Ref<TurmericScript> script = p_resource;
        if (script.is_null()) return ERR_INVALID_PARAMETER;
        Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
        if (f.is_null()) return ERR_CANT_OPEN;
        f->store_string(script->get_source_code());
        return OK;
    }

    PackedStringArray _get_recognized_extensions(const Ref<Resource> &p_resource) const override {
        PackedStringArray a;
        if (Ref<TurmericScript>(p_resource).is_valid()) {
            a.push_back("tur");
        }
        return a;
    }

    bool _recognize(const Ref<Resource> &p_resource) const override {
        return Ref<TurmericScript>(p_resource).is_valid();
    }
};
```

### 3. Register both alongside the language

In `initialize_turmeric_godot_module` (called at
`MODULE_INITIALIZATION_LEVEL_SCENE`), after registering the language:

```cpp
GDREGISTER_CLASS(TurmericResourceLoader);
GDREGISTER_CLASS(TurmericResourceSaver);

s_loader.instantiate();
s_saver.instantiate();
ResourceLoader::get_singleton()->add_resource_format_loader(s_loader);
ResourceSaver::get_singleton()->add_resource_format_saver(s_saver);
```

Where `s_loader` and `s_saver` are `Ref<>` members held alive at module
scope. Unregister them in `uninitialize_turmeric_godot_module` with
`remove_resource_format_loader` / `remove_resource_format_saver` to keep
teardown clean across editor reloads.

---

## How to Verify

Once both are wired, the headless driver from the G1 demo becomes a real
end-to-end test of the resource pipeline:

```gdscript
extends SceneTree
func _init() -> void:
    var s := load("res://scripts/hello.tur")
    print("loaded: ", s, " is_valid=", s and s.is_valid())
    quit()
```

```sh
godot --headless --path examples/spike --script res://scripts/load_hello.gd
```

You should see the script's top-level forms execute as part of `_load`'s
call to `script->reload()`. If the editor is open, dragging `hello.tur`
onto a node should now work, and Save As on an edited script should
produce a valid `.tur` file on disk.

---

## Why This Lives Here, Not in Godot

Godot 4's `ScriptLanguage` (the engine-side base class) is intentionally
narrow -- it covers what a *language runtime* does (parse, compile, run,
debug). Resource I/O is owned by a separate subsystem (`ResourceLoader` /
`ResourceSaver`) that doesn't know about scripts specifically -- the same
plumbing loads textures, scenes, audio. GDScript's binding ships its own
`ResourceFormatLoaderGDScript` and `ResourceFormatSaverGDScript` for the
same reason we have to ship our own.

This is not a design defect to file upstream; it is the engine's
factoring. The guide exists so the gap isn't rediscovered the hard way
each time someone wires a new scripting binding from this codebase.

---

## Related

- [`docs/archive/godot-language-binding-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/godot-language-binding-plan.md)
  -- v1 plan. The G2 phase ("Lifecycle + Inspector") is where this loader
  graduates from "known-needed" to "implemented."
- `docs/archive/history/libturi-embed-include-paths.md` and
  `docs/archive/history/libturi-embed-interpret-mode-flag.md` -- the *other* two
  rough edges surfaced during the G1 slice; both were turmeric-side and are
  resolved (archived).
