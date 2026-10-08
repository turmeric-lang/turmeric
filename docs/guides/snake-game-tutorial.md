---
title: Snake Game Tutorial
category: Tutorials
description: Building the snake game example
---

# Snake Game Tutorial -- Step-by-Step Implementation

A **hands-on guide** to building the Snake game with CMake + CPM + Raylib + Turmeric. Each step builds on the previous one, introducing Turmeric features incrementally. By the end, you'll have a fully working game that showcases algebraic effects, typeclasses, FFI, and more.

> **Prerequisites**: Working Turmeric compiler, CMake 3.20+, C compiler, git

---

## Tutorial Overview

| Step | Topic | Turmeric Features Introduced | Time |
|---|---|---|---|
| 0 | Project Setup | CMake, CPM, directory structure | 15 min |
| 1 | Hello Window | `extern-c`, basic FFI | 20 min |
| 2 | Clear Screen | Function calls, colors | 15 min |
| 3 | Draw a Rectangle | Passing arguments to C | 20 min |
| 4 | Game State Struct | `defstruct`, fields | 20 min |
| 5 | Input Handling | Key polling, conditionals | 25 min |
| 6 | Snake Movement | Vectors, state updates | 30 min |
| 7 | Typeclasses for Drawing | `defclass`, `definstance` | 30 min |
| 8 | Effects for Rendering | `defeffect`, `perform`, `handle` | 40 min |
| 9 | Collision Detection | Pattern matching, boolean logic | 30 min |
| 10 | Food & Scoring | Random generation, state updates | 30 min |
| 11 | Game Over Effect | Effect handling, early exit | 25 min |
| 12 | Resource Cleanup | `defer`, automatic cleanup | 15 min |
| 13 | Polish & Extras | Time-based movement, score display | 30 min |

**Total estimated time**: ~5-6 hours

---

## Step 0: Project Setup

### Create Directory Structure

```bash
# From the turmeric repo root
mkdir -p examples/snake/src examples/snake/assets cmake
```

### Download CPM.cmake

```bash
curl -o cmake/CPM.cmake https://raw.githubusercontent.com/cpm-cmake/CPM.cmake/master/CPM.cmake
```

### Create Root CMakeLists.txt

```cmake
# turmeric/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(turmeric LANGUAGES C)

# CPM setup
include(cmake/CPM.cmake)

# Build Turmeric compiler
add_subdirectory(src)

# Examples
add_subdirectory(examples)
```

### Create Snake CMakeLists.txt

```cmake
# turmeric/examples/snake/CMakeLists.txt
CPMAddPackage(
  NAME raylib
  GITHUB_REPOSITORY raysan5/raylib
  VERSION 5.0
  OPTIONS "BUILD_EXAMPLES OFF"
)

# The shim + main entry point
add_executable(snake src/rayLibShim.c)
target_link_libraries(snake PRIVATE raylib)

# Custom command to compile Turmeric and link
add_custom_command(
  TARGET snake POST_BUILD
  COMMAND ${CMAKE_SOURCE_DIR}/build/tur build ${CMAKE_CURRENT_SOURCE_DIR}/src/main.tur
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/build/tur
  COMMENT "Compiling Turmeric code for Snake"
)
```

### Create the C Shim

```c
// turmeric/examples/snake/src/rayLibShim.c
#include <raylib.h>

// Window
void tur_init_window(int w, int h, const char *title) { InitWindow(w, h, title); }
void tur_close_window(void) { CloseWindow(); }
int tur_window_should_close(void) { return WindowShouldClose(); }

// Drawing
void tur_begin_drawing(void) { BeginDrawing(); }
void tur_end_drawing(void) { EndDrawing(); }
void tur_clear_background(int r, int g, int b) { ClearBackground((Color){r,g,b,255}); }
void tur_draw_rect(int x, int y, int w, int h, int r, int g, int b) {
    DrawRectangle(x, y, w, h, (Color){r, g, b, 255});
}
void tur_set_fps(int fps) { SetTargetFPS(fps); }

// Input
int tur_is_key_down(int key) { return IsKeyDown(key); }
```

### Build and Test

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

If this compiles, your project setup is correct. The next steps add Turmeric code.

---

## Step 1: Hello Window

Create your first Turmeric file that opens a window.

```turmeric
;; turmeric/examples/snake/src/main.tur

(extern-c init-window [^int w ^int h ^cstr title] : void)
(extern-c close-window [] : void)
(extern-c window-should-close [] : int)
(extern-c set-fps [^int fps] : void)

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 1")
  (set-fps 60)
  (while (not (window-should-close))
    ;; Empty loop - just keep window open
    )
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur

extern-c init-window [^int w ^int h ^cstr title] : void
extern-c close-window [] : void
extern-c window-should-close [] : int
extern-c set-fps [^int fps] : void

defn main []
  init-window(800 600 "Turmeric Snake - Step 1")
  set-fps(60)
  while not(window-should-close())
    ;; Empty loop - just keep window open
  close-window()
```

**What's happening:**
- `extern-c` declares C functions for FFI
- `main` is the entry point -- the compiler emits a call to it; a function named `-main` is NOT an entry point and is never invoked
- Window opens and stays open until you close it

**Test:** Run `./examples/snake/snake` -- you should see a black window.

---

## Step 2: Clear Screen

Add screen clearing to prevent flickering.

```turmeric
;; Add to main.tur
(extern-c begin-drawing [] : void)
(extern-c end-drawing [] : void)
(extern-c clear-background [^int r ^int g ^int b] : void)

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 2")
  (set-fps 60)
  (while (not (window-should-close))
    (begin-drawing)
    (clear-background 0 0 0)  ;; Black background
    (end-drawing))
  (close-window))
```

```sweet-exp
;; Add to main.tur
extern-c begin-drawing [] : void
extern-c end-drawing [] : void
extern-c clear-background [^int r ^int g ^int b] : void

defn main []
  init-window(800 600 "Turmeric Snake - Step 2")
  set-fps(60)
  while not(window-should-close())
    begin-drawing()
    clear-background(0 0 0)  ;; Black background
    end-drawing()
  close-window()
```

**Turmeric feature:** Function calls with arguments passed to C.

---

## Step 3: Draw a Rectangle

Draw a white rectangle in the middle of the screen.

```turmeric
;; Add to main.tur
(extern-c draw-rect [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void)

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 3")
  (set-fps 60)
  (while (not (window-should-close))
    (begin-drawing)
    (clear-background 0 0 0)
    (draw-rect 350 250 100 100 255 255 255)  ;; x,y,w,h,r,g,b
    (end-drawing))
  (close-window))
```

```sweet-exp
;; Add to main.tur
extern-c draw-rect [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void

defn main []
  init-window(800 600 "Turmeric Snake - Step 3")
  set-fps(60)
  while not(window-should-close())
    begin-drawing()
    clear-background(0 0 0)
    draw-rect(350 250 100 100 255 255 255)  ;; x,y,w,h,r,g,b
    end-drawing()
  close-window()
```

**Turmeric feature:** Multiple arguments, all integers.

---

## Step 4: Game State Struct

Introduce a struct to hold game state. This will eventually contain the snake, food, score, etc.

```turmeric
;; turmeric/examples/snake/src/state.tur

(defstruct GameState
  [snake-x : int
   snake-y : int
   snake-w : int
   snake-h : int])

(defn init-state [] : GameState
  (GameState 400 300 20 20))
```

```sweet-exp
;; turmeric/examples/snake/src/state.tur

defstruct GameState
  [snake-x : int
   snake-y : int
   snake-w : int
   snake-h : int]

defn init-state [] : GameState
  GameState(400 300 20 20)
```

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)

(extern-c init-window [^int w ^int h ^cstr title] : void)
(extern-c close-window [] : void)
(extern-c window-should-close [] : int)
(extern-c set-fps [^int fps] : void)
(extern-c begin-drawing [] : void)
(extern-c end-drawing [] : void)
(extern-c clear-background [^int r ^int g ^int b] : void)
(extern-c draw-rect [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void)

(defn draw-state [^state/GameState s]
  (draw-rect s.snake-x s.snake-y s.snake-w s.snake-h 255 255 255))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 4")
  (set-fps 60)
  (let [state (state/init-state)]
    (while (not (window-should-close))
      (begin-drawing)
      (clear-background 0 0 0)
      (draw-state state)
      (end-drawing)))
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state

extern-c init-window [^int w ^int h ^cstr title] : void
extern-c close-window [] : void
extern-c window-should-close [] : int
extern-c set-fps [^int fps] : void
extern-c begin-drawing [] : void
extern-c end-drawing [] : void
extern-c clear-background [^int r ^int g ^int b] : void
extern-c draw-rect [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void

defn draw-state [^state/GameState s]
  draw-rect(s.snake-x s.snake-y s.snake-w s.snake-h 255 255 255)

defn main []
  init-window(800 600 "Turmeric Snake - Step 4")
  set-fps(60)
  let [state state/init-state()]
    while not(window-should-close())
      begin-drawing()
      clear-background(0 0 0)
      draw-state(state)
      end-drawing()
  close-window()
```

**Turmeric features:**
- `defstruct` -- defines a record type
- `module` + `import` -- code organization
- Struct field access with `.` syntax

---

## Step 5: Input Handling

Make the rectangle move with arrow keys.

```turmeric
;; Add to main.tur
(extern-c is-key-down [^int key] : int)

;; Key constants
(def KEY_RIGHT 262)
(def KEY_LEFT  263)
(def KEY_UP    265)
(def KEY_DOWN  264)

(defn update-state [^state/GameState s] : state/GameState
  (let [speed 5
        new-x (if (is-key-down KEY_RIGHT) (+ s.snake-x speed)
                   (if (is-key-down KEY_LEFT) (- s.snake-x speed)
                       s.snake-x))
        new-y (if (is-key-down KEY_DOWN) (+ s.snake-y speed)
                   (if (is-key-down KEY_UP) (- s.snake-y speed)
                       s.snake-y))]
    (state/GameState new-x new-y s.snake-w s.snake-h)))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 5")
  (set-fps 60)
  (let [state (state/init-state)]
    (while (not (window-should-close))
      (let [new-state (update-state state)]
        (begin-drawing)
        (clear-background 0 0 0)
        (draw-state new-state)
        (end-drawing))))
  (close-window))
```

```sweet-exp
;; Add to main.tur
extern-c is-key-down [^int key] : int

;; Key constants
def KEY_RIGHT 262
def KEY_LEFT  263
def KEY_UP    265
def KEY_DOWN  264

defn update-state [^state/GameState s] : state/GameState
  let [speed 5
       new-x if(is-key-down(KEY_RIGHT)
               {s.snake-x + speed}
               if(is-key-down(KEY_LEFT)
                 {s.snake-x - speed}
                 s.snake-x))
       new-y if(is-key-down(KEY_DOWN)
               {s.snake-y + speed}
               if(is-key-down(KEY_UP)
                 {s.snake-y - speed}
                 s.snake-y))]
    state/GameState(new-x new-y s.snake-w s.snake-h)

defn main []
  init-window(800 600 "Turmeric Snake - Step 5")
  set-fps(60)
  let [state state/init-state()]
    while not(window-should-close())
      let [new-state update-state(state)]
        begin-drawing()
        clear-background(0 0 0)
        draw-state(new-state)
        end-drawing()
  close-window()
```

**Turmeric features:**
- `if` expressions (they return values!)
- Nested `if` for multi-way conditionals
- Immutable state updates (new struct each frame)

---

## Step 6: Snake Movement with Vectors

Replace the single rectangle with a snake made of segments stored in a vector.

```turmeric
;; turmeric/examples/snake/src/state.tur

(defstruct Segment [x : int, y : int])

(defstruct Snake
  [segments : (vec Segment)
   direction : int])  ;; 0=up, 1=right, 2=down, 3=left

(defstruct GameState
  [snake : Snake])

(defn init-state [] : GameState
  (GameState (Snake (vec (Segment 400 300) (Segment 380 300) (Segment 360 300)) 1)))
```

```sweet-exp
;; turmeric/examples/snake/src/state.tur

defstruct Segment [x : int, y : int]

defstruct Snake
  [segments : (vec Segment)
   direction : int]

defstruct GameState
  [snake : Snake]

defn init-state [] : GameState
  GameState(Snake(vec(Segment(400 300) Segment(380 300) Segment(360 300)) 1))
```

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)

;; ... extern-c declarations ...

(defn draw-segment [^state/Segment seg]
  (draw-rect seg.x seg.y 20 20 0 255 0))  ;; Green

(defn draw-snake [^state/Snake snake]
  (for [seg snake.segments]
    (draw-segment seg)))

(defn draw-state [^state/GameState s]
  (draw-snake s.snake))

(defn update-snake [^state/Snake snake] : state/Snake
  (let [head (vec-get snake.segments 0)
        new-head (match snake.direction
                   0 (Segment head.x (- head.y 20))  ;; UP
                   1 (Segment (+ head.x 20) head.y)  ;; RIGHT
                   2 (Segment head.x (+ head.y 20))  ;; DOWN
                   3 (Segment (- head.x 20) head.y)  ;; LEFT
                   _ head)]
    (state/Snake (vec-conj (vec-rest snake.segments) new-head) snake.direction)))

(defn update-state [^state/GameState s] : state/GameState
  (if (is-key-down KEY_RIGHT) (update s (snake.direction : 1))
   (if (is-key-down KEY_LEFT) (update s (snake.direction : 3))
    (if (is-key-down KEY_UP) (update s (snake.direction : 0))
     (if (is-key-down KEY_DOWN) (update s (snake.direction : 2))
      s))))
  (state/GameState (update-snake s.snake)))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 6")
  (set-fps 10)  ;; Slower for now
  (let [state (state/init-state)]
    (while (not (window-should-close))
      (let [new-state (update-state state)]
        (begin-drawing)
        (clear-background 0 0 0)
        (draw-state new-state)
        (end-drawing))))
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state

;; ... extern-c declarations ...

defn draw-segment [^state/Segment seg]
  draw-rect(seg.x seg.y 20 20 0 255 0)

defn draw-snake [^state/Snake snake]
  for [seg snake.segments]
    draw-segment(seg)

defn draw-state [^state/GameState s]
  draw-snake(s.snake)

defn update-snake [^state/Snake snake] : state/Snake
  let [head vec-get(snake.segments 0)
       new-head (match snake.direction
                  0 Segment(head.x {head.y - 20})
                  1 Segment({head.x + 20} head.y)
                  2 Segment(head.x {head.y + 20})
                  3 Segment({head.x - 20} head.y)
                  _ head)]
    state/Snake(vec-conj(vec-rest(snake.segments) new-head) snake.direction)

defn update-state [^state/GameState s] : state/GameState
  if is-key-down(KEY_RIGHT)
    update(s (snake.direction : 1))
    if is-key-down(KEY_LEFT)
      update(s (snake.direction : 3))
      if is-key-down(KEY_UP)
        update(s (snake.direction : 0))
        if is-key-down(KEY_DOWN)
          update(s (snake.direction : 2))
          s
  state/GameState(update-snake(s.snake))

defn main []
  init-window(800 600 "Turmeric Snake - Step 6")
  set-fps(10)
  let [state state/init-state()]
    while not(window-should-close())
      let [new-state update-state(state)]
        begin-drawing()
        clear-background(0 0 0)
        draw-state(new-state)
        end-drawing()
  close-window()
```

**Turmeric features:**
- `vec` -- persistent vectors
- `vec-get` / `vec-rest` / `vec-conj` -- vector operations
- `match` -- pattern matching on direction
- `for` -- iteration over segments
- `update` -- struct field update syntax

---

## Step 7: Typeclasses for Drawing

Use typeclasses to make drawing polymorphic -- any type that implements `Drawable` can be drawn.

```turmeric
;; turmeric/examples/snake/src/state.tur

(defclass Drawable [a]
  (draw [self : a] : void))

(defstruct Segment [x : int, y : int])

(definstance Drawable Segment
  (draw [self]
    (draw-rect self.x self.y 20 20 0 255 0)))

(defstruct Snake
  [segments : (vec Segment)
   direction : int])

(definstance Drawable Snake
  (draw [self]
    (for [seg self.segments]
      (draw seg))))  ;; Calls Drawable.draw on each Segment

(defstruct GameState
  [snake : Snake])

(definstance Drawable GameState
  (draw [self]
    (draw self.snake)))

(defn init-state [] : GameState
  (GameState (Snake (vec (Segment 400 300) (Segment 380 300) (Segment 360 300)) 1)))
```

```sweet-exp
;; turmeric/examples/snake/src/state.tur

defclass Drawable [a]
  draw [self : a] : void

defstruct Segment [x : int, y : int]

definstance Drawable Segment
  draw [self]
    draw-rect(self.x self.y 20 20 0 255 0)

defstruct Snake
  [segments : (vec Segment)
   direction : int]

definstance Drawable Snake
  draw [self]
    for [seg self.segments]
      draw(seg)

defstruct GameState
  [snake : Snake]

definstance Drawable GameState
  draw [self]
    draw(self.snake)

defn init-state [] : GameState
  GameState(Snake(vec(Segment(400 300) Segment(380 300) Segment(360 300)) 1))
```

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)

;; ... extern-c declarations ...

;; Simplified draw - just delegate to typeclass
(defn draw-state [^state/GameState s]
  (draw s))  ;; Uses Drawable.draw

;; ... rest of main.tur stays the same ...
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state

;; ... extern-c declarations ...

;; Simplified draw - just delegate to typeclass
defn draw-state [^state/GameState s]
  draw(s)

;; ... rest of main.tur stays the same ...
```

**Turmeric features:**
- `defclass` -- define a typeclass
- `definstance` -- implement a typeclass for a type
- Polymorphic `draw` function works on any `Drawable`

---

## Step 8: Effects for Rendering

This is where it gets interesting. Introduce algebraic effects to separate rendering logic from game logic.

```turmeric
;; turmeric/examples/snake/src/effects.tur

;; Define our effects
(defeffect Render [obj : any] : void)
(defeffect Get-Time [] : float)
```

```sweet-exp
;; turmeric/examples/snake/src/effects.tur

;; Define our effects
defeffect Render [obj : any] : void
defeffect Get-Time [] : float
```

```turmeric
;; turmeric/examples/snake/src/state.tur
(import effects)

(defclass Drawable [a]
  (draw [self : a] : void))

(defstruct Segment [x : int, y : int])

(definstance Drawable Segment
  (draw [self]
    (perform (Render self))))

(defstruct Snake
  [segments : (vec Segment)
   direction : int])

(definstance Drawable Snake
  (draw [self]
    (for [seg self.segments]
      (draw seg))))

(defstruct GameState
  [snake : Snake])

(definstance Drawable GameState
  (draw [self]
    (draw self.snake)))

(defn init-state [] : GameState
  (GameState (Snake (vec (Segment 400 300) (Segment 380 300) (Segment 360 300)) 1)))
```

```sweet-exp
;; turmeric/examples/snake/src/state.tur
import effects

defclass Drawable [a]
  draw [self : a] : void

defstruct Segment [x : int, y : int]

definstance Drawable Segment
  draw [self]
    perform(Render(self))

defstruct Snake
  [segments : (vec Segment)
   direction : int]

definstance Drawable Snake
  draw [self]
    for [seg self.segments]
      draw(seg)

defstruct GameState
  [snake : Snake]

definstance Drawable GameState
  draw [self]
    draw(self.snake)

defn init-state [] : GameState
  GameState(Snake(vec(Segment(400 300) Segment(380 300) Segment(360 300)) 1))
```

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)
(import effects)

;; ... extern-c declarations ...

;; Convert Render effect to actual drawing call
(defn handle-render [obj]
  (match obj
    ((state/Segment s) (draw-rect s.x s.y 20 20 0 255 0))
    (_ (println "Unknown render type"))))

(defn game-loop [^state/GameState state]
  (handle
    (let [new-state (update-state state)]
      (draw new-state)
      (game-loop new-state))
    ((Render [obj] k)
      (handle-render obj)
      (resume k))))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 8")
  (set-fps 10)
  (let [initial-state (state/init-state)]
    (handle
      (game-loop initial-state)))
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state
import effects

;; ... extern-c declarations ...

;; Convert Render effect to actual drawing call
defn handle-render [obj]
  match obj
    (state/Segment s) draw-rect(s.x s.y 20 20 0 255 0)
    _ println("Unknown render type")

defn game-loop [^state/GameState state]
  (handle
    (let [new-state update-state(state)]
      draw(new-state)
      game-loop(new-state))
    ((Render [obj] k) (handle-render obj) (resume k)))

defn main []
  init-window(800 600 "Turmeric Snake - Step 8")
  set-fps(10)
  let [initial-state state/init-state()]
    (handle (game-loop initial-state))
  close-window()
```

**Turmeric features:**
- `defeffect` -- define a new effect
- `perform` -- raise/perform an effect
- `handle` -- handle effects with pattern matching
- `resume` -- continue after handling

> **Note**: This is a simplified version. The full effect system will be more sophisticated.

---

## Step 9: Collision Detection

Add wall and self-collision detection using pattern matching.

```turmeric
;; turmeric/examples/snake/src/state.tur
(import effects)

;; ... existing code ...

(defstruct Rectangle [x : int, y : int, w : int, h : int])

(defclass Collidable [a]
  (collides? [self : a other : a] : bool)
  (bounds [self : a] : Rectangle))

(definstance Collidable Segment
  (collides? [self other]
    (and (= self.x other.x)
         (= self.y other.y)))
  (bounds [self]
    (Rectangle self.x self.y 20 20)))

(defn segments-collide? [^Segment a ^Segment b] : bool
  (and (= a.x b.x) (= a.y b.y)))

(defn snake-self-collision? [^Snake snake] : bool
  (let [head (vec-get snake.segments 0)
        tail (vec-rest snake.segments)]
    (any? (fn [seg] (segments-collide? head seg)) tail)))

(defn snake-wall-collision? [^Snake snake ^int width ^int height] : bool
  (let [head (vec-get snake.segments 0)]
    (or (or (or (< head.x 0)
        (>= head.x width))
        (< head.y 0))
        (>= head.y height))))

;; Update GameState to include bounds
(defstruct GameState
  [snake : Snake
   width : int
   height : int])

(defn init-state [] : GameState
  (GameState (Snake (vec (Segment 400 300) (Segment 380 300) (Segment 360 300)) 1) 800 600))

(defn check-collisions [^GameState state] : bool
  (or (snake-wall-collision? state.snake state.width state.height)
      (snake-self-collision? state.snake)))
```

```sweet-exp
;; turmeric/examples/snake/src/state.tur
import effects

;; ... existing code ...

defstruct Rectangle [x : int, y : int, w : int, h : int]

defclass Collidable [a]
  collides? [self : a other : a] : bool
  bounds [self : a] : Rectangle

definstance Collidable Segment
  collides? [self other]
    {{self.x = other.x} and {self.y = other.y}}
  bounds [self]
    Rectangle(self.x self.y 20 20)

defn segments-collide? [^Segment a ^Segment b] : bool
  {{a.x = b.x} and {a.y = b.y}}

defn snake-self-collision? [^Snake snake] : bool
  let [head vec-get(snake.segments 0)
       tail vec-rest(snake.segments)]
    any?((fn [seg] segments-collide?(head seg)) tail)

defn snake-wall-collision? [^Snake snake ^int width ^int height] : bool
  let [head vec-get(snake.segments 0)]
    {{{{head.x < 0} or {head.x >= width}} or {head.y < 0}} or {head.y >= height}}

defstruct GameState
  [snake : Snake
   width : int
   height : int]

defn init-state [] : GameState
  GameState(Snake(vec(Segment(400 300) Segment(380 300) Segment(360 300)) 1) 800 600)

defn check-collisions [^GameState state] : bool
  {snake-wall-collision?(state.snake state.width state.height) or snake-self-collision?(state.snake)}
```

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)
(import effects)

;; Add game over effect
(defeffect Game-Over [score : int] : void)

(defn update-state [^state/GameState s] : state/GameState
  (if (is-key-down KEY_RIGHT) (update s (snake.direction : 1))
   (if (is-key-down KEY_LEFT) (update s (snake.direction : 3))
    (if (is-key-down KEY_UP) (update s (snake.direction : 0))
     (if (is-key-down KEY_DOWN) (update s (snake.direction : 2))
      s))))
  (let [new-snake (update-snake s.snake)]
    (state/GameState new-snake s.width s.height)))

(defn game-loop [^state/GameState state]
  (handle
    (let [new-state (update-state state)]
      (if (state/check-collisions new-state)
        (perform (Game-Over 0))
        (draw new-state))
      (game-loop new-state))
    ((Render [obj] k)
      (handle-render obj)
      (resume k))
    ((Game-Over [score] k)
      (println (concat "Game Over! Score: " (itoa score)))
      (resume k))))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 9")
  (set-fps 10)
  (let [initial-state (state/init-state)]
    (handle
      (game-loop initial-state)
      ((Game-Over [score] k)
        (close-window))))
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state
import effects

;; Add game over effect
defeffect Game-Over [score : int] : void

defn update-state [^state/GameState s] : state/GameState
  if is-key-down(KEY_RIGHT)
    update(s (snake.direction : 1))
    if is-key-down(KEY_LEFT)
      update(s (snake.direction : 3))
      if is-key-down(KEY_UP)
        update(s (snake.direction : 0))
        if is-key-down(KEY_DOWN)
          update(s (snake.direction : 2))
          s
  let [new-snake update-snake(s.snake)]
    state/GameState(new-snake s.width s.height)

defn game-loop [^state/GameState state]
  (handle
    (let [new-state update-state(state)]
      (if state/check-collisions(new-state)
        perform(Game-Over(0))
        draw(new-state))
      game-loop(new-state))
    ((Render [obj] k) (handle-render obj) (resume k))
    ((Game-Over [score] k) (println (concat "Game Over! Score: " (itoa score))) (resume k)))

defn main []
  init-window(800 600 "Turmeric Snake - Step 9")
  set-fps(10)
  let [initial-state state/init-state()]
    (handle (game-loop initial-state)
      ((Game-Over [score] k) (close-window)))
  close-window()
```

**Turmeric features:**
- `and` / `or` -- boolean logic
- `any?` -- higher-order function on collections
- Nested effect handling

---

## Step 10: Food & Scoring

Add food that the snake can eat, with score tracking.

```turmeric
;; turmeric/examples/snake/src/state.tur
(import effects)

;; ... existing code ...

(defstruct Food [x : int, y : int])

(defn random-food [^int width ^int height] : Food
  (let [x (mod (cast (perform (Get-Time)) int) width)
        y (mod (cast (+ (perform (Get-Time)) 100) int) height)]
    (Food x y)))

(defn food-collision? [^Snake snake ^Food food] : bool
  (let [head (vec-get snake.segments 0)]
    (and (= head.x food.x)
         (= head.y food.y))))

(defstruct GameState
  [snake : Snake
   food : Food
   score : int
   width : int
   height : int])

(defn init-state [] : GameState
  (let [food (random-food 800 600)]
    (GameState (Snake (vec (Segment 400 300) (Segment 380 300) (Segment 360 300)) 1)
              food 0 800 600)))

(definstance Drawable Food
  (draw [self]
    (perform (Render self))))

(defn grow-snake [^Snake snake] : Snake
  (let [head (vec-get snake.segments 0)
        new-seg (match snake.direction
                  0 (Segment head.x (- head.y 20))
                  1 (Segment (+ head.x 20) head.y)
                  2 (Segment head.x (+ head.y 20))
                  3 (Segment (- head.x 20) head.y)
                  _ head)]
    (Snake (vec-conj snake.segments new-seg) snake.direction)))

(defn update-on-food [^GameState state] : GameState
  (let [new-snake (grow-snake state.snake)
        new-food (random-food state.width state.height)]
    (update state ($nfx$ snake : new-snake
                         food : new-food
                         score : (+ state.score 10)))))

(defn check-food-collision [^GameState state] : (option GameState)
  (if (food-collision? state.snake state.food)
    (some (update-on-food state))
    none))

(defn check-collisions [^GameState state] : (or bool GameState)
  (cond
    (snake-wall-collision? state.snake state.width state.height) true
    (snake-self-collision? state.snake) true
    else (check-food-collision state)))
```

```sweet-exp
;; turmeric/examples/snake/src/state.tur
import effects

;; ... existing code ...

defstruct Food [x : int, y : int]

defn random-food [^int width ^int height] : Food
  let [x mod(cast(perform(Get-Time()) int) width)
       y mod(cast({perform(Get-Time()) + 100} int) height)]
    Food(x y)

defn food-collision? [^Snake snake ^Food food] : bool
  let [head vec-get(snake.segments 0)]
    {{head.x = food.x} and {head.y = food.y}}

defstruct GameState
  [snake : Snake
   food : Food
   score : int
   width : int
   height : int]

defn init-state [] : GameState
  let [food random-food(800 600)]
    GameState(Snake(vec(Segment(400 300) Segment(380 300) Segment(360 300)) 1)
              food 0 800 600)

definstance Drawable Food
  draw [self]
    perform(Render(self))

defn grow-snake [^Snake snake] : Snake
  let [head vec-get(snake.segments 0)
       new-seg (match snake.direction
                 0 Segment(head.x {head.y - 20})
                 1 Segment({head.x + 20} head.y)
                 2 Segment(head.x {head.y + 20})
                 3 Segment({head.x - 20} head.y)
                 _ head)]
    Snake(vec-conj(snake.segments new-seg) snake.direction)

defn update-on-food [^GameState state] : GameState
  let [new-snake grow-snake(state.snake)
       new-food random-food(state.width state.height)]
    update(state
      { snake : new-snake
        food : new-food
        score : {state.score + 10} })

defn check-food-collision [^GameState state] : (option GameState)
  if food-collision?(state.snake state.food)
    some(update-on-food(state))
    none

defn check-collisions [^GameState state] : (or bool GameState)
  cond
    snake-wall-collision?(state.snake state.width state.height)
    true
    snake-self-collision?(state.snake)
    true
    else
    check-food-collision(state)
```

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)
(import effects)

;; ... extern-c declarations ...

(defn handle-render [obj]
  (match obj
    ((state/Segment s) (draw-rect s.x s.y 20 20 0 255 0))
    ((state/Food f) (draw-rect f.x f.y 20 20 255 0 0))
    (_ (println "Unknown render type"))))

(defn game-loop [^state/GameState state]
  (handle
    (let [new-state (update-state state)
          collision-result (state/check-collisions new-state)]
      (match collision-result
        (true (perform (Game-Over new-state.score)))
        ((state/GameState updated) (game-loop updated))
        (_ (do (draw new-state) (game-loop new-state)))))
    ((Render [obj] k)
      (handle-render obj)
      (resume k))
    ((Get-Time [] k)
      (resume k 0.0))
    ((Game-Over [score] k)
      (println (concat "Game Over! Score: " (itoa score)))
      (resume k))))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 10")
  (set-fps 10)
  (let [initial-state (state/init-state)]
    (handle
      (game-loop initial-state)
      ((Game-Over [score] k)
        (close-window))))
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state
import effects

;; ... extern-c declarations ...

defn handle-render [obj]
  match obj
    (state/Segment s) draw-rect(s.x s.y 20 20 0 255 0)
    (state/Food f) draw-rect(f.x f.y 20 20 255 0 0)
    _ println("Unknown render type")

defn game-loop [^state/GameState state]
  (handle
    (let [new-state update-state(state)
          collision-result state/check-collisions(new-state)]
      (match collision-result
        (true perform(Game-Over(new-state.score)))
        ((state/GameState updated) game-loop(updated))
        (_ (do draw(new-state) game-loop(new-state)))))
    ((Render [obj] k) (handle-render obj) (resume k))
    ((Get-Time [] k) (resume k 0.0))
    ((Game-Over [score] k) (println (concat "Game Over! Score: " (itoa score))) (resume k)))

defn main []
  init-window(800 600 "Turmeric Snake - Step 10")
  set-fps(10)
  let [initial-state state/init-state()]
    (handle (game-loop initial-state)
      ((Game-Over [score] k) (close-window)))
  close-window()
```

**Turmeric features:**
- `option` type -- `some` / `none` for maybe values
- `match` on `option` types
- `do` -- sequential evaluation
- State updates with multiple fields

---

## Step 11: Game Over Effect Handler

Properly handle game over with a clean exit and final score display.

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)
(import effects)

(extern-c draw-text [^cstr text ^int x ^int y ^int fontSize ^int r ^int g ^int b] : void)

(defn draw-game-over [^int score]
  (draw-text (concat "GAME OVER: " (itoa score)) 250 250 40 255 255 255))

(defn game-loop [^state/GameState state]
  (handle
    (let [new-state (update-state state)
          collision-result (state/check-collisions new-state)]
      (match collision-result
        (true (perform (Game-Over new-state.score)))
        ((state/GameState updated) (game-loop updated))
        (_ (do
              (draw new-state)
              (game-loop new-state)))))
    ((Render [obj] k)
      (handle-render obj)
      (resume k))
    ((Get-Time [] k)
      (resume k 0.0))
    ((Game-Over [score] k)
      (begin-drawing)
      (clear-background 0 0 0)
      (draw-game-over score)
      (end-drawing)
      ;; Wait a bit before exiting
      (resume k))))

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 11")
  (set-fps 10)
  (let [initial-state (state/init-state)]
    (handle
      (game-loop initial-state)
      (Game-Over [score] k)))
  (close-window))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state
import effects

extern-c draw-text [^cstr text ^int x ^int y ^int fontSize ^int r ^int g ^int b] : void

defn draw-game-over [^int score]
  draw-text(concat("GAME OVER: " itoa(score)) 250 250 40 255 255 255)

defn game-loop [^state/GameState state]
  (handle
    (let [new-state update-state(state)
          collision-result state/check-collisions(new-state)]
      (match collision-result
        (true perform(Game-Over(new-state.score)))
        ((state/GameState updated) game-loop(updated))
        (_ (do draw(new-state) game-loop(new-state)))))
    ((Render [obj] k) (handle-render obj) (resume k))
    ((Get-Time [] k) (resume k 0.0))
    ;; Paint the final score on its own frame, then continue.
    ((Game-Over [score] k)
      begin-drawing()
      clear-background(0 0 0)
      draw-game-over(score)
      end-drawing()
      ;; Wait a bit before exiting
      resume(k)))

defn main []
  init-window(800 600 "Turmeric Snake - Step 11")
  set-fps(10)
  let [initial-state state/init-state()]
    (handle (game-loop initial-state)
      ;; Sleep for a moment so player sees game over
      ;; Then exit
      (Game-Over [score] k))
  close-window()
```

---

## Step 12: Resource Cleanup with `defer`

Use `defer` to ensure the window is always closed, even if an error occurs.

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)
(import effects)

(defn main []
  (init-window 800 600 "Turmeric Snake - Step 12")
  (defer (close-window))  ;; Automatically called on scope exit
  (set-fps 10)
  (let [initial-state (state/init-state)]
    (handle
      (game-loop initial-state)
      ((Game-Over [score] k)
        (begin-drawing)
        (clear-background 0 0 0)
        (draw-game-over score)
        (end-drawing)
        ;; defer will call close-window when we exit this scope
        ))))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state
import effects

defn main []
  init-window(800 600 "Turmeric Snake - Step 12")
  defer(close-window())
  set-fps(10)
  let [initial-state state/init-state()]
    (handle (game-loop initial-state)
      ;; Paint the final score; the window itself is closed by defer.
      ((Game-Over [score] k)
        begin-drawing()
        clear-background(0 0 0)
        draw-game-over(score)
        end-drawing()
        ;; defer will call close-window when we exit this scope
        ))
```

**Turmeric feature:**
- `defer` -- registers cleanup to run when scope exits

---

## Step 13: Polish & Extras

Final touches to make the game feel polished.

### Frame-Independent Movement

Use actual time instead of fixed speed.

```turmeric
;; Add to extern-c in main.tur
(extern-c get-frame-time [] : float)

(defn update-snake [^state/Snake snake ^float dt] : state/Snake
  (let [speed (* 200 dt)
        head (vec-get snake.segments 0)
        new-head (match snake.direction
                   0 (Segment head.x (- head.y (cast speed int)))
                   1 (Segment (+ head.x (cast speed int)) head.y)
                   2 (Segment head.x (+ head.y (cast speed int)))
                   3 (Segment (- head.x (cast speed int)) head.y)
                   _ head)]
    (state/Snake (vec-conj (vec-rest snake.segments) new-head) snake.direction)))

(defn update-state [^state/GameState s ^float dt] : state/GameState
  (let [dir (cond
               (is-key-down KEY_RIGHT) 1
               (is-key-down KEY_LEFT) 3
               (is-key-down KEY_UP) 0
               (is-key-down KEY_DOWN) 2
               else s.snake.direction)]
    (state/GameState (update-snake s.snake dt) s.food s.score s.width s.height)))

(defn game-loop [^state/GameState state]
  (handle
    (let [dt (perform (Get-Time))
          new-state (update-state state dt)
          collision-result (state/check-collisions new-state)]
      (match collision-result
        (true (perform (Game-Over new-state.score)))
        ((state/GameState updated) (game-loop updated))
        (_ (do
              (draw new-state)
              (game-loop new-state)))))
    ((Render [obj] k)
      (handle-render obj)
      (resume k))
    ((Get-Time [] k)
      (let [time (get-frame-time)]
        (resume k time)))
    ((Game-Over [score] k)
      (begin-drawing)
      (clear-background 0 0 0)
      (draw-game-over score)
      (end-drawing)
      (resume k))))
```

```sweet-exp
;; Add to extern-c in main.tur
extern-c get-frame-time [] : float

defn update-snake [^state/Snake snake ^float dt] : state/Snake
  let [speed {200 * dt}
       head vec-get(snake.segments 0)
       new-head (match snake.direction
                  0 Segment(head.x {head.y - cast(speed int)})
                  1 Segment({head.x + cast(speed int)} head.y)
                  2 Segment(head.x {head.y + cast(speed int)})
                  3 Segment({head.x - cast(speed int)} head.y)
                  _ head)]
    state/Snake(vec-conj(vec-rest(snake.segments) new-head) snake.direction)

defn update-state [^state/GameState s ^float dt] : state/GameState
  let [dir (cond
              (is-key-down KEY_RIGHT) 1
              (is-key-down KEY_LEFT) 3
              (is-key-down KEY_UP) 0
              (is-key-down KEY_DOWN) 2
              else s.snake.direction)]
    state/GameState(update-snake(s.snake dt) s.food s.score s.width s.height)

defn game-loop [^state/GameState state]
  (handle
    (let [dt perform(Get-Time())
          new-state update-state(state dt)
          collision-result state/check-collisions(new-state)]
      (match collision-result
        (true perform(Game-Over(new-state.score)))
        ((state/GameState updated) game-loop(updated))
        (_ (do draw(new-state) game-loop(new-state)))))
    ((Render [obj] k) (handle-render obj) (resume k))
    ((Get-Time [] k) (let [time (get-frame-time)] (resume k time)))
    ;; Paint the final score on its own frame, then continue.
    ((Game-Over [score] k)
      begin-drawing()
      clear-background(0 0 0)
      draw-game-over(score)
      end-drawing()
      resume(k)))
```

### Score Display

Show the score during gameplay.

```turmeric
;; Add to state.tur
(defstruct GameState
  [snake : Snake
   food : Food
   score : int
   width : int
   height : int])

(definstance Drawable GameState
  (draw [self]
    (draw self.snake)
    (draw self.food)))

;; Add to effects.tur
(defeffect Draw-Text [text : cstr x : int y : int] : void)

;; In main.tur, add handler
(defn handle-render [obj]
  (match obj
    ((state/Segment s) (draw-rect s.x s.y 20 20 0 255 0))
    ((state/Food f) (draw-rect f.x f.y 20 20 255 0 0))
    (_ (println "Unknown render type"))))

;; Update game-loop to draw score
(defn game-loop [^state/GameState state]
  (handle
    (let [dt (perform (Get-Time))
          new-state (update-state state dt)
          collision-result (state/check-collisions new-state)]
      (match collision-result
        (true (perform (Game-Over new-state.score)))
        ((state/GameState updated) (game-loop updated))
        (_ (do
              (draw new-state)
              (perform (Draw-Text (concat "Score: " (itoa new-state.score)) 10 10))
              (game-loop new-state)))))
    ((Draw-Text [text x y] k)
      (draw-text text x y 20 255 255 255)
      (resume k))))
```

```sweet-exp
;; Add to state.tur
defstruct GameState
  [snake : Snake
   food : Food
   score : int
   width : int
   height : int]

definstance Drawable GameState
  draw [self]
    draw(self.snake)
    draw(self.food)
    ;; Score will be drawn separately

;; Add to effects.tur
defeffect Draw-Text [text : cstr x : int y : int] : void

;; In main.tur, add handler
defn handle-render [obj]
  match obj
    (state/Segment s) draw-rect(s.x s.y 20 20 0 255 0)
    (state/Food f) draw-rect(f.x f.y 20 20 255 0 0)
    _ println("Unknown render type")

;; Update game-loop to draw score
defn game-loop [^state/GameState state]
  (handle
    (let [dt perform(Get-Time())
          new-state update-state(state dt)
          collision-result state/check-collisions(new-state)]
      (match collision-result
        (true perform(Game-Over(new-state.score)))
        ((state/GameState updated) game-loop(updated))
        (_ (do
              draw(new-state)
              perform(Draw-Text(concat("Score: " itoa(new-state.score)) 10 10))
              game-loop(new-state)))))
    ;; ... existing handlers ...
    ((Draw-Text [text x y] k) (draw-text text x y 20 255 255 255) (resume k)))
```

### Grid-Based Movement

Make the snake move in a grid (20x20 pixels per cell).

```turmeric
;; In state.tur
(defn grid-move [^Segment seg ^int dir] : Segment
  (match dir
    0 (Segment seg.x (- seg.y 20))
    1 (Segment (+ seg.x 20) seg.y)
    2 (Segment seg.x (+ seg.y 20))
    3 (Segment (- seg.x 20) seg.y)
    _ seg))

(defn update-snake [^state/Snake snake ^float dt] : state/Snake
  (if (< dt 0.15)
    snake
    (let [head (vec-get snake.segments 0)
          new-head (grid-move head snake.direction)]
      (state/Snake (vec-conj (vec-rest snake.segments) new-head) snake.direction))))
```

```sweet-exp
;; In state.tur
defn grid-move [^Segment seg ^int dir] : Segment
  match dir
    0
    Segment(seg.x {seg.y - 20})
    1
    Segment({seg.x + 20} seg.y)
    2
    Segment(seg.x {seg.y + 20})
    3
    Segment({seg.x - 20} seg.y)
    _
    seg

defn update-snake [^state/Snake snake ^float dt] : state/Snake
  if {dt < 0.15}
    snake
    let [head vec-get(snake.segments 0)
         new-head grid-move(head snake.direction)]
      state/Snake(vec-conj(vec-rest(snake.segments) new-head) snake.direction)
```

---

## Complete Game Loop

Here's the final, polished game loop:

```turmeric
;; turmeric/examples/snake/src/main.tur
(import state)
(import effects)

;; FFI bindings
(extern-c init-window [^int w ^int h ^cstr title] : void)
(extern-c close-window [] : void)
(extern-c window-should-close [] : int)
(extern-c set-fps [^int fps] : void)
(extern-c begin-drawing [] : void)
(extern-c end-drawing [] : void)
(extern-c clear-background [^int r ^int g ^int b] : void)
(extern-c draw-rect [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void)
(extern-c draw-text [^cstr text ^int x ^int y ^int fontSize ^int r ^int g ^int b] : void)
(extern-c is-key-down [^int key] : int)
(extern-c get-frame-time [] : float)

;; Constants
(def KEY_RIGHT 262)
(def KEY_LEFT  263)
(def KEY_UP    265)
(def KEY_DOWN  264)
(def GRID_SIZE 20)

(defn handle-render [obj]
  (match obj
    ((state/Segment s) (draw-rect s.x s.y GRID_SIZE GRID_SIZE 0 255 0))
    ((state/Food f) (draw-rect f.x f.y GRID_SIZE GRID_SIZE 255 0 0))
    (_ (println "Unknown render type"))))

(defn draw-game-over [^int score]
  (let [text (concat "GAME OVER: " (itoa score))]
    (draw-text text 250 250 40 255 255 255)
    (draw-text "Press ESC to quit" 280 320 20 255 255 255)))

(defn game-loop [^state/GameState state]
  (handle
    (let [dt (perform (Get-Time))
          dir (cond
                (is-key-down KEY_RIGHT) 1
                (is-key-down KEY_LEFT) 3
                (is-key-down KEY_UP) 0
                (is-key-down KEY_DOWN) 2
                else state.snake.direction)
          new-snake (state/update-snake state.snake dir dt)
          new-state (state/GameState new-snake state.food state.score state.width state.height)
          collision-result (state/check-collisions new-state)]
      (match collision-result
        (true (perform (Game-Over new-state.score)))
        ((state/GameState updated) (game-loop updated))
        (_ (do
              (draw new-state)
              (perform (Draw-Text (concat "Score: " (itoa new-state.score)) 10 10))
              (game-loop new-state)))))
    ((Render [obj] k)
      (handle-render obj)
      (resume k))
    ((Draw-Text [text x y] k)
      (draw-text text x y 20 255 255 255)
      (resume k))
    ((Get-Time [] k)
      (let [time (get-frame-time)]
        (resume k time)))
    ((Game-Over [score] k)
      (begin-drawing)
      (clear-background 0 0 0)
      (draw-game-over score)
      (end-drawing)
      (while (not (is-key-down KEY_ESCAPE)))
      (resume k))))

(defn main []
  (init-window 800 600 "Turmeric Snake")
  (defer (close-window))
  (set-fps 60)
  (let [initial-state (state/init-state)]
    (handle
      (game-loop initial-state)
      (Game-Over [score] k))))
```

```sweet-exp
;; turmeric/examples/snake/src/main.tur
import state
import effects

;; FFI bindings
extern-c init-window [^int w ^int h ^cstr title] : void
extern-c close-window [] : void
extern-c window-should-close [] : int
extern-c set-fps [^int fps] : void
extern-c begin-drawing [] : void
extern-c end-drawing [] : void
extern-c clear-background [^int r ^int g ^int b] : void
extern-c draw-rect [^int x ^int y ^int w ^int h ^int r ^int g ^int b] : void
extern-c draw-text [^cstr text ^int x ^int y ^int fontSize ^int r ^int g ^int b] : void
extern-c is-key-down [^int key] : int
extern-c get-frame-time [] : float

;; Constants
def KEY_RIGHT 262
def KEY_LEFT  263
def KEY_UP    265
def KEY_DOWN  264
def GRID_SIZE 20

defn handle-render [obj]
  match obj
    (state/Segment s) draw-rect(s.x s.y GRID_SIZE GRID_SIZE 0 255 0)
    (state/Food f) draw-rect(f.x f.y GRID_SIZE GRID_SIZE 255 0 0)
    _ println("Unknown render type")

defn draw-game-over [^int score]
  let [text concat("GAME OVER: " itoa(score))]
    draw-text(text 250 250 40 255 255 255)
    draw-text("Press ESC to quit" 280 320 20 255 255 255)

defn game-loop [^state/GameState state]
  (handle
    (let [dt perform(Get-Time())
          dir (cond
                (is-key-down KEY_RIGHT) 1
                (is-key-down KEY_LEFT) 3
                (is-key-down KEY_UP) 0
                (is-key-down KEY_DOWN) 2
                else state.snake.direction)
          new-snake state/update-snake(state.snake dir dt)
          new-state state/GameState(new-snake state.food state.score state.width state.height)
          collision-result state/check-collisions(new-state)]
      (match collision-result
        (true perform(Game-Over(new-state.score)))
        ((state/GameState updated) game-loop(updated))
        (_ (do
             draw(new-state)
             perform(Draw-Text(concat("Score: " itoa(new-state.score)) 10 10))
             game-loop(new-state)))))
    ((Render [obj] k) (handle-render obj) (resume k))
    ((Draw-Text [text x y] k) (draw-text text x y 20 255 255 255) (resume k))
    ((Get-Time [] k) (let [time (get-frame-time)] (resume k time)))
    ;; Paint the final score, hold the frame, then continue.
    ((Game-Over [score] k)
      begin-drawing()
      clear-background(0 0 0)
      draw-game-over(score)
      end-drawing()
      ;; Wait for ESC to exit
      (while not(is-key-down(KEY_ESCAPE)))
      resume(k)))

defn main []
  init-window(800 600 "Turmeric Snake")
  defer(close-window())
  set-fps(60)
  let [initial-state state/init-state()]
    (handle (game-loop initial-state)
      (Game-Over [score] k))
```

---

## Final Project Structure

```ascii
turmeric/
|-- CMakeLists.txt
|-- cmake/
|   `-- CPM.cmake
|-- examples/
|   `-- snake/
|       |-- CMakeLists.txt
|       `-- src/
|           |-- main.tur          # Entry point, game loop, FFI
|           |-- state.tur         # Game state, typeclasses
|           |-- effects.tur       # Effect definitions
|           `-- rayLibShim.c      # C shim for Raylib
`-- build/
```

---

## Troubleshooting

### Build fails with Raylib not found
- Make sure you have git installed
- Check CPM.cmake is in the right location
- Try `cmake -DCMAKE_BUILD_TYPE=Debug -DCPM_SOURCE_CACHE=ON ..`

### Window opens but is black
- Check your Turmeric code compiles (run `./build/tur emit-c main.tur`)
- Verify the shim functions are being called

### Snake doesn't move
- Check key codes match your system
- Add debug prints: `(println "Key pressed")`

### Effects not working
- Make sure `handle` wraps the code that calls `perform`
- Check effect names match between `defeffect`, `perform`, and handler

---

## Next Steps

Once you've completed this tutorial, consider:

1. **Add sound effects** -- Use Raylib's audio functions via FFI
2. **Add a pause menu** -- Use nested effect handlers
3. **Add levels** -- Different speeds, obstacles
4. **Add high scores** -- Save to a file
5. **Port to WASM** -- Use Emscripten
6. **Multiplayer** -- Two snakes, different controls

---

## Summary

You've now built a complete Snake game using:
- CMake + CPM for dependency management
- Raylib for graphics and input
- Turmeric for game logic
- FFI for C interop
- Structs for game state
- Vectors for snake segments
- Typeclasses for polymorphic drawing
- Algebraic effects for clean architecture
- Pattern matching for game logic
- `defer` for resource safety
- `match` for control flow

The game showcases Turmeric's most powerful features while maintaining clean, modular code. The effect system in particular demonstrates how Turmeric can provide a clean separation between game logic and rendering/input systems.
