#!/usr/bin/env bash
# run-on-sweet-file-switches-the-session-reader: `:run` / `:reload` on a file
# whose reader differs from the session's evaluated it with turi_eval_file,
# which applies the FILE's reader to the SESSION.  So after
# `:run both.tur.sweet` a plain Turmeric prompt read every later line as
# sweet-exp -- a bare `both-x` sat waiting for the blank line that ends a
# sweet expression and the session printed `(cancelled)` at EOF, which read
# as "the definitions were dropped".  `:reload` was worse: it reset the
# session to its prelude first, so `keep`, typed before it, was gone.
# A Scheme file in a Turmeric session is declined by name, not half-run.
set -u
TUR="${TUR:-./build/tur}"
case "$TUR" in /*) ;; *) TUR="$PWD/$TUR" ;; esac
work="${1:-$(mktemp -d)}"
mkdir -p "$work"
cd "$work" || exit 1
printf 'def both-x 7\n\ndefn main []\n  println("main ran")\n  0\n' > both.tur.sweet
printf '(define sx 5)\n' > s.scm
export TUR_NO_AUTO_SPICE=1 ASAN_OPTIONS=detect_leaks=0
echo "-- :run"
printf ':run both.tur.sweet\nboth-x\n(+ both-x 1)\n' \
  | "$TUR" repl 2>&1 | grep -E '^=>|main ran|cancelled|error'
echo "-- :reload"
printf '(def keep 9)\n:reload both.tur.sweet\nkeep\nboth-x\n' \
  | "$TUR" repl 2>&1 | grep -E '^=>|cancelled|error'
echo "-- :run scheme"
printf ':run s.scm\n(+ 1 2)\n' \
  | "$TUR" repl 2>&1 | grep -E '^=>|is r7rs|error' | sed 's/^:run: .*s\.scm/:run: s.scm/'
exit 0
