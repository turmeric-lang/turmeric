;;; turmeric-mode.el --- Major mode for the Turmeric programming language  -*- lexical-binding: t; -*-

;; Copyright (C) 2024 Turmeric Project
;; URL: https://github.com/turmeric-lang/turmeric
;; Version: 0.1.0
;; Package-Requires: ((emacs "26.1"))
;; Keywords: languages lisp

;;; Commentary:
;;
;; Major mode for editing Turmeric (.tur) source files.  Provides syntax
;; highlighting, indentation, and comment support.
;;
;; Installation:
;;
;;   (add-to-list 'load-path "/path/to/turmeric/emacs")
;;   (require 'turmeric-mode)
;;
;; Or with use-package:
;;
;;   (use-package turmeric-mode
;;     :load-path "/path/to/turmeric/emacs")

;;; Code:

(require 'lisp-mode)

;;;###autoload
(defgroup turmeric nil
  "Major mode for the Turmeric programming language."
  :group 'languages
  :prefix "turmeric-")

;;;###autoload
(defcustom turmeric-indent-offset 2
  "Number of spaces per indentation level in `turmeric-mode'."
  :type 'integer
  :group 'turmeric)

;;; Syntax table

(defvar turmeric-mode-syntax-table
  (let ((table (make-syntax-table)))
    ;; Parentheses and brackets
    (modify-syntax-entry ?\( "()" table)
    (modify-syntax-entry ?\) ")(" table)
    (modify-syntax-entry ?\[ "(]" table)
    (modify-syntax-entry ?\] ")[" table)
    (modify-syntax-entry ?\{ "(}" table)
    (modify-syntax-entry ?\} "){" table)
    ;; String delimiter
    (modify-syntax-entry ?\" "\"" table)
    ;; Escape character in strings
    (modify-syntax-entry ?\\ "\\" table)
    ;; Line comment: ; starts, newline ends
    (modify-syntax-entry ?\; "<" table)
    (modify-syntax-entry ?\n ">" table)
    ;; Identifier-valid punctuation (symbol constituents)
    (dolist (ch '(?- ?! ?? ?* ?_ ?+ ?/ ?= ?< ?> ?& ?%))
      (modify-syntax-entry ch "_" table))
    ;; Prefix/quote characters
    (dolist (ch '(?\' ?\` ?\~ ?\@ ?\^ ?\#))
      (modify-syntax-entry ch "'" table))
    ;; Colon is a symbol constituent (for :keyword literals)
    (modify-syntax-entry ?\: "_" table)
    ;; | is a symbol constituent (for |> operator and union types)
    (modify-syntax-entry ?\| "_" table)
    table)
  "Syntax table for `turmeric-mode'.")

;;; Font-lock

(defconst turmeric--define-keywords
  '("def" "defn" "defmacro" "defmodule" "defdata" "defgadt"
    "defclass" "definstance" "defstruct" "deftuple"
    "defpackage" "use" "import" "export")
  "Turmeric definition-introducing keywords.")

(defconst turmeric--control-keywords
  '("if" "cond" "case" "match" "loop" "while" "for" "else"
    "break" "continue" "return" "and" "or" "not")
  "Turmeric control-flow keywords.")

(defconst turmeric--type-keywords
  '("type" "typeclass" "impl" "where" "forall" "generic" "trait" "any")
  "Turmeric type-system keywords.")

(defconst turmeric--effect-keywords
  '("effect" "handle" "do" "perform" "with")
  "Turmeric effect-system keywords.")

(defconst turmeric--exception-keywords
  '("try" "catch" "throw" "finally" "raise")
  "Turmeric exception keywords.")

(defconst turmeric--special-forms
  '("let" "let*" "lambda" "fn" "quote" "unquote" "quasiquote" "begin")
  "Turmeric core special forms.")

(defconst turmeric--builtins
  '("vec" "push" "pop" "len" "nth" "set-nth!" "append" "first" "rest"
    "cons" "reverse" "list" "apply" "map" "filter" "reduce" "fold"
    "print" "println" "read" "str" "concat" "split" "coerce" "cast"
    "type-of" "is-a?"
    "string?" "vector?" "list?" "number?" "symbol?"
    "boolean?" "null?" "atom?" "pair?" "empty?" "extern-c"
    "head" "tail")
  "Turmeric built-in functions.")

(defconst turmeric--nil-values
  '("nil" "null" "none" "unit")
  "Turmeric nil/unit constants.")

(defun turmeric--sym-rx (words)
  "Build a symbol-boundary font-lock regexp from WORDS."
  (concat "\\_<" (regexp-opt words t) "\\_>"))

(defconst turmeric-font-lock-keywords
  `(
    ;; Doc comments (;;;) -- override the syntax-based comment face
    (";;;.*$" 0 font-lock-doc-face t)

    ;; #lang directive: #lang sweet-exp / #lang turmeric/curly-infix / etc.
    ("\\(#lang\\)\\s-+\\([a-zA-Z][a-zA-Z0-9_/.+-]*\\)"
     (1 font-lock-preprocessor-face)
     (2 font-lock-type-face))

    ;; C inline blocks: ```c ... ``` (single-line form)
    ;; Multi-line blocks are left to Emacs string/comment handling.
    ("```c?[^`]*```" 0 font-lock-preprocessor-face t)
    ;; Opening ``` on its own (multi-line case: highlight the delimiter)
    ("```" 0 font-lock-preprocessor-face)

    ;; Boolean literals: #t #f
    ("#[tf]\\>" 0 font-lock-constant-face)

    ;; Character literals: #\newline  #\a  etc.
    ("#\\\\\\(?:nul\\(?:l\\)?\\|backspace\\|tab\\|newline\\|linefeed\\|vtab\\|page\\|return\\|space\\|rubout\\|altmode\\|escape\\|.\\)"
     0 font-lock-constant-face)

    ;; Reader conditional prefix: #?
    ("#?" 0 font-lock-preprocessor-face)

    ;; Metadata annotations: ^mut  ^linear  ^unique  ^affine
    ("\\^[a-zA-Z][a-zA-Z0-9_-]*" 0 font-lock-preprocessor-face)

    ;; Keyword literals: :foo  :else  :pre  :post
    (":[a-zA-Z_][a-zA-Z0-9_+*/<>=!?$-]*" 0 font-lock-constant-face)

    ;; Special operators: :: and |>
    ("::" 0 font-lock-builtin-face)
    ("|>" 0 font-lock-builtin-face)

    ;; Hexadecimal numbers: 0xDEAD  0XFF
    ("\\<0[xX][0-9a-fA-F]+" 0 font-lock-constant-face)
    ;; Binary numbers: 0b1010  0B1111
    ("\\<0[bB][01]+" 0 font-lock-constant-face)
    ;; Floating-point: 3.14  .5  1e-10  -2.5E+3
    ("-?\\(?:\\<[0-9]+\\.[0-9]*\\|\\.[0-9]+\\)\\(?:[eE][+-]?[0-9]+\\)?\\>"
     0 font-lock-constant-face)

    ;; Nil/unit constants: nil  null  none  unit
    (,(turmeric--sym-rx turmeric--nil-values) 0 font-lock-constant-face)

    ;; Definition keywords: defn  def  defmacro  ...
    (,(turmeric--sym-rx turmeric--define-keywords) 0 font-lock-keyword-face)

    ;; Control-flow keywords: if  cond  match  ...
    (,(turmeric--sym-rx turmeric--control-keywords) 0 font-lock-keyword-face)

    ;; Type-system keywords: type  typeclass  impl  ...
    (,(turmeric--sym-rx turmeric--type-keywords) 0 font-lock-type-face)

    ;; Effect-system keywords: effect  handle  do  perform  with
    (,(turmeric--sym-rx turmeric--effect-keywords) 0 font-lock-keyword-face)

    ;; Exception keywords: try  catch  throw  finally  raise
    (,(turmeric--sym-rx turmeric--exception-keywords) 0 font-lock-keyword-face)

    ;; Special forms: let  let*  lambda  fn  ...
    (,(turmeric--sym-rx turmeric--special-forms) 0 font-lock-keyword-face)

    ;; Built-in functions
    (,(turmeric--sym-rx turmeric--builtins) 0 font-lock-builtin-face)

    ;; Function name in (defn name ...) or (defmacro name ...)
    ("(\\(?:defn\\|defmacro\\)[[:space:]]+\\(\\_<[a-zA-Z_][a-zA-Z0-9_*!?-]*\\_>\\)"
     1 font-lock-function-name-face)

    ;; Type name in (defdata Name) / (defgadt Name) / (defstruct Name) / (deftuple Name)
    ("(\\(?:defdata\\|defgadt\\|defstruct\\|deftuple\\)[[:space:]]+\\(\\_<[a-zA-Z_][a-zA-Z0-9_*!?-]*\\_>\\)"
     1 font-lock-type-face)

    ;; Module/package name in (defmodule Name) / (defpackage Name)
    ("(\\(?:defmodule\\|defpackage\\)[[:space:]]+\\(\\_<[a-zA-Z_][a-zA-Z0-9_*!?.-]*\\_>\\)"
     1 font-lock-function-name-face)

    ;; Variable name in a bare (def name ...) -- not (defn/defdata/etc.)
    ("(def[[:space:]]+\\(\\_<[a-zA-Z_][a-zA-Z0-9_*!?-]*\\_>\\)"
     1 font-lock-variable-name-face)

    ;; Curly-infix delimiters { } (sweet-exp infix context)
    ("[{}]" 0 font-lock-builtin-face)

    ;; Neoteric function application: identifier(args) without space
    ("\\([a-zA-Z_][a-zA-Z0-9_*!?/-]*\\)(" 1 font-lock-function-name-face))
  "Font-lock keywords for `turmeric-mode'.")

;;; Indentation

;; Forms whose bodies are indented by `turmeric-indent-offset' from the
;; opening paren (i.e. treated like `defun' in standard Lisp indentation).
(defconst turmeric--body-indent-forms
  '("defn" "defmacro" "defmodule" "defdata" "defgadt"
    "defclass" "definstance" "defstruct" "deftuple" "defpackage"
    "let" "let*" "fn" "lambda" "begin" "do"
    "if" "cond" "case" "match" "loop" "while" "for"
    "when" "unless" "effect" "handle" "try" "catch" "with")
  "Turmeric forms that use defun-style (body) indentation.")

(defun turmeric--indent-function (indent-point state)
  "Indentation function for `turmeric-mode'.

INDENT-POINT is the position of the start of the current line.
STATE is the result of `parse-partial-sexp' up to INDENT-POINT."
  (let ((normal-indent (current-column)))
    (goto-char (1+ (elt state 1)))
    (parse-partial-sexp (point) calculate-lisp-indent-last-sexp 0 t)
    (if (and (elt state 2)
             (not (looking-at "\\sw\\|\\s_")))
        ;; Car of the sexp is not a symbol -- use regular lisp alignment.
        (progn
          (if (not (> (save-excursion (forward-line 1) (point))
                      calculate-lisp-indent-last-sexp))
              (progn (goto-char calculate-lisp-indent-last-sexp)
                     (beginning-of-line)
                     (parse-partial-sexp (point)
                                        calculate-lisp-indent-last-sexp 0 t)))
          (backward-prefix-chars)
          (current-column))
      (let* ((function (buffer-substring (point)
                                         (progn (forward-sexp 1) (point))))
             (method (get (intern-soft function) 'turmeric-indent-function)))
        (cond
         ((or (eq method 'defun)
              (and (null method)
                   (member function turmeric--body-indent-forms)))
          (lisp-indent-specform 2 state indent-point normal-indent))
         ((integerp method)
          (lisp-indent-specform method state indent-point normal-indent))
         (t
          (lisp-indent-specform 1 state indent-point normal-indent)))))))

;;; Keymap

(defvar turmeric-mode-map
  (let ((map (make-sparse-keymap)))
    map)
  "Keymap for `turmeric-mode'.")

;;; Mode definition

;;;###autoload
(define-derived-mode turmeric-mode prog-mode "Turmeric"
  "Major mode for editing Turmeric (.tur) source files.

Turmeric is a statically-typed Lisp with an effect system and C interop.
File extension: .tur

Highlights:
  - Definition, control-flow, type, effect, and exception keywords
  - Doc comments (;;;), line comments (;), block comments (#| ... |#)
  - String and character literals, boolean and nil constants
  - Keyword literals (:foo), metadata annotations (^mut)
  - Hex/binary/float number literals
  - C inline blocks (```c ... ```)
  - Built-in functions
  - Sweet-exp: #lang directive, curly-infix {a + b}, neoteric calls foo(args)

Indentation follows standard Lisp conventions; forms listed in
`turmeric--body-indent-forms' use defun-style body indentation.

\\{turmeric-mode-map}"
  :syntax-table turmeric-mode-syntax-table
  :group 'turmeric

  ;; Comments
  (setq-local comment-start ";")
  (setq-local comment-end "")
  (setq-local comment-start-skip ";+\\s-*")
  (setq-local comment-add 1)
  (setq-local comment-use-syntax t)

  ;; Font lock
  (setq-local font-lock-defaults
              '(turmeric-font-lock-keywords
                nil    ; use syntax table for strings/comments too
                nil    ; case-sensitive
                nil    ; no syntax-alist overrides
                nil))  ; no syntax-begin override
  (setq-local font-lock-multiline t)

  ;; Indentation
  (setq-local indent-line-function #'lisp-indent-line)
  (setq-local lisp-indent-function #'turmeric--indent-function)
  (setq-local tab-width turmeric-indent-offset)
  (setq-local indent-tabs-mode nil)

  ;; Paragraph and fill
  (setq-local paragraph-ignore-fill-prefix t)
  (setq-local fill-paragraph-function #'lisp-fill-paragraph)

  ;; Electric pair support (works with electric-pair-mode)
  (setq-local electric-pair-pairs
              '((?\" . ?\") (?\( . ?\)) (?\[ . ?\]) (?\{ . ?\}))))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.tur\\'" . turmeric-mode))

(provide 'turmeric-mode)

;;; turmeric-mode.el ends here
