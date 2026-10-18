# `tur fmt` rewrites hex literals as decimal and `\x` escapes as raw bytes

**Severity: low (formatter fidelity).** `tur fmt` prints an integer literal
from its value and a string literal from its resolved bytes, so the spelling
the author chose is lost:
- `0xFF` comes back as `255`;
- `"caf\xc3\xa9"` comes back as `"café"`, in raw UTF-8.

The program means the same afterwards. But two things make this more than
cosmetic:
- **The stdlib can't keep hex.** `tests/run-fmt.sh`'s `fmt-bootstrap-stdlib`
  requires every stdlib file to be fmt-clean, so no stdlib file can keep a hex
  literal in Turmeric code. `stdlib/unicode.tur` writes its UTF-8 byte ranges in
  decimal, with the hex in a comment, for this reason.
- **ASCII files stop being ASCII.** A formatted file holding `\x` escapes of
  bytes 0x80 and up gains non-ASCII bytes. Fixture sources must be ASCII
  (CLAUDE.md, "Fixture Files"), so a fixture written with escapes can't be run
  through `tur fmt`.

Found 2026-10-16 while writing `stdlib/unicode.tur` (markdown-library-plan U1).

## Repro

```sh
printf '(defn main [] : int\n  (do (println "caf\\xc3\\xa9") (println 0xFF) 0))\n' > /tmp/f.tur
./build/tur fmt --diff /tmp/f.tur
```

```
-(defn main [] : int
-  (do (println "caf\xc3\xa9") (println 0xFF) 0))
+(defn main [] : int (do (println "café") (println 255) 0))
```

## Root cause

- **Integers.** The reader folds a `0x` / `0b` lexeme into a value and records
  no radix (`src/compiler/reader.c:589`, the `bitpattern` branch). The
  formatter then prints every `F_INT` with `%lld` (`src/compiler/fmt.c:151`).
- **Strings.** `F_STR` holds the bytes with escapes already resolved.
  `print_str_escaped_b` (`src/compiler/fmt.c:156`) re-escapes only `"`, `\`,
  `\n`, `\t`, `\r` and bytes below 0x20, and copies every byte 0x80 and up
  through raw.

## Fix directions

1. **Keep the radix.** Record it on the `Form` (a `uint8_t` beside
   `lit_suffix` / `fx_prov` in `src/compiler/forms.h`), or have the formatter
   reprint an `F_INT` from its source span when that span spells a `0x` or `0b`
   lexeme. Print `0x` digits in upper case to match the reader's `#\x` output.
2. **Keep strings ASCII.** Print bytes 0x80 and up as `\xNN`, at least when the
   source spelled them that way (the span again). Writing them always as `\xNN`
   would also keep every formatted file ASCII; whether that is wanted for
   source that really contains UTF-8 is a style decision.
3. **Guard both.** A `tests/run-fmt.sh` case that formats `0xFF`, `0b101` and
   `"\xc3\xa9"` and checks they come back unchanged.

## Resolution (2026-10-18)

Fix directions 1 and 2, keyed on how the source spelled the literal.

- **Integers.** The reader records the radix of a `0x` / `0b` literal, and
  whether it was written with a `-`, in a new `Form.lit_spell`
  (`LitSpelling`, `src/compiler/forms.h`; it fits in padding beside
  `fx_prov`). `fmt_num_literal` prints such a literal back in its radix: hex
  digits in upper case, the bit pattern as the reader reads it (so
  `0xFFFFFFFFFFFFFFFF` stays itself), a `-0x10` as its sign and magnitude, a
  type suffix kept. Leading zeros (`0x00FF`) are not kept.
- **Strings.** When an escape wrote a byte 0x80 or up (`\xNN`, octal,
  `\u{...}`), the reader marks the string `LIT_SPELL_HIGH_ESC`, and
  `print_str_escaped_b` prints every such byte as `\xNN`, so an ASCII file
  stays ASCII. A string whose UTF-8 was written raw stays raw, which settles
  the style question by following the source.

Pinned by `tests/run-fmt.sh` `fmt-stdin-literal-spelling` (`0xFF`, `0xff`,
`0b101`, `-0x10`, `0xFFFFFFFFFFFFFFFF`, `0x1Fu8`, `"caf\xc3\xa9"`).
`fmt-bootstrap-stdlib` still passes, so stdlib files can now keep a hex
literal; `stdlib/unicode.tur`'s decimal byte ranges are left as they are.
