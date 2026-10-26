---
title: AI Assistant Integration Guide
category: Editor and IDE
description: Using the Turmeric MCP server and LSP with Copilot CLI, Claude CLI, OpenCode, and VS Code Copilot
tags:
  - lsp
  - vscode
---

# AI Assistant Integration Guide

Turmeric exposes two complementary interfaces for AI-assisted development:

- **LSP** (`tur lsp`) -- a Language Server Protocol server for editors, providing
  diagnostics, symbol navigation, hover, and completion in real time.
- **MCP** (`tur mcp`) -- a [Model Context Protocol](https://modelcontextprotocol.io)
  server for AI assistants, giving them direct access to type information,
  docstrings, diagnostics, and project builds over a simple stdio transport.

Both are built into the `tur` binary -- no extra installation is required.

## Transport framing

`tur mcp` and `tur lsp` use **different framing** over stdio. Getting this
wrong is the most common cause of a client receiving no output.

| Server | Transport | Format |
|--------|-----------|--------|
| `tur mcp` | MCP 2024-11-05 stdio | One JSON object per line, terminated by `\n`. No headers. |
| `tur lsp` | Language Server Protocol | `Content-Length: N\r\n\r\n` header, then N bytes of JSON. |

MCP clients (opencode, Claude Code, VS Code Copilot agent mode, etc.) all
implement the MCP stdio spec and send/read newline-delimited JSON. If you wire
a client that expects MCP framing to `tur lsp` (or vice versa), you will get
silence.

## MCP tools reference

The MCP server exposes eight tools:

| Tool | Required inputs | What it does |
|---|---|---|
| `check_file` | `path` | Compile-checks a `.tur` file; returns a JSON array of LSP diagnostics |
| `symbols` | `path` | Lists all top-level symbols (name, type, docstring, location) |
| `hover` | `path`, `line`, `col` | Returns type and docstring for the symbol at a 0-based position |
| `definition` | `path`, `line`, `col` | Returns the file, line, and column where a symbol is defined |
| `complete` | `path`, `line`, `col` | Returns completion candidates (symbols or module names in `import` context). In an `(import` context the stdlib module list is discovered at runtime by scanning `$TUR_STDLIB_DIR`, so it tracks the installed stdlib without code changes. |
| `doc` | `path`, `name` | Returns the `;;;` docstring for a named symbol |
| `format` | `path` | Runs `tur format` on a file; returns the formatted text |
| `build` | `dir` | Runs `tur build` on a project directory; returns output and exit status |

All `path` and `dir` arguments must be absolute filesystem paths.

---

## Enabling and disabling

The MCP server starts on demand when an AI tool invokes `tur mcp`. To
suppress it without touching config files, set:

```sh
export TUR_NO_MCP=1
```

`tur mcp` will exit immediately with a message and return control to the
caller. Unset the variable to re-enable:

```sh
unset TUR_NO_MCP
```

You can also disable a client's access without touching environment variables
by renaming or removing the relevant config file (see the per-client sections
below).

---

## GitHub Copilot CLI

The GitHub Copilot CLI reads MCP server configuration from
`~/.copilot/mcp-config.json` (user-level, persistent across projects). Add a
`turmeric` entry under `mcpServers`:

```json
{
  "mcpServers": {
    "turmeric": {
      "command": "tur",
      "args": ["mcp"]
    }
  }
}
```

To enable it for a single session without touching the global file, pass
`--additional-mcp-config` pointing at a project-local JSON file with the same
shape:

```sh
copilot --additional-mcp-config ./.copilot/mcp-config.json
```

Inside Copilot CLI, run `/mcp` to list registered servers or use the
`/lsp` slash command to manage `tur lsp` alongside the MCP integration.
See the [Copilot CLI command reference](https://docs.github.com/en/copilot/reference/copilot-cli-reference/cli-command-reference)
for the full migration notes away from the older `.vscode/mcp.json` layout.

### Disabling for a session

```sh
TUR_NO_MCP=1 copilot
```

---

## Claude CLI (Claude Code)

Claude Code picks up MCP servers from `.claude/mcp.json` in the project root.
This file is already committed in the repository:

```json
{
  "mcpServers": {
    "turmeric": {
      "command": "tur",
      "args": ["mcp"],
      "env": {}
    }
  }
}
```

### Verifying the connection

Start Claude Code from the project root:

```sh
claude
```

In the Claude Code chat, ask:

```
/tools
```

You should see `turmeric_check_file`, `turmeric_symbols`, etc. listed. If they
are absent, confirm that `tur` is on your `PATH`:

```sh
which tur
tur --version
```

### Quick smoke test

Without opening a full session, you can verify the server protocol directly:

```sh
printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}\n' | tur mcp
```

Expected output is a single JSON line containing
`"serverInfo":{"name":"turmeric",...}`.

### Disabling for a session

```sh
TUR_NO_MCP=1 claude
```

Or remove `.claude/mcp.json` to disable permanently for this project.

---

## OpenCode CLI

OpenCode reads `opencode.json` from the current working directory. The file
is already committed at the repository root:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "turmeric": {
      "type": "local",
      "command": ["tur", "mcp"]
    }
  }
}
```

### Usage

```sh
# From the repository root:
opencode
```

OpenCode auto-discovers `opencode.json` on startup. Type `/tools` or ask the
assistant to list available tools -- the Turmeric tools will appear immediately.

### Example prompts

```
Check /path/to/my-file.tur for errors.

List all exported symbols in /path/to/lib.tur.

What does the `vec-push` function do in /path/to/vec.tur?

Build the project at /path/to/my-spice and show any errors.
```

### Disabling for a session

```sh
TUR_NO_MCP=1 opencode
```

Or rename `opencode.json` to `opencode.json.disabled` to turn it off
for all sessions in this project.

---

## VS Code Copilot

`.vscode/mcp.json` is already committed in the repository:

```json
{
  "servers": {
    "turmeric": {
      "type": "stdio",
      "command": "tur",
      "args": ["mcp"]
    }
  }
}
```

### Enabling MCP tools in VS Code

1. Open the repository folder in VS Code.
2. Open **Copilot Chat** (`Ctrl+Shift+I` / `Cmd+Shift+I`).
3. Switch to **Agent mode** (the drop-down next to the chat input).
4. Click the **Tools** button -- `turmeric_check_file`, `turmeric_symbols`,
   and the other tools should be listed and checked.

VS Code reads `.vscode/mcp.json` automatically when you open the folder; no
extension or marketplace install is needed beyond GitHub Copilot itself.

### LSP diagnostics in VS Code

For inline red-underlines and the Problems panel, the LSP server runs
separately from MCP. Configure it via the VS Code settings:

```json
// .vscode/settings.json
{
  "turmeric.languageServer.enable": true
}
```

If you use a generic LSP client extension (e.g. **clangd** or
**Generic LSP Client**), point it at `tur lsp`:

```json
{
  "lsp.languages": [
    {
      "languageId": "tur",
      "command": ["tur", "lsp"]
    }
  ]
}
```

### Disabling

To disable MCP without deleting the config file:

```sh
TUR_NO_MCP=1 code .
```

Or comment out the server entry in `.vscode/mcp.json`.

---

## Neovim

See [lsp-guide.md](lsp-guide.md) for the full Neovim LSP setup. Once `tur lsp`
is running, hover, go-to-definition, and completions are available natively via
`vim.lsp`.

For MCP-based AI assistance inside Neovim, use a plugin such as
[mcphub.nvim](https://github.com/ravitemer/mcphub.nvim) and point it at
`tur mcp` as a stdio server. The configuration mirrors the VS Code example
above.

---

## Raw CLI smoke tests

These tests work without any AI client and are useful for CI or troubleshooting.

`tur mcp` uses the MCP 2024-11-05 stdio transport: **one JSON object per line,
terminated by `\n`**. No `Content-Length` headers. Each `printf` below
produces exactly one `\n`-terminated JSON line.

### Test initialize

```sh
printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}\n' | tur mcp
```

Expected: a single JSON line with `"serverInfo":{"name":"turmeric",...}`.

### Test tools/list

```sh
printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  | tur mcp
```

The second response line should contain all eight tool names.

### Test check_file

```sh
TMP=$(mktemp /tmp/test_XXXX.tur)
printf '(defn add [a : int b : int] : int (+ a b))\n' > "$TMP"

printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}' \
  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"check_file\",\"arguments\":{\"path\":\"$TMP\"}}}" \
  | tur mcp
rm "$TMP"
```

A clean file produces `"content":[{"type":"text","text":"[]"}]` in the response
(empty diagnostics array).

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `tur: command not found` | `tur` not on PATH | Add the `build/` directory to `PATH` |
| `tur mcp` produces no output at all | Wrong framing: client sent `Content-Length` headers instead of newline-delimited JSON | `tur mcp` speaks MCP stdio (newline-terminated JSON), not LSP. Check the [Transport framing](#transport-framing) section. |
| Tools missing from Copilot/OpenCode | Config file not loaded | Confirm you opened the repo root, not a subdirectory |
| Empty tool responses | File path not absolute | Use full `/path/to/file.tur` paths |
| `tur mcp: disabled by TUR_NO_MCP` | Env variable set | `unset TUR_NO_MCP` |
| Diagnostics but no symbols | File has parse errors | Fix errors first; `tur_collect_symbols` requires a successful elaboration pass |

---

## Test coverage

### Automated test coverage

End-to-end JSON-RPC coverage for the MCP server and the LSP handlers
lives in `tests/lsp/mcp_lsp_test.py` (driven by `tests/lsp/run-mcp-lsp.sh`,
wired into ctest as `tur_mcp_lsp_tests`). It spawns `tur mcp` and `tur lsp`
as subprocesses and drives each with its correct framing: **newline-delimited
JSON for MCP** and **Content-Length headers for LSP**. Assertions cover all
eight MCP tools (except `build`, exercised indirectly via the suite) and the
three LSP handlers (hover, definition, documentSymbol), including the
`TUR_NO_MCP=1` kill switch. The `tests/lsp/docscanner_test.c` unit test is
wired in as `tur_docscanner_unit`.

`tur build` is not driven from `mcp_lsp_test.py` because it shells out to a
full project compile; if the rest of the test suite passes, the MCP
`build` tool's only added behavior (subprocess launch + exit-status
plumbing) is exercised by every other tool that uses the same
`run_subprocess` helper.

### Build and test status

Before relying on the MCP or LSP integration in production, run:

```sh
cmake --build build -j
bash tests/run.sh
```

and confirm zero `FAIL` lines. The [Raw CLI smoke tests](#raw-cli-smoke-tests)
above provide a quick sanity check without a full test suite run.

---

## See also

- [lsp-guide.md](lsp-guide.md) -- full editor LSP setup (Neovim, Vim, Emacs)
- [vscode-guide.md](vscode-guide.md) -- VS Code syntax highlighting extension
- [formatter-guide.md](formatter-guide.md) -- `tur format` usage
- [developing-spices-guide.md](developing-spices-guide.md) -- project layout for `tur build`
