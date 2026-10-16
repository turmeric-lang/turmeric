// tests/markdown-spec/oracle/render.js -- commonmark.js's HTML for every
// document in a bundle, for tests/markdown-spec/diff.py to compare with
// tur/markdown's.
//
// Usage: node render.js BUNDLE
//
// BUNDLE holds the documents, each followed by one byte 0x1E (record
// separator).  The output is each document's HTML followed by 0x1E, then a
// line feed -- the same shape tests/markdown-spec/render.tur writes.
//
// commonmark.js 0.31.2 is the reference tur/markdown's parser was ported
// from.  It departs from the spec, and from cmark, in a few places; each patch
// below makes it follow the spec there, so that a difference the comparison
// reports is ours to look at.

'use strict';

const fs = require('fs');
const commonmark = require('commonmark');

// 1. JavaScript's trim() strips every Unicode space (U+00A0, U+3000, ...).
//    commonmark.js trims a paragraph's and heading's content, an info string
//    and a reference label with it; the spec strips spaces, tabs and line
//    endings only.  Trim ASCII whitespace, as cmark does.
String.prototype.trim = function () {
  return this.replace(/^[ \t\n\v\f\r]+|[ \t\n\v\f\r]+$/g, '');
};

const R = commonmark.HtmlRenderer.prototype;

// 2. Raw HTML inside an image's alt text: commonmark.js writes it unescaped,
//    into the attribute.  cmark, and tur/markdown, escape it.
R.html_inline = function (node) {
  if (this.disableTags > 0) {
    this.out(node.literal);
  } else {
    this.lit(node.literal);
  }
};

// 3. A fence's language class is the info string's first word.  commonmark.js
//    splits on JavaScript's \s, which matches U+00A0; cmark, and tur/markdown,
//    split on ASCII whitespace.  It also leaves out the language- prefix when
//    the word already starts with it, where cmark always writes it; cmark's
//    rule is kept here too.
R.code_block = function (node) {
  const words = node.info ? node.info.split(/[ \t\n\v\f\r]+/) : [];
  const attrs = this.attrs(node);
  if (words.length > 0 && words[0].length > 0) {
    attrs.push(['class', 'language-' + this.esc(words[0])]);
  }
  this.cr();
  this.tag('pre');
  this.tag('code', attrs);
  this.out(node.literal);
  this.tag('/code');
  this.tag('/pre');
  this.cr();
};

// Two more departures cannot be patched from outside the module: an HTML
// block of kind 7 may end its start line with \s*, and a closing tag may hold
// \s* before its >.  Both take in U+00A0 too.  diff.py's random documents
// leave U+00A0 out for that reason.

const docs = fs.readFileSync(process.argv[2], 'utf8').split('\x1e');
docs.pop();
const out = [];
for (const src of docs) {
  const doc = new commonmark.Parser().parse(src);
  out.push(new commonmark.HtmlRenderer().render(doc));
  out.push('\x1e');
}
out.push('\n');
process.stdout.write(out.join(''));
