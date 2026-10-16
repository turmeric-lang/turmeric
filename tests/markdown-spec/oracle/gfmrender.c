/* tests/markdown-spec/oracle/gfmrender.c -- cmark-gfm's HTML for a bundle of
 * documents, for tests/markdown-spec/diff.py to compare with tur/markdown's
 * (markdown-library-plan M3).
 *
 * Usage: gfmrender core|gfm < BUNDLE
 *
 * BUNDLE holds the documents, each followed by one byte 0x1E.  The output is
 * each document's HTML followed by 0x1E.  `core` is plain CommonMark; `gfm`
 * attaches the five extensions GitHub uses: table, strikethrough, autolink,
 * tagfilter and tasklist.  Raw HTML is passed through (CMARK_OPT_UNSAFE) in
 * both, as tur/markdown's MdRawPass and MdRawTagFilter do.
 *
 * diff.py builds this against the cmark-gfm checkout cmark-gfm.pin names. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"

int main(int argc, char **argv) {
  static const char *exts[] = {"table", "strikethrough", "autolink", "tagfilter", "tasklist"};
  int gfm = argc > 1 && strcmp(argv[1], "gfm") == 0;
  size_t cap = 1 << 20, len = 0, got;
  char *buf = malloc(cap);
  if (!buf) return 2;
  while ((got = fread(buf + len, 1, cap - len, stdin)) > 0) {
    len += got;
    if (len == cap) {
      char *bigger = realloc(buf, cap *= 2);
      if (!bigger) return 2;
      buf = bigger;
    }
  }
  cmark_gfm_core_extensions_ensure_registered();
  for (size_t i = 0; i < len;) {
    size_t j = i;
    while (j < len && buf[j] != 0x1e) j++;
    cmark_parser *p = cmark_parser_new(CMARK_OPT_UNSAFE);
    if (gfm)
      for (size_t k = 0; k < sizeof exts / sizeof exts[0]; k++)
        cmark_parser_attach_syntax_extension(p, cmark_find_syntax_extension(exts[k]));
    cmark_parser_feed(p, buf + i, j - i);
    cmark_node *doc = cmark_parser_finish(p);
    char *html = cmark_render_html(doc, CMARK_OPT_UNSAFE, cmark_parser_get_syntax_extensions(p));
    fputs(html, stdout);
    fputc(0x1e, stdout);
    free(html);
    cmark_node_free(doc);
    cmark_parser_free(p);
    i = j + 1;
  }
  free(buf);
  return 0;
}
