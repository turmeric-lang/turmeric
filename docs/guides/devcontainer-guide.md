---
title: Dev Container
category: Editor and IDE
description: Running Turmeric development in a devcontainer from VS Code or the CLI
tags:
  - devcontainer
  - vscode
  - containers
  - development-environment
---

# Dev Container

The `.devcontainer/` directory at the repo root configures a fully-featured
development environment based on Ubuntu 22.04. It includes everything needed
to build Turmeric, run tests, and develop the language itself:

- GCC / Clang, CMake 3.22+, and `gdb`
- `libedit` (REPL line editing)
- Python 3 (doc generation and doctests)
- Node.js 20 LTS (web REPL / Vite dev server)
- clangd with `compile_commands.json` wired up automatically

On first start the container runs `cmake` configure + `cmake --build` so
the `build/` directory is ready immediately. The directory is also added to
`PATH`, so `tur`, `tur_eval_basic`, and `tur_eval_sandbox` are runnable
directly without a `./build/` prefix.

---

## VS Code

### Prerequisites

Install the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)
extension (`ms-vscode-remote.remote-containers`).

Docker must be running.

### Open the project in the container

1. Open the repo folder in VS Code.
2. A notification appears: **"Reopen in Container"** -- click it.

   Or open the Command Palette (`Cmd+Shift+P` / `Ctrl+Shift+P`) and run:

   ```
   Dev Containers: Reopen in Container
   ```

3. VS Code builds the Docker image on the first run (a few minutes). Subsequent
   opens reuse the cached image.

4. Once attached, open a terminal (`` Ctrl+` ``) -- the project is at
   `/workspaces/turmeric` and `tur` is already on `PATH`.

### Run project tasks

```sh
tur run build    # debug build
tur run test     # build + test suite
tur run repl     # interactive REPL
tur run docs     # regenerate HTML docs from docstrings
tur run web-dev  # Vite dev server for the web REPL
```

### Rebuild the container

If you change `.devcontainer/Dockerfile` or `devcontainer.json`, rebuild from
the Command Palette:

```
Dev Containers: Rebuild Container
```

---

## CLI

The `@devcontainers/cli` package provides a `devcontainer` command that drives
the same spec VS Code uses.

### Prerequisites

Docker must be running.

Install the CLI once:

```sh
npm install -g @devcontainers/cli
```

### Start the container

```sh
devcontainer up --workspace-folder .
```

This builds the image (first run only), starts the container, mounts the
workspace, and runs the `postCreateCommand` (cmake configure + build).

### Open an interactive shell

```sh
devcontainer exec --workspace-folder . bash
```

Inside the shell, the workspace is at `/workspaces/turmeric` and `build/` is
on `PATH`:

```sh
tur run test   # run the test suite
tur repl       # start the Turmeric REPL
tur run examples/cellular-automata.tur
```

### Run a single command non-interactively

```sh
devcontainer exec --workspace-folder . tur run test
devcontainer exec --workspace-folder . tur run examples/cellular-automata.tur
```

### Stop the container

```sh
docker ps                        # find the container name
docker stop <container-name>
```

---

## What is pre-installed

| Tool | Version | Purpose |
|---|---|---|
| GCC / Clang | system (Ubuntu 22.04) | C11 compiler |
| CMake | 3.22+ | Build system |
| `libedit` | system | REPL line editing |
| Python 3 | system | `gendocs.py`, `doctest.py`, guide tools |
| Node.js | 20 LTS | Web REPL (Vite), web build |
| clangd | bundled with C++ devcontainer | Code intelligence |

clangd is configured via `.clangd` to use `build/compile_commands.json`, which
is generated automatically during cmake configure. No manual setup is needed.

---

## See also

- `.devcontainer/Dockerfile` -- image definition
- `.devcontainer/devcontainer.json` -- container configuration
- [vscode-guide.md](vscode-guide.md) -- VS Code syntax extension
- [lsp-guide.md](lsp-guide.md) -- Language server setup
- [compiler-flags-guide.md](compiler-flags-guide.md) -- `-X` feature flags and build options
