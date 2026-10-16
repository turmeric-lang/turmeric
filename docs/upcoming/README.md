# docs/upcoming/ -- open plans

Plans for work intended but not yet done. The layout is deliberately flat:

```
docs/upcoming/
  <plan>.md            compiler, language, stdlib, tooling, web, CI plans
  spices/<plan>.md     plans about a spice (a package in turmeric-spices)
```

That is all. **Do not add `hold/`, `v1/`, `v2/` or any other status or
milestone subdirectory.** A plan's status (draft, on hold, v1-scoped,
implemented, blocked on X) belongs in the `> Status:` line at the top of the
plan itself, where it travels with the file and cannot go stale in a path.
Moving a plan between directories to change its status also breaks every link
to it.

Rules of thumb:

- **Spice or not?** If the work lands in a spice (`spices/<name>/` in
  turmeric-spices), it goes under `spices/`. If it lands in this repo
  (compiler, `stdlib/`, tooling, the web UI, CI), it stays at the top level,
  even when it mentions spices in passing.
- **Finished?** Move it to `docs/archive/` (see CLAUDE.md), do not leave it
  parked here.
- **Moving a plan?** Use `git mv`, then update links to it. Guides link plans
  by GitHub URL; reports and other plans link by relative path.

The `/ci` page counts plans from this tree (`tools/ci/collect-docs.py`):
top-level files are the core plans, `spices/` the spice plans. The retired
`held_plans` / `v1_plans` series only chart history.
