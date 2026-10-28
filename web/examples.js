// examples.js -- the programs behind Try Turmeric's Examples menu.
//
// Keyed by the menu item's data-example (try/index.html); loadExample() in
// main.js puts one in the editor.

export const EXAMPLES = {
    hello: `(println "Hello, World!")`,
    math: `;; Basic arithmetic
(+ 1 2 3)
(- 10 5)
(* 2 3 4)
(/ 100 4)`,
    factorial: `;; Factorial function
(defn factorial [n :int] :int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))

(factorial 10)`,
    fibonacci: `;; Fibonacci sequence
(defn fib [n :int] :int
  (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))

(fib 10)`,
    closure: `;; Closures example
(defn make-adder [x :int] (fn [y :int] (+ x y)))

(let [add5 (make-adder 5)]
  (println (add5 10)))

(let [add10 (make-adder 10)]
  (println (add10 20)))`,
    effects: `(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))`,
    sweet: `#lang turmeric/sweet
;; Sweet-exp syntax: indentation, curly-infix, neoteric, $

defn square [x : int] : int
  {x * x}

defn sum-squares [a : int b : int] : int
  +(square(a) square(b))

println $ sum-squares 3 4
`,
    'adt': `;; Algebraic data types and pattern matching
(defdata ^copy Shape
  (Circle :float)
  (Rect   :float :float))

(defn area [s : Shape] : float
  (match s
    (Circle r) (* 3.14159 (* r r))
    (Rect w h) (* w h)))

(println (area (Circle 2.5)))
(println (area (Rect 3.5 4.0)))
`,
    'result': `;; Option and Result: no nulls, no exceptions
(defn safe-div [a : int b : int] : (Result int cstr)
  (if (= b 0)
    (err "division by zero")
    (ok (/ a b))))

(defn show [r : (Result int cstr)] : void
  (match r
    (Ok v)  (println v)
    (Err e) (println e)))

(show (safe-div 84 2))
(show (safe-div 1 0))
`,
    'literals': `;; Data literals: vectors, maps and sets
(let [v [10 20 30]
      m #map{:name "Ada" :lang "Turmeric"}
      s #set{1 1 2 3 3}]
  (println (vec-len v))
  (println (vec-get v 1))
  (println (map-get m :name))
  (println (set-count s)))   ; duplicates collapse
`,
    // Runs on the Compile engine (wasm-jit-plan P4); the interpreter cannot
    // call C, and says so -- with a pointer to Compile -- when asked to.
    'inline-c': `;; Inline C -- pick Compile in the language menu's Engine section.
;; Compile turns the whole program into C, so a C body is just more of it.
(defn popcount [x : int] : int
  \`\`\`c
  int n = 0;
  for (unsigned long long v = (unsigned long long)x; v; v &= v - 1) n++;
  return n;
  \`\`\`)

(defn hypot [a : float b : float] : float
  \`\`\`c
  #include <math.h>
  return sqrt(a * a + b * b);
  \`\`\`)

(println (popcount 255))
(println (hypot 3.0 4.25))`,
    'saffron': `#lang saffron
;; Saffron: Turmeric with the types made optional.
;; Unannotated parameters and returns are any.

(defn add [a b] (+ a b))
(defn kind-of [x] (type-of x))
(defn describe [x] (if x "truthy" "falsy"))

(println (add 1 2))         ; ints
(println (add 1.5 2.25))    ; floats, same function
(println (kind-of "hi"))
(println (describe 0))      ; only nil and false are falsy
(println (describe false))
`,
    'saffron-hof': `#lang saffron
;; Higher-order Saffron: closures and vectors, no annotations
(defn make-adder [n] (fn [x] (+ x n)))
(defn apply-twice [f x] (f (f x)))
(defn sum [v] (vec-fold v 0 (fn [acc x] (+ acc x))))

(println (apply-twice (make-adder 7) 28))
(println (sum [1 2 3 4]))
(println (vec-len (vec-filter [1 2 3 4] (fn [x] (> x 2)))))
(println (vec-len [1 "two" 3.5]))   ; heterogeneous
`,
    'saffron-adt': `#lang saffron
;; Saffron ADTs: annotate the data's shape, leave the code dynamic
(defdata ^copy Shape
  (Circle :float)
  (Rect   :float :float))

(defn area [s]
  (match s
    (Circle r) (* 3.14159 (* r r))
    (Rect w h) (* w h)))

(defn classify [n]
  (cond (> n 10) "big"
        (> n 0)  "small"
        else     "none"))

(println (area (Circle 2.5)))
(println (classify (area (Rect 3.5 4.0))))
(println (classify (area (Rect 0.5 1.5))))
`,
    'saffron-sweet': `#lang saffron/sweet
;; Saffron in sweet-expression syntax

defn abs [n]
  if {n < 0}
    {0 - n}
    n

defn countdown [n]
  when {n > 0}
    do
      println(n)
      countdown({n - 1})

println $ abs -7.1
countdown(3)
`,
    'scheme': `#lang r7rs
;; R7RS Scheme on the Turmeric runtime
(import (scheme base) (scheme write))

(define (fact n)
  (if (= n 0) 1 (* n (fact (- n 1)))))

(display (fact 25))     ; exact integers never overflow
(newline)
(write (map (lambda (x) (* x x)) (list 1 2 3 4)))
(newline)
(write (list (/ 7 2) (+ 1/2 1/3) (sqrt -4)))
(newline)
`,
    'scheme-macros': `#lang r7rs
;; Hygienic macros, named let, and call/cc
(import (scheme base) (scheme write))

(define-syntax swap!
  (syntax-rules ()
    ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))

(let ((p 1) (q 2))
  (swap! p q)
  (write (list p q))
  (newline))

(let loop ((i 0) (acc '()))
  (if (< i 5)
      (loop (+ i 1) (cons i acc))
      (begin (write acc) (newline))))

(define (find-first pred xs)
  (call-with-current-continuation
    (lambda (return)
      (for-each (lambda (x) (if (pred x) (return x))) xs)
      #f)))

(write (find-first even? '(1 3 4 5 6)))
(newline)
`,
    'scheme-records': `#lang r7rs
;; Records and error handling with guard
(import (scheme base) (scheme write))

(define-record-type point
  (make-point x y)
  point?
  (x point-x)
  (y point-y))

(define (dist2 p)
  (+ (* (point-x p) (point-x p)) (* (point-y p) (point-y p))))

(write (dist2 (make-point 3 4)))
(newline)

(define (checked-sqrt n)
  (if (< n 0)
      (raise 'negative)
      (sqrt n)))

(write (guard (e ((symbol? e) (list 'caught e)))
  (checked-sqrt -1)))
(newline)
(write (checked-sqrt 16))
(newline)
`,
    'scheme-sweet': `#lang r7rs/sweet
;; Scheme written with sweet-expressions (SRFI-110)
import (scheme base) (scheme write)

define (fib n)
  if {n < 2}
    n
    {fib{n - 1} + fib{n - 2}}

display $ fib 20
newline()
write $ map fib '(1 2 3 4 5 6 7 8)
newline()
`
};
