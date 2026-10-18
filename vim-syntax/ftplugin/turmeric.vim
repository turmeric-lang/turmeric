" Turmeric filetype settings.
if exists("b:did_ftplugin") | finish | endif
let b:did_ftplugin = 1

setlocal commentstring=;\ %s
setlocal comments=:;;;,:;;,:;
" 2-space indent throughout, per the style guide in CLAUDE.md.
setlocal expandtab shiftwidth=2 softtabstop=2
" `-` `?` `!` `*` `/` are ordinary identifier characters here, so `w` and `*`
" treat `vec-push!` and `is?` as single words rather than four.
setlocal iskeyword+=-,?,!,*,/,<,>,=,+
setlocal lisp
setlocal lispwords=defn,defmacro,defstruct,defdata,defgadt,defclass,definstance,defmodule,defopaque,defeffect,deftype,defalias,let,letrec,loop,fn,if,when,unless,cond,case,match,do,for,while,handle,with-handler,with-region,bt-scope,reset,shift

" r7rs-lang-plan R9: Scheme's body forms indent like Turmeric's, in a
" `#lang r7rs` file (matching `tur fmt`, which re-indents Scheme the same way).
if getline(1) =~# '^#lang\s\+r7rs\>'
  setlocal lispwords+=define,define-library,define-syntax,define-record-type,define-values,lambda,case-lambda,let*,letrec*,let-values,let*-values,let-syntax,letrec-syntax,syntax-rules,begin,guard,parameterize,delay,delay-force,with-exception-handler,dynamic-wind
endif

let b:undo_ftplugin = "setlocal commentstring< comments< expandtab< shiftwidth< softtabstop< iskeyword< lisp< lispwords<"
