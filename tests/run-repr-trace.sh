#!/usr/bin/env bash
# tests/run-repr-trace.sh -- smoke for the representation trace
# (representation-consolidation-meta-plan, increment 0).
#
# `--emit-abi-trace` now also prints `repr-trace` lines: one per fn-typed
# parameter naming the representation the elaborator routed it onto
# (carrier / fat / cfnptr / thin-fn + reason), and one per emit-side
# representation bridge (bare-to-fat, poly-to-fat).  Consolidation
# increments diff these traces to prove "only the intended boundaries
# moved"; this smoke pins that each classification actually prints.
#
# Env:
#   TUR_BIN   compiler to test (default ./build/tur)

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
TUR="${TUR_BIN:-./build/tur}"

if [ ! -x "$TUR" ]; then
  echo "SKIP repr-trace: no compiler at $TUR"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/probe.tur" <<'EOF'
(defn use-carrier [f : (fn [int] int)] : int (f 1))
(defn use-fat [^fat f : (fn [] int)] : int (f))
(defn use-thin-fx [f : (fn [] #fx{Write} int)] : int (f))
(defn top2 [x : int] : int (+ x 2))
(defn main [] : int
  (let [k 5]
    (println (use-carrier top2))
    (println (use-fat (fn [] k))))
  0)
(defeffect Write [s :cstr] :nil)
EOF

# Value-position bridges: a Vec of by-value parametric-monomorph elements
# crosses concrete->carrier on push (heap reinterpret of the Vec handle) and
# carrier->concrete on the ascribed vec-get read (aggregate).  A by-value
# aggregate through a bare `:fn` poly-carrier param heap-boxes on the way
# in (agg-box) and derefs back on the way out (agg-unbox).
#
# SR2b: this probe used `(Option int)` while Option was a by-value record
# product.  Option is a real sum now and rides the int64 carrier on the
# default path, so it no longer exercises ANY of these bridges; a local
# parametric `:copy` product (`(Q2 int)`, the same 16-byte two-field shape)
# is the by-value monomorph that still does.
cat > "$tmp/vprobe.tur" <<'EOF'
(defstruct Q2 :copy [A] [a : A b : A])
(defn q-get [o : (Q2 int)] : int (.a o))
(defn call-it [f : fn x : (Q2 int)] : int (f x))
(defn mk [n : int] : (Q2 int) (make-struct Q2 n (+ n 1)))
(defn main [] : int
  (let [vo (:: (vec-new) (Vec (Q2 int)))]
    (vec-push! vo (mk 5))
    (let [a (:: (vec-get vo 0) (Q2 int))]
      (println (.a a))))
  (println (call-it q-get (mk 7)))
  0)
EOF

# `Write` is declared LAST so the checks' line anchors (1:/2:/3:) stay put: an
# undeclared name in #fx{...} is TUR-E0026, which stops emit-c before any
# emit-side bridge is traced.
trace="$("$TUR" emit-c --emit-abi-trace "$tmp/probe.tur" 2>&1 >/dev/null | grep '^repr-trace')"
vtrace="$("$TUR" emit-c --emit-abi-trace "$tmp/vprobe.tur" 2>&1 >/dev/null | grep '^repr-trace')"

rc=0
check() {
  local label="$1" pattern="$2"
  if grep -qE "$pattern" <<< "$trace"; then
    echo "  ok  $label"
  else
    echo "  FAIL $label -- no line matching: $pattern"
    rc=1
  fi
}

# The probe file's own params (lines 1-3).  Stdlib params also appear in the
# trace; the leading `^repr-trace 1:`/`2:`/`3:` anchors pin OUR lines.
check "carrier param traced"        "^repr-trace 1:[0-9]+ fn-param f carrier$"
check "fat param traced"            "^repr-trace 2:[0-9]+ fn-param f fat$"
check "thin-fn + reason traced"     "^repr-trace 3:[0-9]+ fn-param f thin-fn effect-row$"
# Emit-side bridges (any location -- stdlib exercises them too).
check "bare-to-fat bridge traced"   "^repr-trace [0-9]+:[0-9]+ bridge bare-to-fat arity=[0-9]+ (typed|int64)-shim$"

# Value-position bridge classifications, from the second probe.
vcheck() {
  local label="$1" pattern="$2"
  if grep -qE "$pattern" <<< "$vtrace"; then
    echo "  ok  $label"
  else
    echo "  FAIL $label -- no line matching: $pattern"
    rc=1
  fi
}
vcheck "heap reinterpret crossing traced" "^repr-trace bridge concrete->carrier heap-reinterpret "
vcheck "aggregate crossing traced"        "^repr-trace bridge carrier->concrete aggregate \(Q2 int\)$"
vcheck "agg-box traced"                   "^repr-trace bridge agg-box tur_adt_Q2__int$"
vcheck "agg-unbox traced"                 "^repr-trace bridge agg-unbox tur_adt_Q2__int$"

# Increment 4 stage 2/3: the repr_of shadow log.  Two smokes: (a) a known
# UNMIGRATED shape fires a `repr-shadow` disagreement line -- proving the
# shadow instrument is alive; (b) a fully-consolidated shape fires none --
# proving the spec does not false-positive on clean code.
#
# (a) GRADUATED to the silence criterion (2026-07-31): the lens family --
# the last shadowed-site disagreement in the corpus -- was migrated (a
# :heap record with heap-struct fields now gets its typed-pointer binding
# via adt_heap_ptr_c_name), emptying the let-bind/merge-temp shadow log
# corpus-wide.  The check now asserts SILENCE on the former anchor: any
# repr-shadow line reappearing here means a site regressed away from the
# protocol (or the spec changed) -- triage it against
# docs/archive/repr-decision-function-plan.md.  History of the anchor:
# phantom int-newtype app (silenced by the SC7 spec fix) -> lens Line
# binding (silenced by migration) -> silence.
strace="$("$TUR" emit-c --emit-abi-trace tests/fixtures/van-laarhoven-lens-compose/input.tur 2>&1 >/dev/null | grep '^repr-shadow' || true)"
if [ -z "$strace" ]; then
  echo "  ok  shadow log silent on the former lens anchor (stage-3 criterion)"
else
  echo "  FAIL shadow silence -- the migrated lens fixture fires again:"
  echo "$strace"
  rc=1
fi

cat > "$tmp/shadow-clean.tur" <<'EOF'
(defstruct FzShadow [a : int])
(defn main [] : int
  (let [v (:: (vec-new) (Vec FzShadow))]
    (vec-push! v (FzShadow 31))
    (let [b (:: (vec-get v 0) FzShadow)]
      (println (.a b))))
  0)
EOF
ctrace="$("$TUR" emit-c --emit-abi-trace "$tmp/shadow-clean.tur" 2>&1 >/dev/null | grep '^repr-shadow' || true)"
if [ -z "$ctrace" ]; then
  echo "  ok  no shadow noise on consolidated shape"
else
  echo "  FAIL shadow noise -- expected no repr-shadow lines on the clean shape, got:"
  echo "$ctrace"
  rc=1
fi

# Increment 4 stage 3: the STRUCT_FIELD position (shadowed at the
# adt_ctor_field_c_type chokepoint, which all nine field-emission sites route
# through).  The probe carries all three OWNER classes the position
# distinguishes, because the owner -- not the field type -- picks which
# protocol a slot follows:
#
#   FdInline   by-value owner, drop-glue-free aggregate field -> INLINED
#   FdBoxed    by-value owner, rc-owning aggregate field      -> BOXED (int64)
#   FdCarrier  carrier owner: scalar, :heap pointer, fn slots -> one-word sink
#
# FdBoxed is the load-bearing row: it is the shape whose `full_type` is
# deliberately NULL (reconstructed for the shadow from `drop_inner_def`), and
# the only field shape whose form disagreed before repr_of learned that an
# owning by-value product is BOXED_AGG at a field.  Sabotage-verified: with
# that arm stripped, FdBoxed fires two lines and this check fails.
#
# The position was measured silent corpus-wide (84 -> 0), so any line here
# means a field decision drifted from the protocol -- triage against
# docs/archive/repr-decision-function-plan.md.
cat > "$tmp/shadow-fields.tur" <<'EOF'
(defstruct FdPoint :heap [x : int y : int])
(defstruct FdFlat [a : int b : int])
(defstruct FdOwn [r : rc<int> tag : int])
(defstruct FdInline [i : FdFlat n : int])
(defstruct FdBoxed [o : FdOwn n : int])
(defstruct FdCarrier [s : int p : FdPoint f : (fn [int] int)])
(defn bump [n : int] : int (+ n 1))
(defn main [] : int
  (let [a (make-struct FdInline :i (FdFlat 4 5) :n 1)]
    (let [b (make-struct FdBoxed :o (make-struct FdOwn :r (rc/of 3) :tag 1) :n 2)]
      (let [c (make-struct FdCarrier :s 7 :p (FdPoint 1 2) :f bump)]
        (println (+ (.n a) (+ (.n b) (.s c)))))))
  0)
EOF
# Assert the probe COMPILES and produced each owner layout before reading its
# silence.  A probe that fails to compile emits no repr-shadow lines either,
# so a silence check alone would pass vacuously -- which is exactly how the
# first draft of this check passed with the guard it was meant to pin removed.
# NB: grep the emitted C as a FILE, not through `echo "$var" | grep -q`.  This
# script runs under `set -o pipefail`, and `grep -q` closes the pipe on its
# first match, so `echo` dies of SIGPIPE and the pipeline reports failure even
# though the pattern matched -- which reads exactly like a missing layout.
"$TUR" emit-c "$tmp/shadow-fields.tur" >"$tmp/fields.c" 2>"$tmp/fields.err"
if [ -s "$tmp/fields.err" ] || [ ! -s "$tmp/fields.c" ]; then
  echo "  FAIL adt-field probe did not compile:"
  head -5 "$tmp/fields.err"
  rc=1
elif ! grep -q "tur_adt_FdFlat i;" "$tmp/fields.c" \
   || ! grep -q "typedef struct tur_adt_FdBoxed {" "$tmp/fields.c" \
   || ! grep -q "typedef struct tur_adt_FdCarrier {" "$tmp/fields.c"; then
  echo "  FAIL adt-field probe did not produce the three owner layouts"
  rc=1
else
  ftrace="$("$TUR" emit-c --emit-abi-trace "$tmp/shadow-fields.tur" 2>&1 >/dev/null | grep '^repr-shadow' || true)"
  if [ -z "$ftrace" ]; then
    echo "  ok  adt-field position silent across all three owner classes"
  else
    echo "  FAIL adt-field shadow -- expected no repr-shadow lines, got:"
    echo "$ftrace"
    rc=1
  fi
fi

# Increment 4, THE COLLAPSE: `type_is_boxed_container_elem` is now defined as
# `repr_of(t, CONTAINER_ELEM) == BOXED_AGG`, so its shadow is retired by
# construction -- want and got are the same expression. What used to be the
# pinned TY_APP disagreement row must therefore be SILENT now, and the boxing
# it names must be paired with a matching ownership fold.
#
# The fold is the load-bearing half. Before the collapse the push side boxed a
# concrete by-value app element while `tur-vec-elem-wide?` folded to 0, so
# `vec-free` never released those boxes -- a leak of one box per push. Assert
# the emitted fold is 1, which is the codegen-visible face of the fix; the
# runtime face is tests/fixtures/vec-app-element-box-lifecycle.
cat > "$tmp/celem-app.tur" <<'EOF'
(defn main [] : int
  (let [vo (:: (vec-new) (Vec (Option int)))]
    (vec-push! vo (:: (some 5) (Option int)))
    (let [a (:: (vec-get vo 0) (Option int))]
      (println (unwrap-or a -1)))
    (vec-free vo))
  0)
EOF
cat > "$tmp/celem-adt.tur" <<'EOF'
(defstruct CeSm [a : int])
(defn main [] : int
  (let [v (:: (vec-new) (Vec CeSm))]
    (vec-push! v (CeSm 31))
    (let [b (:: (vec-get v 0) CeSm)] (println (.a b)))
    (vec-free v))
  0)
EOF
"$TUR" emit-c --emit-abi-trace "$tmp/celem-app.tur" >"$tmp/celem-app.c" 2>"$tmp/celem-app.err"
"$TUR" emit-c --emit-abi-trace "$tmp/celem-adt.tur" >"$tmp/celem-adt.c" 2>"$tmp/celem-adt.err"
if grep -q '^repr-shadow container-elem' "$tmp/celem-app.err" \
   || grep -q '^repr-shadow container-elem' "$tmp/celem-adt.err"; then
  echo "  FAIL container-elem shadow fired -- the position is defined by repr_of and cannot disagree:"
  grep -h '^repr-shadow container-elem' "$tmp/celem-app.err" "$tmp/celem-adt.err"
  rc=1
elif ! grep -q 'vec_hyfree_hyo' "$tmp/celem-app.c"; then
  echo "  FAIL container-elem probe never reached vec-free"
  rc=1
elif grep -qE 'vec_hyfree_hyo\(.*INT64_C\(0\)\) \+' "$tmp/celem-app.c"; then
  echo "  FAIL app-element vec freed with owned=0 -- the element boxes leak"
  rc=1
else
  echo "  ok  container-elem collapsed into repr_of; app elements own their boxes"
fi

# Increment 4 stage 3: the fn-value TAIL/JOIN classification, shadowed in
# `elab_normalize_fn_tail_leaves` against `repr_of_binding` -- the
# binding-context decision function, which consults the `is_poly_fn` /
# `is_fat` flags the Type does not carry.  Only leaves whose BINDING is
# authoritative (params) are shadowed; a let-bound alias carries its
# representation in its initialiser, which no binding-only signature can see.
#
# Silence alone would prove nothing here (the walker runs on few shapes: 8
# evaluations corpus-wide), so the check first proves the probe REACHES the
# classification by requiring a to-fat conversion in its emitted C.  Both
# arms of the join are fn params returned through a fn-typed result, which is
# exactly what the walker normalizes.  Grep files, never `... | grep -q`:
# under `set -o pipefail` an early-exiting grep SIGPIPEs its producer and a
# match reports failure.
cat > "$tmp/fn-tail.tur" <<'EOF'
(defn pick [c : int f : (fn [int] int) g : (fn [int] int)] : (fn [int] int)
  (if (> c 0) f g))
(defn main [] : int
  (let [h (pick 1 (fn [x] (+ x 1)) (fn [x] (* x 2)))]
    (println (h 5)))
  0)
EOF
"$TUR" emit-c --emit-abi-trace "$tmp/fn-tail.tur" >"$tmp/fn-tail.c" 2>"$tmp/fn-tail.err"
if [ ! -s "$tmp/fn-tail.c" ]; then
  echo "  FAIL fn-tail probe did not compile:"
  head -5 "$tmp/fn-tail.err"
  rc=1
elif ! grep -q "to_fat" "$tmp/fn-tail.c"; then
  echo "  FAIL fn-tail probe never reached the fn-value classification"
  rc=1
elif grep -q '^repr-shadow fn-tail-leaf' "$tmp/fn-tail.err"; then
  echo "  FAIL fn-tail-leaf shadow -- classification disagrees with repr_of_binding:"
  grep '^repr-shadow fn-tail-leaf' "$tmp/fn-tail.err"
  rc=1
else
  echo "  ok  fn-tail-leaf classification agrees with repr_of_binding"
fi

# Increment 4 stage 3: METHOD-RESULT carrier production, shadowed at the
# `fn_body_tail_byvalue_carrier_type` wrapper -- the walker that tells a
# carrier-return slot which concrete type sits on the far side of the bridge.
#
# The watched invariant is what that walker PROMISES its callers: a type they
# can spell concretely.  Naming a type the protocol calls the erased carrier
# would hand a caller an int64 dressed as a concrete spelling, which is the
# shape increment 2 chased through the `bind` cell.
#
# Liveness comes free on the value probe: `repr-trace bridge carrier->concrete
# aggregate (Option int)` (asserted above) is emitted by the bridge
# that CONSUMES this walker's answer, so the same probe proves the walker ran
# and that it stayed silent.  Corpus-wide the population is 7211 concrete
# answers with 0 disagreements -- see the plan.
vshadow="$("$TUR" emit-c --emit-abi-trace "$tmp/vprobe.tur" 2>&1 >/dev/null | grep '^repr-shadow method-result' || true)"
if [ -z "$vshadow" ]; then
  echo "  ok  method-result walker names only concrete types"
else
  echo "  FAIL method-result shadow -- walker named an erased-carrier type:"
  echo "$vshadow"
  rc=1
fi

# Increment 4 stage 3: the PER-ARG BRIDGE position -- the "long tail" the
# plan expected to be the big one.  It turned out to be one chokepoint after
# all: every per-argument crossing in emit_expr.c routes through
# `emit_carrier_bridge` (the escaping sibling delegates), so the shadow sits
# with the existing repr-trace there and covers all ~24 call sites at once.
#
# Invariant: a crossing is a contract that `concrete_ty` really is the
# CONCRETE side.  A type the protocol calls the erased carrier means the
# bridge is about to spill/address/reinterpret an int64 across a boundary
# that is not there.  Corpus population 13787 crossings, 0 disagreements.
#
# The value probe crosses on both directions (heap-reinterpret for the Vec
# handle, aggregate for the element read), so requiring its trace lines --
# already asserted above -- doubles as this check's liveness proof.
abshadow="$("$TUR" emit-c --emit-abi-trace "$tmp/vprobe.tur" 2>&1 >/dev/null | grep '^repr-shadow arg-bridge' || true)"
if [ -z "$abshadow" ]; then
  echo "  ok  arg-bridge crossings all name concrete types"
else
  echo "  FAIL arg-bridge shadow -- a crossing named an erased-carrier type:"
  echo "$abshadow"
  rc=1
fi

# Increment 4 stage 3 -> R3: ENFORCEMENT mode.  A Debug build now treats a
# representation-shadow disagreement as an ICE with no flag required, which is
# licensed only because stage 3's whole position list runs silent.  The two
# modes must stay distinguishable, so pin both:
#
#   measurement  --emit-abi-trace collects disagreements as lines and never
#                aborts -- a sweep that died on its first finding could not
#                calibrate a spec, which is how every position here was
#                calibrated;
#   enforcement  a plain Debug run ICEs on a disagreement.
#
# There are currently NO known (exempt) rows: the container-elem row that used
# to be one was closed by the collapse. The `known` exemption stays in the
# code because the next pinned row will want it, but nothing exercises it
# today -- so this check pins what is still true, rather than pretending to
# test the exemption. If a future increment pins a new known row, assert here
# that it appears under trace AND does not abort a plain run.
"$TUR" emit-c --emit-abi-trace "$tmp/celem-app.tur" >/dev/null 2>"$tmp/mode-trace.err"
"$TUR" emit-c "$tmp/celem-app.tur" >/dev/null 2>"$tmp/mode-plain.err"
plain_rc=$?
if [ $plain_rc -ne 0 ]; then
  echo "  FAIL enforcement mode aborted on a consolidated program (exit $plain_rc):"
  head -4 "$tmp/mode-plain.err"
  rc=1
elif grep -q 'internal error (ICE)' "$tmp/mode-plain.err"; then
  echo "  FAIL enforcement mode ICE'd on a consolidated program"
  rc=1
elif ! grep -q '^repr-trace ' "$tmp/mode-trace.err"; then
  echo "  FAIL measurement mode emitted no trace at all -- the instrument is dead"
  rc=1
else
  echo "  ok  measurement traces, enforcement stays quiet on consolidated code"
fi

if [ $rc -ne 0 ]; then
  echo "FAIL repr-trace"
  echo "--- fn-param trace ---"
  echo "$trace"
  echo "--- value-position trace ---"
  echo "$vtrace"
else
  echo "PASS repr-trace"
fi
exit $rc
