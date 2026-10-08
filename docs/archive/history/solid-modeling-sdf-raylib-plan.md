# Turmeric Solid Modeling: SDF + raylib + GLSL Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** DSL Design + Architecture + Implementation Roadmap

---

## Overview

Design a constructive solid geometry (CSG) and signed distance field (SDF) modeling library for Turmeric, using raylib for rendering and GLSL for SDF evaluation and mesh generation. This addresses the key limitation of libfive: **no support for coloring individual objects** (critical for texturing pipelines).

The library will:
- Define shapes via SDF functions (pure Turmeric functions: `vec3 -> float`)
- Render live previews using raylib + GLSL shaders
- Export colored meshes for external texturing tools
- Support per-object colors/materials at the SDF level

---

## Motivation

| Library | CSG | SDF | Colors | Python Bindings | Turmeric Fit |
|---------|-----|-----|--------|-----------------|--------------|
| libfive | ✅ | ✅ | ❌ | ✅ | Poor - no color, Scheme API |
| manifold3d | ✅ | ❌ | ❌ | ✅ | Poor - mesh-based, no SDF |
| **This proposal** | ✅ | ✅ | ✅ | N/A | Native Turmeric |

### Why Not libfive?

1. **No color support**: libfive trees produce a single monolithic mesh; cannot tag sub-shapes with colors for later texturing
2. **Scheme API**: Turmeric would need to wrap an alien DSL
3. **No per-primitive metadata**: Cannot attach material IDs, UV coordinates, or user data to shape nodes

### Why Not Wrap manifold3d?

1. **Mesh-based, not SDF**: manifold3d operates on triangle soups, not functional distance fields
2. **No exact CSG**: Mesh boolean ops introduce approximation errors; SDF CSG is exact
3. **Same color problem**: manifold3d's output is still a single mesh without per-face attribution
4. **C++ API complexity**: Heavy wrapper overhead; harder to embed in Turmeric's FFI

---

## Core Idea: SDF + Dual Contouring + raylib

The StackExchange reference ([answer 7516](https://computergraphics.stackexchange.com/a/7516)) describes using **multi-pass GLSL rendering** to convert SDFs to meshes. We adapt this:

```
Turmeric SDF definitions
    ↓ (compile to GLSL via codegen)
GLSL fragment shader (SDF evaluator)
    ↓ (render to multi-channel FBO)
Dual Contouring / Marching Cubes on GPU
    ↓ (extract mesh + per-vertex colors)
Colored triangle mesh
    ↓
raylib rendering / STL export with color metadata
```

### The Key Insight: SDF Composition with Object IDs

Instead of a single distance field, we render a **multi-channel SDF**:

```glsl
// Fragment shader output
struct SDFResult {
    float distance;     // Combined SDF (min of all)
    int   objectId;      // Which primitive is closest
    vec3  color;        // Per-object color/material
    vec3  normal;       // Analytic normal from gradient
};
```

This allows us to:
1. Compute the union SDF (min distance across all objects)
2. Track WHICH object contributed the surface point
3. Assign per-object colors during mesh extraction
4. Export meshes with per-face or per-vertex color attributes

---

## Architecture

### Representation Choice: AST, Not Opaque Closures

The first draft of this plan represented every SDF as a `:fn` (a
`tur_poly_fn_t` fat-closure of type `:vec3 -> :float`). That representation
makes Turmeric-side evaluation cheap, but it kills GLSL codegen: a closure
is an opaque function pointer + environment. There is no way to walk into
`(sdf-union a b)` and recover "this is a union of two spheres" once the
union has been collapsed into a lambda.

We therefore reify SDFs as an **algebraic data type** (`SdfExpr`) -- a
small tagged tree describing the geometry. Two interpreters consume it:

| Interpreter | Lives in | Used for |
|---|---|---|
| `sdf-eval :: SdfExpr -> :vec3 -> :float` | Turmeric, derived once via `cata` | CPU mesh extraction, tests, fallback raymarch |
| `sdf->glsl :: SdfExpr -> :cstr` | Turmeric, emits GLSL source | GPU preview, optional GPU mesh extraction |

Closures still exist as an **escape hatch**: an `SdfCustom` node can wrap a
user-provided `:fn` for SDFs that cannot be expressed in the AST (e.g. a
data-driven height field). Custom nodes participate in CPU eval but force
the GPU path to fall back to CPU raymarching (or to a hybrid evaluator
that uploads the custom result as a 3D texture).

### Layer 1: The `SdfExpr` AST

```turmeric
;;; SdfExpr -- algebraic description of an SDF scene.
;;;
;;; This is a flat tagged union so that both the Turmeric evaluator and
;;; the GLSL emitter can pattern-match on the tag. Fields union over
;;; constructor arity; only the slots named in the docstring per tag are
;;; meaningful.
(defstruct SdfExpr
  [tag    :int        ;; one of SDF_SPHERE, SDF_BOX, ... below
   v0     :vec3       ;; center / lo / axis / ...
   v1     :vec3       ;; hi / direction / ...
   s0     :float      ;; radius / thickness / blend-k / ...
   s1     :float      ;; secondary scalar
   left   :SdfExpr    ;; sub-expression (or sentinel for leaves)
   right  :SdfExpr    ;; sub-expression (or sentinel for leaves)
   custom :fn         ;; only used by SDF_CUSTOM; nil otherwise
   ])

;; Tag constants (kept tight so a single switch dispatches in both
;; languages).
(def SDF_SPHERE       0)
(def SDF_BOX          1)
(def SDF_CYLINDER     2)
(def SDF_PLANE        3)
(def SDF_UNION        10)
(def SDF_INTERSECT    11)
(def SDF_DIFFERENCE   12)
(def SDF_SMOOTH_UNION 13)
(def SDF_OFFSET       20)
(def SDF_SHELL        21)
(def SDF_TRANSLATE    30)
(def SDF_CUSTOM       99)
```

Primitives are smart constructors over this struct:

```turmeric
;;; sdf-sphere -- signed distance to a sphere centred at `center`.
(defn sdf-sphere [center :vec3 r :float] :SdfExpr
  (sdf-leaf SDF_SPHERE center (vec3 0.0 0.0 0.0) r 0.0))

;;; sdf-box -- AABB from `lo` to `hi`.
(defn sdf-box [lo :vec3 hi :vec3] :SdfExpr
  (sdf-leaf SDF_BOX lo hi 0.0 0.0))

;;; sdf-cylinder -- infinite cylinder along +Z, projected radius `r`.
(defn sdf-cylinder [center :vec3 r :float] :SdfExpr
  (sdf-leaf SDF_CYLINDER center (vec3 0.0 0.0 1.0) r 0.0))

;;; sdf-custom -- escape hatch for SDFs that cannot be expressed in the
;;; AST. The closure is :vec3 -> :float. GLSL emission for a subtree
;;; containing SDF_CUSTOM falls back to a CPU evaluator (see Layer 4).
(defn sdf-custom [f :fn] :SdfExpr
  (sdf-leaf-fn SDF_CUSTOM f))
```

### Layer 2: CSG Operations Over the AST

```turmeric
;;; sdf-union -- union (min) of two SDFs.
(defn sdf-union [a :SdfExpr b :SdfExpr] :SdfExpr
  (sdf-node SDF_UNION a b 0.0 0.0))

;;; sdf-intersection -- intersection (max).
(defn sdf-intersection [a :SdfExpr b :SdfExpr] :SdfExpr
  (sdf-node SDF_INTERSECT a b 0.0 0.0))

;;; sdf-difference -- a minus b.
(defn sdf-difference [a :SdfExpr b :SdfExpr] :SdfExpr
  (sdf-node SDF_DIFFERENCE a b 0.0 0.0))

;;; sdf-smooth-union -- soft min with blend parameter k (Quilez).
(defn sdf-smooth-union [a :SdfExpr b :SdfExpr k :float] :SdfExpr
  (sdf-node SDF_SMOOTH_UNION a b k 0.0))

;;; sdf-offset -- expand (d>0) or shrink (d<0) by a constant amount.
(defn sdf-offset [a :SdfExpr d :float] :SdfExpr
  (sdf-unary SDF_OFFSET a d 0.0))

;;; sdf-shell -- keep only a `thickness`-wide shell around the surface.
(defn sdf-shell [a :SdfExpr thickness :float] :SdfExpr
  (sdf-unary SDF_SHELL a thickness 0.0))
```

The CPU evaluator is derived once and shared:

```turmeric
;;; sdf-eval -- evaluate an SdfExpr at a point.
;;; Single dispatch on `tag`; the structure of the recursion is mirrored
;;; exactly by sdf->glsl so the two interpreters stay in sync.
(defn sdf-eval [e :SdfExpr p :vec3] :float
  (cond
    (= (SdfExpr-tag e) SDF_SPHERE)
      (- (vec3-length (vec3-sub p (SdfExpr-v0 e))) (SdfExpr-s0 e))
    (= (SdfExpr-tag e) SDF_BOX)
      (sdf-box-eval p (SdfExpr-v0 e) (SdfExpr-v1 e))
    (= (SdfExpr-tag e) SDF_UNION)
      (min (sdf-eval (SdfExpr-left e) p)
           (sdf-eval (SdfExpr-right e) p))
    (= (SdfExpr-tag e) SDF_INTERSECT)
      (max (sdf-eval (SdfExpr-left e) p)
           (sdf-eval (SdfExpr-right e) p))
    (= (SdfExpr-tag e) SDF_DIFFERENCE)
      (max (sdf-eval (SdfExpr-left e) p)
           (- (sdf-eval (SdfExpr-right e) p)))
    (= (SdfExpr-tag e) SDF_CUSTOM)
      ;; Call the escape-hatch closure. SdfExpr-custom is :fn.
      (call-poly-fn-vec3-float (SdfExpr-custom e) p)
    :else
      (error "sdf-eval: unknown tag")))
```

### Layer 3: Colored SDF Scene

Colours and object IDs are carried on the AST itself, not in a parallel
closure. Each `SdfExpr` is paired with a `Surface` describing how the
result should be shaded when this subtree is the closest:

```turmeric
;;; Surface -- material attribution attached to an SdfExpr subtree.
(defstruct Surface
  [color    :vec3       ;; RGB in [0,1]
   objectId :int        ;; >= 0; -1 reserved for "background"
   metadata :int        ;; HAMT pointer for UV transform, BSDF params, etc.
   ])

;;; ColoredSDF -- an SDF paired with its surface attribution.
(defstruct ColoredSDF
  [sdf     :SdfExpr
   surface :Surface
   ])

;;; colored-sphere -- sphere primitive with a fixed surface.
(defn colored-sphere [center :vec3 r :float color :vec3 objectId :int] :ColoredSDF
  (ColoredSDF (sdf-sphere center r)
              (Surface color objectId 0)))

;;; colored-union -- the colour of the union at a point is the colour of
;;; whichever child SDF is closest. Mesh extraction reads this attribution
;;; via sdf-attribute below.
(defn colored-union [a :ColoredSDF b :ColoredSDF] :ColoredSDF
  (ColoredSDF (sdf-union (ColoredSDF-sdf a) (ColoredSDF-sdf b))
              ;; The combined surface is "either a or b"; the picker is
              ;; deferred until query time.
              (Surface (vec3 0.0 0.0 0.0) -1 0)))

;;; sdf-attribute -- given a colored scene tree, return the Surface of
;;; whichever leaf is closest to `p`. Used by CPU mesh extraction.
(defn sdf-attribute [scene :ColoredSDF p :vec3] :Surface
  ;; Walk the tree, tracking the (distance, surface) of the winning leaf
  ;; through unions / intersections / differences.
  (sdf-attribute-walk scene p))
```

The picker (`sdf-attribute-walk`) is structurally identical to `sdf-eval`
but returns the *attribution* of whichever leaf won the `min`/`max`
contest. Keeping the two recursions side-by-side in one file makes it
easy to audit that they agree on which leaf "wins" at any given point.

### Layer 4: GLSL Code Generation

GLSL emission is now a pure tree walk -- no introspection of opaque
closures required.

```turmeric
;;; sdf->glsl -- emit a GLSL function `sceneSDF` for the given scene.
;;;
;;; Strategy: each leaf becomes a numbered helper (sdf0, sdf1, ...) and
;;; sceneSDF is a fold over the tree that calls them. Object IDs and
;;; colours travel through `out` parameters so the union/min logic can
;;; pick a winner.
;;;
;;; Returns a complete fragment of GLSL ready to be `#include`d into the
;;; raymarching shader; see Layer 5 for the host shader.
(defn sdf->glsl [scene :ColoredSDF] :cstr
  (let [leaves   (collect-leaves scene)        ;; (vec ColoredSDF)
        prim-src (str-join "\n" (map emit-leaf-glsl leaves))
        scene-fn (emit-tree-glsl (ColoredSDF-sdf scene) leaves)]
    (str-concat
      "// Auto-generated from Turmeric SdfExpr -- DO NOT EDIT BY HAND\n"
      prim-src "\n"
      "float sceneSDF(vec3 p, out int objId, out vec3 col) {\n"
      scene-fn
      "}\n")))

;;; emit-leaf-glsl -- one GLSL function per leaf primitive.
;;;
;;; Example output for a sphere at (0,0,2) radius 1.5:
;;;
;;;   float sdf3(vec3 p) { return length(p - vec3(0.0,0.0,2.0)) - 1.5; }
(defn emit-leaf-glsl [leaf :ColoredSDF] :cstr ...)

;;; emit-tree-glsl -- emit the body of sceneSDF as a fold over the AST.
;;;
;;; Each combinator emits a fixed code template. For SDF_UNION we emit
;;; the equivalent of:
;;;
;;;   { float dL; int iL; vec3 cL; <left>;
;;;     float dR; int iR; vec3 cR; <right>;
;;;     if (dL < dR) { d = dL; objId = iL; col = cL; }
;;;     else         { d = dR; objId = iR; col = cR; } }
;;;
;;; so colour and object ID follow whichever child won the min.
(defn emit-tree-glsl [e :SdfExpr leaves :vec] :cstr ...)
```

#### Handling `SDF_CUSTOM` nodes

A subtree containing `SDF_CUSTOM` cannot be statically compiled to GLSL.
Three strategies, in order of preference:

1. **Sample to texture**: evaluate the custom SDF on a 3D grid in
   Turmeric, upload as a `sampler3D`, and emit a GLSL trilinear lookup.
   Works for static custom SDFs; lossy at the texture's resolution.
2. **Hybrid pass**: GPU raymarches everything except the custom subtree,
   then CPU evaluates the custom subtree only at hit candidates. Slower
   but exact.
3. **Refuse**: `sdf->glsl` returns an `:err` Result documenting which
   node forced the fallback. The caller can choose CPU rendering.

Phase 3 (GLSL Acceleration) ships strategy 1; strategy 2 is Phase 4.

#### Why not just store closures and JIT them?

Two reasons we considered and rejected:

- **No GLSL backend in libturi today.** Turmeric compiles to C, not to
  shading languages. A GLSL backend would be a major effort and would
  only help this one spice.
- **Even with a backend, closure environments leak host pointers.**
  Reifying the AST keeps the GPU/CPU contract narrow: only the data in
  `SdfExpr` crosses the language boundary, and there is exactly one
  table (the tag constants) that both sides must agree on.

### Layer 5: Dual Contouring Mesh Extraction

Use the approach from the StackExchange answer: multi-pass rendering with FBOs.

**Pass 1 - SDF Evaluation:**
```glsl
// Render to FBO with 4 attachments:
//   R32F - distance
//   R32I - objectId
//   RGB8 - color
//   RGB16F - normal

#version 330
uniform vec3 u_volumeMin;
uniform vec3 u_volumeMax;
uniform vec3 u_resolution;
uniform mat4 u_invViewProj;

out float outDistance;
out int   outObjectId;
out vec3  outColor;
out vec3  outNormal;

float sceneSDF(vec3 p, out int objId, out vec3 col);

vec2 ndcToRay(vec2 ndc) {
    // Convert NDC to world ray
}

void main() {
    vec2 ndc = gl_FragCoord.xy / u_resolution.xy * 2.0 - 1.0;
    vec3 ro = ndcToRay(ndc).xyz;
    vec3 rd = normalize(ndcToRay(ndc).wzy);
    
    // Raymarch
    float t = 0.0;
    for (int i = 0; i < 128; i++) {
        vec3 p = ro + rd * t;
        int objId;
        vec3 col, norm;
        float d = sceneSDF(p, objId, col);
        if (abs(d) < 0.001) {
            // Hit surface
            outDistance = d;
            outObjectId = objId;
            outColor = col;
            outNormal = normalize(norm);
            return;
        }
        t += max(abs(d), 0.01);
    }
    // Background
    outDistance = 1e20;
    outObjectId = -1;
    outColor = vec3(0);
    outNormal = vec3(0);
}
```

**Pass 2 - Dual Contouring:**
- Read the 3D texture of SDF values, object IDs, and colors
- For each voxel on the zero isosurface:
  - Compute the sign configuration of the 8 corners
  - Use a precomputed lookup table to find the triangle configuration
  - Generate vertices with interpolated colors and normals

**Pass 3 - Mesh Optimization:**
- Weld vertices that share position+normal+color
- Remove degenerate triangles
- Optional: apply quadric edge collapse for simplification

### Layer 6: raylib Integration

raylib is consumed through Turmeric's standard `extern-c` FFI. Each
imported symbol is declared with its C signature (one per form -- there
is no `:refer` list form). See `docs/guides/c-integration-guide.md` for
the full conventions.

```turmeric
(extern-c InitWindow       [^int ^int ^cstr] :void)
(extern-c CloseWindow      [^]               :void)
(extern-c WindowShouldClose [^]              :int)
(extern-c BeginDrawing     [^]               :void)
(extern-c EndDrawing       [^]               :void)
(extern-c ClearBackground  [^int]            :void)   ;; raylib Color packed as int
(extern-c BeginShaderMode  [^ptr]            :void)
(extern-c EndShaderMode    [^]               :void)
(extern-c LoadShaderFromMemory [^cstr ^cstr] :ptr)
(extern-c SetShaderValue   [^ptr ^int ^ptr ^int] :void)
(extern-c DrawRectangle    [^int ^int ^int ^int ^int] :void)
(extern-c DrawFPS          [^int ^int]       :void)

;;; sdf-render-window -- open a raylib window and live-preview `scene`.
;;;
;;; The GLSL source is compiled once up front; uniforms are pushed each
;;; frame. Camera/orbit input is handled by raylib's built-in helpers
;;; (not shown).
(defn sdf-render-window [scene :ColoredSDF width :int height :int title :cstr] :void
  (do
    (InitWindow width height title)
    (let [glsl-src     (sdf->glsl scene)
          frag-shader  (LoadShaderFromMemory (cstr-null) glsl-src)]
      (while (= (WindowShouldClose) 0)
        (do
          (BeginDrawing)
          (ClearBackground COLOR_DARK_GRAY)
          (BeginShaderMode frag-shader)
          (push-frame-uniforms frag-shader width height)
          ;; A full-screen quad triggers the fragment shader for every pixel.
          (DrawRectangle 0 0 width height COLOR_WHITE)
          (EndShaderMode)
          (DrawFPS 10 10)
          (EndDrawing)))
      (CloseWindow))))
```

Notes:

- `extern-c` takes one symbol per form with a C-typed signature. The
  earlier `:refer`-style import was Clojure-flavoured pseudocode and is
  not valid Turmeric.
- `LoadShaderFromMemory` is used in preference to `LoadShader` (which
  reads from disk) because the GLSL source is generated in memory by
  `sdf->glsl`.
- raylib `Color` values are 32-bit packed (RGBA8) and pass as `:int`. The
  `COLOR_*` constants in the spice are `(def COLOR_WHITE 0xFFFFFFFF)`
  etc.
- `SetShaderValue`'s real signature takes a uniform location (`int`), a
  void pointer to the value, and a `ShaderUniformDataType` enum; see the
  raylib headers. The helper `push-frame-uniforms` in the spice wraps
  the raw call so the user code stays readable.

### Layer 7: Mesh Export with Colors

```turmeric
(defstruct Mesh
  [vertices   :(vec vec3)   ;; Position
   normals    :(vec vec3)   ;; Optional
   colors     :(vec vec3)   ;; Per-vertex color
   texcoords  :(vec vec2)   ;; Optional
   indices    :(vec int)    ;; Triangle indices
   ])

;;; extract-mesh-from-sdf -- run dual contouring on GPU results.
(defn extract-mesh-from-sdf [sdf-texture :int resolution :vec3 bounds :bounds] :Mesh
  ;; Run compute shader for dual contouring
  ;; Download results to CPU
  ;; Build Mesh struct with per-vertex colors from objectId
  ...)

;;; export-stl -- export mesh to STL (monochrome).
;;; Note: STL doesn't support colors; use OBJ or glTF instead.
(defn export-stl [mesh :Mesh path :cstr] :void ...)

;;; export-obj -- export colored mesh to OBJ + MTL.
(defn export-obj [mesh :Mesh path :cstr] :void
  ;; OBJ format supports per-face material groups
  ;; Generate MTL file with one material per objectId
  ...)

;;; export-gltf -- export to glTF for modern pipelines.
(defn export-gltf [mesh :Mesh path :cstr] :void
  ;; glTF supports per-vertex colors and materials
  ...)
```

---

## Comparison with Alternatives

### Alternative A: Wrap manifold3d via C FFI

**Pros:**
- Mature, well-tested boolean operations
- Actively maintained
- Supports exact geometry (not just SDF)

**Cons:**
- **No SDF**: Mesh-based, loses functional representation
- **No per-primitive colors**: Output is single mesh without attribution
- **C++ API**: Heavy wrapper overhead
- **No GPU acceleration**: CPU-only
- **License**: manifold3d is MIT (compatible), but dependency burden

**Verdict:** Rejected - doesn't solve the color problem, and SDF approach is more elegant for Turmeric.

### Alternative B: Wrap CGAL via C FFI

**Pros:**
- Supports implicit function meshing (CGAL::Mesh_3)
- High quality meshes
- Well-tested

**Cons:**
- **Heavy dependency**: CGAL is massive
- **Complex API**: Steep learning curve
- **C++ templates**: Hard to wrap in C FFI
- **Build complexity**: Requires specific compiler versions
- **Still limited color support**: Would need custom extensions

**Verdict:** Rejected - too heavy, and SDF approach is more natural.

### Alternative C: Pure CPU Marching Cubes

**Pros:**
- Simpler implementation
- No GLSL codegen needed
- Easier to debug

**Cons:**
- **Slower**: CPU ray marching for preview
- **Lower quality**: Marching cubes produces "stair-step" artifacts
- **No GPU parallelism**: Misses opportunity for interactive previews

**Verdict:** Could be a fallback/CPU path, but GPU approach is preferred.

### Alternative D: Hybrid libfive + Custom Color Layer

**Pros:**
- Reuse libfive's SDF and boolean ops
- Only add color tracking on top

**Cons:**
- **Libfive limitation**: The C API doesn't expose per-node metadata
- **Would require libfive modification**: Forking libfive to add color support
- **Maintenance burden**: Tracking upstream changes

**Verdict:** Rejected - defeats the purpose of a clean Turmeric-native solution.

---

## File Structure

```
docs/solid-modeling-sdf-raylib-plan.md    # This document (in turmeric repo)

../turmeric-spices/spices/sdf-raylib/   # Spice lives in sibling repo
  build.tur
  tur.lock
  src/
    sdf/
      primitives.tur       # sphere, box, cylinder, torus, etc.
      transforms.tur       # translate, rotate, scale (function wrappers)
      boolean.tur          # union, intersection, difference
      blends.tur           # smooth union, shell, offset, erosion
      repeats.tur          # array, polar array, grid
      colors.tur           # ColoredSDF, color operations
      scene.tur            # Scene graph, object IDs
    glsl/
      codegen.tur          # Turmeric -> GLSL compiler
      shaders.tur          # GLSL shader sources (as Turmeric strings)
      fbo.tur              # Multi-channel FBO management
    mesh/
      extraction.tur       # Dual contouring implementation
      marching-cubes.tur   # Fallback CPU path
      optimization.tur     # Vertex welding, simplification
    export/
      obj.tur              # OBJ + MTL export
      stl.tur              # STL export (monochrome)
      gltf.tur             # glTF export
    raylib/
      integration.tur      # raylib window, input, rendering
      camera.tur           # Orbit camera, FPS camera
    ffi/
      raylib.tur           # raylib FFI bindings
      gl.tur                # OpenGL FFI bindings
  tests/
    primitives_test.tur
    boolean_test.tur
    color_test.tur
    export_test.tur
  examples/
    hello-sphere.tur
    colored-csg.tur
    gear-demo.tur
    parametric.tur
```

**Note:** As per project conventions (see `AGENTS.md` and `CLAUDE.md`), spice implementations live in the sibling repository `../turmeric-spices/`. Do not create a local `./spices/` directory in this repo.

---

## Spice Manifest

The spice manifest lives in `../turmeric-spices/spices/sdf-raylib/build.tur`:

```turmeric
(defpackage tur-sdf-raylib
  :name        "tur-sdf-raylib"
  :version     "0.1.0"
  :description "SDF-based solid modeling with raylib rendering and colored mesh export"
  :license     "MIT"
  :repository  "https://github.com/turmeric-lang/turmeric-spices"

  :cmake-deps {
    "raylib" {:url     "https://github.com/raysan5/raylib"
              :ref     "5.0"
              :options {:BUILD_EXAMPLES "OFF"}}
    "glad"   {:url     "https://github.com/Dav1dde/glad"
              :ref     "v2.0"}
  }

  :exports {
    "sdf/primitives" ["sdf-sphere" "sdf-box" "sdf-cylinder" "sdf-cone" "sdf-torus"
                      "sdf-plane" "sdf-half-space"]
    "sdf/transforms" ["sdf-translate" "sdf-rotate-x" "sdf-rotate-y" "sdf-rotate-z"
                       "sdf-scale" "sdf-mirror"]
    "sdf/boolean" ["sdf-union" "sdf-intersection" "sdf-difference" "sdf-xor"]
    "sdf/blend" ["sdf-smooth-union" "sdf-smooth-intersection" "sdf-shell" "sdf-offset"]
    "sdf/repeat" ["sdf-array" "sdf-polar-array" "sdf-grid"]
    "sdf/colors" ["ColoredSDF" "colored-sphere" "colored-union" "colored-intersection"]
    "sdf/scene" ["Scene" "add-object" "render"]
    "glsl/codegen" ["compile-sdf-to-glsl"]
    "mesh/extraction" ["extract-mesh" "Mesh"]
    "export" ["export-obj" "export-stl" "export-gltf"]
    "raylib" ["sdf-render-window" "OrbitCamera" "FPSCamera"]
  })
```

---

## Example: Colored CSG Scene

```turmeric
(import sdf/primitives :refer [sdf-sphere sdf-box sdf-cylinder])
(import sdf/transforms :refer [sdf-translate])
(import sdf/boolean :refer [sdf-union sdf-difference])
(import sdf/colors :refer [ColoredSDF colored-sphere colored-box colored-cylinder
                          colored-union colored-difference])
(import export :refer [export-obj])

(defn make-scene [] :ColoredSDF
  (let [red-sphere (colored-sphere (vec3 0.0 0.0 2.0)  1.5
                                    (vec3 1.0 0.0 0.0) 1)
        green-box  (colored-box    (vec3 -1.0 -1.0 0.0) (vec3 1.0 1.0 1.0)
                                    (vec3 0.0 1.0 0.0) 2)
        blue-cyl   (colored-cylinder (vec3 0.0 0.0 -1.0) 0.8
                                      (vec3 0.0 0.0 1.0) 3)
        ;; Per-point colour resolution is deferred to sdf-attribute (see
        ;; Layer 3); colored-union just builds the tree.
        combined   (colored-union (colored-union red-sphere green-box)
                                  blue-cyl)]
    combined))

(defn main [] :int
  (let [scene (make-scene)
        mesh (extract-mesh-from-sdf scene [256 256 256] [[-5 -5 -5] [5 5 5]])]
    (export-obj mesh "output.obj")
    (println "Exported output.obj with per-vertex colors")
    0))
```

---

## Technical Challenges & Solutions

### Challenge 1: AST/Evaluator Drift

The CPU evaluator (`sdf-eval`), the attribution walker
(`sdf-attribute-walk`), and the GLSL emitter (`sdf->glsl`) all
pattern-match on the same set of `SDF_*` tags. Any drift between them is
a silent miscompile: a scene renders correctly on CPU but wrong on GPU,
or vice versa.

**Solution:**
- Keep the three interpreters in one file (`sdf/eval.tur`) so the
  `cond`/`switch` tables sit next to each other and review naturally.
- A golden-image test renders a fixed scene through both paths and
  asserts they agree within a tolerance per pixel.
- Adding a new `SDF_*` tag is a single PR that updates all three
  interpreters together; CI fails if any branch is missing (an
  exhaustiveness check on the tag enum).

### Challenge 2: GLSL Code Generation

GLSL emission walks the AST, so the hard problem ("compile arbitrary
Turmeric code to GLSL") is sidestepped. The remaining work is the per-tag
code template and a handful of correctness concerns.

**Solution:**
- **Per-tag template**: each `SDF_*` constructor has one fixed GLSL
  snippet. Adding a new tag means adding a new snippet, not extending a
  general-purpose compiler.
- **Numeric formatting**: emit floats with `%.9g` to round-trip exactly;
  Turmeric `:float` is 32-bit so this is lossless.
- **Validation**: compile the emitted shader once at preview-window
  startup; on `glCompileShader` failure, print the GLSL with line
  numbers alongside the AST node that produced each block.
- **`SDF_CUSTOM` fallback**: covered in Layer 4 above.

### Challenge 3: Dual Contouring Implementation

GPU dual contouring with per-object color tracking is complex.

**Solution:**
- **Phase 1**: Implement CPU dual contouring first (simpler, for export)
- **Phase 2**: Implement GPU version using compute shaders
- **Use existing algorithms**: Adapt [Dual Contouring of Hermite Data](https://www.cs.wustl.edu/~taoju/research/dualContour.pdf) paper

### Challenge 4: Per-Object Colors in Mesh Extraction

Need to track objectId through the entire pipeline.

**Solution:**
- Store objectId in a 3D texture alongside distance
- Interpolate objectId at mesh vertices using trilinear interpolation
- For hard edges between objects, use the objectId of the closest primitive
- Option: implement **sharp feature detection** to prevent color bleeding

---

## Implementation Phases

### Phase 1: CPU SDF + Marching Cubes (MVP)

**Goal:** Basic SDF modeling with CPU mesh extraction and monochrome STL export.

| Task | File | Status |
|------|------|--------|
| SDF primitives (sphere, box, cylinder) | `sdf/primitives.tur` | Pending |
| SDF boolean ops | `sdf/boolean.tur` | Pending |
| SDF transforms | `sdf/transforms.tur` | Pending |
| CPU Marching Cubes | `mesh/marching-cubes.tur` | Pending |
| STL export | `export/stl.tur` | Pending |
| raylib window with ray marched preview | `raylib/integration.tur` | Pending |

**Deliverable:** Can model basic shapes, boolean ops, preview in window, export STL.

### Phase 2: Colored SDFs

**Goal:** Add per-object color support throughout the pipeline.

| Task | File | Status |
|------|------|--------|
| ColoredSDF struct | `sdf/colors.tur` | Pending |
| Colored boolean ops | `sdf/colors.tur` | Pending |
| CPU mesh extraction with colors | `mesh/extraction.tur` | Pending |
| OBJ + MTL export | `export/obj.tur` | Pending |

**Deliverable:** Can assign colors to objects, export colored meshes.

### Phase 3: GLSL Acceleration

**Goal:** GPU-accelerated SDF evaluation and preview.

| Task | File | Status |
|------|------|--------|
| GLSL code generation | `glsl/codegen.tur` | Pending |
| Multi-channel FBO | `glsl/fbo.tur` | Pending |
| Ray marching shader | `glsl/shaders.tur` | Pending |
| GPU raylib rendering | `raylib/integration.tur` | Pending |

**Deliverable:** Interactive preview with GPU ray marching.

### Phase 4: Dual Contouring

**Goal:** Higher quality mesh extraction with sharp features.

| Task | File | Status |
|------|------|--------|
| CPU dual contouring | `mesh/extraction.tur` | Pending |
| GPU dual contouring (compute shader) | `mesh/extraction.tur` | Pending |
| Mesh optimization | `mesh/optimization.tur` | Pending |
| glTF export | `export/gltf.tur` | Pending |

**Deliverable:** High-quality colored meshes with sharp edges.

### Phase 5: Polish

**Goal:** Production-ready library.

| Task | File | Status |
|------|------|--------|
| More primitives (torus, cone, etc.) | `sdf/primitives.tur` | Pending |
| Smooth blending ops | `sdf/blend.tur` | Pending |
| Repetition patterns | `sdf/repeat.tur` | Pending |
| Orbit camera | `raylib/camera.tur` | Pending |
| Documentation | `docs/` | Pending |
| Tests | `tests/` | Pending |

**Deliverable:** Complete library ready for use.

---

## Performance Considerations

### CPU vs GPU Tradeoffs

| Operation | CPU | GPU |
|-----------|-----|-----|
| SDF evaluation (preview) | Slow (10-100 FPS for simple scenes) | Fast (60+ FPS) |
| Mesh extraction | Required (for export) | Optional (for high-res) |
| Memory | Lower | Higher (textures, FBOs) |

**Recommendation:**
- Preview: GPU ray marching (interactive)
- Export: CPU mesh extraction (accurate, supports all features)
- Future: GPU mesh extraction (for very high resolutions)

### Quality vs Speed Tradeoffs

| Mesh Quality | Resolution | Time (CPU) | Time (GPU) |
|--------------|------------|------------|-------------|
| Draft | 64³ | <100ms | ~10ms |
| Medium | 128³ | ~500ms | ~50ms |
| High | 256³ | ~4s | ~200ms |
| Ultra | 512³ | ~30s | ~2s |

**Recommendation:**
- Preview: 128³ or adaptive (lower near camera)
- Export: 256³ - 512³ depending on use case

---

## References

1. [Modeling with Distance Functions](http://iquilezles.org/www/articles/distfunctions/distfunctions.htm) - Inigo Quilez SDF cookbook
2. [Dual Contouring of Hermite Data](https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf) - Original dual contouring paper
3. [raylib](https://www.raylib.com/) - Simple and easy-to-use library for game development
4. [libfive Scheme API](https://libfive.com/stdlib/) - Reference for CSG operations
5. [Computer Graphics StackExchange - SDF to Mesh](https://computergraphics.stackexchange.com/a/7516) - Multi-pass GLSL approach
6. [SDF Mesher](https://sketchfab.com/Aiekick) - Commercial product demonstrating the technique
7. [CGAL Mesh_3](https://doc.cgal.org/latest/Mesh_3/index.html) - Alternative meshing approach
8. [manifold3d](https://github.com/elalish/manifold) - Mesh-based CSG library

---

## Open Questions

1. **Color representation**: Per-vertex vs per-face vs texture atlas?
   - Recommendation: Per-vertex for simplicity; texture atlas for advanced use

2. **UV coordinates**: How to generate meaningful UVs for SDF surfaces?
   - Options: Planar projection, spherical projection, automatic unwrapping

3. **Level of Detail**: Should we support adaptive resolution?
   - Yes, for preview: lower res near camera, higher res farther away

4. **Animation**: Should SDFs be animatable (time-dependent)?
   - Future consideration; add `time :float` parameter to SDF functions

5. **2D support**: Should we support 2D SDFs for SVG export?
   - Yes, useful for laser cutting and documentation

---

## Decision: Proceed with This Approach

✅ **Solves the color problem** - fundamental limitation of libfive
✅ **Native Turmeric** - no alien DSL, leverages existing strengths
✅ **GPU-accelerated** - interactive previews using raylib
✅ **SDF-based** - exact CSG, infinite resolution, easy to compose
✅ **Export-ready** - colored meshes for texturing pipelines

**Next Step:** Implement Phase 1 (CPU SDF + Marching Cubes) to validate the approach, then proceed to coloring and GPU acceleration.
