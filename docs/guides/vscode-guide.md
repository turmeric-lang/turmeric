---
title: VS Code Extension
category: Editor and IDE
description: VS Code extension installation and configuration
tags:
  - lsp
  - vscode
---

# VS Code Extension for Turmeric

The Turmeric VS Code extension provides syntax highlighting for `.tur` files
(both standard S-expression syntax and sweet-exp indentation-based syntax),
document formatting via `tur format`, and a language client that launches the
compiler's built-in language server (`tur lsp`).

The extension lives in its own repo,
[turmeric-lang/vscode-turmeric](https://github.com/turmeric-lang/vscode-turmeric). It also wires up the
interpreter debugger (`tur dap`); see the [debugging guide](debugging-guide.md).

## Installation

### From source

```sh
git clone https://github.com/turmeric-lang/vscode-turmeric
cd vscode-turmeric

# Option A: copy to extensions directory
mkdir -p ~/.vscode/extensions/turmeric-syntax-0.2.0
cp -r * ~/.vscode/extensions/turmeric-syntax-0.2.0/

# Option B: symlink for live editing
ln -s "$(pwd)" ~/.vscode/extensions/turmeric-syntax-dev
```

Reload VS Code after either option.

### From a packaged `.vsix`

Build the package (see "Building a `.vsix`" below), then:

```sh
code --install-extension turmeric-syntax-0.2.0.vsix
```

## Features

Once installed, any `.tur` file is automatically highlighted:

- **Keywords** -- `defn`, `let`, `if`, `match`, `handle`, `defclass`,
  `definstance`, `defeffect`, and all other Turmeric special forms
- **Built-in functions** -- `cons`, `car`, `cdr`, `map`, `filter`, etc.
- **Literals** -- integers, floats, hex (`0xFF`), binary (`0b1010`),
  scientific notation, strings, booleans (`#t`, `#f`), characters (`#\a`)
- **Comments** -- line comments (`;`) and block comments (`#| ... |#`)
- **Operators** -- arithmetic, comparison, logical, threading (`->`, `->>`, `|>`)
- **Meta-programming** -- quote (`'`), quasiquote (`` ` ``), unquote (`,`)

### Bracket and indentation support

- Bracket pair colorization and auto-close for `()`, `[]`, `{}`
- Comment toggle with `Cmd+/` (`;` for line comments)
- Auto-indentation increases after opening brackets and control forms

### Formatting

The extension registers a document formatter that pipes the buffer through
`tur format` (requires `tur` on `PATH`). Use the standard Format Document
command, the extension's "Format Turmeric Document" command, or enable
`editor.formatOnSave` for the `[turmeric]` language.

### Language server

The extension launches `tur lsp` for `.tur` files (diagnostics, hover docs,
go-to-definition). The `turmeric.serverPath` setting (default `"tur"`)
points it at the executable to use.

## Recommended VS Code settings

```json
{
  "[turmeric]": {
    "editor.tabSize": 2,
    "editor.insertSpaces": true,
    "editor.formatOnSave": false
  }
}
```

## Scope names (for theme customization)

| Scope | Matches |
|---|---|
| `keyword.control.turmeric` | `if`, `match`, `loop`, `while`, etc. |
| `keyword.definition.turmeric` | `defn`, `defmacro`, `defstruct`, etc. |
| `keyword.type.turmeric` | `:int`, `:cstr`, type annotations |
| `keyword.effect.turmeric` | `defeffect`, `handle`, `perform` |
| `support.function.builtin.turmeric` | Built-in functions |
| `entity.name.function.turmeric` | User-defined function names |
| `constant.numeric.integer.turmeric` | Integer literals |
| `constant.numeric.float.turmeric` | Float literals |
| `constant.language.boolean.true.turmeric` | `#t` |
| `constant.language.boolean.false.turmeric` | `#f` |
| `constant.character.turmeric` | Character literals |
| `string.quoted.double.turmeric` | String literals |
| `comment.line.semicolon.turmeric` | Line comments |
| `comment.block.turmeric` | Block comments |

## Known limitations

- Sweet-exp detection is heuristic (indentation patterns); a full parser
  would be needed for perfect accuracy.
- Macro expansions are not highlighted differently from regular calls.
- The language server requires a `tur` binary on `PATH` (or a
  `turmeric.serverPath` setting pointing at one).

## Building a `.vsix`

```sh
cd vscode-turmeric
npm install -g @vscode/vsce
vsce package
# produces turmeric-syntax-X.Y.Z.vsix
```

## See also

- [turmeric-lang/vscode-turmeric](https://github.com/turmeric-lang/vscode-turmeric) -- Extension source
  (`syntaxes/turmeric.tmLanguage.json` is the TextMate grammar,
  `language-configuration.json` the bracket / comment config)
- [formatter-guide.md](formatter-guide.md) -- the `tur format` CLI the
  extension's formatter shells out to
