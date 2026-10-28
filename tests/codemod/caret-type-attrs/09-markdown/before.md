# Types

A `(defopaque Chan :ptr<void> :linear)` is a handle; `(defstruct Point :copy [x : int])`
is a value. The `:linear` word alone, in prose, is left for a human.

```turmeric
(defstruct Socket :linear [fd : int])
(defn answer [] : int
  ```c
  return 42;
  ```)
(defopaque Fut :ptr<void> :affine)
```

```sweet-exp
defstruct Socket :linear [fd : int]
defn answer [] : int
  ```c
  return 42;
  ```
defdata Shape :copy
  Circle(int)
```

```
(defstruct NotTurmeric :copy [x : int])
```

```c
/* (defstruct InC :copy) */
int main(void) { return 0; }
```
