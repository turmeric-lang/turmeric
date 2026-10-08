# OpenGL Spice for Turmeric

> **Status:** Draft Plan
> **Last Updated:** 2026-05-22
> **Type:** Spice Design + Tutorial + Example Project

---

## Overview

`tur-opengl` is a Turmeric spice that wraps OpenGL 3.3 Core Profile together
with GLFW (windowing and input) and GLAD (OpenGL function loader). It provides
type-safe bindings for the full modern OpenGL pipeline: window creation, VAO/VBO
buffer management, shader compilation and linking, texture loading, uniform
upload, and draw calls.

The spice pairs naturally with `tur-glsl`, which generates GLSL source strings
from Turmeric DSL expressions. Together they give a fully Turmeric-hosted
graphics stack with no hand-written C or shader strings required.

---

## Motivation

- **No manual FFI plumbing**: raw OpenGL calls via `extern-c` are verbose and
  error-prone; the spice exposes a clean, named API
- **Type safety**: buffer objects, shader programs, textures, and VAOs are
  distinct opaque handle types -- passing the wrong one is a compile-time error
- **GLSL integration**: `compile-glsl` from `tur-glsl` returns a `:cstr`;
  `shader-program` in this spice accepts those strings directly
- **Resource management**: `with-window`, `with-vao`, `with-program` forms
  guarantee cleanup even if the body throws
- **Minimal dependencies**: GLFW and GLAD are fetched automatically by
  `tur build` via `:cmake-deps`; no system packages needed

---

## Adding the Spice

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref opengl-v0.1.0 --subdir spices/opengl --name opengl
```

This updates `build.tur` and `tur.lock`. Then fetch and build:

```sh
tur fetch
tur build
```

---

## `build.tur` Manifest

```turmeric
(defpackage tur-opengl
  :name        "tur-opengl"
  :version     "0.1.0"
  :description "OpenGL 3.3 Core + GLFW + GLAD bindings for Turmeric"
  :license     "MIT"
  :repository  "https://github.com/turmeric-lang/turmeric-spices"

  :cmake-deps {
    "glfw" {:url     "https://github.com/glfw/glfw"
            :ref     "3.4"
            :options {:GLFW_BUILD_EXAMPLES "OFF"
                      :GLFW_BUILD_TESTS    "OFF"
                      :GLFW_BUILD_DOCS     "OFF"}}
    "glad" {:url     "https://github.com/Dav1dde/glad"
            :ref     "v2.0.6"
            :options {:GLAD_SOURCES_DIR "."
                      :GLAD_PROFILE     "core"
                      :GLAD_API         "gl:core=3.3"}}}

  :exports {
    "opengl/window"   ["make-window" "window-should-close?" "poll-events"
                       "swap-buffers" "set-clear-color" "clear"
                       "with-window"]
    "opengl/buffers"  ["make-vao" "make-vbo" "make-ebo"
                       "bind-vao" "bind-vbo" "bind-ebo"
                       "upload-vertices" "upload-indices"
                       "vertex-attrib" "with-vao"]
    "opengl/shaders"  ["compile-shader" "shader-program" "use-program"
                       "with-program"
                       "set-uniform-int" "set-uniform-float"
                       "set-uniform-vec2" "set-uniform-vec3" "set-uniform-vec4"
                       "set-uniform-mat4"]
    "opengl/textures" ["make-texture" "bind-texture" "upload-texture-rgba"
                       "set-texture-wrap" "set-texture-filter"
                       "generate-mipmaps" "active-texture"]
    "opengl/draw"     ["draw-arrays" "draw-elements"
                       "enable" "disable" "depth-test" "blending"]
    "opengl/input"    ["key-pressed?" "mouse-pos" "mouse-button-pressed?"]
  })
```

---

## API Reference

### Window Management (`opengl/window`)

```turmeric
;;; make-window -- open a GLFW window and initialise an OpenGL 3.3 Core context.
;;;
;;; Parameters:
;;;   width  -- window width in pixels
;;;   height -- window height in pixels
;;;   title  -- window title string
;;;
;;; Returns:
;;;   An opaque :window handle, or panics on failure.
;;;
;;; Example:
;;;   (make-window 800 600 "My App")  ; => :window
;;;
;;; Since: v0.1.0
(defn make-window [width :int height :int title :cstr] :window ...)

;;; with-window -- open a window for the duration of body, then destroy it.
;;;
;;; Parameters:
;;;   w      -- name bound to the :window handle
;;;   width  -- window width
;;;   height -- window height
;;;   title  -- window title
;;;   body   -- expressions evaluated with w in scope
;;;
;;; Example:
;;;   (with-window [w 800 600 "Demo"]
;;;     (render-loop w))
;;;
;;; Since: v0.1.0
(defmacro with-window [w width height title & body] ...)

(defn window-should-close? [w :window] :bool ...)
(defn poll-events          []          :void ...)
(defn swap-buffers         [w :window] :void ...)
(defn set-clear-color      [r :float g :float b :float a :float] :void ...)
(defn clear                []          :void ...)
```

### Buffers and Geometry (`opengl/buffers`)

```turmeric
;;; make-vao -- generate a Vertex Array Object.
(defn make-vao [] :vao ...)

;;; make-vbo -- generate a Vertex Buffer Object.
(defn make-vbo [] :vbo ...)

;;; make-ebo -- generate an Element Buffer Object (index buffer).
(defn make-ebo [] :ebo ...)

;;; upload-vertices -- upload a float array to the currently-bound VBO.
;;;
;;; Parameters:
;;;   data   -- flat :float array of vertex data
;;;   size   -- byte size of data
;;;   usage  -- GL_STATIC_DRAW / GL_DYNAMIC_DRAW / GL_STREAM_DRAW
;;;
;;; Example:
;;;   (upload-vertices verts (* 9 4) :static-draw)
;;;
;;; Since: v0.1.0
(defn upload-vertices [data :ptr size :int usage :draw-usage] :void ...)

;;; vertex-attrib -- describe a vertex attribute layout.
;;;
;;; Parameters:
;;;   index      -- attribute index (matches layout location in GLSL)
;;;   size       -- number of components (1-4)
;;;   type       -- component type (:float :int :uint)
;;;   normalized -- normalise integer values to [-1,1] or [0,1]
;;;   stride     -- byte distance between consecutive vertices
;;;   offset     -- byte offset of this attribute within the stride
;;;
;;; Example:
;;;   (vertex-attrib 0 3 :float false (* 5 4) 0)
;;;
;;; Since: v0.1.0
(defn vertex-attrib [index :int size :int type :attrib-type
                     normalized :bool stride :int offset :int] :void ...)

(defmacro with-vao [v & body] ...)
```

### Shader Compilation (`opengl/shaders`)

```turmeric
;;; compile-shader -- compile a single shader stage.
;;;
;;; Parameters:
;;;   stage  -- :vertex :fragment :compute :geometry
;;;   source -- GLSL source string (e.g. from compile-glsl)
;;;
;;; Returns:
;;;   Opaque :shader handle; panics with the driver error log on failure.
;;;
;;; Example:
;;;   (compile-shader :vertex vert-src)  ; => :shader
;;;
;;; Since: v0.1.0
(defn compile-shader [stage :shader-stage source :cstr] :shader ...)

;;; shader-program -- link compiled shaders into a program.
;;;
;;; Parameters:
;;;   shaders -- one or more :shader handles
;;;
;;; Returns:
;;;   Opaque :program handle; panics with the linker error log on failure.
;;;   The individual :shader objects are deleted after linking.
;;;
;;; Example:
;;;   (shader-program vs fs)  ; => :program
;;;
;;; Since: v0.1.0
(defn shader-program [& shaders] :program ...)

(defn use-program        [p :program]          :void ...)
(defmacro with-program   [p & body]            ...)

(defn set-uniform-int    [p :program name :cstr v :int]    :void ...)
(defn set-uniform-float  [p :program name :cstr v :float]  :void ...)
(defn set-uniform-vec2   [p :program name :cstr x :float y :float] :void ...)
(defn set-uniform-vec3   [p :program name :cstr x :float y :float z :float] :void ...)
(defn set-uniform-vec4   [p :program name :cstr x :float y :float z :float w :float] :void ...)
(defn set-uniform-mat4   [p :program name :cstr mat :ptr]  :void ...)
```

### Textures (`opengl/textures`)

```turmeric
(defn make-texture          []                             :texture ...)
(defn bind-texture          [t :texture]                  :void    ...)
(defn upload-texture-rgba   [data :ptr w :int h :int]     :void    ...)
(defn set-texture-wrap      [s :wrap-mode t :wrap-mode]   :void    ...)
(defn set-texture-filter    [min :filter-mode mag :filter-mode] :void ...)
(defn generate-mipmaps      []                             :void    ...)
(defn active-texture        [unit :int]                    :void    ...)
```

### Draw Calls (`opengl/draw`)

```turmeric
(defn draw-arrays   [mode :draw-mode first :int count :int]          :void ...)
(defn draw-elements [mode :draw-mode count :int type :index-type]    :void ...)

(defn enable     [cap :gl-cap] :void ...)
(defn disable    [cap :gl-cap] :void ...)
(defn depth-test []             :void ...)   ; enables GL_DEPTH_TEST
(defn blending   []             :void ...)   ; enables GL_BLEND with alpha
```

---

## Tutorial: Hello Triangle

### Step 1 -- Open a Window

```turmeric
(import opengl/window :refer [with-window window-should-close? poll-events
                               swap-buffers set-clear-color clear])

(defn main [] :int
  (with-window [w 800 600 "Hello Triangle"]
    (set-clear-color 0.1 0.1 0.1 1.0)
    (loop-while (not (window-should-close? w))
      (clear)
      (swap-buffers w)
      (poll-events)))
  0)
```

### Step 2 -- Upload Geometry

```turmeric
(import opengl/buffers :refer [make-vao make-vbo bind-vao bind-vbo
                                upload-vertices vertex-attrib with-vao])

;;; Triangle vertices: x y z (three points, nine floats)
(def triangle-verts
  (float-array [-0.5 -0.5 0.0
                 0.5 -0.5 0.0
                 0.0  0.5 0.0]))

(defn setup-triangle [] :vao
  (let [vao (make-vao)
        vbo (make-vbo)]
    (bind-vao vao)
    (bind-vbo vbo)
    (upload-vertices triangle-verts (* 9 4) :static-draw)
    ;; attribute 0: 3 floats, stride = 12 bytes, offset = 0
    (vertex-attrib 0 3 :float false 12 0)
    (bind-vao nil)
    vao))
```

### Step 3 -- Compile Shaders with tur-glsl

```turmeric
(import glsl/shaders  :refer [glsl-vertex-shader glsl-fragment-shader])
(import glsl/builtins :refer [vec4])
(import glsl/codegen  :refer [compile-glsl])
(import opengl/shaders :refer [compile-shader shader-program])

(defn triangle-vert [] :cstr
  (compile-glsl
    (glsl-vertex-shader "330 core"
      :inputs [[aPos :vec3 0]]
      :main
      (glsl-set! gl-Position (vec4 aPos 1.0)))))

(defn triangle-frag [] :cstr
  (compile-glsl
    (glsl-fragment-shader "330 core"
      :outputs [[FragColor :vec4]]
      :main
      (glsl-set! FragColor (vec4 1.0 0.5 0.2 1.0)))))

(defn make-triangle-program [] :program
  (shader-program
    (compile-shader :vertex   (triangle-vert))
    (compile-shader :fragment (triangle-frag))))
```

### Step 4 -- Render Loop

```turmeric
(import opengl/draw :refer [draw-arrays depth-test])

(defn main [] :int
  (with-window [w 800 600 "Hello Triangle"]
    (let [vao (setup-triangle)
          prog (make-triangle-program)]
      (set-clear-color 0.1 0.1 0.1 1.0)
      (loop-while (not (window-should-close? w))
        (clear)
        (with-program prog
          (bind-vao vao)
          (draw-arrays :triangles 0 3))
        (swap-buffers w)
        (poll-events))))
  0)
```

---

## Tutorial: Textured Quad

Building on the triangle, add a texture loaded from a file.

```turmeric
(import opengl/textures :refer [make-texture bind-texture upload-texture-rgba
                                 set-texture-wrap set-texture-filter
                                 generate-mipmaps active-texture])

;;; Upload raw RGBA pixel data (32x32 checkerboard)
(defn checkerboard-texture [] :texture
  (let [t    (make-texture)
        size 32
        data (make-bytes (* size size 4))]
    (for [y :int 0] (< y size) (c-inc! y)
      (for [x :int 0] (< x size) (c-inc! x)
        (let [i  (* (+ (* y size) x) 4)
              on (= (% (+ x y) 2) 0)]
          (bytes-set! data (+ i 0) (if on 255 0))
          (bytes-set! data (+ i 1) (if on 255 0))
          (bytes-set! data (+ i 2) (if on 255 0))
          (bytes-set! data (+ i 3) 255))))
    (bind-texture t)
    (upload-texture-rgba data size size)
    (set-texture-wrap :repeat :repeat)
    (set-texture-filter :linear-mipmap-linear :linear)
    (generate-mipmaps)
    t))
```

Add a `TexCoord` attribute and pass `texture1` uniform:

```turmeric
(defn quad-vert [] :cstr
  (compile-glsl
    (glsl-vertex-shader "330 core"
      :inputs  [[aPos :vec3 0] [aTexCoord :vec2 1]]
      :outputs [[TexCoord :vec2]]
      :main
      (glsl-let [TexCoord :vec2 aTexCoord
                 gl-Position :vec4 (vec4 aPos 1.0)]))))

(defn quad-frag [] :cstr
  (compile-glsl
    (glsl-fragment-shader "330 core"
      :inputs   [[TexCoord :vec2]]
      :uniforms [[texture1 :sampler-2d]]
      :outputs  [[FragColor :vec4]]
      :main
      (glsl-set! FragColor (texture texture1 TexCoord)))))

(defn render-textured [w vao prog tex]
  (loop-while (not (window-should-close? w))
    (clear)
    (active-texture 0)
    (bind-texture tex)
    (with-program prog
      (set-uniform-int prog "texture1" 0)
      (bind-vao vao)
      (draw-elements :triangles 6 :unsigned-int))
    (swap-buffers w)
    (poll-events)))
```

---

## Integration with `tur-glsl`

The two spices are designed to be used together but are kept independent so
that `tur-glsl` can be used with Vulkan, WebGL, or offline shader toolchains
without pulling in GLFW.

Recommended import pattern:

```turmeric
;; Shader authoring
(import glsl/shaders  :refer [glsl-vertex-shader glsl-fragment-shader])
(import glsl/builtins :refer [vec2 vec3 vec4 mat4 normalize dot mix texture])
(import glsl/codegen  :refer [compile-glsl])

;; OpenGL runtime
(import opengl/window  :refer [with-window window-should-close?
                                poll-events swap-buffers
                                set-clear-color clear])
(import opengl/buffers :refer [make-vao make-vbo bind-vao bind-vbo
                                upload-vertices vertex-attrib with-vao])
(import opengl/shaders :refer [compile-shader shader-program
                                use-program with-program set-uniform-mat4])
(import opengl/draw    :refer [draw-arrays draw-elements depth-test])
```

---

## Example Project: Spinning Cube

A self-contained project using both spices.

### File Layout

```
examples/opengl-cube/
  main.tur       -- render loop and setup
  shaders.tur    -- vertex and fragment shader definitions (tur-glsl)
  geometry.tur   -- cube vertices and indices
  README.md
```

### `shaders.tur`

```turmeric
(import glsl/shaders  :refer [glsl-vertex-shader glsl-fragment-shader])
(import glsl/builtins :refer [vec3 vec4 mat4])
(import glsl/codegen  :refer [compile-glsl])

(defn cube-vert-src [] :cstr
  (compile-glsl
    (glsl-vertex-shader "330 core"
      :inputs   [[aPos :vec3 0] [aNormal :vec3 1]]
      :outputs  [[FragPos :vec3] [Normal :vec3]]
      :uniforms [[model :mat4] [view :mat4] [projection :mat4]
                 [normal-matrix :mat4]]
      :main
      (glsl-let [FragPos :vec3 (vec3 (* model (vec4 aPos 1.0)))
                 Normal  :vec3 (vec3 (* normal-matrix (vec4 aNormal 0.0)))
                 gl-Position :vec4 (* projection view (vec4 FragPos 1.0))]))))

(defn cube-frag-src [] :cstr
  (compile-glsl
    (glsl-fragment-shader "330 core"
      :inputs  [[FragPos :vec3] [Normal :vec3]]
      :outputs [[FragColor :vec4]]
      :uniforms [[light-pos :vec3] [light-color :vec3] [object-color :vec3]]
      :main
      (glsl-let [norm      :vec3  (normalize Normal)
                 light-dir :vec3  (normalize (- light-pos FragPos))
                 diff      :float (max (dot norm light-dir) 0.0)
                 diffuse   :vec3  (* diff light-color)
                 ambient   :vec3  (* 0.1 light-color)
                 result    :vec3  (* (+ ambient diffuse) object-color)
                 FragColor :vec4  (vec4 result 1.0)]))))
```

### `main.tur`

```turmeric
(import opengl/window  :refer [with-window window-should-close?
                                poll-events swap-buffers
                                set-clear-color clear])
(import opengl/buffers :refer [make-vao make-vbo make-ebo bind-vao bind-vbo
                                bind-ebo upload-vertices upload-indices
                                vertex-attrib with-vao])
(import opengl/shaders :refer [compile-shader shader-program with-program
                                set-uniform-vec3 set-uniform-mat4])
(import opengl/draw    :refer [draw-elements depth-test])
(import shaders :refer [cube-vert-src cube-frag-src])
(import geometry :refer [cube-vertices cube-indices])

(defn main [] :int
  (with-window [w 800 600 "Spinning Cube"]
    (depth-test)
    (let [vao  (make-vao)
          vbo  (make-vbo)
          ebo  (make-ebo)
          prog (shader-program
                 (compile-shader :vertex   (cube-vert-src))
                 (compile-shader :fragment (cube-frag-src)))]
      (bind-vao vao)
      (bind-vbo vbo)
      (upload-vertices cube-vertices (sizeof cube-vertices) :static-draw)
      (bind-ebo ebo)
      (upload-indices cube-indices (sizeof cube-indices) :static-draw)
      ;; position attrib (location 0): 3 floats, stride 24, offset 0
      (vertex-attrib 0 3 :float false 24 0)
      ;; normal attrib  (location 1): 3 floats, stride 24, offset 12
      (vertex-attrib 1 3 :float false 24 12)
      (set-clear-color 0.1 0.1 0.1 1.0)
      (let [angle (ref 0.0)]
        (loop-while (not (window-should-close? w))
          (clear)
          (ref-set! angle (+ (ref-get angle) 0.01))
          (let [model      (rotate-y-mat4 (ref-get angle))
                view       (translate-mat4 [0.0 0.0 -5.0])
                projection (perspective-mat4 0.785 (/ 800.0 600.0) 0.1 100.0)]
            (with-program prog
              (set-uniform-mat4 prog "model"      (mat4-ptr model))
              (set-uniform-mat4 prog "view"       (mat4-ptr view))
              (set-uniform-mat4 prog "projection" (mat4-ptr projection))
              (set-uniform-mat4 prog "normal-matrix" (mat4-ptr (normal-mat4 model)))
              (set-uniform-vec3 prog "light-pos"    1.5 1.5 1.5)
              (set-uniform-vec3 prog "light-color"  1.0 1.0 1.0)
              (set-uniform-vec3 prog "object-color" 0.6 0.3 0.8)
              (bind-vao vao)
              (draw-elements :triangles 36 :unsigned-int)))
          (swap-buffers w)
          (poll-events)))))
  0)
```

Run it:

```sh
tur run examples/opengl-cube/main.tur
```

---

## FFI Backend

The spice wraps GLFW and glad via a thin C shim. Core bindings (abbreviated):

```turmeric
;; src/ffi.tur
(include-c "glad/gl.h")
(include-c "GLFW/glfw3.h")

(extern-c glfwInit           []       :int)
(extern-c glfwTerminate      []       :void)
(extern-c glfwCreateWindow   [:int :int :cstr :ptr :ptr] :ptr)
(extern-c glfwDestroyWindow  [:ptr]   :void)
(extern-c glfwWindowShouldClose [:ptr] :int)
(extern-c glfwSwapBuffers    [:ptr]   :void)
(extern-c glfwPollEvents     []       :void)
(extern-c glfwMakeContextCurrent [:ptr] :void)

(extern-c gladLoadGL         [:ptr]   :int)

(extern-c glClearColor [:float :float :float :float] :void)
(extern-c glClear      [:int]  :void)
(extern-c glGenVertexArrays [:int :ptr] :void)
(extern-c glGenBuffers      [:int :ptr] :void)
(extern-c glBindVertexArray [:int] :void)
(extern-c glBindBuffer      [:int :int] :void)
(extern-c glBufferData      [:int :int :ptr :int] :void)
(extern-c glVertexAttribPointer [:int :int :int :int :int :ptr] :void)
(extern-c glEnableVertexAttribArray [:int] :void)
(extern-c glCreateShader    [:int] :int)
(extern-c glShaderSource    [:int :int :ptr :ptr] :void)
(extern-c glCompileShader   [:int] :void)
(extern-c glCreateProgram   [] :int)
(extern-c glAttachShader    [:int :int] :void)
(extern-c glLinkProgram     [:int] :void)
(extern-c glUseProgram      [:int] :void)
(extern-c glGetUniformLocation [:int :cstr] :int)
(extern-c glUniform1i       [:int :int] :void)
(extern-c glUniform3f       [:int :float :float :float] :void)
(extern-c glUniformMatrix4fv [:int :int :int :ptr] :void)
(extern-c glDrawArrays      [:int :int :int] :void)
(extern-c glDrawElements    [:int :int :int :ptr] :void)
(extern-c glEnable          [:int] :void)
```

Higher-level wrappers provide error checking and resource handle types:

```turmeric
;; src/shaders.tur
(defn compile-shader [stage :shader-stage source :cstr] :shader
  (let [gl-stage (shader-stage->gl stage)
        id       (glCreateShader gl-stage)]
    (glShaderSource id 1 (ptr-to source) (nil-value))
    (glCompileShader id)
    (let [ok (get-shader-iv id GL_COMPILE_STATUS)]
      (when (= ok 0)
        (let [log (get-shader-info-log id)]
          (panic (str "shader compile error:\n" log)))))
    (shader id)))
```

---

## Spice Layout inside turmeric-spices

```
spices/opengl/
  build.tur
  tur.lock
  src/
    window.tur    -- GLFW window, context, and event loop
    buffers.tur   -- VAO, VBO, EBO creation and upload
    shaders.tur   -- shader compilation, program linking, uniforms
    textures.tur  -- texture creation and upload
    draw.tur      -- draw calls and GL state toggles
    input.tur     -- keyboard and mouse query
    types.tur     -- opaque handle types (:window :vao :vbo :shader :program ...)
    ffi.tur       -- raw extern-c bindings to GLFW and OpenGL
    math.tur      -- mat4 helpers (perspective, rotate, translate, normal matrix)
  tests/
    window_test.tur
    shaders_test.tur
    buffers_test.tur
```

---

## Future Enhancements

1. **Framebuffers and render targets**: off-screen rendering for post-processing
2. **Uniform Buffer Objects (UBOs)**: shared uniform blocks across programs
3. **Instanced rendering**: `draw-arrays-instanced` / `draw-elements-instanced`
4. **Compute shaders**: dispatch compute work and read back results
5. **Vulkan spice** (`tur-vulkan`): lower-level companion spice targeting Vulkan 1.3
6. **WebGL via WASM**: share shader code with the Turmeric web REPL
7. **`tur-imgui` spice**: Dear ImGui bindings for in-app debug panels

---

## References

- [OpenGL 3.3 Core Reference](https://www.khronos.org/opengl/wiki/OpenGL_3.3)
- [GLFW documentation](https://www.glfw.org/docs/latest/)
- [glad generator](https://gen.glad.sh/)
- [learnopengl.com](https://learnopengl.com/) -- the authoritative modern OpenGL tutorial series
- [tur-glsl plan](glsl-dsl-plan.md) -- companion DSL for authoring GLSL shaders in Turmeric
