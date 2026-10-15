# `web/public/docs/`

`html` here is a symlink to the repository's `docs/html/`, the *rendered*
documentation that `tur run docs` produces. Vite copies it into `dist/docs/html/`,
so the worker serves `/docs/html/guides/`, `/docs/html/api/`, and
`/docs/html/spices/` -- the URL space the site has always used.

It used to be `web/public/docs -> ../../docs`, the whole documentation *source*
tree. That shipped `docs/archive/`, `docs/reported/`, `docs/upcoming/`, and
every loose planning note into `dist/` on every deploy: megabytes of markdown
the site never links to and no reader ever asked for. Narrowing the symlink to
`docs/html/` drops all of it while leaving every public URL exactly where it
was (OD5).

Consequently a doc link that points *outside* `docs/html/` -- say
`../upcoming/some-plan.md` from a rendered guide -- does not resolve on the
site. Such links were already dead before this change (they resolved to
`/docs/html/upcoming/...`, which never existed); `tools/genpack.py` now reports
them when it builds the docs pack.
