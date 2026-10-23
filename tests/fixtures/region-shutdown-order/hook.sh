#!/usr/bin/env bash
# RM3 R5 graduation item 3, the pooled slab: a reclaimed generation is REWOUND,
# not released -- region.c pools the arena for the next push -- so at exit one
# 64 KiB slab per pooled arena was reachable but never freed.  tur_region_shutdown
# frees live, retired and pooled arenas; this pins WHERE it is registered,
# because the first attempt put it in the wrong place and a probe caught it.
#
# atexit runs LIFO, so shutdown must be REGISTERED BEFORE every module `defer`
# -- a defer may read a value that ESCAPED its bracket, which lives in a RETIRED
# generation, and freeing first is a use-after-free.  Registering it in a main
# prologue is too late: a __attribute__((constructor)) runs __tur_static_init
# (and so registers the defers) before main, and that attempt read 0 where 42
# was right, with valgrind's "Invalid read".  The only always-earlier place is
# the first statement of __tur_static_init itself, behind its idempotent guard.
#
# In a unit with `main` and def initializers the runtime half of the init --
# this registration and the module defers among it -- is __tur_static_init_rt,
# which the constructor runs; the def initializers wait for main
# (r7rs-toplevel-define-sees-empty-command-line).  The awk reads whichever
# holds the registration.
#
# Three assertions, and the third is the one that matters:
#   1. PLACEMENT: in the emitted C, atexit(tur_region_shutdown) appears inside
#      __tur_static_init, AFTER `__tur_static_init_done = 1` and BEFORE the
#      call to __module_defers_init -- and NOT in main.
#   2. GATING: with the flag off, the program references no region symbol.
#   3. VALUE: at run time the module defer prints the escaped node's field,
#      42, read from a retired generation BEFORE shutdown frees it.  A wrong
#      order does not leak -- it prints 0 (or traps under the Debug poison).
#      That is the SR4 lesson again: assert the value, not the build.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/in.tur" <<'EOF'
(defdata Link :heap (Link [v : int nxt : int]))
;; The result IS a node, so the walk refuses and the bracket RETIRES: the node
;; escapes into a retired generation that only shutdown may free.
(defn mk [n : int] : Link
  (with-region (fn [] (Link n 0))))
(def ^mut keep 0)
;; A module-level defer: runs at exit, reads the retired node.
(defer (println (.v (:: keep Link))))
(defn main [] : int
  (set! keep (:: (mk 42) :int))
  (println 1)
  0)
EOF

"$TUR" emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/on.c"
TUR_REGIONS=0 "$TUR" emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/off.c"

# 1. placement: line numbers inside __tur_static_init, in order
read -r l_done l_atexit l_defers < <(awk '
  /^static void __tur_static_init(_rt)?\(void\) \{/ && !f {f=1}
  f && /__tur_static_init_done = 1/       {d=NR}
  f && /atexit\(tur_region_shutdown\)/     {a=NR}
  f && /__module_defers_init\(\)/          {m=NR}
  f && /^}/                                {exit}
  END {print d+0, a+0, m+0}' "$TMP/on.c")
if [ "$l_atexit" -gt "$l_done" ] && [ "$l_atexit" -lt "$l_defers" ]; then
    echo "placement: shutdown registered after the guard and before the module defers"
else
    echo "placement: WRONG (done=$l_done atexit=$l_atexit defers=$l_defers)"
fi
in_main=$(awk '/^int main\(/{f=1} f&&/atexit\(tur_region_shutdown\)/{c++} f&&/^}/{exit} END{print c+0}' "$TMP/on.c")
echo "in main: $in_main"
# 2. gating
echo "flag-off references: $(grep -c 'tur_region_shutdown' "$TMP/off.c")"
# 3. the value, both arms
echo "regions off: $(TUR_REGIONS=0 "$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
echo "regions on:  $("$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
exit 0
