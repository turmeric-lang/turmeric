# Types

A `(defopaque ^linear Chan :ptr<void>)` is a handle; `(defstruct ^copy Point [x : int])`
is a value. The `:linear` word alone, in prose, is left for a human.

```turmeric
(defstruct ^linear Socket [fd : int])
(defn answer [] : int
  ```c
  return 42;
  ```)
(defopaque ^affine Fut :ptr<void>)
```

```sweet-exp
defstruct ^linear Socket [fd : int]
defn answer [] : int
  ```c
  return 42;
  ```
defdata ^copy Shape
  Circle(int)
```

```
(defstruct NotTurmeric :copy [x : int])
```

```c
/* (defstruct InC :copy) */
int main(void) { return 0; }
```
