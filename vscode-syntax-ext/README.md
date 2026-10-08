# Turmeric Syntax Highlighting for VSCode

Syntax highlighting for [Turmeric](https://github.com/fith-lang/fith) `.tur` files with support for sweet-exp (indentation-based) syntax.

## Features

✨ **Comprehensive Syntax Highlighting**
- Keywords: control flow, definitions, types, effects, exception handling
- Built-in functions: vector operations, type checks, functional operations
- Numbers: integers, floats, hex, binary, scientific notation
- Strings with escape sequence highlighting
- Comments: line (`;`) and block (`#| ... |#`)
- Booleans (`#t`, `#f`) and special constants (`nil`, `null`)
- Characters (`#\a`, `#\newline`, etc.)

🎯 **Language Features**
- S-expression syntax highlighting
- Sweet-exp indentation support
- Quote and meta-programming operators
- Threading/piping operators (`->`, `->>`, `|>`)
- Operator highlighting (arithmetic, comparison, logical)
- Function and variable name distinction

📦 **VSCode Integration**
- Bracket pair colorization
- Auto-indentation rules
- Bracket auto-closing
- Comment toggling (⌘+/)
- Proper bracket matching and navigation

## Installation

### From Marketplace (Coming Soon)
1. Open VSCode Extensions marketplace
2. Search for "turmeric"
3. Click Install

### From Source (Development)

1. Clone the repository:
```bash
git clone https://github.com/turmeric-lang/turmeric.git
cd turmeric/vscode-syntax-ext
```

2. Install in VSCode:
```bash
# Option A: Copy to extensions directory
mkdir -p ~/.vscode/extensions/turmeric-syntax-0.1.0
cp -r * ~/.vscode/extensions/turmeric-syntax-0.1.0/

# Option B: Link for development
ln -s "$(pwd)" ~/.vscode/extensions/turmeric-syntax-dev
```

3. Reload VSCode

## Usage

Once installed, any `.tur` file will automatically be highlighted with Turmeric syntax.

Open the test file to see syntax highlighting in action:
```bash
code vscode-syntax-ext/test/test-syntax.tur
```

## Scope Names (For Theme Creators)

Use these scope names to customize colors in your theme:

### Keywords
- `keyword.control.turmeric` - Control flow (if, match, loop, etc.)
- `keyword.definition.turmeric` - Definitions (def, defn, defmacro, etc.)
- `keyword.type.turmeric` - Type system (type, typeclass, impl, etc.)
- `keyword.effect.turmeric` - Effects (effect, handle, do, etc.)
- `keyword.exception.turmeric` - Exception handling (try, catch, throw)
- `keyword.other.turmeric` - Other keywords (let, lambda, quote, etc.)
- `keyword.constant.null.turmeric` - Null/nil constants

### Functions
- `support.function.builtin.turmeric` - Built-in functions
- `entity.name.function.turmeric` - User-defined function names

### Literals
- `constant.numeric.integer.turmeric` - Integer literals
- `constant.numeric.float.turmeric` - Float literals
- `constant.numeric.hex.turmeric` - Hexadecimal numbers
- `constant.numeric.binary.turmeric` - Binary numbers
- `constant.language.boolean.true.turmeric` - `#t` true value
- `constant.language.boolean.false.turmeric` - `#f` false value
- `constant.character.turmeric` - Character literals
- `string.quoted.double.turmeric` - Strings

### Structure
- `punctuation.section.sexp.begin.turmeric` - Opening parenthesis
- `punctuation.section.sexp.end.turmeric` - Closing parenthesis
- `punctuation.section.bracket.begin.turmeric` - Opening bracket
- `punctuation.section.bracket.end.turmeric` - Closing bracket
- `punctuation.section.brace.begin.turmeric` - Opening brace
- `punctuation.section.brace.end.turmeric` - Closing brace

### Comments
- `comment.line.semicolon.turmeric` - Line comments
- `comment.block.turmeric` - Block comments

### Variables
- `variable.other.turmeric` - Variable names

## Supported Syntax

### Control Flow
```turmeric
(if condition then-expr else-expr)
(cond [test1 expr1] [test2 expr2] [else default])
(match value [pattern1 expr1] [pattern2 expr2])
(loop [var init] body)
(while condition body)
```

### Definitions
```turmeric
(def name value)
(defn name [params] body)
(defmacro name [params] body)
(defmodule name exports definitions)
```

### Types and Effects
```turmeric
(type TypeName [field1: Type1 field2: Type2])
(typeclass ClassName [a] [method1 signature] [method2 signature])
(effect EffectName [a] [operation1 sig] [operation2 sig])
(handle effect-expr handler-patterns)
```

### Collections
```turmeric
'(a b c)                ; list
[a b c]                 ; vector
{key: value}            ; map (sweet-exp)
(list 1 2 3)            ; create list
(vec 1 2 3)             ; create vector
(map func collection)   ; functional operations
```

### Sweet-Exp (Indentation-based)
```turmeric
defn fibonacci [n]
  if {n < 2}
    n
    {fibonacci{n - 1} + fibonacci{n - 2}}

let
  x: 10
  y: 20
  z: {x + y}
  body
```

## Testing

The extension comes with a comprehensive test file:

```bash
code vscode-syntax-ext/test/test-syntax.tur
```

This file covers:
- All control flow constructs
- Function definitions
- Type and typeclass definitions
- Effect and exception handling
- All number formats
- String operations
- Collections
- Functional operations
- Quote and meta-programming
- Threading/piping operators
- Sweet-exp syntax

## Configuration

### In Your VSCode Settings

```json
{
  "[turmeric]": {
    "editor.tabSize": 2,
    "editor.insertSpaces": true,
    "editor.formatOnSave": false
  }
}
```

### Language Configuration

The extension automatically configures:
- **Comment toggle**: `;` for single lines, `#|...|#` for blocks
- **Bracket pairs**: Automatic matching and colorization
- **Auto-indentation**: Increases after opening brackets/control forms
- **Auto-closing pairs**: Brackets and quotes

## Changelog

### 0.2.0
- Added syntax highlighting for inline-C blocks (` ```c ... ``` `) using the
  embedded `source.c` grammar. C keywords, types, preprocessor directives, and
  comments are now highlighted inside inline-C blocks in both `.tur` files and
  Turmeric fenced code blocks in Markdown.
- Added `punctuation.section.c-inline.begin.turmeric` and
  `punctuation.section.c-inline.end.turmeric` scope names for fence delimiters
  so themes can style them independently from C content.

### 0.1.0
- Initial release with keyword, string, number, boolean, comment, operator,
  and s-expression highlighting.
- Markdown injection grammar for fenced `turmeric` / `tur` code blocks.

## Known Limitations

- Sweet-exp syntax detection is limited to indentation patterns; perfect parsing would require a full parser
- Macro expansions are not highlighted differently than regular expressions
- Type inference visualization not yet supported

## Future Enhancements

### Phase 2: Language Server Protocol (LSP)
- IntelliSense / code completion
- Go to definition
- Find references
- Hover documentation
- Real-time type checking

### Phase 3: Additional Features
- Code snippets for common patterns
- Bracket pair colorizer integration
- Code formatter integration
- Linter integration

## Contributing

Contributions welcome! Please:
1. Test changes against `test/test-syntax.tur`
2. Update scope names in `syntaxes/turmeric.tmLanguage.json`
3. Document new features in this README

## License

MIT - See LICENSE file

## Links

- [Turmeric Language Repository](https://github.com/turmeric-lang/turmeric)
- [Turmeric Language Plan](../docs/turmeric-plan.md)
- [VSCode Language Extension Docs](https://code.visualstudio.com/api/language-extensions/overview)
- [TextMate Grammar Guide](https://macromates.com/manual/en/language_grammars)
