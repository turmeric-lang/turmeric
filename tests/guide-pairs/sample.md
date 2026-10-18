# check-guide-pairs sample

tests/check-guide-pairs-tool.sh runs tools/check-guide-pairs.tur over this
file with --strict-unpaired and --verbose, and compares what it reports with
expected.txt. Each section seeds one outcome.

A pair that agrees.

```turmeric
(defn sq [x : int] : int (* x x))
```
```sweet-exp
defn sq [x : int] : int *(x x)
```

A pair that does not.

```turmeric
(+ 1 2)
```
```sweet-exp
-(1 2)
```

An empty sweet-exp side.

```turmeric
(+ 1 2)
```
```sweet-exp
```

A non-ASCII turmeric side.

```turmeric
(println "a — b")
```
```sweet-exp
println("a - b")
```

A turmeric block on its own, and one marked no-check.

```turmeric
(+ 3 4)
```

```turmeric no-check
(+ 5 6)
```

A pair in a list item, and one in a block quote.

- item

  ```turmeric
  (* 2 3)
  ```
  ```sweet-exp
  *(2 3)
  ```

> ```turmeric
> (- 9 1)
> ```
> ```sweet-exp
> -(9 1)
> ```

A manifest that parses, one that does not, and one opted out.

```turmeric no-check
(defpackage ok
  :name    "ok"
  :version "0.1.0")
```

```turmeric no-check
(defpackage bad
  :name   "bad"
  :spices {"x" {:path "y"}})
```

```turmeric no-check no-manifest-check
(defpackage skipped
  :spices {"x" {:path "y"}})
```

A four-backtick block holds inline C closing on a line of its own.

````turmeric no-check
(defn answer [] : int
  ```c
  return 42;
  ```
  )
````

A ```turmeric line inside another fence is not a block.

````markdown
```turmeric
(this is not checked
```
````

Last, as everything after it is broken: inline C closing on a line of its
own ends a three-backtick block early.

```turmeric no-check
(defn answer [] : int
  ```c
  return 42;
  ```
  )
```
