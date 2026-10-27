# New Spices Plan: tur-scscm and tur-tidal

> **Status:** Draft Plan
> **Last Updated:** 2026-05-23
> **Type:** Spice Design

---

## Overview

Two new spices for the `turmeric-spices` monorepo:

| Spice | Tag | Tier | Purpose |
|-------|-----|------|---------|
| `tur-scscm` | `scscm-v0.1.0` | 1 -- pure Turmeric + inline-C | s-expression -> sclang compiler + scsynth/hcsynth OSC client |
| `tur-tidal` | `tidal-v0.1.0` | 1 -- pure Turmeric + inline-C | Tidal-like mini-notation -> Pbind/event text |

**Neither spice touches audio synthesis.** `tur-scscm` is a string-to-string compiler
(scscm source text in, sclang source text out) plus an OSC client for sending
note/performance control messages to a running scsynth or hcsynth server.
`tur-tidal` parses a mini-notation shorthand and emits scscm or sclang Pbind
expressions describing note/performance data. Both are purely about note and
performance data; no UGens, no audio buffers, no synthesis.

The reference implementation of scscm lives in
`hypercollider/cli/lhc_{lexer,parser,macros,codegen}.js`. These plans port that
logic to Turmeric.

---

## Conventions

Both spices are Tier 1 (no cmake-deps). Inline-C is used only for
character-level string scanning in the lexer stages; all higher-level logic is
pure Turmeric. Each spice follows the standard layout:

```
../turmeric-spices/spices/<name>/
  build.tur
  src/<name>/*.tur
  tests/<name>/*_test.tur
```

Spices live in the sibling `turmeric-spices` repo at `../turmeric-spices/`, not
inside the main `turmeric/` tree.

---

## tur-scscm

### What it does

`tur-scscm` is a compiler from scscm (a Scheme-like s-expression syntax) to
sclang (SuperCollider language) text. The pipeline is:

```
scscm text
  -> [lexer]    token list
  -> [parser]   AST (nested cons cells)
  -> [expander] expanded AST (macros resolved)
  -> [codegen]  sclang text
```

It replicates the functionality of `lhc_lexer.js`, `lhc_parser.js`,
`lhc_macros.js`, and `lhc_codegen.js` from the hypercollider project. The
compiler never executes sclang; it only produces text.

### AST node representation

AST nodes are opaque `:int` handles pointing to heap-allocated C structs.
The node tag lives in the struct; Turmeric code passes them as `:int` and
calls accessor functions to inspect them. This mirrors how tur-sqlite and
tur-http handle opaque handles.

Node kinds (mirroring hclang's AST):

| Kind | scscm source | sclang output |
|------|-------------|---------------|
| `atom` | `nil`, `true`, `false` | `nil`, `true`, `false` |
| `symbol` | `foo-bar` | `foo_bar` |
| `keyword` | `:freq` | `\freq` |
| `number` | `440`, `0.5` | `440`, `0.5` |
| `string` | `"hello"` | `"hello"` |
| `list` | `(foo a b)` | `foo(a, b)` |
| `vector` | `[a b c]` | `[a, b, c]` |
| `dot-call` | `(. obj msg)` | `obj.msg` |
| `dot-new` | `(.dot Cls new ...)` | `Cls.new(...)` |
| `binary-op` | `(+ a b)` | `a + b` |
| `quote` | `'x` | `\x` (symbol) |
| `quasiquote` | `` `x `` | (expanded before codegen) |

### Modules and exports

```
scscm/lexer    -- tokenize scscm text into a flat token list
scscm/parser   -- parse token list into an AST
scscm/expander -- expand macros (built-in stdlib + user defmacro)
scscm/codegen  -- emit sclang text from an expanded AST
scscm/compile  -- public API: text or file -> sclang text
scscm/server   -- OSC client for scsynth/hcsynth node/group control
```

`scscm/server` depends on `tur-osc`. All other modules are stdlib-only.

### API sketch

```turmeric
;; scscm/lexer
(tokenize source :cstr)              ;; => result<tokens :int>
(token-type tok :int) :cstr          ;; => ":symbol" ":keyword" ":number" etc.
(token-value tok :int) :cstr         ;; => raw text value
(token-line tok :int) :int
(token-col tok :int) :int
(tokens-free ts :int) :void

;; scscm/parser
(parse tokens :int)                  ;; => result<ast :int>
(ast-kind node :int) :cstr           ;; => ":list" ":symbol" ":number" etc.
(ast-symbol-name node :int) :cstr    ;; for :symbol nodes
(ast-number-value node :int) :cstr   ;; text representation of number
(ast-string-value node :int) :cstr   ;; string contents
(ast-list-len node :int) :int
(ast-list-get node :int i :int) :int ;; nth child (0-indexed)
(ast-free node :int) :void
(ast-free-all nodes :int) :void      ;; free a cons list of top-level ASTs

;; scscm/expander
(expand ast :int) :int               ;; => expanded ast (caller frees original)
(expand-all asts :int) :int          ;; expand a cons list of top-level forms

;; scscm/codegen
(generate ast :int) :cstr            ;; => sclang text for one node (heap-allocated)
(generate-all asts :int) :cstr       ;; => sclang text for all top-level forms

;; scscm/compile  (public API)
(compile-text source :cstr)          ;; => result<sclang-text :cstr>
(compile-file path :cstr)            ;; => result<sclang-text :cstr>

;; scscm/server  -- OSC client for scsynth / hcsynth (identical protocol)
;;
;; All message-send functions return result<:void>.
;; The server speaks standard scsynth UDP OSC on port 57110 by default.

;; Connection
(sc-connect host :cstr port :cstr)   ;; => result<client :int>  (UDP OSC client via tur-osc)
(sc-disconnect c :int) :void

;; Add-action constants (passed as :int to sc-s-new / sc-g-new)
;; 0 = add-head, 1 = add-tail, 2 = add-before, 3 = add-after, 4 = replace
(sc-add-head) :int
(sc-add-tail) :int
(sc-add-before) :int
(sc-add-after) :int
(sc-add-replace) :int

;; Synth node control
(sc-s-new c :int                     ;; /s_new -- instantiate a synth (play a note)
          synthname :cstr
          node-id :int               ;; -1 = server assigns
          add-action :int
          target-id :int             ;; group or node to add relative to; 1 = default group
          params :int)               ;; cons list of alternating :cstr name / :cstr value
    :ptr<void>

(sc-n-set c :int                     ;; /n_set -- set named controls on a running node
          node-id :int
          params :int)               ;; cons list of alternating :cstr name / :cstr value
    :ptr<void>

(sc-n-free c :int node-id :int)      ;; /n_free -- free a node (stop a note)
    :ptr<void>

(sc-n-run c :int node-id :int on :int) ;; /n_run -- pause (0) or resume (1) a node
    :ptr<void>

;; Group control
(sc-g-new c :int                     ;; /g_new -- create a named group
          group-id :int
          add-action :int
          target-id :int)
    :ptr<void>

(sc-g-free-all c :int group-id :int) ;; /g_freeAll -- free all nodes in group
    :ptr<void>

(sc-g-deep-free c :int group-id :int) ;; /g_deepFree -- recursively free nested groups
    :ptr<void>

;; Server lifecycle
(sc-notify c :int on :int)           ;; /notify 1 -- subscribe to /n_end, /done etc.
    :ptr<void>

(sc-status c :int)                   ;; /status -- request server status reply
    :ptr<void>

(sc-quit c :int)                     ;; /quit -- ask server to exit
    :ptr<void>

;; Timed bundles for sample-accurate scheduling
;;   timetag is an NTP-style fixed-point float (seconds since 1900-01-01);
;;   pass 1.0 for "immediate" (sc convention: bundle timetag 1 = now).
(sc-bundle-new timetag :float)       ;; => bundle :int  (wraps tur-osc bundle-new)
(sc-bundle-s-new b :int              ;; queue /s_new into bundle
                 synthname :cstr node-id :int add-action :int target-id :int
                 params :int) :void
(sc-bundle-n-set b :int node-id :int params :int) :void
(sc-bundle-n-free b :int node-id :int) :void
(sc-bundle-send c :int b :int)       ;; => result<:void>  (sends bundle via tur-osc client-send-bundle)
    :ptr<void>
(sc-bundle-free b :int) :void        ;; free bundle without sending
```

### Built-in macro stdlib

The expander includes all macros from `lhc_macros.js` STDLIB. For reference,
organized by category:

**Control flow / functional:**
`->`, `->>`, `do`, `when`, `unless`, `doseq`, `dotimes`, `collect`,
`icollect`, `accumulate`, `comp`, `partial`

**SuperCollider structural:**
`defsynth`, `definst`

**Scheduling / time:**
`routine`, `wait`, `now`, `metronome`, `at`, `in`, `demo`

**Pattern constructors (note/performance data):**
`pbind`, `pseq`, `prand`, `pwhite`, `pgeom`

**Synth control:**
`ctl`, `kill`

**Music theory utilities:**
`midi->hz`, `hz->midi`, `db->amp`, `amp->db`, `chord`, `degrees->pitches`

**Envelope constructors:**
`adsr`, `perc`, `env-line`

User code may define additional macros with `(defmacro name params body)`.

### Implementation phases

- [ ] **SC0** -- `build.tur`; `scscm/lexer`: tokenize atoms, numbers (int + float),
  strings, keywords (`:foo`), `(`, `)`, `[`, `]`, `{`, `}`, `'`, `` ` ``, `~`, `~@`,
  `#(`, `;` line comments, EOF; return flat token cons list; `tokens-free`
- [ ] **SC1** -- `scscm/parser`: parse token list to nested AST; handle all
  node kinds (atom, symbol, keyword, number, string, list, vector, quote,
  quasiquote, unquote, unquote-splicing, hash-paren); `ast-kind` and accessor fns;
  error result on malformed input; `ast-free`/`ast-free-all`
- [ ] **SC2** -- `scscm/codegen`: emit sclang text for atoms, symbols (hyphen->underscore,
  `?`->suffix handling), keywords (`\sym`), numbers, strings, vectors (`[a, b]`),
  binary ops (`a + b`), generic list calls (`foo(a, b)`), dot-calls (`obj.msg`),
  dot-new calls (`Cls.new(...)`), multi-statement join with `;\\n`
- [ ] **SC3** -- `scscm/expander`: implement macro environment with user-defined
  `defmacro` support; quasiquote expansion; built-in special forms: `fn`, `defn`,
  `var`, `set!`, `let`, `if`, `cond`, `.`, `.dot`, `class`, `list`, `array`,
  `dict`, `quote`, `quasiquote`, `super`, `this`; `expand` and `expand-all`
- [ ] **SC4** -- Expander: load full STDLIB (all macros from the list above);
  `scscm/compile` combining all four stages; `compile-text`; `compile-file`
  (reads file, calls compile-text)
- [ ] **SC5** -- Tests (round-trip: compile known scscm snippets, compare output
  against expected sclang strings); README section
- [ ] **SC6** -- `scscm/server`: `sc-connect`/`sc-disconnect` (thin wrapper over
  `tur-osc` `client-new`/`client-free`); add-action constants; `sc-s-new` (builds
  `/s_new` OSC msg from synthname + node-id + add-action + target + params cons
  list, sends via `tur-osc` `client-send`); `sc-n-set`, `sc-n-free`, `sc-n-run`;
  `sc-g-new`, `sc-g-free-all`, `sc-g-deep-free`; `sc-notify`, `sc-status`,
  `sc-quit`; timed-bundle helpers (`sc-bundle-new` wraps `tur-osc` `bundle-new`,
  `sc-bundle-s-new`/`n-set`/`n-free` add pre-built OSC msgs to the bundle,
  `sc-bundle-send` wraps `client-send-bundle`); add `tur-osc` to `:spices` in
  `build.tur`; tests (send to a mock port, verify msg bytes); `scscm-v0.1.0` tag

---

## tur-tidal

### What it does

`tur-tidal` provides a Tidal Cycles-inspired mini-notation parser and pattern
combinator library. It takes a compact string shorthand describing note/event
sequences, builds an in-memory pattern data structure, and renders it to scscm
(or raw sclang) Pbind expressions. **It produces note/performance data only.**
No audio synthesis, no UGens, no buffers.

It depends on `tur-scscm` for the final render step (wrapping output in a
`pbind` call via the scscm macro).

### Mini-notation reference

| Syntax | Meaning | Example |
|--------|---------|---------|
| `a b c` | sequence, equal duration | `"bd sd cp"` -> 3 events, each dur 1/3 |
| `~` | rest (silence) | `"bd ~ sd ~"` |
| `[a b]` | subsequence (subdivides its slot) | `"[bd sd] cp"` -> bd=1/4, sd=1/4, cp=1/2 |
| `<a b c>` | alternating: one per cycle, cycles through | `"<60 62 64>"` |
| `a*n` | repeat n times within the slot | `"bd*3 sd"` |
| `a/n` | slow by factor n (spans n cycles) | `"bd/2"` |
| `a(n,k)` | Euclidean rhythm: n pulses in k steps | `"bd(3,8)"` |
| `a@w` | set relative weight/duration | `"bd@2 sd"` (bd gets 2x slot) |

The pattern combinators operate on pattern values in memory; rendering to
scscm/sclang is a separate step.

### Modules and exports

```
tidal/notation  -- parse mini-notation string into a pattern value
tidal/pattern   -- pattern data and combinators (fast, slow, stack, cat, rev, ...)
tidal/event     -- event data (onset, dur, value) and event-list operations
tidal/render    -- render pattern to scscm Pbind text or raw sclang
```

### API sketch

```turmeric
;; tidal/event -- a single musical event in a cycle
;; onset and dur are fractions of one cycle (0.0 to 1.0)
(event-new onset :float dur :float value :cstr) :int  ;; => event handle
(event-onset e :int) :float
(event-dur e :int) :float
(event-value e :int) :cstr
(event-free e :int) :void

;; tidal/notation -- parse mini-notation
(parse-notation text :cstr)           ;; => result<pattern :int>
(notation-free p :int) :void

;; tidal/pattern -- combinators
;;   All combinators return a new pattern; caller frees inputs if desired.

;; Query: evaluate pattern at a given cycle number, return event list
(pattern-events p :int cycle :int)    ;; => event-list :int (cons of events)

(pattern-fast p :int n :float) :int   ;; speed up by factor n
(pattern-slow p :int n :float) :int   ;; slow down by factor n
(pattern-stack p1 :int p2 :int) :int  ;; overlay (union of events from both)
(pattern-cat p1 :int p2 :int) :int    ;; concatenate end-to-end
(pattern-rev p :int) :int             ;; reverse event order within cycle
(pattern-every n :int f :int p :int) :int  ;; apply combinator f every nth cycle
(pattern-degrade p :int prob :float) :int  ;; randomly drop events with prob
(pattern-free p :int) :void

;; tidal/render -- emit code from a pattern
;;
;; render-pbind evaluates pattern at cycle 0 (for static Pbind) or generates
;; a Pseq of midinote/dur lists for multi-cycle patterns.
;;
;; The :midinote values come from parsing event values as note names or raw
;; MIDI integers ("c4" -> 60, "d4" -> 62, "60" -> 60, etc.).
(render-pbind p :int instrument :cstr) :cstr
;; => scscm text, e.g.:
;;    (. (pbind :instrument "bass" :midinote (pseq [60 62 64] inf) :dur (pseq [0.25 0.25 0.5] inf)) play)

(render-sclang p :int instrument :cstr) :cstr
;; => raw sclang (skips scscm layer):
;;    (Pbind.new([\instrument, "bass", \midinote, Pseq.new([60,62,64],inf), \dur, Pseq.new([0.25,0.25,0.5],inf)])).play;

(render-events p :int cycle :int) :cstr
;; => plain text event table for inspection, one line per event:
;;    0.000  0.250  60
;;    0.250  0.250  62
;;    0.500  0.500  64
```

### Note name parser (inside tidal/event)

Event values that look like note names are converted to MIDI integers when
rendering. Supported format: `[a-g](#|b)?[0-9]` with middle C = `c4` = 60.
Raw integers pass through unchanged. `~` (rest) maps to MIDI -1 (omitted from
Pbind output).

| Input | MIDI |
|-------|------|
| `c4` | 60 |
| `d4` | 62 |
| `eb4` | 63 |
| `f#4` | 66 |
| `60` | 60 |
| `~` | -1 (rest) |

### Implementation phases

- [x] **TD0** -- `build.tur`; `tidal/event`: `event-new`, `event-onset`, `event-dur`,
  `event-value`, `event-free`; note-name -> MIDI integer parser (handles `c4`, `d#3`,
  `bb5`, raw integers, `~`); event-list helpers using stdlib cons
- [x] **TD1** -- `tidal/notation` (Phase 1): parse flat space-separated sequences;
  handle `~` rests; return a pattern that produces N equal-duration events per
  cycle; `parse-notation`, `notation-free`; `pattern-events` for a flat sequence
- [x] **TD2** -- `tidal/notation` (Phase 2): parse `[a b c]` subsequences
  (subdivide the slot); parse `<a b c>` alternating groups (one element per cycle,
  advancing each cycle); nested `[...]` groupings
- [x] **TD3** -- `tidal/notation` (Phase 3): modifiers -- `a*n` (repeat), `a/n`
  (slow/span), `a(n,k)` (Euclidean rhythm via Bjorklund algorithm), `a@w`
  (relative weight)
- [x] **TD4** -- `tidal/pattern` combinators: `pattern-fast`, `pattern-slow`,
  `pattern-stack`, `pattern-cat`, `pattern-rev`, `pattern-every`, `pattern-degrade`;
  multi-cycle `pattern-events` evaluation
- [x] **TD5** -- `tidal/render`: `render-pbind` (produces scscm text using
  `tur-scscm`'s `compile-text` on the generated scscm string), `render-sclang`
  (produces raw sclang directly), `render-events` (plain-text inspection table);
  tests; README section; `tidal-v0.1.0` tag

---

## Dependency graph

```
tur-tidal
  -> tur-scscm              (for render-pbind: generates scscm text, compiles to sclang)
  -> stdlib/result          (error handling throughout)
  -> stdlib/str             (string building)
  -> stdlib/vec             (event list scratch space)
  -> stdlib/math            (Bjorklund algorithm in TD3)

tur-scscm
  -> tur-osc (optional)     (scscm/server only -- requires liblo; compiler modules work without it)
  -> stdlib only            (lexer, parser, expander, codegen, compile)
```

---

## Shared work

### Guides (docs/guides/ in turmeric core)

Once both spices reach v0.1.0:

- `docs/guides/scscm-guide.md` -- language reference, API walkthrough,
  `compile-text` example, defmacro extension guide, `scscm/server` cookbook
  (connect, play a note, schedule a bundle, stop all)
- `docs/guides/tidal-guide.md` -- mini-notation reference table, combinator
  cookbook (polyrhythm, Euclidean rhythm, alternating chords), `render-pbind`
  usage, integration with tur-scscm

### turmeric-spices README additions

| Spice | Description | Tier | C dep |
|-------|-------------|------|-------|
| `tur-scscm` | scscm s-expression -> sclang compiler + scsynth/hcsynth OSC client | 1 -- inline-C only | tur-osc (optional, server module only) |
| `tur-tidal` | Tidal-like mini-notation -> Pbind/event text | 1 -- inline-C only | none |

### Suggested build order

Build `tur-scscm` first (SC0-SC6), then `tur-tidal` (TD0-TD5). SC0-SC5
(the compiler) and SC6 (the server client) are independent of each other within
tur-scscm and can be built in parallel. The TD5 render step depends on
`compile-text` from `tur-scscm`; all earlier TD phases are independent.
