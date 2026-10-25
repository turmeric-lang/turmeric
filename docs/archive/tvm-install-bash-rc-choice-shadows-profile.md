# tvm's `install.sh` picks the wrong bash startup file both ways: it can disable `~/.profile` on Linux, and on macOS it misses Terminal's login shells

**Status 2026-10-25: RESOLVED** in turmeric-lang/tvm#2 (`5217967`), and shipped
by bumping `TVM_REF` in `.github/workflows/release.yml` to that commit. The fix
followed the directions below. Bash on Linux writes `~/.bashrc`, creating it if
missing. Bash on macOS writes the first existing of `~/.bash_profile`,
`~/.bash_login` and `~/.profile`, or a new `~/.bash_profile`. The script still
never prompts, and it says when it created the file. tvm's `tests/run.sh` covers
each case through a `uname` stub (15 cases, green on both the ubuntu and macOS
legs), including that a bash login shell still reads `~/.profile`. Against the
old `install.sh`, 11 of those cases fail. turmeric's
`tests/tvm-install-script.sh` greps the whole of `$HOME` for the snippet, so the
new choice does not affect it.

**Severity:** medium-low. `install.sh` ships in every release, and the
`/install` bootstrap runs it (`web/worker.js:77-90`, `release.yml:483`). The
code lives in **turmeric-lang/tvm**, so the fix lands there. It is indexed here
because this repo ships the installer.

Filed 2026-10-24. An agent reported it as "with no `.bashrc`, the setup goes to
`.bash_profile`, which only login shells read; only an empty test `$HOME` hits
this". That framing is too narrow on both counts.

## What happens

`detect_rc` (tvm `install.sh`), bash branch:

```sh
if [ -f "$HOME/.bashrc" ]; then echo "$HOME/.bashrc"; else echo "$HOME/.bash_profile"; fi
```

1. **Linux, no `.bashrc`, but a `.profile` (or no file at all).** The script
   creates `~/.bash_profile` holding only the tvm snippet. A bash login shell
   reads the *first* of `.bash_profile`, `.bash_login` and `.profile`, so from
   then on it never reads `~/.profile`. Whatever the user kept there, such as
   PATH entries for `~/.local/bin` or cargo's env line, silently stops loading
   at login. Interactive non-login shells (most Linux terminal emulators) read
   neither file, so tvm is not loaded in them either. Minimal images, containers
   and users who deleted `.bashrc` hit this, not only test homes.
2. **macOS, bash, with a `.bashrc`.** The snippet goes to `.bashrc`. Terminal.app
   opens *login* shells, which do not read `.bashrc` unless `.bash_profile`
   sources it, and macOS ships no `.bash_profile` that does. So tvm is not on
   PATH in a new Terminal window.

The tvm README (line 27) lists `~/.zshrc`, `~/.bashrc` or `~/.profile` and
never mentions `.bash_profile`.

## Repro (Linux, bash 5)

```sh
H=$(mktemp -d); echo 'export FROM_PROFILE=yes' > $H/.profile
env -i HOME=$H SHELL=/bin/bash PATH=/usr/bin:/bin bash -lc 'echo $FROM_PROFILE'   # yes
env -i HOME=$H SHELL=/bin/bash PATH=/usr/bin:/bin sh tvm/install.sh               # creates .bash_profile
env -i HOME=$H SHELL=/bin/bash PATH=/usr/bin:/bin bash -lc 'echo ${FROM_PROFILE-unset}'  # unset
env -i HOME=$H SHELL=/bin/bash PATH=/usr/bin:/bin bash -ic 'echo ${TVM_DIR-unset}'      # unset

H=$(mktemp -d); touch $H/.bashrc                                                    # case 2
env -i HOME=$H SHELL=/bin/bash PATH=/usr/bin:/bin sh tvm/install.sh               # -> .bashrc
env -i HOME=$H SHELL=/bin/bash PATH=/usr/bin:/bin bash -lc 'echo ${TVM_DIR-unset}'      # unset (Terminal's case)
```

## Fix directions

Do not prompt and do not error. The script normally runs as `curl ... | sh`, so
stdin is the pipe rather than a terminal, and both tvm's CI and the bootstrap
run it unattended. Choose by platform instead, and say what was done:

```sh
bash)
  case "$(uname -s)" in
    Darwin)   # Terminal opens login shells; never shadow an existing file
      for f in .bash_profile .bash_login .profile; do
        [ -f "$HOME/$f" ] && { echo "$HOME/$f"; return; }
      done
      echo "$HOME/.bash_profile" ;;
    *)        # interactive shells read .bashrc; distro login profiles source it
      echo "$HOME/.bashrc" ;;   # created if missing -- shadows nothing
  esac ;;
```

- Print `tvm: created ~/.bashrc` (or the file name) when the file did not exist
  before.
- Fix the README's rc list.
- Add cases to tvm's `tests/run.sh`: an empty `$HOME`, a `$HOME` holding only
  `.profile` (assert that `.profile` still loads at login), and a Darwin
  `uname` stub with only `.bashrc`.
