# Projection gives a third role a `Branch` it can never be offered

**RESOLVED 2026-10-17** (filed and fixed the same day) -- see
[Resolution](#resolution-2026-10-17). Pinned by
`errors/session-project-nonprojectable`.

**Severity:** medium. A multiparty protocol the session-types guide calls
non-projectable was accepted. The role it is projected onto gets a local type
that waits for a choice nothing ever sends it.

## Repro

```turmeric
(defprotocol Bad3 [A B C]
  (choice A
    [left  (-> B C int)]
    [right (-> C B int)]))

(defn f [^linear ch : (project Bad3 C)] : nil (close ch))

(defn main [] : int 0)
```

`tur check` accepted it; `(project Bad3 C)` was
`Branch (Recv int End) (Send int End)`. `A` decides, but neither branch has
`A` tell `C` anything, so `C` cannot learn which branch was taken.

The fixture expected TUR-E0220 all along. Its one-line `expected.diag` had no
trailing newline, so the harness never read the needle
([needle-files-without-trailing-newline-assert-nothing](needle-files-without-trailing-newline-assert-nothing.md)),
and the harness only checked that the program failed to build.

## Root cause

`src/compiler/project.c:project_inner`, `GI_CHOICE`: a role that is not the
decider and whose two branches project differently always got
`type_branch(b0, b1)`. That is right for the decider's peer in a two-role
protocol, where the choice travels on the one channel between them. With a
third role there is no channel that carries the choice to it.

## Resolution (2026-10-17)

`project_inner` gives such a role a `Branch` only when the protocol has no
role other than it and the decider (`gi_mentions_other_role`, run over the
whole protocol, which `session_project` records). Otherwise it reports
TUR-E0220: "role 'C' behaves differently in the branches of the choice made by
'A', but cannot learn which branch was taken; make its protocol the same in
every branch". The session-types guide's projection rules say so.

The multiparty runtime has no choice yet, so no working program lost
anything. When it gains one, this rule loosens to "a role the decider
informs".
