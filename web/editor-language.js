// editor-language.js -- the Turmeric language for Monaco: registration, the
// Monarch tokenizer, brackets and comments, and the theme hookup.

import { getMonaco } from './editor-ref.js';
import { applyMonacoTheme } from './editor-theme.js';

/**
 * Configure Monaco Editor for Turmeric
 */
export function configureMonaco() {
    const monaco = getMonaco();
    // Define Turmeric language
    monaco.languages.register({
        id: 'turmeric',
        extensions: ['.tur'],
        aliases: ['Turmeric', 'tur'],
        mimetypes: ['text/x-turmeric']
    });
    
    // The characters that may continue a symbol, as the reader takes them
    // (is_sym_cont in src/compiler/reader.c): alphanumerics and
    // `- + * / = < > ! ? _ $ & . # ^ | '`. A word is a keyword only when none
    // of them follows it, which is what `\b` got wrong: it counts `-`, `/` and
    // `!` as boundaries, so `apply-to` read as the builtin `apply` plus `-to`.
    const SYM_CONT = "[a-zA-Z0-9_\\-+*/=<>!?$&.#^|']";
    const wholeSymbol = (alternatives) => new RegExp('\\b(?:' + alternatives + ')(?!' + SYM_CONT + ')');

    // `<...>` after a type name: space-free, balanced, up to three deep
    // (`ptr<ptr<ptr<void>>>`) -- a regex cannot count, so the depth is fixed.
    const GENERIC_ARGS = '<(?:[^\\s<>()\\[\\]{}",;]|<(?:[^\\s<>()\\[\\]{}",;]|<[^\\s<>()\\[\\]{}",;]*>)*>)*>';

    // The `^` modifiers the elaborator recognises (the sym_caret_* table in
    // src/compiler/elab_core.c, plus `^tailcall`). Keep the two in step: a name
    // missing here is coloured as a kind, which is what the compiler also
    // takes an unrecognised `^name` to be.
    const CARET_MODIFIERS = [
        'mut', 'private', 'persistent', 'linear', 'unique', 'affine', 'relevant',
        'atomic', 'thread-local', 'deferred-init', 'borrow', 'fat', 'extends',
        'capability', 'multishot', 'deprecated', 'reflect', 'construct', 'byval',
        'non-exhaustive', 'tailcall',
        // The flags of a defopaque / defstruct / defdata / defgadt, written
        // before the name (caret-type-attributes-plan); `linear` and `affine`
        // above are the same words.
        'copy', 'move', 'heap', 'sealed', 'non-null', 'no-auto-ctor',
    ].join('|');

    // The C types an extern-c parameter may be written with, `^int`.
    const CARET_C_TYPES = [
        'int', 'float', 'bool', 'char', 'cstr', 'void', 'nil', 'size', 'usize',
        'isize', 'ssize', 'float32', 'float64', 'int8', 'int16', 'int32', 'int64',
        'uint8', 'uint16', 'uint32', 'uint64', 'i8', 'i16', 'i32', 'i64', 'u8',
        'u16',
    ].join('|');

    // Define syntax highlighting rules
    monaco.languages.setMonarchTokensProvider('turmeric', {
        tokenizer: {
            root: [
                // Comments
                [/;.*/, 'comment'],
                
                // Strings
                [/"([^"\\]|\\.)*"/, 'string'],
                
                // A `^` starts a metadata symbol in the reader, so the caret is
                // part of the name and the whole symbol is one token. What it
                // MEANS is decided by the name, as the elaborator does:
                //  - a closed list of modifiers (CARET_MODIFIERS) -> 'annotation'
                //  - an extern-c parameter's C type, `^int` / `^ptr<void>` -> 'type'
                //  - anything else is a kind (`^f`, `^^f`, `^&r`) or a typeclass
                //    constraint (`^Eq`) -> 'kind'; the compiler too reads an
                //    unrecognised `^name` as a higher-kinded type parameter.
                [new RegExp('\\^(?:' + CARET_MODIFIERS + ')(?!' + SYM_CONT + ')'), 'annotation'],
                [new RegExp('\\^(?:' + CARET_C_TYPES + ')(?!' + SYM_CONT + ')'), 'type'],
                [new RegExp('\\^ptr' + GENERIC_ARGS), 'type'],
                [/\^[^\s()\[\]{}",;'`]+/, 'kind'],

                // A type annotation, `:int` or `:ptr<void>`: the colon and any
                // generic arguments belong to the name, so it is one token.
                [new RegExp(':[a-zA-Z_][a-zA-Z0-9_\\-?!]*(?:' + GENERIC_ARGS + ')?'), 'type'],
                // The same annotation spelled the way the stdlib writes it,
                // with a space after the colon: `[name : cstr] : void`. The
                // colon and the name are both type, the gap between them not.
                [new RegExp('(:)([ \\t]+)([a-zA-Z_][a-zA-Z0-9_\\-?!/]*(?:' + GENERIC_ARGS + ')?)'),
                    ['type', '', 'type']],
                // A colon that stands alone is the annotation separator whatever
                // follows it -- `x : (w a)`, `g : (fn [int] int)` -- so it takes
                // the type colour too, and every separator in a signature matches.
                [/:(?=[ \t]|$)/, 'type'],
                // A generic type name on its own, `ptr<void>` or
                // `option<ptr<int>>`, as in a function type `(fn [ptr<void>] nil)`.
                // A lone `<` (less-than) never reaches here: it needs a name
                // before it and a closing `>` after the arguments.
                [new RegExp('[a-zA-Z_][a-zA-Z0-9_\\-?!/]*' + GENERIC_ARGS), 'type'],

                // Numbers
                [/\b\d+\b/, 'number'],
                [/\b\d+\.\d+\b/, 'number.float'],
                
                // Keywords. Each is a whole symbol: `apply` is a builtin but
                // `apply-to` is not, so a keyword matches only when nothing the
                // reader would continue the symbol with follows it (SYM_END).
                [wholeSymbol('defn|def|fn|let|if|when|unless|loop|for|while|do|return|break|continue'), 'keyword'],
                [wholeSymbol('println|print|read|eval|apply|map|filter|reduce|fold'), 'keyword.operator'],
                
                // Built-in types and functions
                [wholeSymbol('int|float|bool|str|char|nil|true|false'), 'type'],
                [wholeSymbol('rc\\/of|rc\\/clone|rc\\/drop|rc\\/strong-count'), 'keyword.operator'],
                [wholeSymbol('defeffect|perform|handle|resume|cont'), 'keyword.operator'],
                [wholeSymbol('try|catch|throw|raise'), 'keyword.operator'],
                
                // An earmuffed global, `*args*`: a `*` that runs straight into a
                // name is the start of a symbol, not multiplication (`(* a b)`
                // has a space after it).
                [new RegExp('\\*[a-zA-Z_]' + SYM_CONT + '*'), 'identifier'],

                // Symbols
                [/[\+\-\*\/\=\<\>\!\&\|\%\^~]/, 'operator'],
                [/[\(\)\[\]\{\}]/, 'delimiter'],
                
                // Identifiers: a symbol runs on through every character the reader
                // allows inside one, so `set-field!`, `string/adopt-cstr` and
                // `string->cstr` are each a single token.
                [new RegExp('[a-zA-Z_]' + SYM_CONT + '*'), 'identifier']
            ]
        }
    });

    // Language configuration — teaches Monaco which characters form bracket
    // pairs so its native bracket-pair colorization (rainbow parens, matching
    // the trowel editor) can depth-color them. `colorizedBracketPairs` lists
    // the pairs that participate in the rainbow cycle.
    monaco.languages.setLanguageConfiguration('turmeric', {
        brackets: [
            ['(', ')'],
            ['[', ']'],
            ['{', '}'],
        ],
        colorizedBracketPairs: [
            ['(', ')'],
            ['[', ']'],
            ['{', '}'],
        ],
        autoClosingPairs: [
            { open: '(', close: ')' },
            { open: '[', close: ']' },
            { open: '{', close: '}' },
            { open: '"', close: '"' },
        ],
        surroundingPairs: [
            { open: '(', close: ')' },
            { open: '[', close: ']' },
            { open: '{', close: '}' },
            { open: '"', close: '"' },
        ],
        comments: { lineComment: ';' },
        // Monaco's default word pattern is C-shaped: it breaks on `-`, `?`,
        // `!`, `/` and the comparison characters, so `nil-value` is three
        // words and `vec-push!` is four. That is wrong everywhere it is used
        // here -- double-click selection, occurrence highlight, the range a
        // completion replaces, and the name the hover fallback looks up.
        //
        // This is exactly the character class the server's own tokenizer
        // accepts (is_ident_char, src/lsp/lsp_util.c:6). Keeping the two in
        // step is what makes a client-side word and a server-side word the
        // same word.
        wordPattern: /[A-Za-z0-9\-?!*/><+=_]+/g,
    });

    // The editor follows the site's light/dark switch (public/theme.js). Its
    // theme is a JS object, not CSS, so it is rebuilt from the stylesheet on
    // every change rather than defined once.
    applyMonacoTheme();
    document.addEventListener('tur-themechange', applyMonacoTheme);
}
