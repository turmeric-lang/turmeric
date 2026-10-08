# Turmeric raygui Spice Plan

> **Status:** Complete
> **Last Updated:** 2026-05-28
> **Type:** Spice Design + Architecture + Implementation Roadmap
> **Depends on:** `tur-raylib` spice (for window, drawing loop, Rectangle, Color)

---

## Overview

Design and implement `tur-raygui`, a Turmeric spice that wraps
[raygui](https://github.com/raysan5/raygui) -- the immediate-mode GUI toolkit
that ships alongside raylib. It provides buttons, sliders, text boxes, panels,
scroll areas, and a full icon set with zero extra dependencies beyond the
raylib spice.

The spice exposes raygui controls as plain Turmeric functions that are called
inside the standard `begin-drawing` / `end-drawing` loop already provided by
`tur-raylib`. No retained GUI tree, no callbacks, no event queue: each control
returns its current value or an activation flag every frame.

---

## Motivation

The `tur-raylib` spice gives you a window and a draw loop, but no way to add
interactive controls without writing custom hit-testing. raygui fills that gap
with ~40 battle-tested controls, built-in themes, and a one-header C
implementation that links directly into the same executable as raylib.

| Approach | Controls | Immediate-mode | Deps | Turmeric fit |
|---|---|---|---|---|
| raw raylib (draw calls) | None | N/A | none | Low -- all hand-rolled |
| **tur-raygui (this proposal)** | ~40 | Yes | raylib only | Native |
| Dear ImGui via C FFI | ~60 | Yes | separate C++ lib | Moderate -- heavy wrapper |
| Nuklear via C FFI | ~20 | Yes | separate C lib | Moderate -- wrapper needed |

raygui is the natural fit because it is designed to live inside the raylib draw
loop and shares raylib's `Rectangle`, `Color`, and `Font` types, which the
raylib spice already binds.

---

## Architecture

### Dependency chain

```
tur-raygui
  :spices  { "raylib" ... }    ;; window, draw loop, Rectangle, Color, Font
  :cmake-deps { "raygui" ... } ;; header-only; CPM fetches and exposes the header
```

raygui is a single-header library (`raygui.h`). The cmake-dep entry fetches
`github.com/raysan5/raygui` at the desired tag and adds its `src/` to the
include path; no separate library is built or linked.

### The Rectangle type

raygui uses raylib's `Rectangle { float x; float y; float width; float height; }`
throughout. Rather than re-declare it, the raygui spice imports and re-exports
the `Rectangle` struct from `tur-raylib` once it is added there, or defines a
local `Rect` struct when the raylib spice does not yet export one:

```turmeric
;;; Rect -- axis-aligned bounding box for a GUI control.
;;;
;;; Parameters:
;;;   x      -- left edge in pixels
;;;   y      -- top edge in pixels
;;;   width  -- pixel width
;;;   height -- pixel height
;;;
;;; Since: Phase 1
(defstruct Rect
  [x      :float
   y      :float
   width  :float
   height :float])
```

All control functions take a `Rect` as their first argument. A short
constructor alias is provided:

```turmeric
;;; rect -- construct a Rect.
;;;
;;; Example:
;;;   (rect 10.0 10.0 120.0 30.0)  ; => Rect
;;;
;;; Since: Phase 1
(defn rect [x :float y :float w :float h :float] :Rect
  (Rect x y w h))
```

### How raygui integrates with the raylib draw loop

raygui calls raylib draw primitives internally. Every control must be called
between `begin-drawing` and `end-drawing`. No separate raygui init or shutdown
is needed.

```turmeric
#lang sweet-exp

import raylib/core  :refer [init-window close-window window-should-close
                             begin-drawing end-drawing clear-background
                             set-target-fps get-frame-time]
import raylib/color :refer [raywhite]
import raygui/core     :refer [gui-set-style]
import raygui/controls :refer [gui-button gui-slider gui-check-box gui-label]

defn main [] :int
  init-window(800 600 "raygui demo")
  set-target-fps(60)
  let [checked 0
       speed   1.0]
    while not(window-should-close())
      begin-drawing()
      clear-background(raywhite())
      gui-label(rect(10.0 10.0 200.0 20.0) "Speed:")
      set! speed gui-slider(rect(10.0 40.0 200.0 20.0) "0" "10" speed 0.0 10.0)
      set! checked gui-check-box(rect(10.0 80.0 20.0 20.0) "Enabled" checked)
      when gui-button(rect(10.0 120.0 120.0 30.0) "Go!")
        println("button pressed")
      end-drawing()
  close-window()
  0
```

---

## Module Breakdown

### `raygui/core`

State management, font override, fade, and lock.

| Function | Wraps | Returns |
|---|---|---|
| `gui-enable` | `GuiEnable` | `:void` |
| `gui-disable` | `GuiDisable` | `:void` |
| `gui-lock` | `GuiLock` | `:void` |
| `gui-unlock` | `GuiUnlock` | `:void` |
| `gui-is-locked` | `GuiIsLocked` | `:int` (0/1) |
| `gui-set-alpha` | `GuiSetAlpha` | `:void` |
| `gui-set-state` | `GuiSetState` | `:void` |
| `gui-get-state` | `GuiGetState` | `:int` |
| `gui-set-font` | `GuiSetFont` | `:void` |
| `gui-get-font` | `GuiGetFont` | `:int` (Font handle) |
| `gui-set-style` | `GuiSetStyle` | `:void` |
| `gui-get-style` | `GuiGetStyle` | `:int` |
| `gui-load-style` | `GuiLoadStyle` | `:void` |
| `gui-load-style-default` | `GuiLoadStyleDefault` | `:void` |

### `raygui/controls`

The full set of interactive controls. Each takes a `Rect` as its first
argument; additional arguments follow raygui's C signature.

**Basic controls:**

| Function | C function | Signature | Returns |
|---|---|---|---|
| `gui-label` | `GuiLabel` | `rect :Rect text :cstr` | `:void` |
| `gui-button` | `GuiButton` | `rect :Rect text :cstr` | `:int` (1 = clicked) |
| `gui-label-button` | `GuiLabelButton` | `rect :Rect text :cstr` | `:int` |
| `gui-toggle` | `GuiToggle` | `rect :Rect text :cstr active :int` | `:int` |
| `gui-toggle-group` | `GuiToggleGroup` | `rect :Rect text :cstr active :int` | `:int` |
| `gui-check-box` | `GuiCheckBox` | `rect :Rect text :cstr checked :int` | `:int` |

**Range / numeric controls:**

| Function | C function | Signature | Returns |
|---|---|---|---|
| `gui-slider` | `GuiSlider` | `rect :Rect lo :cstr hi :cstr val :float min :float max :float` | `:float` |
| `gui-slider-bar` | `GuiSliderBar` | `rect :Rect lo :cstr hi :cstr val :float min :float max :float` | `:float` |
| `gui-progress-bar` | `GuiProgressBar` | `rect :Rect lo :cstr hi :cstr val :float min :float max :float` | `:float` |
| `gui-spinner` | `GuiSpinner` | `rect :Rect text :cstr val :int min :int max :int edit :int` | `:int` |
| `gui-value-box` | `GuiValueBox` | `rect :Rect text :cstr val :int min :int max :int edit :int` | `:int` |

**Text controls:**

| Function | C function | Signature | Returns |
|---|---|---|---|
| `gui-text-box` | `GuiTextBox` | `rect :Rect text :cstr size :int edit :int` | `:int` (edit mode active) |
| `gui-text-box-multi` | `GuiTextBoxMulti` | `rect :Rect text :cstr size :int edit :int` | `:int` |

**Selection controls:**

| Function | C function | Signature | Returns |
|---|---|---|---|
| `gui-combo-box` | `GuiComboBox` | `rect :Rect text :cstr active :int` | `:int` |
| `gui-dropdown-box` | `GuiDropdownBox` | `rect :Rect text :cstr active :int edit :int` | `:int` |
| `gui-list-view` | `GuiListView` | `rect :Rect text :cstr scroll :int active :int` | `:int` |
| `gui-list-view-ex` | `GuiListViewEx` | `rect :Rect text :cstr count :int scroll :int active :int focus :int` | `:int` |

**Misc controls:**

| Function | C function | Signature | Returns |
|---|---|---|---|
| `gui-color-picker` | `GuiColorPicker` | `rect :Rect text :cstr color :int` | `:int` (packed RGBA) |
| `gui-color-bar-alpha` | `GuiColorBarAlpha` | `rect :Rect text :cstr alpha :float` | `:float` |
| `gui-color-bar-hue` | `GuiColorBarHue` | `rect :Rect text :cstr value :float` | `:float` |
| `gui-message-box` | `GuiMessageBox` | `rect :Rect title :cstr message :cstr buttons :cstr` | `:int` |
| `gui-text-input-box` | `GuiTextInputBox` | `rect :Rect title :cstr message :cstr buttons :cstr text :cstr size :int secret :int` | `:int` |
| `gui-grid` | `GuiGrid` | `rect :Rect text :cstr spacing :float divs :int mouse :int` | `:int` |

### `raygui/layout`

Container controls that group other controls visually.

| Function | C function | Signature | Returns |
|---|---|---|---|
| `gui-window-box` | `GuiWindowBox` | `rect :Rect title :cstr` | `:int` (1 = close button clicked) |
| `gui-group-box` | `GuiGroupBox` | `rect :Rect text :cstr` | `:void` |
| `gui-line` | `GuiLine` | `rect :Rect text :cstr` | `:void` |
| `gui-panel` | `GuiPanel` | `rect :Rect text :cstr` | `:void` |
| `gui-tab-bar` | `GuiTabBar` | `rect :Rect text :cstr count :int active :int` | `:int` |
| `gui-scroll-panel` | `GuiScrollPanel` | `rect :Rect text :cstr content :Rect scroll :int view :int` | `:int` |
| `gui-dummy-rec` | `GuiDummyRec` | `rect :Rect text :cstr` | `:void` |
| `gui-status-bar` | `GuiStatusBar` | `rect :Rect text :cstr` | `:void` |

### `raygui/style`

Style properties map to `(gui-set-style control property value)` /
`(gui-get-style control property)`. The style constants are exposed as
Turmeric `def` values so callers can refer to them by name.

```turmeric
;; Control selector constants
(def RAYGUI_DEFAULT     0)
(def RAYGUI_LABEL       1)
(def RAYGUI_BUTTON      2)
(def RAYGUI_TOGGLE      3)
(def RAYGUI_SLIDER      4)
(def RAYGUI_PROGRESSBAR 5)
(def RAYGUI_CHECKBOX    6)
(def RAYGUI_COMBOBOX    7)
(def RAYGUI_DROPDOWNBOX 8)
(def RAYGUI_TEXTBOX     9)
(def RAYGUI_VALUEBOX    10)
(def RAYGUI_SPINNER     11)
(def RAYGUI_LISTVIEW    12)
(def RAYGUI_COLORPICKER 13)
(def RAYGUI_SCROLLBAR   14)
(def RAYGUI_STATUSBAR   15)

;; Default property constants
(def RAYGUI_BORDER_COLOR_NORMAL   0)
(def RAYGUI_BASE_COLOR_NORMAL     1)
(def RAYGUI_TEXT_COLOR_NORMAL     2)
(def RAYGUI_BORDER_COLOR_FOCUSED  3)
(def RAYGUI_BASE_COLOR_FOCUSED    4)
(def RAYGUI_TEXT_COLOR_FOCUSED    5)
(def RAYGUI_BORDER_COLOR_PRESSED  6)
(def RAYGUI_BASE_COLOR_PRESSED    7)
(def RAYGUI_TEXT_COLOR_PRESSED    8)
(def RAYGUI_BORDER_COLOR_DISABLED 9)
(def RAYGUI_BASE_COLOR_DISABLED   10)
(def RAYGUI_TEXT_COLOR_DISABLED   11)
(def RAYGUI_BORDER_WIDTH          12)
(def RAYGUI_TEXT_PADDING          13)
(def RAYGUI_TEXT_ALIGNMENT        14)
```

### `raygui/icons`

raygui ships a built-in 16x16 icon atlas. Icons are addressed by integer
ID constants and can be used in control labels with the `#` prefix notation
raygui uses internally. This module exposes the icon ID constants and a
helper for drawing icons directly.

```turmeric
(def RAYGUI_ICON_NONE            0)
(def RAYGUI_ICON_FOLDER_FILE_OPEN 1)
(def RAYGUI_ICON_FILE_SAVE_CLASSIC 2)
;; ... full table of ~200 constants ...

;;; gui-draw-icon -- draw a single built-in icon.
;;;
;;; Parameters:
;;;   icon-id   -- icon constant (e.g. RAYGUI_ICON_FILE_SAVE_CLASSIC)
;;;   x         -- left pixel position
;;;   y         -- top pixel position
;;;   pixel-size -- scaling factor (1 = 16px, 2 = 32px, ...)
;;;   color     -- packed RGBA color
;;;
;;; Since: Phase 3
(defn gui-draw-icon [icon-id :int x :int y :int pixel-size :int color :int] :void
  ```c
  GuiDrawIcon(icon_id, x, y, pixel_size, (Color){ color & 0xFF,
              (color >> 8) & 0xFF, (color >> 16) & 0xFF, (color >> 24) & 0xFF });
  ```)
```

---

## Spice Manifest

Lives in `../turmeric-spices/spices/raygui/build.tur`:

```turmeric
(defpackage tur-raygui
  :name        "tur-raygui"
  :version     "0.1.0"
  :description "Immediate-mode GUI controls for Turmeric, layered on tur-raylib"
  :license     "MIT"
  :spices #{
    "raylib" #{:url    "https://github.com/turmeric-lang/turmeric-spices"
              :ref    "raylib-v0.1.0"
              :subdir "spices/raylib"}
    "test"   #{:url    "https://github.com/turmeric-lang/turmeric-spices"
              :ref    "test-v0.1.0"
              :subdir "spices/test"
              :optional true}
  }
  :cmake-deps #{
    "raygui" #{:url     "https://github.com/raysan5/raygui"
              :ref     "4.0"
              :options #{}}
  }
  :exports #{
    "raygui/core"     ["gui-enable" "gui-disable" "gui-lock" "gui-unlock"
                       "gui-is-locked" "gui-set-alpha" "gui-set-state"
                       "gui-get-state" "gui-set-font" "gui-get-font"
                       "gui-set-style" "gui-get-style"
                       "gui-load-style" "gui-load-style-default"]
    "raygui/controls" ["gui-label" "gui-button" "gui-label-button"
                       "gui-toggle" "gui-toggle-group" "gui-check-box"
                       "gui-slider" "gui-slider-bar" "gui-progress-bar"
                       "gui-spinner" "gui-value-box"
                       "gui-text-box" "gui-text-box-multi"
                       "gui-combo-box" "gui-dropdown-box"
                       "gui-list-view" "gui-list-view-ex"
                       "gui-color-picker" "gui-color-bar-alpha" "gui-color-bar-hue"
                       "gui-message-box" "gui-text-input-box" "gui-grid"]
    "raygui/layout"   ["gui-window-box" "gui-group-box" "gui-line" "gui-panel"
                       "gui-tab-bar" "gui-scroll-panel" "gui-dummy-rec"
                       "gui-status-bar"]
    "raygui/style"    ["RAYGUI_DEFAULT" "RAYGUI_LABEL" "RAYGUI_BUTTON"
                       "RAYGUI_TOGGLE" "RAYGUI_SLIDER" "RAYGUI_PROGRESSBAR"
                       "RAYGUI_CHECKBOX" "RAYGUI_COMBOBOX" "RAYGUI_DROPDOWNBOX"
                       "RAYGUI_TEXTBOX" "RAYGUI_VALUEBOX" "RAYGUI_SPINNER"
                       "RAYGUI_LISTVIEW" "RAYGUI_COLORPICKER"
                       "RAYGUI_SCROLLBAR" "RAYGUI_STATUSBAR"
                       "RAYGUI_BORDER_COLOR_NORMAL" "RAYGUI_BASE_COLOR_NORMAL"
                       "RAYGUI_TEXT_COLOR_NORMAL" "RAYGUI_BORDER_COLOR_FOCUSED"
                       "RAYGUI_BASE_COLOR_FOCUSED" "RAYGUI_TEXT_COLOR_FOCUSED"
                       "RAYGUI_BORDER_COLOR_PRESSED" "RAYGUI_BASE_COLOR_PRESSED"
                       "RAYGUI_TEXT_COLOR_PRESSED" "RAYGUI_BORDER_COLOR_DISABLED"
                       "RAYGUI_BASE_COLOR_DISABLED" "RAYGUI_TEXT_COLOR_DISABLED"
                       "RAYGUI_BORDER_WIDTH" "RAYGUI_TEXT_PADDING"
                       "RAYGUI_TEXT_ALIGNMENT"]
    "raygui/icons"    ["RAYGUI_ICON_NONE" "RAYGUI_ICON_FOLDER_FILE_OPEN"
                       "RAYGUI_ICON_FILE_SAVE_CLASSIC"
                       ;; ... full icon constant list ...
                       "gui-draw-icon"]
    "raygui/rect"     ["Rect" "rect"]
  })
```

---

## File Structure

```
docs/raygui-spice-plan.md          ;; This document (in turmeric repo)

../turmeric-spices/spices/raygui/
  build.tur
  tur.lock
  README.md
  src/
    raygui/
      rect.tur          ;; Rect struct + rect constructor
      core.tur          ;; GuiEnable/Disable/Lock/SetStyle/LoadStyle
      controls.tur      ;; All interactive controls
      layout.tur        ;; Panel, ScrollPanel, WindowBox, GroupBox, TabBar
      style.tur         ;; Style property and control constants
      icons.tur         ;; Icon ID constants + gui-draw-icon
  tests/
    controls_test.tur
    layout_test.tur
    style_test.tur
  examples/
    hello-gui.tur       ;; Button, label, slider minimal example
    inspector.tur       ;; Scrollable property panel demo
    color-picker.tur    ;; Color picker + progress bar demo
    file-dialog.tur     ;; WindowBox + ListViewEx + TextInputBox demo
```

---

## Text Buffer Handling

raygui's text box controls (`GuiTextBox`, `GuiTextBoxMulti`) require a
mutable `char*` buffer. Turmeric's `:cstr` is an immutable pointer; the
spice allocates a fixed-size C buffer per text box, managed by the
`make-text-buf` / `free-text-buf` helpers. The caller owns the buffer
lifetime and must free it when the widget is no longer used.

```turmeric
;;; make-text-buf -- allocate a mutable text buffer for a text-box control.
;;;
;;; Parameters:
;;;   capacity -- maximum byte capacity including null terminator
;;;
;;; Returns:
;;;   Opaque :int handle; pass to gui-text-box and free with free-text-buf.
;;;
;;; Since: Phase 1
(defn make-text-buf [capacity :int] #{Unsafe} :int
  ```c
  char *buf = (char *)calloc(capacity, 1);
  return (int64_t)(intptr_t)buf;
  ```)

;;; free-text-buf -- release a buffer allocated by make-text-buf.
;;;
;;; Since: Phase 1
(defn free-text-buf [buf :int] #{Unsafe} :void
  ```c
  free((void *)(intptr_t)buf);
  ```)

;;; text-buf->cstr -- read current contents of a text buffer as :cstr.
;;;
;;; Since: Phase 1
(defn text-buf->cstr [buf :int] #{Unsafe} :cstr
  ```c
  return (int64_t)(intptr_t)(const char *)(intptr_t)buf;
  ```)
```

Usage:

```turmeric
(let [name-buf (make-text-buf 64)
      name-editing 0]
  (while (not (window-should-close))
    (begin-drawing)
    (clear-background (raywhite))
    (set! name-editing (gui-text-box (rect 10.0 10.0 200.0 30.0)
                                     name-buf 64 name-editing))
    (end-drawing))
  (free-text-buf name-buf))
```

---

## Edit-mode State Pattern

Several controls -- `gui-spinner`, `gui-value-box`, `gui-dropdown-box`,
`gui-text-box` -- use raygui's "edit mode" convention: a boolean flag is
passed in and returned; the control activates edit mode when clicked and
deactivates on Enter or focus loss. The canonical Turmeric pattern:

```turmeric
;;; Only one control may be in edit mode at a time in raygui.
;;; Use a single edit-state int to track which (if any) is active.
(let [speed     5
      speed-edit 0]
  (while (not (window-should-close))
    (begin-drawing)
    (clear-background (raywhite))
    ;; gui-spinner returns new edit flag; new value is written back via the
    ;; pointer raygui uses internally. In the Turmeric binding we return a
    ;; pair: (pair new-edit-flag new-value).
    (let [result (gui-spinner (rect 10.0 10.0 120.0 30.0) "Speed" speed 0 20 speed-edit)]
      (set! speed-edit (pair-fst result))
      (set! speed      (pair-snd result)))
    (end-drawing)))
```

The binding for edit-mode controls returns a `(Pair :int :int)` -- edit
flag and updated value -- so neither requires a mutable pointer from the
caller side.

---

## Implementation Phases

### Phase 1: Core controls + Rect (MVP)

**Goal:** Button, label, checkbox, slider, progress bar, text box working
inside a raylib draw loop.

| Task | File | Status |
|---|---|---|
| `Rect` struct and `rect` constructor | `src/raygui/rect.tur` | Done |
| `gui-set-style` / `gui-load-style-default` | `src/raygui/core.tur` | Done |
| `gui-label`, `gui-button`, `gui-check-box` | `src/raygui/controls.tur` | Done |
| `gui-slider`, `gui-slider-bar`, `gui-progress-bar` | `src/raygui/controls.tur` | Done |
| `make-text-buf`, `free-text-buf`, `gui-text-box` | `src/raygui/controls.tur` | Done |
| `hello-gui.tur` example | `examples/hello-gui.tur` | Done |
| Phase 1 smoke tests | `tests/controls_test.tur` | Done |

**Deliverable:** Minimal interactive tool panel in a raylib window.

### Phase 2: Layout containers

**Goal:** Panels, scroll areas, and window boxes that let controls be
grouped and scrolled.

| Task | File | Status |
|---|---|---|
| `gui-panel`, `gui-group-box`, `gui-line` | `src/raygui/layout.tur` | Done |
| `gui-window-box` | `src/raygui/layout.tur` | Done |
| `gui-scroll-panel` | `src/raygui/layout.tur` | Done |
| `gui-tab-bar` | `src/raygui/layout.tur` | Done |
| `gui-status-bar` | `src/raygui/layout.tur` | Done |
| `inspector.tur` example | `examples/inspector.tur` | Done |
| Layout tests | `tests/layout_test.tur` | Done |

**Deliverable:** Scrollable inspector panel with grouped controls and tabs.

### Phase 3: Advanced controls and icons

**Goal:** Selection controls, color picker, spinner, icon drawing.

| Task | File | Status |
|---|---|---|
| `gui-combo-box`, `gui-dropdown-box` | `src/raygui/controls.tur` | Done |
| `gui-list-view`, `gui-list-view-ex` | `src/raygui/controls.tur` | Done |
| `gui-spinner`, `gui-value-box` | `src/raygui/controls.tur` | Done |
| `gui-text-box-multi` | `src/raygui/controls.tur` | Done |
| `gui-color-picker`, `gui-color-bar-*` | `src/raygui/controls.tur` | Done |
| `gui-message-box`, `gui-text-input-box` | `src/raygui/controls.tur` | Done |
| Icon constants + `gui-draw-icon` | `src/raygui/icons.tur` | Done |
| `color-picker.tur` example | `examples/color-picker.tur` | Done |
| Advanced control tests | `tests/controls_test.tur` | Done |

**Deliverable:** Full control set with icon-decorated buttons.

### Phase 4: Style system

**Goal:** Load external `.rgs` style files and override individual
properties; include at least two bundled themes.

| Task | File | Status |
|---|---|---|
| Style property constants | `src/raygui/style.tur` | Done |
| `gui-load-style` from file | `src/raygui/core.tur` | Done |
| Bundled dark / light themes | `src/raygui/themes.tur` | Done (programmatic via gui-set-style; .rgs path via gui-load-style) |
| Style tests | `tests/style_test.tur` | Done |

**Deliverable:** Themeable GUI; swap entire style with one call.

---

## Design Notes

### Why immediate-mode fits Turmeric

Retained-mode GUIs rely on callback closures attached to widget objects.
Turmeric can express this, but it requires more GC pressure from
closure allocation and complicates ownership. Immediate-mode GUI state
lives in plain stack variables that are updated by the return value of
each control call -- a natural fit for Turmeric's value-oriented style.

### One raygui instance per window

raygui uses process-global state (font, style, lock flag). Multiple
raylib windows in the same process would share that state. The spice
documents this clearly but does not attempt to work around it; raygui's
own documentation has the same limitation.

### raygui header-only build

raygui is a single-header library that expects `RAYGUI_IMPLEMENTATION`
to be defined in exactly one translation unit. The cmake-dep wrapper
creates a minimal `raygui.c` that does:

```c
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
```

and links it into the spice's shared object. All other source files
include `raygui.h` without the define.

---

## Example: Property Inspector

A scrollable property inspector panel -- a common tool-window pattern.

```turmeric
#lang sweet-exp

import raylib/core  :refer [init-window close-window window-should-close
                             begin-drawing end-drawing clear-background
                             set-target-fps]
import raylib/color :refer [raywhite]
import raygui/controls :refer [gui-label gui-slider gui-check-box gui-button]
import raygui/layout   :refer [gui-scroll-panel gui-panel]
import raygui/rect     :refer [rect]

defstruct Props
  [speed   :float
   gravity :float
   debug   :int]

defn draw-props [p :Props panel :Rect] :Props
  ;; Controls inside a scroll panel area
  let [speed2   gui-slider(rect(+(Rect-x panel) 80.0)
                               +(Rect-y panel) 120.0 20.0)
                           "0" "20" (Props-speed   p) 0.0 20.0)
       gravity2 gui-slider(rect(+(Rect-x panel) 80.0)
                               +(Rect-y panel) 30.0) 120.0 20.0)
                           "-20" "0" (Props-gravity p) -20.0 0.0)
       debug2   gui-check-box(rect(+(Rect-x panel) 80.0)
                                   +(Rect-y panel) 60.0) 20.0 20.0)
                              "Debug" (Props-debug p))]
    Props(speed2 gravity2 debug2)

defn main [] :int
  init-window(800 600 "Inspector")
  set-target-fps(60)
  let [props Props(5.0 -9.8 0)
       scroll 0]
    while not(window-should-close())
      begin-drawing()
      clear-background(raywhite())
      let [content rect(0.0 0.0 280.0 200.0)
           view    0]
        set! scroll gui-scroll-panel(rect(10.0 10.0 300.0 400.0)
                                         "Properties" content scroll view)
        set! props draw-props(props rect(10.0 10.0 280.0 400.0))
      end-drawing()
  close-window()
  0
```

---

## References

1. [raygui](https://github.com/raysan5/raygui) -- official repository, examples, and `.rgs` style files
2. [raygui API reference](https://github.com/raysan5/raygui/blob/master/src/raygui.h) -- all control signatures in the header
3. [raylib](https://www.raylib.com/) -- underlying window and draw library
4. [raygui style editor](https://raylibtech.itch.io/rguistyler) -- GUI tool for creating `.rgs` theme files

---

## Open Questions

1. **Scroll panel coordinate transform**: `GuiScrollPanel` returns a clipped
   `view` rect and a scroll offset. Controls drawn inside must offset their
   rects by the scroll position. Should the spice expose a helper that applies
   the offset, or leave it to the caller?

2. **Toggle group from a list**: `GuiToggleGroup` takes a semicolon-separated
   string of labels. Should the spice provide a higher-level version that
   accepts a Turmeric cons-list of `:cstr`s and joins them internally?

3. **Text box capacity**: Fixed-capacity buffers are safe but require the
   caller to know the max size upfront. A stretchy-buffer variant that
   reallocates when the user types past the limit would be more ergonomic but
   is harder to implement safely with raygui's C API.

4. **Grid control return value**: `GuiGrid` returns the cell the mouse is over
   as an `(x, y)` pair. The current plan returns a packed int; a `(Pair :int
   :int)` return would be cleaner but requires the `pair` spice as a dep.
