---
title: Bibliography
category: Reference
description: Academic papers, theses, and technical references cited across Turmeric design documents and guides
tags:
  - bibliography
  - references
  - academic-papers
---

# Bibliography

Academic papers, theses, and technical references cited across the Turmeric
design documents and guides.

---

## Type Inference

**Damas-Milner Type Inference**
Luis Damas and Robin Milner. *Principal type-schemes for functional programs.*
POPL 1982.
<https://dl.acm.org/doi/10.1145/359576.359587>

Referenced by: `docs/archive/history/higher-kinded-types-plan.md` (kind
inference algorithm).

---

## GADTs and Higher-Rank Polymorphism

**Simple Unification-Based Type Inference for GADTs**
Simon Peyton Jones, Dimitrios Vytiniotis, Stephanie Weirich, Geoffrey Washburn.
*Simple Unification-Based Type Inference for GADTs.* ICFP 2006.
<https://www.microsoft.com/en-us/research/publication/simple-unification-based-type-inference-for-gadts/>

Referenced by: `docs/archive/gadts-plan.md`.

---

**Wobbly Types: Type Inference for GADTs**
Simon Peyton Jones, Geoffrey Washburn, Stephanie Weirich.
*Wobbly Types: Type Inference for GADTs.* 2004.
<https://www.microsoft.com/en-us/research/publication/wobbly-types-type-inference-for-gadts/>

Referenced by: `docs/archive/gadts-plan.md`.

---

**Complete and Easy Bidirectional Typechecking for Higher-Rank Polymorphism**
Joshua Dunfield and Neelakantan R. Krishnaswami.
*Complete and Easy Bidirectional Typechecking for Higher-Rank Polymorphism.*
ICFP 2013.
<https://arxiv.org/abs/1306.6032>

Referenced by: `docs/archive/gadts-plan.md`.

---

**Fun with Phantom Types**
Ralf Hinze.
*Fun with Phantom Types.* In *The Fun of Programming*, 2003.
<https://www.cs.ox.ac.uk/ralf.hinze/publications/With.pdf>

Referenced by: `docs/archive/gadts-plan.md`.

---

**Type-Safe Observable Sharing in Haskell**
Andy Gill.
*Type-Safe Observable Sharing in Haskell.* Haskell Symposium 2009.
<https://dl.acm.org/doi/10.1145/1596638.1596641>

Referenced by: `docs/archive/gadts-plan.md` (motivation for typed ASTs).

---

## Algebraic Effects and Handlers

**Programming with Algebraic Effects and Handlers**
Andrej Bauer and Matija Pretnar.
*Programming with Algebraic Effects and Handlers.* JLAMP 2015.
(Eff language.)
<https://doi.org/10.1016/j.jlamp.2014.02.001>

Referenced by: `docs/archive/effect-types-row-polymorphism-plan.md`.

---

**Koka: Programming with Row-Polymorphic Effect Types**
Daan Leijen.
*Koka: Programming with Row-Polymorphic Effect Types.*
MSFP 2014 (EPTCS 153).
<https://doi.org/10.4204/EPTCS.153.8>

Referenced by: `docs/archive/effect-types-row-polymorphism-plan.md`.

---

**Retrofitting Effect Handlers onto OCaml**
K.C. Sivaramakrishnan, Tom Kelly, Leo White, Stephen Dolan, Sadiq Jaffer,
Anil Madhavapeddy.
*Retrofitting Effect Handlers onto OCaml.* PLDI 2021. (OCaml 5.)
<https://doi.org/10.1145/3453483.3454039>

Referenced by: `docs/archive/effect-types-row-polymorphism-plan.md`.

---

**Liberating Effects with Rows and Handlers**
Daniel Hillerstrom and Sam Lindley.
*Liberating Effects with Rows and Handlers.*
TyDe 2016.
<https://doi.org/10.1145/3007263.3007271>

Referenced by: `docs/archive/effect-types-row-polymorphism-plan.md`.

---

## Delimited Continuations

**Abstracting Control**
Olivier Danvy and Andrzej Filinski.
*Abstracting Control.* LFP 1990.
(Introduces the static `shift`/`reset` semantics.)
<https://doi.org/10.1145/91556.91622>

Referenced by: `docs/guides/delimited-control-operators-guide.md`.

---

**The Theory and Practice of First-Class Prompts**
Matthias Felleisen.
*The Theory and Practice of First-Class Prompts.* POPL 1988.
(Prompts and the `control`/`prompt` family.)
<https://doi.org/10.1145/73560.73576>

Referenced by: `docs/guides/delimited-control-operators-guide.md`.

---

**Subtyping Delimited Continuations**
Marek Materzok and Dariusz Biernacki.
*Subtyping Delimited Continuations.* ICFP 2011.
(The `shift0`/`reset0` hierarchy and its relation to `shift`/`reset`.)
<https://doi.org/10.1145/2034773.2034786>

Referenced by: `docs/guides/delimited-control-operators-guide.md`.

---

**A Monadic Framework for Delimited Continuations**
R. Kent Dybvig, Simon Peyton Jones, and Amr Sabry.
*A Monadic Framework for Delimited Continuations.* JFP 2007.
<https://doi.org/10.1017/S0956796807006259>

Referenced by: `docs/guides/delimited-control-operators-guide.md`.

---

## Contract Types

**Contracts for Higher-Order Functions**
Robert Bruce Findler and Matthias Felleisen.
*Contracts for Higher-Order Functions.* ICFP 2002.
<https://dl.acm.org/doi/10.1145/351492.351503>

Referenced by: `docs/archive/history/contract-types-plan.md`.

---

## Intersection and Union Types

**Intersection and Union Types: Syntax and Semantics**
Franco Barbanera and Mariangiola Dezani-Ciancaglini.
*Intersection and Union Types: Syntax and Semantics.*
(Attributed to Barbanera & Franzese in the plan document.)
<https://dl.acm.org/doi/10.1145/357052.357055>

Referenced by: `docs/archive/history/intersection-union-types-plan.md`.

---

## Session Types

**Types for Dyadic Interaction**
Kohei Honda.
*Types for Dyadic Interaction.* CONCUR 1993.
<https://dl.acm.org/doi/10.1145/248206.248214>

Referenced by: `docs/archive/history/session-types-plan.md`;
`docs/guides/advanced-type-system-rationale.md` (Honda-Yoshida-Carbone
projection algorithm).

---

**Multiparty Asynchronous Session Types**
Kohei Honda, Nobuko Yoshida, Marco Carbone.
*Multiparty Asynchronous Session Types.* POPL 2008.
<https://dl.acm.org/doi/10.1145/1328897.1328472>

Referenced by: `docs/archive/history/session-types-plan.md`;
`docs/guides/advanced-type-system-rationale.md`.

---

**Session Types for C**
Nels Covington, Robert Dockins, Andrew Kennedy, Amal Ahmed.
*Session Types for C.* (Attributed to Covington et al. in the plan document.)
<https://dl.acm.org/doi/10.1145/1596553.1596586>

Referenced by: `docs/archive/history/session-types-plan.md`.

---

## Substructural and Linear Types

**Linear Types Can Change the World!**
Philip Wadler.
*Linear Types Can Change the World!* IFIP TC2 Working Conference, 1990.
<https://philipwadler.com/papers/linearity/linearity.pdf>

Referenced by: `docs/archive/history/linear-types-plan.md`.

---

## Refinement Types and Program Verification

**Liquid Types**
Patrick M. Rondon, Ming Kawaguchi, Ranjit Jhala.
*Liquid Types.* PLDI 2008.
(The template-based inference approach in the Turmeric refinement-types design
follows this paper closely.)

Referenced by: `docs/archive/refinement-types-plan.md`.

---

**F\*: Towards a Verified Extraction-Based Compiler for Mutual Recursive Definitions**
Nikhil Swamy et al.
(The two-phase static-then-runtime-fallback strategy in the Turmeric refinement
types design is modelled on F\*'s `Tot`/`Dv` effect distinction.)

Referenced by: `docs/archive/refinement-types-plan.md`.

---

**Dafny: An Automatic Program Verifier for Functional Correctness**
K. Rustan M. Leino.
*Dafny: An Automatic Program Verifier for Functional Correctness.*
LPAR 2010.
(The counterexample hint generation in the refinement-types design mirrors
Dafny's error reporting.)

Referenced by: `docs/archive/refinement-types-plan.md`.

---

## Standards and Specifications

**SMT-LIB 2.6 Standard**
Clark Barrett, Pascal Fontaine, Cesare Tinelli et al.
<https://smtlib.cs.uiowa.edu/>

Referenced by: `docs/archive/refinement-types-plan.md` (SMT-LIB2 encoder for
refinement-type obligations).
