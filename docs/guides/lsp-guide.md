---
title: Language Server (LSP) Guide
category: Editor and IDE
description: Configuring editors to use the Turmeric language server for diagnostics
---

# Language Server Protocol (LSP) Guide

Turmeric ships a built-in language server that speaks
[Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
over stdio (JSON-RPC 2.0). The server is invoked as:

```sh
tur lsp
```

Editors launch it as a subprocess and communicate via stdin/stdout.

## Current capabilities

| Capability | Status |
|---|---|
| Diagnostics (`textDocument/publishDiagnostics`) | Supported |
| Document symbols (`textDocument/documentSymbol`) | Supported |
| Workspace symbols (`workspace/symbol`) | Supported (open documents only) |
| Hover documentation | Supported |
| Go-to-definition | Supported |
| Completion (`textDocument/completion`) | Supported |
| Signature help (`textDocument/signatureHelp`) | Supported |
| Formatting (`textDocument/formatting`) | Supported |
| Document highlight (`textDocument/documentHighlight`) | Supported, scope-aware |
| Rename (`textDocument/prepareRename`, `textDocument/rename`) | Supported |
| References (`textDocument/references`) | Supported |
| Cancellation (`$/cancelRequest`) | Supported |
| Semantic tokens | Not supported |

When you open or edit a `.tur` file, the server compiles it in check-only mode
and publishes any parse or type errors back to the editor as diagnostics
(red underlines, error panel entries, etc.). The document's extension selects
its reader the way it does for the compiler -- a `.tur.sweet` file is
sweet-exp and a `.scm` file is Scheme with no `#lang` line needed -- and a
`#lang` line takes over when the extension says plain Turmeric.

Behaviours worth knowing about:

- **Analysis is debounced.** Compiling is not free and runs on the same thread
  that serves requests, so a changed file is not analyzed until the editor has
  been quiet for ~200ms. Requests that need symbols (hover, completion,
  definition, document symbols) force any pending analysis to run first, so
  this delays diagnostics slightly but never returns a stale answer.
- **Signature help triggers on space, not `(`.** In a lisp the callee is typed
  after the paren, so nothing names it yet at the moment `(` is typed; the
  space after the head is the first position with an answer. A signature
  request reads the current text and the callee's type from the existing
  index, and only analyzes the pending edit when that index does not know the
  callee -- so firing on every space does not cost a compile per keystroke.
- **Positions are byte offsets.** The server advertises
  `"positionEncoding": "utf-8"` (LSP 3.17), so `character` in a `Position`
  counts UTF-8 bytes rather than the UTF-16 code units the specification
  defaults to. Clients that honour the negotiated encoding need no special
  handling; a client that assumes UTF-16 regardless will be off on lines
  containing non-ASCII.
- **Completion degrades in two tiers, never to nothing.** Completion, hover,
  and go-to-definition are driven by the symbols a compile produced. A buffer
  that does not parse -- one with an unclosed paren, which is the normal state
  mid-keystroke -- produces none, so the server falls back rather than
  answering with an empty list. If the document has *ever* parsed
  successfully, the last good index is retained and served until a compile
  yields a new one; a few stale entries are far more useful than nothing, and
  the index changes much more slowly than the text does. If it has never
  parsed -- opening a file that already contains a syntax error -- there is no
  last-good index, so the server serves a process-wide stdlib-only fallback
  instead. That cache is primed lazily from an empty buffer (the stdlib is
  auto-loaded, so an empty file alone yields the whole surface), costs roughly
  8ms once per process, and its membership is a path-prefix test against
  `TUR_STDLIB_DIR`. It deliberately carries no document-local symbols: while
  the file does not parse, what the document itself declares cannot be known.
- **Completion returns a `CompletionList`**, with `isIncomplete` set when the
  200-item cap truncated the result. Items are prefix-filtered against what has
  been typed before the cursor, and the document's own definitions are emitted
  ahead of stdlib symbols so they are never the ones the cap drops.
- **Cancellation has a real window.** The server is single-threaded, so a
  `$/cancelRequest` is honoured if it arrives while the request it names is
  waiting on analysis — the expensive step. A cancel for a request already
  answered is ignored, as the specification requires. Cancelled requests are
  answered with error code `-32800`.
- **Hover honours `contentFormat`.** A client that advertises only
  `plaintext` in `capabilities.textDocument.hover.contentFormat` gets
  unfenced text instead of markdown it would have to strip itself.
- **Formatting runs in-process.** `textDocument/formatting` returns a single
  full-document `TextEdit` produced by the same code as `tur fmt`, so there is
  no need to shell out to the binary. `FormattingOptions.tabSize` and
  `insertSpaces` are ignored: the formatter is not configurable (two-space
  indent, 80 columns), and honouring the request halfway would produce output
  that `tur fmt --check` then rejects. A buffer that does not parse yields
  `null` — "no edits" — rather than an error.
- **Unsaved buffers work.** Analysis routes the buffer through a temp file, so
  a document with an `untitled:` URI and no filesystem path still gets
  diagnostics, symbols, completion, and formatting. A buffer that *does* have a
  path also gets its spice resolved from that path rather than from the temp
  copy, so intra-spice imports resolve in the editor exactly as they do for
  `tur check`.

## Scope, highlight, rename and references

The symbol index records global bindings. On its own that is enough for hover
and an outline and not enough for any question about a name the cursor is
standing *inside*: a parameter named `x` has no entry at all, so highlighting
it matched every `x` in the file.

A second pass (`src/lsp/lsp_scope.c`) records the locals -- `defn` and `fn`
parameters, `let` and `letrec` bindings, `match` binders -- each with the
source region it is visible in. It rides the same elaboration hook the symbol
harvest does, so it is one walk over the tree the compiler already built, not
a second front end.

What that buys:

- **`documentHighlight` is lexical.** A local's marks stop at its own scope,
  and a global's marks skip every region where a local of the same name
  shadows it. Both halves matter and they fail in opposite directions: without
  the first, highlighting the inner `total` of `(let [total ...] ...)` paints
  the outer one; without the second, highlighting the outer one paints the
  inner. With the minimap on, the answer is painted down the whole file, so a
  wrong one is visible at a glance.
- **`rename` is safe or it refuses.** Renaming a local edits exactly the
  occurrences in its scope. Renaming a top-level name defined in this file
  edits this file and every workspace file that imports its module -- the file
  set comes from the manifest (the project's own `src/`, plus each `:path`
  `:spices` dep's `src/`), resolved in the compiler rather than in the client,
  because a second implementation of the module search path would eventually
  disagree with the first and the disagreement would be an edit applied to the
  wrong file.

  Every other case refuses, with a reason, through `prepareRename` -- which is
  why the server advertises `"renameProvider": {"prepareProvider": true}`
  rather than the bare boolean: the message reaches the user *before* they
  type a new name.

  | Situation | Message |
  |---|---|
  | The cursor is not on an identifier | (null -- no rename here) |
  | The binder came from a macro expansion | `cannot rename a macro-introduced binding` |
  | The binding table for the document was truncated | `file too large to rename safely` |
  | The name is a stdlib symbol | `cannot rename stdlib symbol` |
  | The name is defined in another file | `cannot rename a symbol defined in another file -- rename it at its definition` |
  | The module is listed in the manifest's `:exports` | `renaming an exported symbol needs --rename-exports` |
  | Too many files import the module to verify | `too many files import this module to rename safely` |
  | A file that uses the name does not compile | `cannot rename: <file> uses this name but does not compile, so its own bindings are unknown` |

  The last two are about the workspace half. Each importing file is compiled
  for its *own* binding table before it is edited, because rewriting every
  textual occurrence in a file that imports the module is the cross-file
  version of the shadowing bug -- and the destructive one: a sibling module
  that binds a local `total` and separately imports the global `total` would
  have its local silently renamed too.

  `tur lsp --rename-exports` lifts the `:exports` refusal. It is off by
  default because such a name is published surface: a spice that fetched this
  one by `:url` may import it, and those files are outside the workspace.

- **`references` is the workspace walk without the edit.** `includeDeclaration`
  is honoured. A local's references are its scope; a global's are every
  importing file's real uses. An oversized workspace returns a shorter list
  rather than an error -- an incomplete list of references is still true about
  every entry in it, where an incomplete *edit* is not.

## Editor configuration

### Neovim (nvim-lspconfig)

Add a custom server entry -- `nvim-lspconfig` does not bundle Turmeric by
default, so you register it manually:

```lua
local lspconfig = require("lspconfig")
local configs   = require("lspconfig.configs")

if not configs.turmeric then
  configs.turmeric = {
    default_config = {
      cmd        = { "tur", "lsp" },
      filetypes  = { "tur" },
      root_dir   = lspconfig.util.root_pattern("build.tur", ".git"),
      single_file_support = true,
    },
  }
end

lspconfig.turmeric.setup({})
```

If you use `lazy.nvim` and want this to load alongside `nvim-lspconfig`:

```lua
{
  "neovim/nvim-lspconfig",
  config = function()
    -- paste the block above here
  end,
}
```

Neovim does not auto-detect `.tur` files as `tur` filetype. Add this to your
config or to `~/.config/nvim/ftdetect/tur.lua`:

```lua
vim.filetype.add({ extension = { tur = "tur" } })
```

### Neovim (built-in LSP, no plugin)

```lua
vim.api.nvim_create_autocmd("FileType", {
  pattern = "tur",
  callback = function(ev)
    vim.lsp.start({
      name    = "turmeric",
      cmd     = { "tur", "lsp" },
      root_dir = vim.fs.dirname(
        vim.fs.find({ "build.tur", ".git" }, { upward = true })[1]
      ),
    })
  end,
})
```

### Vim (vim-lsp)

Install [vim-lsp](https://github.com/prabirshrestha/vim-lsp), then add:

```vim
if executable('tur')
  au User lsp_setup call lsp#register_server({
    \ 'name': 'turmeric',
    \ 'cmd': {server_info -> ['tur', 'lsp']},
    \ 'allowlist': ['tur'],
    \ })
endif
```

Add filetype detection if not already present:

```vim
augroup TurmericFt
  autocmd!
  autocmd BufRead,BufNewFile *.tur setfiletype tur
augroup END
```

### Vim (ALE)

[ALE](https://github.com/dense-analysis/ale) can drive an LSP binary directly.
Add to your `~/.vim/after/ftplugin/tur.vim` (or equivalent):

```vim
let g:ale_linters = { 'tur': ['turmeric_lsp'] }
```

Then register the linter in your vimrc or in
`~/.vim/ale_linters/tur/turmeric_lsp.vim`:

```vim
call ale#linter#Define('tur', {
\   'name':            'turmeric_lsp',
\   'lsp':             'stdio',
\   'executable':      'tur',
\   'command':         '%e lsp',
\   'project_root':    function('ale#util#FindProjectRoot'),
\   'language':        'tur',
\ })
```

### VS Code

The bundled VS Code extension (`vscode-syntax-ext/`) provides syntax
highlighting only. To add LSP diagnostics, install a generic LSP client such as
[vscode-glspc](https://marketplace.visualstudio.com/items?itemName=coc-extensions.coc-glspc)
or configure `vscode-languageclient` in your own extension wrapper.

A minimal `settings.json` entry using the
[multi-lsp](https://marketplace.visualstudio.com/items?itemName=iehiehieh.multi-lsp)
extension:

```json
{
  "multi-lsp.servers": [
    {
      "language": "tur",
      "command": "tur",
      "args": ["lsp"]
    }
  ]
}
```

The bundled extension in `vscode-syntax-ext/` already speaks LSP natively via
`vscode-languageclient`, spawning `tur lsp` over stdio; the `turmeric.serverPath`
setting overrides which `tur` it uses. The multi-lsp recipe above is only needed
if you would rather drive the server yourself.

### Emacs (eglot)

[Eglot](https://github.com/joaotavora/eglot) is built into Emacs 29+. Add a
major mode for Turmeric first (or use `lisp-mode` as a fallback), then register
the server:

```emacs-lisp
;; Simple major mode derived from lisp-mode
(define-derived-mode turmeric-mode lisp-mode "Turmeric"
  "Major mode for Turmeric source files.")
(add-to-list 'auto-mode-alist '("\\.tur\\'" . turmeric-mode))

;; Register the LSP server with eglot
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(turmeric-mode . ("tur" "lsp"))))

;; Auto-start eglot when opening .tur files
(add-hook 'turmeric-mode-hook #'eglot-ensure)
```

### Emacs (lsp-mode)

```emacs-lisp
(with-eval-after-load 'lsp-mode
  (lsp-register-client
   (make-lsp-client
    :new-connection (lsp-stdio-connection '("tur" "lsp"))
    :activation-fn  (lsp-activate-on "tur")
    :server-id      'turmeric)))

(add-hook 'turmeric-mode-hook #'lsp)
```

### Helix

Add to `~/.config/helix/languages.toml`:

```toml
[[language]]
name              = "turmeric"
scope             = "source.tur"
file-types        = ["tur"]
comment-token     = ";"
indent            = { tab-width = 2, unit = "  " }
language-servers  = ["turmeric-lsp"]

[language-server.turmeric-lsp]
command = "tur"
args    = ["lsp"]
```

### Zed

In `~/.config/zed/settings.json`:

```json
{
  "lsp": {
    "turmeric": {
      "binary": {
        "path": "tur",
        "arguments": ["lsp"]
      }
    }
  }
}
```

Zed also requires a language extension for filetype detection. Until an
official extension is published, use the Zed extension API to register `.tur`
manually or rely on generic highlighting.

## How it works

When a `.tur` file is opened or modified, the server:

1. Writes the current buffer text to a temporary file in the platform temp
   directory (`$TMPDIR` on Unix, `%TMP%`/`%TEMP%` on Windows).
2. Runs the Turmeric compiler in type-check-only mode (`tur_check_only`).
3. Collects all diagnostics via the internal `diag_lsp_*` API.
4. Remaps temp-file paths back to the real document URI.
5. Adopts the collected symbols as the document's index — unless the compile
   produced none *and* failed, in which case the previous index is kept (see
   "the last good symbol index is retained" above).
6. Sends a `textDocument/publishDiagnostics` notification to the editor.
7. Deletes the temporary file.

Diagnostics whose span is zero-width are widened by one character before they
go out. A zero-width range paints nothing, so the diagnostic would be present
in the response but invisible in the editor.

The server uses `TextDocumentSyncKind.Full` (sync kind 1): the entire file
content is sent on every change, not just diffs. This keeps the implementation
simple at the cost of slightly more data per keystroke for large files.

## Troubleshooting

**No diagnostics appear.**
Verify `tur` is on your `$PATH`:

```sh
which tur
tur --version
```

**Server exits immediately.**
Run `tur lsp` in a terminal and type a minimal JSON-RPC initialize request.
Any startup error (missing stdlib, bad install) will appear on stderr.

**Diagnostics are stale or missing after save.**
Some editors only sync on save (not on every keystroke). Check your editor's
LSP sync setting; the server responds to both `textDocument/didOpen` and
`textDocument/didChange`.

## See also

- `src/lsp/lsp.c` -- JSON-RPC dispatcher and diagnostic publisher
- `src/lsp/lsp_docs.c` -- In-memory document store
- `src/lsp/lsp_io.c` -- Framed stdio read/write
- [vscode-guide.md](vscode-guide.md) -- VS Code syntax extension
- [vim-guide.md](vim-guide.md) -- Vim / Neovim syntax highlighting
- [formatter-guide.md](formatter-guide.md) -- `tur format` CLI
