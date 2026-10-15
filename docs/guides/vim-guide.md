---
title: Vim Guide
category: Editor and IDE
description: Vim / Neovim syntax highlighting installation and configuration
---

# Vim Syntax Highlighting for Turmeric

The Turmeric Vim plugin provides syntax highlighting for `.tur` files in Vim
and Neovim. It lives in `vim-syntax/` at the repo root.

## Installation

### Manual (no plugin manager)

Copy or symlink the two directories into `~/.vim` (Vim) or `~/.config/nvim`
(Neovim):

```sh
# Vim
cp -r vim-syntax/syntax   ~/.vim/syntax/
cp -r vim-syntax/ftdetect ~/.vim/ftdetect/

# Neovim
cp -r vim-syntax/syntax   ~/.config/nvim/syntax/
cp -r vim-syntax/ftdetect ~/.config/nvim/ftdetect/
```

Alternatively, symlink the whole plugin directory so updates are picked up
automatically:

```sh
# Vim
ln -s "$(pwd)/vim-syntax" ~/.vim/pack/turmeric/start/turmeric

# Neovim
ln -s "$(pwd)/vim-syntax" ~/.config/nvim/pack/turmeric/start/turmeric
```

### vim-plug

```vim
Plug '/path/to/turmeric/vim-syntax'
```

### lazy.nvim

```lua
{
  dir = "/path/to/turmeric/vim-syntax",
  ft = "tur",
}
```

### packer.nvim

```lua
use { "/path/to/turmeric/vim-syntax", ft = "tur" }
```

## Features

Once installed, any `.tur` file is automatically highlighted:

- **Definition forms** -- `defn`, `defmacro`, `defstruct`, `defclass`,
  `definstance`, `defgadt`, `defmodule`, and all other definition keywords
- **Control flow** -- `if`, `cond`, `case`, `match`, `loop`, `while`, `for`,
  `and`, `or`, `not`
- **Type keywords** -- `type`, `typeclass`, `forall`, `where`, `impl`, `trait`
- **Effect system** -- `effect`, `handle`, `perform`, `do`, `with`
- **Exception forms** -- `try`, `catch`, `throw`, `finally`, `raise`
- **Core special forms** -- `let`, `let*`, `lambda`, `fn`, `begin`, quote forms
- **Built-in functions** -- `cons`, `map`, `filter`, `reduce`, `vec`, `len`,
  predicates (`string?`, `list?`, `null?`, etc.), and more
- **Literals** -- integers, hex (`0xFF`), binary (`0b1010`), floats,
  scientific notation, booleans (`#t`, `#f`), characters (`#\a`, `#\newline`)
- **Strings** -- with escape sequence validation (`\n`, `\t`, `A`, etc.)
- **Comments** -- line comments (`;`), docstring comments (`;;;`), and block
  comments (`#| ... |#`, nestable)
- **Keyword literals** -- `:as`, `:pre`, `:post`, `:name`, and other
  colon-prefixed symbols
- **Metadata annotations** -- `^linear`, `^unique`, `^affine`, `^mut`, etc.
- **Reader macros** -- quote (`'`), quasiquote (`` ` ``), unquote (`~`),
  unquote-splicing (`~@`)
- **Operators** -- `::` (type ascription), `|>` (pipe)
- **C inline blocks** -- `` ```c ... ``` `` blocks highlighted as preprocessor

## Recommended settings

Add to your `~/.vim/after/ftplugin/turmeric.vim` (Vim) or the equivalent Lua
config (Neovim):

```vim
" Turmeric ftplugin settings
setlocal tabstop=2
setlocal shiftwidth=2
setlocal expandtab
setlocal textwidth=80

" Use ; as the comment leader for commentary.vim / comment operators
setlocal commentstring=;\ %s

" Treat hyphens as part of words for motions (already set by syntax file,
" but explicit here for clarity)
setlocal iskeyword+=45
```

For Neovim with `nvim-treesitter` not yet supporting Turmeric, the Vim syntax
file is the correct fallback -- no additional configuration is needed.

## Color schemes

`vim-syntax/colors/` carries the two themes turmeric-lang.com uses, ported
from the Try Turmeric editor: `turmeric-dark` and `turmeric-light`. Both need
a true-color terminal. Copy the directory next to `syntax/` and pick one:

```sh
cp -r vim-syntax/colors ~/.vim/colors/            # Vim
cp -r vim-syntax/colors ~/.config/nvim/colors/    # Neovim
```

```vim
set termguicolors
colorscheme turmeric-light   " or turmeric-dark
```

Each scheme sets `background` itself. To follow the terminal's own setting
instead, choose by it:

```vim
execute 'colorscheme turmeric-' . (&background ==# 'light' ? 'light' : 'dark')
```

## Highlight groups

| Group | Default link | Matches |
|---|---|---|
| `turmericDefine` | `Define` | `defn`, `defmacro`, `defstruct`, etc. |
| `turmericControl` | `Conditional` | `if`, `cond`, `match`, `loop`, etc. |
| `turmericType` | `Type` | `type`, `typeclass`, `forall`, `where` |
| `turmericEffect` | `Keyword` | `effect`, `handle`, `perform`, `with` |
| `turmericExcept` | `Exception` | `try`, `catch`, `throw`, `finally` |
| `turmericSpecial` | `Special` | `let`, `let*`, `lambda`, `fn`, `begin` |
| `turmericBuiltin` | `Function` | Built-in functions and predicates |
| `turmericNil` | `Constant` | `nil`, `null`, `none`, `unit` |
| `turmericInt` | `Number` | Integer literals |
| `turmericFloat` | `Float` | Floating-point literals |
| `turmericHex` | `Number` | Hex literals (`0xFF`) |
| `turmericBin` | `Number` | Binary literals (`0b1010`) |
| `turmericBool` | `Boolean` | `#t`, `#f` |
| `turmericChar` | `Character` | Character literals (`#\a`, `#\newline`) |
| `turmericString` | `String` | String literals |
| `turmericStringEsc` | `SpecialChar` | Valid escape sequences |
| `turmericBadEsc` | `Error` | Unrecognized escape sequences |
| `turmericKeyLit` | `Constant` | Keyword literals (`:name`) |
| `turmericMeta` | `PreProc` | Metadata annotations (`^linear`) |
| `turmericLineComment` | `Comment` | `;` and `;;` line comments |
| `turmericDocComment` | `SpecialComment` | `;;;` docstring comments |
| `turmericBlockComment` | `Comment` | `#| ... |#` block comments |
| `turmericTodo` | `Todo` | `TODO`, `FIXME`, `HACK`, `NOTE`, `XXX` |
| `turmericQuote` | `Special` | `'`, `` ` `` |
| `turmericUnquote` | `Special` | `~` |
| `turmericSplice` | `Special` | `~@` |
| `turmericOp` | `Operator` | `::`, `\|>` |
| `turmericCBlock` | `PreProc` | `` ```c ... ``` `` inline C blocks |
| `turmericDelim` | `Delimiter` | `()`, `[]`, `{}` |

To override a highlight group, add lines like this to your colorscheme or
`after/syntax/turmeric.vim`:

```vim
hi turmericDocComment guifg=#a0c4ff gui=italic
hi turmericDefine     guifg=#ffd166 gui=bold
```

## Rainbow parentheses

The syntax file links `turmericDelim` to `Delimiter`. If you use a rainbow
parentheses plugin (e.g. `rainbow`, `rainbow_parentheses.vim`), add Turmeric
to its file type list:

```vim
" rainbow_parentheses.vim
au VimEnter * RainbowParenthesesActivate
au Syntax turmeric RainbowParenthesesLoadRound
au Syntax turmeric RainbowParenthesesLoadSquare
au Syntax turmeric RainbowParenthesesLoadBraces
```

For `nvim-ts-rainbow` or `rainbow-delimiters.nvim` in Neovim, no extra
configuration is needed once Treesitter supports Turmeric.

## Known limitations

- `#| ... |#` block comment nesting is syntactically correct but Vim's
  highlight engine does not track nesting depth, so deeply nested block
  comments may mis-highlight.
- C inline blocks (`` ```c ... ``` ``) are highlighted as `PreProc` rather
  than embedded C syntax. Embedded language highlighting is possible with
  `:syn include` but requires `vim-syntax/syntax/turmeric.vim` to be
  restructured as a cluster.
- The plugin itself is syntax-only. For hover docs, go-to-definition, and
  diagnostics, wire up the compiler's built-in language server (`tur lsp`)
  through your LSP client of choice (e.g. Neovim's `vim.lsp.start` or
  `nvim-lspconfig` with a custom server entry running `tur lsp`).
- Operator highlighting is limited to `::` and `|>`. Arithmetic and comparison
  operators (`+`, `-`, `<`, `>`, `=`, etc.) are left uncolored, which matches
  typical Lisp syntax conventions.

## See also

- `vim-syntax/` -- Plugin source
- `vim-syntax/syntax/turmeric.vim` -- Syntax definitions
- `vim-syntax/ftdetect/turmeric.vim` -- File type detection
- [vscode-guide.md](vscode-guide.md) -- VS Code extension
- [formatter-guide.md](formatter-guide.md) -- `tur format` CLI
