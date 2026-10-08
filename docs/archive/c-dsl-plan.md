# C DSL for Turmeric

## Overview

Design a domain-specific language (DSL) embedded in Turmeric for writing C code. This DSL provides a Lisp-like syntax for C programming while maintaining direct correspondence with C semantics. The DSL compiles to C99 source code, enabling seamless integration with Turmeric's existing C FFI and inline-C capabilities.

## Motivation

- **Safety**: Leverage Turmeric's type system to catch C errors at compile time
- **Metaprogramming**: Use Turmeric macros to generate C code
- **Code Reuse**: Share logic between Turmeric and C
- **Inline C**: Embed C code directly in Turmeric source files with better syntax
- **FFI Integration**: Simplify foreign function interface with type-safe bindings

## DSL Design

### Core Principles

1. **Direct C Mapping**: Each DSL construct maps 1:1 to C concepts
2. **No Hidden Allocations**: Explicit memory management, matching C's model
3. **Type Alignment**: Turmeric types correspond to C types
4. **Zero Overhead**: DSL abstraction should compile away completely
5. **Compilation Target**: Output standard C99 source code

### Type Correspondence

| Turmeric Type | C Type | Notes |
|---------------|--------|-------|
| `:void` | `void` | No value |
| `:bool` / `:cbool` | `bool` (stdbool.h) | Boolean |
| `:int8` | `int8_t` | Signed 8-bit integer |
| `:uint8` | `uint8_t` | Unsigned 8-bit integer |
| `:int16` | `int16_t` | Signed 16-bit integer |
| `:uint16` | `uint16_t` | Unsigned 16-bit integer |
| `:int` / `:int32` | `int32_t` | Signed 32-bit integer |
| `:uint` / `:uint32` | `uint32_t` | Unsigned 32-bit integer |
| `:int64` | `int64_t` | Signed 64-bit integer |
| `:uint64` | `uint64_t` | Unsigned 64-bit integer |
| `:float` | `float` | IEEE 754 single precision |
| `:double` | `double` | IEEE 754 double precision |
| `:char` | `char` | Character |
| `:cstr` | `char*` | Null-terminated string |
| `[:T N]` | `T[N]` | Fixed-size array |
| `[:T]` | `T*` | Pointer (dynamic array) |
| `(struct ...)` | `struct { ... }` | Structure type |
| `(union ...)` | `union { ... }` | Union type |
| `(enum ...)` | `enum { ... }` | Enumeration type |
| `(fn [...] T)` | `T(*)(...)` | Function pointer |

### Syntax Mapping

#### Variable Declaration

**C:**
```c
int x = 42;
float y = 3.14f;
char* str = "hello";
```

**DSL:**
```lisp
(c-let [x :int 42]
        [y :float 3.14]
        [str :cstr "hello"])
```

#### Constants

**C:**
```c
const int MAX_SIZE = 100;
static const float PI = 3.14159f;
```

**DSL:**
```lisp
(c-const [MAX-SIZE :int 100]
         [PI :float 3.14159])
```

#### Functions

**C:**
```c
int add(int a, int b) {
    return a + b;
}
```

**DSL:**
```lisp
(c-defn add [a :int b :int] :int
  (+ a b))
```

#### Control Flow

**C:**
```c
if (x > 0) {
    y = 1;
} else if (x < 0) {
    y = -1;
} else {
    y = 0;
}
```

**DSL:**
```lisp
(c-if (> x 0)
  (c-set! y 1)
  (< x 0)
  (c-set! y -1)
  (c-set! y 0))
```

**C:**
```c
for (int i = 0; i < 10; i++) {
    printf("%d\n", i);
}
```

**DSL:**
```lisp
(c-for [i :int 0] (< i 10) (c-inc! i)
  (c-call printf "%d\n" i))
```

**C:**
```c
while (condition) {
    // body
}
```

**DSL:**
```lisp
(c-while condition
  ;; body
  )
```

**C:**
```c
do {
    // body
} while (condition);
```

**DSL:**
```lisp
(c-do-while
  ;; body
  condition)
```

**C:**
```c
switch (x) {
    case 1:
        a();
        break;
    case 2:
        b();
        break;
    default:
        c();
}
```

**DSL:**
```lisp
(c-switch x
  (1 (c-call a) (c-break))
  (2 (c-call b) (c-break))
  :default (c-call c))
```

#### Pointers and Memory

**C:**
```c
int* ptr = &x;
int y = *ptr;
*ptr = 42;
```

**DSL:**
```lisp
(c-let [ptr :[:int] (c-addr x)]
        [y :int (c-deref ptr)])
(c-set! (c-deref ptr) 42)
```

**C:**
```c
int arr[10];
int* ptr = arr;
ptr[5] = 42;
int x = *(ptr + 3);
```

**DSL:**
```lisp
(c-let [arr :[:int 10] (c-array :int 10)]
        [ptr :[:int] arr])
(c-set! (c-index ptr 5) 42)
(c-let [x :int (c-deref (c-add ptr 3))])
```

#### Structs

**C:**
```c
typedef struct {
    int x;
    int y;
} Point;

Point p = {1, 2};
int x = p.x;
p.y = 3;
```

**DSL:**
```lisp
(c-defstruct Point
  [x :int]
  [y :int])

(c-let [p :Point (Point 1 2)]
        [x :int (c-get p :x)])
(c-set! (c-get p :y) 3)
```

#### Unions

**C:**
```c
typedef union {
    int i;
    float f;
    char* s;
} Value;

Value v;
v.i = 42;
```

**DSL:**
```lisp
(c-defunion Value
  [i :int]
  [f :float]
  [s :cstr])

(c-let [v :Value])
(c-set! (c-get v :i) 42)
```

#### Enums

**C:**
```c
typedef enum {
    RED,
    GREEN,
    BLUE
} Color;

Color c = RED;
```

**DSL:**
```lisp
(c-defenum Color
  RED GREEN BLUE)

(c-let [c :Color Color/RED])
```

#### Type Definitions

**C:**
```c
typedef int* IntPtr;
typedef struct Point Point;
```

**DSL:**
```lisp
(c-typedef :[:int] IntPtr)
(c-typedef :Point Point)
```

#### Function Pointers

**C:**
```c
typedef int (*CompareFunc)(void*, void*);

int compare_ints(void* a, void* b) {
    return *(int*)a - *(int*)b;
}

CompareFunc cmp = compare_ints;
```

**DSL:**
```lisp
(c-typedef (fn [:void* :void*] :int) CompareFunc)

(c-defn compare-ints [a :void* b :void*] :int
  (- (c-deref (c-cast a :[:int])) (c-deref (c-cast b :[:int]))))

(c-let [cmp :CompareFunc compare-ints])
```

#### Preprocessor Directives

**C:**
```c
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PI 3.14159

#ifdef DEBUG
printf("Debug mode\n");
#endif
```

**DSL:**
```lisp
(c-define "MAX(a, b)" "((a) > (b) ? (a) : (b))")
(c-define "PI" "3.14159")

(c-ifdef "DEBUG"
  (c-call printf "Debug mode\n"))
```

#### Include Files

**C:**
```c
#include <stdio.h>
#include "myheader.h"
```

**DSL:**
```lisp
(c-include "stdio.h")
(c-include "myheader.h")
```

#### Casts

**C:**
```c
int x = (int)3.14;
float* f = (float*)&x;
```

**DSL:**
```lisp
(c-let [x :int (c-cast 3.14 :int)]
        [f :[:float] (c-cast (c-addr x) :[:float])])
```

#### sizeof and alignof

**C:**
```c
size_t size = sizeof(int);
size_t align = alignof(struct Point);
```

**DSL:**
```lisp
(c-let [size :size-t (c-sizeof :int)]
        [align :size-t (c-alignof :Point)])
```

#### Static Assertions

**C:**
```c
static_assert(sizeof(int) == 4, "int must be 4 bytes");
```

**DSL:**
```lisp
(c-static-assert (= (c-sizeof :int) 4) "int must be 4 bytes")
```

### Memory Management

The DSL provides explicit memory management constructs:

```lisp
;; Allocate on stack
(c-let [x :int (c-alloca :int)])

;; Allocate on heap
(c-let [ptr :[:int] (c-malloc (* 10 (c-sizeof :int)))])

;; Reallocate
(c-let [ptr2 :[:int] (c-realloc ptr (* 20 (c-sizeof :int)))])

;; Free memory
(c-free ptr)

;; Calloc (alloc + zero)
(c-let [ptr :[:int] (c-calloc 10 (c-sizeof :int))])
```

### String Operations

```lisp
;; String literal
(c-let [s :cstr "hello"])

;; String concatenation
(c-let [s :cstr (c-strcat "hello " "world")])

;; String length
(c-let [len :size-t (c-strlen s)])

;; String copy
(c-let [copy :cstr (c-strdup s)])
(c-free copy)

;; Format string (printf-style)
(c-let [buf :cstr (c-asprintf "Value: %d" x)])
(c-free buf)
```

## Compilation to C

The DSL compiler translates Turmeric DSL expressions to C source code:

```lisp
(defn compile-c [dsl-expr] :string
  ;; Returns C source code as a string
  )

;; Usage in inline-c
(defn use-c-code []
  (let [c-src (compile-c my-c-dsl)]
    (inline-c c-src)))

;; Or emit to a file
(defn emit-c-file [path expr]
  (let [c-src (compile-c expr)]
    (write-file path c-src)))
```

## Integration with Turmeric

### Inline C DSL

The DSL can be used directly in Turmeric code via `inline-c-dsl`:

```lisp
(defn main [] :int
  (inline-c-dsl
    (c-include "stdio.h")
    (c-defn helper [x :int] :int
      (* x x))
    (c-let [result :int (helper 5)])
    (c-call printf "Result: %d\n" result)
    (c-return 0))
  0)
```

This compiles to approximately:

```c
#include "stdio.h"

int helper(int x) {
    return x * x;
}

int main() {
    int result = helper(5);
    printf("Result: %d\n", result);
    return 0;
}
```

### Type-Safe FFI

The DSL makes FFI easier by providing type-safe bindings:

```lisp
;; Define a C function
(c-defn my_c_function [a :int b :float] :double
  (+ (c-cast a :double) b))

;; Call from Turmeric
(defn use-ffi [] :double
  (my-c-function 42 3.14))
```

### Calling C from Turmeric

```lisp
;; Bind a C library function
(extern-c printf [fmt :cstr] :int :variadic)

;; Or define it in DSL
(c-declare (fn [:cstr] :int :variadic) printf)

;; Call it
(c-call printf "Hello from C: %d\n" 42)
```

### Calling Turmeric from C

```lisp
;; Export a Turmeric function for C
(defn exported-add [a :int b :int] :int
  (+ a b))

(c-export exported-add "tur_add")

;; In C:
;; int result = tur_add(1, 2);
```

## Tutorial: Writing C Code in Turmeric

### Step 1: Basic Function

Let's start with a simple function:

```lisp
(c-defn square [x :int] :int
  (* x x))
```

This compiles to:

```c
int square(int x) {
    return x * x;
}
```

### Step 2: Using Variables

```lisp
(c-defn sum-and-difference [a :int b :int] :[:int 2]
  (c-let [sum :int (+ a b)
          diff :int (- a b)]
    (c-array :int [sum diff])))
```

Compiles to:

```c
int* sum_and_difference(int a, int b) {
    int sum = a + b;
    int diff = a - b;
    int result[2] = {sum, diff};
    return result;
}
```

### Step 3: Pointers and Arrays

```lisp
(c-defn array-sum [arr :[:int] len :int] :int
  (c-let [sum :int 0]
    (c-for [i :int 0] (< i len) (c-inc! i)
      (c-set! sum (+ sum (c-index arr i))))
    sum))
```

Compiles to:

```c
int array_sum(int* arr, int len) {
    int sum = 0;
    for (int i = 0; i < len; i++) {
        sum = sum + arr[i];
    }
    return sum;
}
```

### Step 4: Structs

```lisp
(c-defstruct Person
  [name :cstr]
  [age :int])

(c-defn create-person [name :cstr age :int] :Person
  (Person name age))

(c-defn print-person [p :Person] :void
  (c-call printf "Name: %s, Age: %d\n" (c-get p :name) (c-get p :age)))
```

Compiles to:

```c
typedef struct {
    char* name;
    int age;
} Person;

Person create_person(char* name, int age) {
    Person result = {name, age};
    return result;
}

void print_person(Person p) {
    printf("Name: %s, Age: %d\n", p.name, p.age);
}
```

### Step 5: Memory Management

```lisp
(c-defn create-int-array [size :int] :[:int]
  (c-let [arr :[:int] (c-malloc (* size (c-sizeof :int)))]
    (c-for [i :int 0] (< i size) (c-inc! i)
      (c-set! (c-index arr i) 0))
    arr))

(c-defn free-int-array [arr :[:int]] :void
  (c-free arr))
```

Compiles to:

```c
int* create_int_array(int size) {
    int* arr = malloc(size * sizeof(int));
    for (int i = 0; i < size; i++) {
        arr[i] = 0;
    }
    return arr;
}

void free_int_array(int* arr) {
    free(arr);
}
```

### Step 6: Function Pointers

```lisp
(c-typedef (fn [:int :int] :int) IntBinaryOp)

(c-defn apply-op [op :IntBinaryOp a :int b :int] :int
  (op a b))

(c-defn add [a :int b :int] :int (+ a b))
(c-defn mul [a :int b :int] :int (* a b))

(c-defn main [] :int
  (c-let [op :IntBinaryOp add]
    (apply-op op 3 4))
  0)
```

### Step 7: Complete Example - Binary Search

```lisp
(c-typedef (fn [:void* :void*] :int) CompareFunc)

(c-defn binary-search [arr :[:void*] len :int size :size-t 
                      cmp :CompareFunc key :void*] :[:void*]
  (c-let [low :int 0
          high :int (- len 1)]
    (c-while (<= low high)
      (c-let [mid :int (/ (+ low high) 2)
              mid-ptr :[:void*] (c-add arr (* mid size))
              comparison :int (cmp mid-ptr key)]
        (c-cond
          (= comparison 0) (c-return mid-ptr)
          (< comparison 0) (c-set! low (+ mid 1))
          :else (c-set! high (- mid 1))))))
    (c-return (c-null :void*))))

(c-defn int-compare [a :[:int] b :[:int]] :int
  (- (c-deref a) (c-deref b)))

(c-defn search-in-array [arr :[:int] len :int key :int] :[:int]
  (c-let [key-ptr :[:void*] (c-cast (c-addr key) :[:void*])]
    (c-cast (binary-search (c-cast arr :[:void*]) len (c-sizeof :int) int-compare key-ptr) :[:int])))
```

## Standard Library Bindings

The DSL includes bindings for common C standard library functions:

### stdio.h

```lisp
(c-declare (fn [:cstr] :int :variadic) printf)
(c-declare (fn [:cstr] :int) puts)
(c-declare (fn [:cstr :cstr] :int) sprintf)
(c-declare (fn [:cstr] :[:FILE*]) fopen)
(c-declare (fn [:[:FILE*]] :int) fclose)
(c-declare (fn [:[:FILE*] :cstr] :int) fprintf)
(c-declare (fn [:[:FILE*]] :[:char]) fgetc)
(c-declare (fn [:[:FILE*] :char] :int) fputc)
```

### stdlib.h

```lisp
(c-declare (fn [:size-t] :[:void*]) malloc)
(c-declare (fn [:[:void*] :size-t] :[:void*]) realloc)
(c-declare (fn [:[:void*]] :void) free)
(c-declare (fn [:size-t :size-t] :[:void*]) calloc)
(c-declare (fn [:void] :void) exit)
(c-declare (fn [:int] :void) abort)
```

### string.h

```lisp
(c-declare (fn [:cstr] :size-t) strlen)
(c-declare (fn [:cstr :cstr] :[:char]) strcpy)
(c-declare (fn [:cstr :cstr :size-t] :[:char]) strncpy)
(c-declare (fn [:cstr :cstr] :[:char]) strcat)
(c-declare (fn [:cstr :cstr] :int) strcmp)
(c-declare (fn [:cstr :cstr :size-t] :int) strncmp)
(c-declare (fn [:[:char*] :cstr :size-t] :[:char]) memmove)
(c-declare (fn [:[:char*] :[:char*] :size-t] :int) memcmp)
```

### math.h

```lisp
(c-declare (fn [:double] :double) sin)
(c-declare (fn [:double] :double) cos)
(c-declare (fn [:double] :double) tan)
(c-declare (fn [:double] :double) sqrt)
(c-declare (fn [:double] :double) exp)
(c-declare (fn [:double] :double) log)
(c-declare (fn [:double] :double) pow)
(c-declare (fn [:double] :double) fabs)
(c-declare (fn [:double] :double) floor)
(c-declare (fn [:double] :double) ceil)
```

### time.h

```lisp
(c-declare (fn [[:time-t*]] :time-t) time)
(c-declare (fn [[:time-t*]] :[:tm*]) localtime)
(c-declare (fn [[:time-t*]] :[:tm*]) gmtime)
(c-declare (fn [:double] :double) difftime)
```

## Macros for Code Generation

The DSL can be extended with Turmeric macros to generate C code:

```lisp
;; Macro to define a getter function for a struct field
(defmacro def-getter [struct-name field field-type]
  `(c-defn ,(symbol (str "get_" (stringify struct-name) "_" (stringify field)))
     [s :,struct-name] ,field-type
     (c-get s :,field)))

;; Usage
(def-getter Person name :cstr)
(def-getter Person age :int)

;; Generates:
;; c-defn get_Person_name [s :Person] :cstr (c-get s :name)
;; c-defn get_Person_age [s :Person] :int (c-get s :age)

;; Macro to define a setter function
(defmacro def-setter [struct-name field field-type]
  `(c-defn ,(symbol (str "set_" (stringify struct-name) "_" (stringify field)))
     [s :,struct-name v ,field-type] :void
     (c-set! (c-get s :,field) v)))

;; Macro to generate comparison functions
(defmacro def-compare [type]
  `(c-defn ,(symbol (str "compare_" (stringify type)))
     [a ,type b ,type] :int
     (c-cond
       (< a b) -1
       (> a b) 1
       :else 0)))

;; Usage
(def-compare :int)
(def-compare :float)
```

## Error Handling

The DSL compiler validates:

1. **Type Mismatches**: Catches operations on incompatible types
2. **Pointer Arithmetic**: Validates pointer arithmetic operations
3. **Memory Safety**: Warns about potential memory issues
4. **Function Signatures**: Validates function call signatures
5. **Struct Access**: Validates struct field access
6. **Array Bounds**: Optional bounds checking for arrays

## Performance Considerations

1. **Avoid Bounds Checking**: Use `-O2` or `-O3` for release builds
2. **Inline Functions**: Use `__attribute__((always_inline))` for hot functions
3. **Memory Alignment**: Use `aligned_alloc` for aligned memory
4. **Restrict Keyword**: Use `restrict` for pointer aliases
5. **Branch Prediction**: Use `__builtin_expect` for likely/unlikely branches

## Example: Complete C Program

Here's a complete example of a C program written in the DSL:

```lisp
(defn my-c-program []
  (inline-c-dsl
    
    ;; Includes
    (c-include "stdio.h")
    (c-include "stdlib.h")
    (c-include "string.h")
    
    ;; Constants
    (c-const [MAX-LINE-LENGTH :int 256])
    
    ;; Struct definition
    (c-defstruct StringList
      [strings :[:[:char*]]]
      [count :int]
      [capacity :int])
    
    ;; Function to create a string list
    (c-defn string-list-create [capacity :int] :StringList
      (c-let [sl :StringList]
        (c-set! (c-get sl :strings) (c-malloc (* capacity (c-sizeof :[:char*]))))
        (c-set! (c-get sl :capacity) capacity)
        (c-set! (c-get sl :count) 0)
        sl))
    
    ;; Function to add a string
    (c-defn string-list-add [sl :[:StringList] s :cstr] :void
      (c-let [count :int (c-get sl :count)
              capacity :int (c-get sl :capacity)]
        (c-if (= count capacity)
          (c-set! (c-get sl :strings) 
                  (c-realloc (c-get sl :strings) 
                            (* (* capacity 2) (c-sizeof :[:char*]))))
          (c-set! (c-get sl :capacity) (* capacity 2)))
        (c-set! (c-index (c-get sl :strings) count) (c-strdup s))
        (c-set! (c-get sl :count) (+ count 1))))
    
    ;; Function to free a string list
    (c-defn string-list-free [sl :[:StringList]] :void
      (c-let [count :int (c-get sl :count)
              strings :[:[:char*]] (c-get sl :strings)]
        (c-for [i :int 0] (< i count) (c-inc! i)
          (c-free (c-index strings i)))
        (c-free strings)
        (c-free sl)))
    
    ;; Function to print all strings
    (c-defn string-list-print [sl :[:StringList]] :void
      (c-let [count :int (c-get sl :count)
              strings :[:[:char*]] (c-get sl :strings)]
        (c-for [i :int 0] (< i count) (c-inc! i)
          (c-call puts (c-index strings i)))))
    
    ;; Main function
    (c-defn main [] :int
      (c-let [sl :[:StringList] (c-addr (string-list-create 10))]
        (string-list-add sl "Hello")
        (string-list-add sl "World")
        (string-list-add sl "from")
        (string-list-add sl "C DSL")
        (string-list-print sl)
        (string-list-free sl)
        0))
    
    ))

;; Call the C program from Turmeric
(defn run-c-program [] :int
  (my-c-program))
```

## Example: FFI Wrapper for SQLite

```lisp
;; Define SQLite types
(c-typedef :void* sqlite3)
(c-typedef :void* sqlite3_stmt)

;; Bind SQLite functions
(c-declare (fn [:cstr :[:sqlite3**]] :int) sqlite3_open)
(c-declare (fn [:sqlite3* :cstr] :int) sqlite3_close)
(c-declare (fn [:sqlite3* :cstr :int :[:sqlite3_stmt**] :[:cstr*]] :int) 
          sqlite3_prepare_v2)
(c-declare (fn [:sqlite3_stmt*] :int) sqlite3_step)
(c-declare (fn [:sqlite3_stmt*] :int) sqlite3_finalize)
(c-declare (fn [:sqlite3_stmt* :int] :cstr) sqlite3_column_text)

;; Wrapper functions
(c-defn db-open [path :cstr] :[:sqlite3]
  (c-let [db :[:sqlite3] (c-alloca :[:sqlite3])]
    (c-if (= (sqlite3_open path (c-addr db)) 0)
      (c-return db)
      (c-return (c-null :[:sqlite3])))))

(c-defn db-close [db :[:sqlite3]] :void
  (sqlite3_close (c-deref db)))

(c-defn db-query [db :[:sqlite3*] sql :cstr] :[:sqlite3_stmt]
  (c-let [stmt :[:sqlite3_stmt] (c-alloca :[:sqlite3_stmt])
          tail :[:cstr] (c-alloca :[:cstr])]
    (c-if (= (sqlite3_prepare_v2 db sql -1 (c-addr stmt) tail) 0)
      (c-return stmt)
      (c-return (c-null :[:sqlite3_stmt])))))

(c-defn db-step [stmt :[:sqlite3_stmt*]] :int
  (sqlite3_step (c-deref stmt)))

(c-defn db-column-text [stmt :[:sqlite3_stmt*] col :int] :cstr
  (sqlite3_column_text (c-deref stmt) col))

(c-defn db-finalize [stmt :[:sqlite3_stmt]] :void
  (sqlite3_finalize (c-deref stmt)))

;; Higher-level wrapper in Turmeric
(defn query-sqlite [db-path sql]
  (let [db (db-open db-path)]
    (when db
      (let [stmt (db-query db sql)]
        (when stmt
          (loop []
            (case (db-step stmt)
              100 ;; SQLITE_ROW
              (do
                (println (db-column-text stmt 0))
                (recur))
              101 ;; SQLITE_DONE
              (db-finalize stmt)
              _
              (do
                (db-finalize stmt)
                (println "Error executing query"))))
          (db-close db))))))
```

## Example: Embedding C in Turmeric Data Structures

```lisp
;; Define a C struct for a custom type
(c-defstruct CustomType
  [data :[:uint8 16]]
  [length :int])

;; Function to create a custom type
(c-defn create-custom [] :CustomType
  (CustomType (c-alloca :[:uint8 16]) 0))

;; Function to manipulate custom type
(c-defn custom-add [ct :[:CustomType] value :uint8] :void
  (c-let [len :int (c-get ct :length)]
    (c-if (< len 16)
      (c-set! (c-index (c-get ct :data) len) value)
      (c-set! (c-get ct :length) (+ len 1)))))

;; Use in Turmeric
(defn use-custom-type []
  (let [ct (create-custom)]
    (custom-add ct 42)
    (custom-add ct 99)
    ct))
```

## Future Enhancements

1. **C++ Support**: Extend DSL to support C++ features
2. **RAII Wrappers**: Automatic resource management
3. **Template Metaprogramming**: C++-style templates
4. **Variadic Templates**: Type-safe variadic functions
5. **Custom Allocators**: Support for custom memory allocators
6. **C11/C17 Features**: Support for newer C standards
7. **Static Analysis**: Integration with Clang Static Analyzer
8. **Sanitizers**: Integration with AddressSanitizer, UndefinedBehaviorSanitizer

## Distributing as a Spice

The C DSL is a pure-Turmeric library (no C dependencies of its own), so it
ships as a simple spice with no `:cmake-deps`.

### Adding the spice

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref c-dsl-v0.1.0 --subdir spices/c-dsl --name c-dsl
```

### `build.tur` manifest

```turmeric
(defpackage tur-c-dsl
  :name        "tur-c-dsl"
  :version     "0.1.0"
  :description "Lisp-syntax DSL that compiles to C99 source code"
  :license     "MIT"
  :repository  "https://github.com/turmeric-lang/turmeric-spices"

  :exports {
    "c-dsl/core"     ["c-let" "c-const" "c-set!" "c-do" "c-return"
                      "c-if" "c-cond" "c-while" "c-for" "c-do-while"
                      "c-switch" "c-break" "c-continue"]
    "c-dsl/types"    ["c-defstruct" "c-defunion" "c-defenum" "c-typedef"]
    "c-dsl/fns"      ["c-defn" "c-call" "c-declare" "c-extern-c" "c-export"]
    "c-dsl/mem"      ["c-addr" "c-deref" "c-index" "c-cast"
                      "c-malloc" "c-calloc" "c-realloc" "c-free" "c-alloca"]
    "c-dsl/pp"       ["c-include" "c-define" "c-ifdef" "c-ifndef"
                      "c-static-assert"]
    "c-dsl/codegen"  ["compile-c" "emit-c-file"]
    "c-dsl/builtins" ["c-sizeof" "c-alignof" "c-null"
                      "c-strcat" "c-strlen" "c-strdup" "c-asprintf"]
  })
```

### Consuming the spice

```turmeric
(import c-dsl/core   :refer [c-defn c-let c-for c-set! c-return])
(import c-dsl/mem    :refer [c-malloc c-free c-sizeof])
(import c-dsl/codegen :refer [compile-c])
```

### Spice layout inside turmeric-spices

```
spices/c-dsl/
  build.tur
  tur.lock
  src/
    core.tur      -- control flow, variable bindings
    types.tur     -- struct, union, enum, typedef
    fns.tur       -- function definition and declaration
    mem.tur       -- pointer and memory operations
    pp.tur        -- preprocessor directives
    codegen.tur   -- compile-c / emit-c-file entry points
    builtins.tur  -- standard library symbol declarations
  tests/
    core_test.tur
    codegen_test.tur
```

## File Structure

```
src/
├── compiler/
│   └── c/
│       ├── compiler.tur      # Main DSL compiler
│       ├── types.tur         # Type definitions and validation
│       ├── codegen.tur       # C code generation
│       ├── builtins.tur      # Standard library bindings
│       ├── ffi.tur           # FFI utilities
│       └── validate.tur      # Validation and error checking
├── stdlib/
│   └── c/
│       ├── stdio.tur         # stdio.h bindings
│       ├── stdlib.tur        # stdlib.h bindings
│       ├── string.tur        # string.h bindings
│       ├── math.tur          # math.h bindings
│       ├── time.tur          # time.h bindings
│       └── posix.tur         # POSIX bindings
```

## References

- [C99 Standard](https://www.iso.org/standard/29237.html)
- [C11 Standard](https://www.iso.org/standard/50526.html)
- [GNU C Manual](https://www.gnu.org/software/gnu-c-manual/)
- [Clang Documentation](https://clang.llvm.org/docs/)
- [c2x11 (C11 reference)](https://en.cppreference.com/w/c)
