// repo.js -- the GitHub coordinates of the Turmeric repo, in one place.
//
// Same shape as csp.js: one source, several places it is applied. Everything
// on the web side that names the repo derives from GH_REPO -- site.js's nav
// and link-title entries, the /ci dashboard's commit permalinks, and both the
// installer's REPO and the ci-metrics raw base in worker.js. A repo transfer
// (personal account -> org) is then a one-line change here instead of a sweep
// across four files, two of which are template strings where a missed `$`
// escape fails silently.
//
// Deliberately a literal rather than an env var: these strings are baked into
// a static build and served to browsers, so a mistyped or unset variable would
// ship a dead link rather than fail the build. One literal a grep can find
// beats an indirection that hides.
export const GH_REPO = 'turmeric-lang/turmeric';
export const GITHUB_URL = `https://github.com/${GH_REPO}`;
