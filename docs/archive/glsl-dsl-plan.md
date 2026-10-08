# GLSL Shader DSL for Turmeric

## Overview

Design a domain-specific language (DSL) embedded in Turmeric for writing GLSL (OpenGL Shading Language) shaders. This DSL provides a Lisp-like syntax for shader authoring while maintaining direct correspondence with GLSL semantics. The DSL compiles to GLSL source code, which can then be passed to OpenGL/Vulkan/Metal APIs via Turmeric's FFI.

## Motivation

- **Type Safety**: Leverage Turmeric's type system to catch shader errors at compile time
- **Metaprogramming**: Use Turmeric macros to generate shader variants
- **Code Reuse**: Share shader logic between Turmeric and GLSL
- **Inline Shaders**: Embed shader code directly in Turmeric source files
- **Composition**: Build complex shaders from reusable components

## DSL Design

### Core Principles

1. **Direct GLSL Mapping**: Each DSL construct maps 1:1 to GLSL concepts
2. **Minimal Abstraction**: Stay close to GLSL semantics for predictability
3. **Type Alignment**: Turmeric types correspond to GLSL types
4. **Compilation Target**: Output standard GLSL source code (v1.00+)

### Type Correspondence

| Turmeric Type | GLSL Type | Notes |
|---------------|-----------|-------|
| `:float` | `float` | 32-bit floating point |
| `:double` | `double` | 64-bit floating point (if supported) |
| `:int` | `int` | Signed integer |
| `:uint` | `uint` | Unsigned integer |
| `:bool` | `bool` | Boolean |
| `[:float 2]` | `vec2` | 2-component vector |
| `[:float 3]` | `vec3` | 3-component vector |
| `[:float 4]` | `vec4` | 4-component vector |
| `[:int 2]` | `ivec2` | 2-component integer vector |
| `[:int 3]` | `ivec3` | 3-component integer vector |
| `[:int 4]` | `ivec4` | 4-component integer vector |
| `[:uint 2]` | `uvec2` | 2-component unsigned vector |
| `[:uint 3]` | `uvec3` | 3-component unsigned vector |
| `[:uint 4]` | `uvec4` | 4-component unsigned vector |
| `[:bool 2]` | `bvec2` | 2-component boolean vector |
| `[:bool 3]` | `bvec3` | 3-component boolean vector |
| `[:bool 4]` | `bvec4` | 4-component boolean vector |
| `[:float 2 2]` | `mat2` | 2x2 matrix |
| `[:float 3 3]` | `mat3` | 3x3 matrix |
| `[:float 4 4]` | `mat4` | 4x4 matrix |
| `[:float 3 2]` | `mat2x3` | 2x3 matrix |
| `[:float 4 3]` | `mat3x4` | 3x4 matrix |
| `:sampler-2d` | `sampler2D` | 2D texture sampler |
| `:sampler-3d` | `sampler3D` | 3D texture sampler |
| `:sampler-cube` | `samplerCube` | Cube map sampler |

### Syntax Mapping

#### Variable Declaration

**GLSL:**
```glsl
float x = 1.0;
vec3 color = vec3(1.0, 0.0, 0.0);
```

**DSL:**
```lisp
(glsl-let [x :float 1.0]
          [color :vec3 (vec3 1.0 0.0 0.0)])
```

#### Functions

**GLSL:**
```glsl
float square(float x) {
    return x * x;
}
```

**DSL:**
```lisp
(glsl-defn square [x :float] :float
  (* x x))
```

#### Control Flow

**GLSL:**
```glsl
if (x > 0.0) {
    y = 1.0;
} else {
    y = -1.0;
}
```

**DSL:**
```lisp
(glsl-if (> x 0.0)
  (glsl-set! y 1.0)
  (glsl-set! y -1.0))
```

**GLSL:**
```glsl
for (int i = 0; i < 10; i++) {
    // loop body
}
```

**DSL:**
```lisp
(glsl-for [i :int 0] (< i 10) (glsl-inc! i)
  ;; loop body
  )
```

#### Vector/Matrix Operations

**GLSL Built-ins:**
```glsl
vec3 a = vec3(1.0);
vec3 b = vec3(2.0);
vec3 c = a + b;
float d = dot(a, b);
vec3 e = cross(a, b);
vec3 f = normalize(a);
float g = length(a);
```

**DSL:**
```lisp
(glsl-let [a :vec3 (vec3 1.0)]
          [b :vec3 (vec3 2.0)]
          [c :vec3 (+ a b)]
          [d :float (dot a b)]
          [e :vec3 (cross a b)]
          [f :vec3 (normalize a)]
          [g :float (length a)])
```

#### Swizzling

**GLSL:**
```glsl
vec4 color = vec4(1.0, 0.5, 0.25, 1.0);
vec3 rgb = color.rgb;
float alpha = color.a;
vec2 rg = color.rg;
```

**DSL:**
```lisp
(glsl-let [color :vec4 (vec4 1.0 0.5 0.25 1.0)]
          [rgb :vec3 (swizzle color :rgb)]
          [alpha :float (swizzle color :a)]
          [rg :vec2 (swizzle color :rg)])
```

### Shader Stage Definitions

#### Vertex Shader

**GLSL:**
```glsl
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

out vec3 ourColor;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    gl_Position = projection * view * model * vec4(aPos, 1.0);
    ourColor = aColor;
}
```

**DSL:**
```lisp
(glsl-vertex-shader "330 core"
  :inputs [[aPos :vec3 0]
           [aColor :vec3 1]]
  :outputs [[ourColor :vec3]]
  :uniforms [[model :mat4]
             [view :mat4]
             [projection :mat4]]
  :main
  (glsl-let [gl-Position :vec4 (* projection view model (vec4 aPos 1.0))]
            [ourColor :vec3 aColor]))
```

#### Fragment Shader

**GLSL:**
```glsl
#version 330 core

in vec3 ourColor;
out vec4 FragColor;

uniform sampler2D texture1;
uniform sampler2D texture2;

void main() {
    vec4 texColor = mix(texture(texture1, TexCoords), 
                        texture(texture2, TexCoords), 0.2);
    FragColor = mix(texColor, vec4(ourColor, 1.0), 0.5);
}
```

**DSL:**
```lisp
(glsl-fragment-shader "330 core"
  :inputs [[ourColor :vec3]
           [TexCoords :vec2]]
  :outputs [[FragColor :vec4]]
  :uniforms [[texture1 :sampler-2d]
             [texture2 :sampler-2d]]
  :main
  (glsl-let [texColor :vec4 (mix (texture texture1 TexCoords)
                                (texture texture2 TexCoords)
                                0.2)]
            [FragColor :vec4 (mix texColor (vec4 ourColor 1.0) 0.5)]))
```

#### Compute Shader

**GLSL:**
```glsl
#version 450 core

layout(local_size_x = 10, local_size_y = 10, local_size_z = 1) in;

layout(binding = 0) buffer Data {
    float data[];
};

void main() {
    uint idx = gl_GlobalInvocationID.x;
    data[idx] *= 2.0;
}
```

**DSL:**
```lisp
(glsl-compute-shader "450 core"
  :work-group [10 10 1]
  :buffers [[Data :buffer 0 [float]]]
  :main
  (glsl-let [idx :uint (gl-GlobalInvocationID :x)]
            [{} (buffer-set! Data idx (* (buffer-get Data idx) 2.0))]))
```

### Built-in Functions

The DSL provides wrappers for all GLSL built-in functions:

#### Math Functions

```lisp
;; Trigonometry
(sin x) (cos x) (tan x)
(asin x) (acos x) (atan x)
(atan y x)  ; two-argument atan

;; Exponential
(exp x) (log x) (exp2 x) (log2 x)
(sqrt x) (inversesqrt x)

;; Common
(abs x) (sign x) (floor x) (ceil x) (fract x)
(mod x y) (modf x) (min x y) (max x y) (clamp x min max)
(mix x y a) (step edge x) (smoothstep edge1 edge2 x)
```

#### Vector Functions

```lisp
;; Component-wise
(dot x y) (cross x y) (distance x y)
(length x) (normalize x) (faceforward n i nref)
(reflect i n) (refract i n eta)

;; Vector construction
(vec2 x) (vec2 x y)
(vec3 x) (vec3 x y z)
(vec4 x) (vec4 x y z w)
```

#### Matrix Functions

```lisp
(mat2 ...) (mat3 ...) (mat4 ...)
(matrixCompMult x y)  ; component-wise multiplication
(transpose m) (determinant m) (inverse m)
```

#### Texture Functions

```lisp
(texture sampler coord) (texture sampler coord bias)
(textureLod sampler coord lod)
(textureProj sampler coord)
(textureGrad sampler coord dPdx dPdy)

;; For GLSL 1.30+
(texture2D sampler coord)
(textureCube sampler coord)
```

#### Derivative Functions

```lisp
(dFdx x) (dFdy x)
(fwidth x)
```

#### Noise Functions

```lisp
(noise1 x) (noise2 x) (noise3 x) (noise4 x)
```

### Uniform Blocks and Storage Blocks

**GLSL:**
```glsl
uniform Matrices {
    mat4 model;
    mat4 view;
    mat4 projection;
};
```

**DSL:**
```lisp
(glsl-uniform-block Matrices
  [model :mat4]
  [view :mat4]
  [projection :mat4])
```

### Including Other Shaders

```lisp
;; Include a common utilities shader
(glsl-include "common.glsl")

;; Or define reusable components in Turmeric
(defn lighting-functions []
  (glsl-defn calculate-light [pos norm color lightPos lightColor] :vec3
    ;; ... light calculation
    ))
```

## Compilation to GLSL

The DSL compiler translates Turmeric DSL expressions to GLSL source code:

```lisp
(defn compile-glsl [dsl-expr] :string
  ;; Returns GLSL source code as a string
  )

;; Usage
(defn create-shader-program []
  (let [vertex-src (compile-glsl my-vertex-shader-dsl)
        frag-src (compile-glsl my-fragment-shader-dsl)]
    ;; Pass to OpenGL API via FFI
    (let [vertex-shader (glCreateShader GL_VERTEX_SHADER)
          fragment-shader (glCreateShader GL_FRAGMENT_SHADER)]
      (glShaderSource vertex-shader vertex-src)
      (glShaderSource fragment-shader frag-src)
      ;; ... compile and link
      )))
```

## Integration with Turmeric

### Inline GLSL in Turmeric

For simple cases where you want to inline GLSL directly:

```lisp
;; Raw GLSL string
(defn my-fragment []
  (inline-glsl "
    void main() {
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
    }
  "))
```

### DSL Macros

The DSL can be extended via Turmeric macros:

```lisp
;; Define a macro for a common pattern
(defmacro with-lighting [light & body]
  `(glsl-let [lightDir :vec3 (normalize ~light)]
              [lightColor :vec3 (vec3 1.0 1.0 1.0)]
     ,@body))

;; Usage
(glsl-defn shade [pos norm] :vec3
  (with-lighting (vec3 0.0 1.0 0.0)
    (calculate-diffuse lightDir norm)))
```

### Type Checking

The DSL leverages Turmeric's type system to validate shader code:

```lisp
;; This will cause a type error
(glsl-defn bad-add [a :vec2] :vec2
  (+ a 1.0))  ; Error: cannot add vec2 and float

;; Correct version
(glsl-defn good-add [a :vec2 b :vec2] :vec2
  (+ a b))
```

## Tutorial: Creating a Simple Shader

### Step 1: Basic Vertex Shader

Let's create a simple pass-through vertex shader:

```lisp
(defn simple-vertex []
  (glsl-vertex-shader "330 core"
    :inputs [[aPos :vec3 0]]
    :uniforms [[model :mat4]
               [view :mat4]
               [projection :mat4]]
    :main
    (glsl-let [gl-Position :vec4 (* projection view model (vec4 aPos 1.0))])))
```

This compiles to:

```glsl
#version 330 core

layout(location = 0) in vec3 aPos;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    gl_Position = projection * view * model * vec4(aPos, 1.0);
}
```

### Step 2: Simple Fragment Shader

Now let's create a fragment shader that outputs a solid color:

```lisp
(defn solid-color-fragment [r g b] :string
  (compile-glsl
    (glsl-fragment-shader "330 core"
      :outputs [[FragColor :vec4]]
      :main
      (glsl-set! FragColor (vec4 r g b 1.0)))))

;; Usage
(defn create-red-shader []
  (let [frag-src (solid-color-fragment 1.0 0.0 0.0)]
    ;; Use frag-src with OpenGL API
    ))
```

### Step 3: Adding Vertex Colors

Let's extend both shaders to pass vertex colors:

```lisp
(defn colored-vertex []
  (glsl-vertex-shader "330 core"
    :inputs [[aPos :vec3 0]
             [aColor :vec3 1]]
    :outputs [[ourColor :vec3]]
    :uniforms [[model :mat4]
               [view :mat4]
               [projection :mat4]]
    :main
    (glsl-let [gl-Position :vec4 (* projection view model (vec4 aPos 1.0))]
              [ourColor :vec3 aColor])))

(defn colored-fragment []
  (glsl-fragment-shader "330 core"
    :inputs [[ourColor :vec3]]
    :outputs [[FragColor :vec4]]
    :main
    (glsl-set! FragColor (vec4 ourColor 1.0))))
```

### Step 4: Adding Lighting

Now let's add simple diffuse lighting:

```lisp
(defn lit-fragment [light-pos :vec3 light-color :vec3]
  (glsl-fragment-shader "330 core"
    :inputs [[ourColor :vec3]
             [FragPos :vec3]
             [Normal :vec3]]
    :uniforms [[viewPos :vec3]]
    :outputs [[FragColor :vec4]]
    :main
    (glsl-let [;; Calculate light direction
                lightDir :vec3 (normalize (- light-pos FragPos))
                
                ;; Diffuse shading
                norm :vec3 (normalize Normal)
                diff :float (max (dot norm lightDir) 0.0)
                diffuse :vec3 (* diff light-color)
                
                ;; Final color
                result :vec3 (* diffuse ourColor)
                
                FragColor :vec4 (vec4 result 1.0)])))
```

### Step 5: Using the Shader in Turmeric

```lisp
(defn create-shader-program [vertex-dsl fragment-dsl] :uint
  (let [vertex-src (compile-glsl vertex-dsl)
        frag-src (compile-glsl fragment-dsl)]
    
    (let [program (glCreateProgram)]
      (let [vs (compile-shader GL_VERTEX_SHADER vertex-src)
            fs (compile-shader GL_FRAGMENT_SHADER frag-src)]
        (glAttachShader program vs)
        (glAttachShader program fs)
        (glLinkProgram program)
        (glDeleteShader vs)
        (glDeleteShader fs)
        program))))

(defn main [] :int
  (let [program (create-shader-program (colored-vertex) (colored-fragment))]
    (glUseProgram program)
    ;; ... render loop
    0))
```

## Advanced Features

### Shader Variants

Use macros to generate multiple variants of a shader:

```lisp
(defmacro with-fog [fog-color fog-density & body]
  (if fog-color
    `(glsl-let [fog-factor :float (exp (- (* (length gl-FragCoord.xyz) ~fog-density)))]
                [fog-color :vec3 ~fog-color]
       (glsl-let [final-color :vec3 (mix (progn ,@body) fog-color fog-factor)]
         (glsl-set! FragColor (vec4 final-color 1.0))))
    `(progn ,@body)))

;; Generate two versions
(defn fragment-with-fog []
  (with-fog (vec3 0.5 0.5 0.5) 0.01
    (vec3 1.0 0.0 0.0)))

(defn fragment-without-fog []
  (with-fog nil 0.0
    (vec3 1.0 0.0 0.0)))
```

### Geometry Shaders

```lisp
(glsl-geometry-shader "330 core"
  :input-layout :points
  :output-layout :triangle-strip
  :max-vertices 4
  :inputs [[gs-in :vec3]]
  :outputs [[gs-out :vec3]]
  :main
  (glsl-let [;; Emit 4 vertices for a quad
                {} (gl-Position :vec4 (vec4 (-0.5 -0.5 0.0) 1.0))
                {} (gs-out :vec3 (vec3 1.0 0.0 0.0))
                {} (gl-EmitVertex)
                
                {} (gl-Position :vec4 (vec4 (0.5 -0.5 0.0) 1.0))
                {} (gs-out :vec3 (vec3 0.0 1.0 0.0))
                {} (gl-EmitVertex)
                
                {} (gl-Position :vec4 (vec4 (-0.5 0.5 0.0) 1.0))
                {} (gs-out :vec3 (vec3 0.0 0.0 1.0))
                {} (gl-EmitVertex)
                
                {} (gl-Position :vec4 (vec4 0.5 0.5 0.0) 1.0)
                {} (gs-out :vec3 (vec3 1.0 1.0 0.0))
                {} (gl-EmitVertex)
                
                {} (gl-EndPrimitive)]))
```

### Tessellation Shaders

```lisp
;; Tessellation control shader
(glsl-tess-control-shader "450 core"
  :input-layout :triangles
  :output-layout :triangles
  :vertices 3
  :main
  (glsl-let [gl-TessLevelOuter :vec4 (vec4 1.0 1.0 1.0 1.0)]
            [gl-TessLevelInner :vec2 (vec2 1.0 1.0)]))

;; Tessellation evaluation shader
(glsl-tess-evaluation-shader "450 core"
  :input-layout :triangles
  :output-layout :triangles
  :spacing :equal
  :vertex-order :cw
  :main
  (glsl-let [u :float (gl-TessCoord :x)
              v :float (gl-TessCoord :y)
              w :float (gl-TessCoord :z)
              ;; Barycentric interpolation
              pos :vec3 (+ (* u gl-in[0].gl-Position)
                          (* v gl-in[1].gl-Position)
                          (* w gl-in[2].gl-Position))]
            [gl-Position :vec4 (vec4 pos 1.0)]))
```

### Preprocessor Directives

```lisp
;; Define a preprocessor macro
(glsl-define "NUM_LIGHTS" 4)

;; Conditional compilation
(glsl-ifdef "USE_SHADOWS"
  (glsl-let [shadowFactor :float (calculate-shadow)])
  (glsl-else
    (glsl-let [shadowFactor :float 1.0])))
```

## Standard Library Functions

The DSL includes a standard library of common shader operations:

```lisp
;; Phong lighting
(defn phong-lighting [normal :vec3 position :vec3 light-pos :vec3 
                      light-color :vec3 light-ambient :vec3 
                      material-diffuse :vec3 material-specular :vec3 
                      material-shininess :float 
                      view-pos :vec3] :vec3
  (glsl-let [;; Ambient
              ambient :vec3 (* light-ambient material-diffuse)
              
              ;; Diffuse
              light-dir :vec3 (normalize (- light-pos position))
              diff :float (max (dot normal light-dir) 0.0)
              diffuse :vec3 (* diff light-color material-diffuse)
              
              ;; Specular
              view-dir :vec3 (normalize (- view-pos position))
              reflect-dir :vec3 (reflect (- 0.0 light-dir) normal)
              spec :float (pow (max (dot view-dir reflect-dir) 0.0) material-shininess)
              specular :vec3 (* spec light-color material-specular)
              
              ;; Result
              result :vec3 (+ ambient (+ diffuse specular))]
    result))

;; Fresnel effect
(defn fresnel [normal :vec3 view-dir :vec3] :float
  (glsl-let [cos-theta :float (abs (dot (normalize normal) (normalize view-dir)))
              r0 :float 0.04
              result :float (+ r0 (* (- 1.0 r0) (pow (- 1.0 cos-theta) 5.0))]
    result))

;; Gamma correction
(defn gamma-correct [color :vec3 gamma :float] :vec3
  (glsl-let [inv-gamma :float (/ 1.0 gamma)]
    (mapv (fn [c] (pow c inv-gamma)) color)))
```

## Error Handling

The DSL compiler validates:

1. **Type Mismatches**: Catches operations on incompatible types
2. **Shader Stage Validity**: Ensures constructs are valid for the shader stage
3. **Variable Scoping**: Validates variable access within scope
4. **Built-in Availability**: Checks if built-in functions are available in the target GLSL version
5. **Precision Qualifiers**: Validates precision qualifier usage

## Performance Considerations

1. **Minimize Branching**: Use `mix` and `step` instead of `if` when possible
2. **Avoid Discards**: Minimize use of `discard` in fragment shaders
3. **Texture Access**: Use appropriate texture filtering and mipmapping
4. **Precision**: Use lowest sufficient precision (`lowp`, `mediump`, `highp`)
5. **Uniforms**: Group frequently-changing uniforms in uniform blocks

## Example: Complete Phong Shader

Here's a complete example with vertex and fragment shaders:

```lisp
(defn phong-vertex-shader []
  (glsl-vertex-shader "330 core"
    :inputs [[aPos :vec3 0]
             [aNormal :vec3 1]
             [aTexCoord :vec2 2]]
    :outputs [[FragPos :vec3]
              [Normal :vec3]
              [TexCoord :vec2]]
    :uniforms [[model :mat4]
               [view :mat4]
               [projection :mat4]
               [normal-matrix :mat3]]
    :main
    (glsl-let [FragPos :vec3 (vec3 (* model (vec4 aPos 1.0)))
              Normal :vec3 (* normal-matrix aNormal)
              TexCoord :vec2 aTexCoord
              gl-Position :vec4 (* projection view (vec4 FragPos 1.0))])))

(defn phong-fragment-shader [light-pos :vec3 light-color :vec3 view-pos :vec3]
  (glsl-fragment-shader "330 core"
    :inputs [[FragPos :vec3]
             [Normal :vec3]
             [TexCoord :vec2]]
    :uniforms [[material-diffuse :vec3]
               [material-specular :vec3]
               [material-shininess :float]
               [material-ambient :vec3]
               [diffuse-map :sampler-2d]
               [specular-map :sampler-2d]]
    :outputs [[FragColor :vec4]]
    :main
    (glsl-let [;; Normalize the normal
              norm :vec3 (normalize Normal)
              
              ;; Calculate lighting
              light-dir :vec3 (normalize (- light-pos FragPos))
              view-dir :vec3 (normalize (- view-pos FragPos))
              
              ;; Ambient
              ambient :vec3 (* 0.1 material-ambient)
              
              ;; Diffuse
              diff :float (max (dot norm light-dir) 0.0)
              diffuse :vec3 (* diff light-color material-diffuse)
              
              ;; Specular
              reflect-dir :vec3 (reflect (- 0.0 light-dir) norm)
              spec :float (pow (max (dot view-dir reflect-dir) 0.0) material-shininess)
              specular :vec3 (* spec light-color material-specular)
              
              ;; Texture
              tex-diffuse :vec3 (texture diffuse-map TexCoord)
              tex-specular :vec3 (texture specular-map TexCoord)
              
              ;; Combine
              diffuse-total :vec3 (* (+ ambient diffuse) tex-diffuse)
              result :vec3 (+ diffuse-total (* specular tex-specular))
              
              FragColor :vec4 (vec4 result 1.0)])))

;; Create shader program
(defn create-phong-shader [light-pos light-color view-pos]
  (let [vs (compile-glsl (phong-vertex-shader))
        fs (compile-glsl (phong-fragment-shader light-pos light-color view-pos))]
    (create-shader-program vs fs)))
```

## Future Enhancements

1. **SPIR-V Support**: Compile to SPIR-V for Vulkan
2. **Metal Shader Language**: Support for Apple's MSL
3. **HLSL**: Support for DirectX HLSL
4. **Shader Optimization**: Automatically optimize shader code
5. **Shader Hot Reloading**: Support for live shader editing
6. **Visual Shader Editor**: GUI for creating shaders visually
7. **Shader Analysis**: Static analysis for performance optimization

## Distributing as a Spice

The GLSL DSL is a pure-Turmeric library -- it only emits strings of GLSL
source code and has no C dependencies itself. The `tur-opengl` spice (which
wraps GLFW and OpenGL) is kept separate so the DSL can be used with any
graphics backend.

### Adding the spice

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref glsl-v0.1.0 --subdir spices/glsl --name glsl
```

### `build.tur` manifest

```turmeric
(defpackage tur-glsl
  :name        "tur-glsl"
  :version     "0.1.0"
  :description "Lisp-syntax DSL that compiles to GLSL shader source code"
  :license     "MIT"
  :repository  "https://github.com/turmeric-lang/turmeric-spices"

  :exports {
    "glsl/core"     ["glsl-let" "glsl-set!" "glsl-if" "glsl-for"
                     "glsl-defn" "glsl-return" "glsl-discard"]
    "glsl/types"    ["glsl-defstruct" "glsl-typedef"]
    "glsl/shaders"  ["glsl-vertex-shader" "glsl-fragment-shader"
                     "glsl-compute-shader" "glsl-geometry-shader"
                     "glsl-tess-control-shader" "glsl-tess-evaluation-shader"]
    "glsl/builtins" ["dot" "cross" "normalize" "length" "distance"
                     "mix" "clamp" "smoothstep" "step" "reflect" "refract"
                     "texture" "textureLod" "swizzle"
                     "vec2" "vec3" "vec4" "mat2" "mat3" "mat4"
                     "sin" "cos" "tan" "sqrt" "pow" "exp" "log"
                     "abs" "min" "max" "floor" "ceil" "fract"
                     "dFdx" "dFdy" "fwidth"]
    "glsl/codegen"  ["compile-glsl"]
    "glsl/stdlib"   ["phong-lighting" "fresnel" "gamma-correct"]
  })
```

### Consuming the spice

```turmeric
(import glsl/shaders :refer [glsl-vertex-shader glsl-fragment-shader])
(import glsl/codegen :refer [compile-glsl])
(import glsl/builtins :refer [vec3 vec4 normalize dot mix])
```

Then pass the compiled string to the `tur-opengl` spice's shader compiler:

```turmeric
(import opengl/shaders :refer [shader-program])

(defn my-program [] :uint
  (shader-program
    (compile-glsl (my-vertex-shader))
    (compile-glsl (my-fragment-shader))))
```

### Spice layout inside turmeric-spices

```
spices/glsl/
  build.tur
  tur.lock
  src/
    core.tur      -- control flow and variable binding forms
    types.tur     -- struct and typedef forms
    shaders.tur   -- shader-stage entry-point forms
    builtins.tur  -- GLSL built-in function wrappers
    codegen.tur   -- compile-glsl entry point
    stdlib.tur    -- reusable lighting / math helpers
  tests/
    codegen_test.tur
    shaders_test.tur
```

## File Structure

```
src/
├── compiler/
│   └── glsl/
│       ├── compiler.tur      # Main DSL compiler
│       ├── types.tur         # Type definitions
│       ├── builtins.tur      # Built-in function wrappers
│       ├── codegen.tur       # GLSL code generation
│       └── validate.tur      # Validation and error checking
├── stdlib/
│   └── glsl/
│       ├── lighting.tur      # Lighting functions
│       ├── math.tur          # Math utilities
│       ├── texture.tur       # Texture sampling functions
│       └── noise.tur         # Noise functions
```

## References

- [GLSL 4.60 Reference](https://www.khronos.org/registry/OpenGL/specs/gl/GLSLangSpec.4.60.pdf)
- [OpenGL Shader Language](https://www.khronos.org/opengl/)
- [The Book of Shaders](https://thebookofshaders.com/)
- [ShaderToy](https://www.shadertoy.com/)
