// editor-theme.js -- the Monaco themes: token colors, editor chrome, rainbow
// brackets, and the minimap colors read from the stylesheet.
//
// The token colors are written down once, in web/editor-palette.json;
// tests/check-editor-palette.py (ctest `tur_editor_palette`) reads
// MONACO_THEMES out of this file and fails when the two disagree.

import { getMonaco } from './editor-ref.js';

/**
 * Read a CSS custom property and return it only if Monaco can parse it.
 *
 * Monaco runs theme colors through Color.fromHex(), which returns opaque RED
 * on any parse failure -- so an `rgba()` or a `var()` chain that did not
 * resolve does not degrade, it repaints the strip bright red. Anything that is
 * not a literal #RGB / #RRGGBB / #RRGGBBAA is refused here and the caller's
 * fallback is used instead, which is the same color the stylesheet declares.
 */
function cssHex(varName, fallback) {
    try {
        const raw = getComputedStyle(document.documentElement)
            .getPropertyValue(varName).trim();
        if (/^#(?:[0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.test(raw)) return raw;
    } catch {
        /* no document, or a browser that refuses computed styles here */
    }
    return fallback;
}

/** Replace (or append) the alpha byte of a #RRGGBB / #RRGGBBAA color. */
function withAlpha(hex, alphaByte) {
    const base = /^#[0-9a-f]{8}$/i.test(hex) ? hex.slice(0, 7) : hex;
    return /^#[0-9a-f]{6}$/i.test(base) ? base + alphaByte : base;
}

/** The site theme public/theme.js put on <html>: 'light' or 'dark'. */
function siteTheme() {
    return document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark';
}

/** The Monaco theme matching the site theme -- what every editor is created with. */
export function monacoThemeName() {
    return siteTheme() === 'light' ? 'turmeric-light' : 'turmeric-dark';
}

// Rainbow-bracket palettes. The dark set is mirrored from trowel's
// turmeric-dark theme (resources/turmeric-dark.theme.json rainbow0..6 +
// bracketError). Monaco supports six depth colors before the cycle repeats, so
// we take six of trowel's seven levels spanning the spectrum; the
// unexpected-bracket color is trowel's BracketError red. The light set walks
// the same spectrum, each step darkened to clear 3.7:1 on the light canvas.
const RAINBOW_BRACKETS = {
    dark: {
        'editorBracketHighlight.foreground1':               '#EFA030', // orange
        'editorBracketHighlight.foreground2':               '#D7C94A', // yellow
        'editorBracketHighlight.foreground3':               '#A8C98A', // green
        'editorBracketHighlight.foreground4':               '#7AC4B8', // teal
        'editorBracketHighlight.foreground5':               '#8AB0E8', // blue
        'editorBracketHighlight.foreground6':               '#C4A0E8', // purple
        'editorBracketHighlight.unexpectedBracket.foreground': '#FF5C57', // error red
    },
    light: {
        'editorBracketHighlight.foreground1':               '#B5650A', // orange
        'editorBracketHighlight.foreground2':               '#8A7800', // yellow
        'editorBracketHighlight.foreground3':               '#4E7F24', // green
        'editorBracketHighlight.foreground4':               '#13756A', // teal
        'editorBracketHighlight.foreground5':               '#2F62B0', // blue
        'editorBracketHighlight.foreground6':               '#7748AE', // purple
        'editorBracketHighlight.unexpectedBracket.foreground': '#D0312D', // error red
    },
};

/**
 * Minimap + overview ruler (try-turmeric-navigation-and-minimap-plan M1).
 *
 * Colors are resolved out of the stylesheet rather than written here as
 * literals. The strip sits directly against the editor canvas and paints marks
 * in the same semantic colors the console and the status dots use; a hardcoded
 * hex would silently stop matching the first time one of those tokens moves --
 * or the first time the reader switches theme. c2mp reaches into CSS custom
 * properties for the same reason (minimap.js:204-209).
 *
 * Monaco parses these with Color.fromHex() and falls back to opaque RED on any
 * parse failure, so cssHex() refuses anything that is not #RGB / #RRGGBB /
 * #RRGGBBAA and hands back the literal fallback instead. A theme token that
 * fails to resolve must look like the theme it belongs to, never like an
 * error -- hence one fallback set per theme (`fb`).
 */
function minimapColors(fb) {
    return {
        // Canvas: the strip is part of the editor, not a panel beside it.
        'minimap.background':                   cssHex('--bg-base', fb.bgBase),
        'minimapSlider.background':             withAlpha(cssHex('--text-dim', fb.textDim), '59'),
        'minimapSlider.hoverBackground':        withAlpha(cssHex('--text-sec', fb.textSec), '59'),
        'minimapSlider.activeBackground':       withAlpha(cssHex('--text-sec', fb.textSec), '8C'),

        // Marks inside the strip. Diagnostics keep full opacity -- they are
        // the reason to look at it; selection and find are washes, because a
        // mark you cannot see past is a mark that hides the shape of the file.
        // c2mp's rule (minimap.js §"marks as washes"), same conclusion.
        'minimap.errorHighlight':               cssHex('--error-color', fb.error),
        'minimap.warningHighlight':             cssHex('--warning-color', fb.warning),
        'minimap.findMatchHighlight':           withAlpha(cssHex('--gold', fb.gold), '99'),
        'minimap.selectionHighlight':           withAlpha(cssHex('--text-dim', fb.textDim), '80'),
        'minimap.selectionOccurrenceHighlight': withAlpha(cssHex('--text-sec', fb.textSec), '66'),
        'minimapGutter.addedBackground':        withAlpha(cssHex('--success-color', fb.success), 'B3'),
        'minimapGutter.modifiedBackground':     withAlpha(cssHex('--gold', fb.gold), 'B3'),
        'minimapGutter.deletedBackground':      withAlpha(cssHex('--error-color', fb.error), 'B3'),

        // The outboard ruler. This is the affordance c2mp builds by hand
        // (minimap.js RULER_W): a diagnostic gets a tick here because it must
        // stay findable when the minimap is scrolled past it; an occurrence
        // does not, because there can be fifty of them and they are noise at
        // this width.
        'editorOverviewRuler.background':       cssHex('--bg-base', fb.bgBase),
        'editorOverviewRuler.border':           '#00000000',
        'editorOverviewRuler.errorForeground':  cssHex('--error-color', fb.error),
        'editorOverviewRuler.warningForeground': cssHex('--warning-color', fb.warning),
        'editorOverviewRuler.infoForeground':   cssHex('--info-color', fb.info),
        'editorOverviewRuler.findMatchForeground': withAlpha(cssHex('--gold', fb.gold), 'CC'),
        'editorOverviewRuler.selectionHighlightForeground':
            withAlpha(cssHex('--text-sec', fb.textSec), '80'),
        'editorOverviewRuler.bracketMatchForeground': '#00000000',
    };
}

// The two editor themes. `fallback` is what minimapColors() uses when a CSS
// token does not resolve: the stylesheet's own value for that theme.
//
// NOTE: Monaco parses theme colors with Color.fromHex() and falls back to
// opaque RED on any parse failure -- rgba() strings are NOT supported here.
// Always use #RRGGBB or #RRGGBBAA.
//
// The editor ports -- the VS Code, Vim and Emacs packs -- carry both themes
// too. The token colors are written down once, in web/editor-palette.json;
// tests/check-editor-palette.py (ctest `tur_editor_palette`) fails when this
// block, that file, or an editor theme disagree. Change the JSON first.
const MONACO_THEMES = {
    dark: {
        base: 'vs-dark',
        rules: [
            // Dark Spice Market syntax colors — keywords (amber), types, kinds
            // and constraints (violet), modifiers (teal), builtins
            // (periwinkle), strings (coral), comments (dark italic), numbers
            // (sage green) per design-notes.md
            { token: 'comment',          foreground: '48433D', fontStyle: 'italic' },
            { token: 'string',           foreground: 'D9735A' },
            { token: 'number',           foreground: 'A8C98A' },
            { token: 'number.float',     foreground: 'A8C98A' },
            { token: 'keyword',          foreground: 'EFA030', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: '8AB0E8' },
            { token: 'annotation',       foreground: '7AC4B8' },
            { token: 'kind',             foreground: 'C4A0E8' },
            { token: 'type',             foreground: 'C4A0E8' },
            { token: 'operator',         foreground: '6A5F58' },
            { token: 'delimiter',        foreground: '5A5448' },
            { token: 'identifier',       foreground: 'EAE0D2' },
        ],
        colors: {
            // Editor canvas
            'editor.background':                    '#0C0A08',
            'editor.foreground':                    '#EAE0D2',
            'editorGutter.background':              '#0C0A08',
            'editorLineNumber.foreground':          '#4A4438',
            'editorLineNumber.activeForeground':    '#8A7D6E',

            // Cursor & selection -- neutral medium gray, never a warning color.
            // Selection/bracket-match/scrollbar are structural chrome; red is
            // reserved for diagnostics.
            'editorCursor.foreground':              '#D48B1C',
            'editor.selectionBackground':           '#5A544C59',
            'editor.inactiveSelectionBackground':   '#5A544C33',
            'editor.selectionHighlightBackground':  '#5A544C2E',
            'editor.wordHighlightBackground':       '#5A544C2E',
            'editor.wordHighlightStrongBackground': '#5A544C40',

            // Line highlight
            'editor.lineHighlightBackground':       '#111009',
            'editor.lineHighlightBorder':           '#00000000',

            // Indent guides
            'editorIndentGuide.background1':        '#222018',
            'editorIndentGuide.activeBackground1':  '#3D3A2F',

            // Bracket matching — medium gray, so a matched delimiter never
            // reads as an error highlight
            'editorBracketMatch.background':        '#6A625926',
            'editorBracketMatch.border':            '#8C847A99',

            // Find matches
            'editorFindMatch.background':           '#D48B1C47',
            'editorFindMatch.border':               '#D48B1CA6',
            'editorFindMatchHighlight.background':  '#D48B1C1F',

            // Autocomplete / hover / suggest widgets
            'editorWidget.background':                       '#161411',
            'editorWidget.border':                           '#2E2B22',
            'editorWidget.foreground':                       '#EAE0D2',
            'editorSuggestWidget.background':                '#161411',
            'editorSuggestWidget.border':                    '#2E2B22',
            'editorSuggestWidget.foreground':                '#EAE0D2',
            'editorSuggestWidget.selectedBackground':        '#D48B1C26',
            'editorSuggestWidget.selectedForeground':        '#EAE0D2',
            'editorSuggestWidget.highlightForeground':       '#EFA030',
            'editorHoverWidget.background':                  '#161411',
            'editorHoverWidget.border':                      '#2E2B22',
            'editorHoverWidget.foreground':                  '#EAE0D2',

            // Scrollbars — medium gray, brightening on hover/drag
            'scrollbar.shadow':                     '#00000000',
            'scrollbarSlider.background':           '#5A544C8C',
            'scrollbarSlider.hoverBackground':      '#7A736ABF',
            'scrollbarSlider.activeBackground':     '#8C847ACC',

            // Focus ring — gold instead of VS Code blue
            'focusBorder':                          '#D48B1C66',
        },
        fallback: {
            bgBase: '#0C0A08', textDim: '#4A4438', textSec: '#8A7D6E', gold: '#D48B1C',
            error: '#D9735A', warning: '#EFA030', info: '#7AC4B8', success: '#A8C98A',
        },
    },

    // Light Spice Market: the same roles on warm paper (web/vars.css,
    // :root[data-theme="light"]). Every syntax ink clears 4.5:1 on the canvas;
    // comments and delimiters are deliberately faint, as in the dark theme.
    light: {
        base: 'vs',
        rules: [
            { token: 'comment',          foreground: 'A39682', fontStyle: 'italic' },
            { token: 'string',           foreground: 'AE3C26' },
            { token: 'number',           foreground: '456F20' },
            { token: 'number.float',     foreground: '456F20' },
            { token: 'keyword',          foreground: '965500', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: '3B63B0' },
            { token: 'annotation',       foreground: '127065' },
            { token: 'kind',             foreground: '7748AE' },
            { token: 'type',             foreground: '7748AE' },
            { token: 'operator',         foreground: '8A7D6C' },
            { token: 'delimiter',        foreground: '9F927F' },
            { token: 'identifier',       foreground: '2A2016' },
        ],
        colors: {
            'editor.background':                    '#FBF8F2',
            'editor.foreground':                    '#2A2016',
            'editorGutter.background':              '#FBF8F2',
            'editorLineNumber.foreground':          '#978874',
            'editorLineNumber.activeForeground':    '#6A5B4B',

            // Same rule as dark: selection is a neutral wash, never a color
            // that could be mistaken for a diagnostic.
            'editorCursor.foreground':              '#945602',
            'editor.selectionBackground':           '#9C8B7438',
            'editor.inactiveSelectionBackground':   '#9C8B7420',
            'editor.selectionHighlightBackground':  '#9C8B741F',
            'editor.wordHighlightBackground':       '#9C8B741F',
            'editor.wordHighlightStrongBackground': '#9C8B742E',

            'editor.lineHighlightBackground':       '#F3EDE2',
            'editor.lineHighlightBorder':           '#00000000',

            'editorIndentGuide.background1':        '#E3D9C8',
            'editorIndentGuide.activeBackground1':  '#C0B099',

            'editorBracketMatch.background':        '#9C8B7422',
            'editorBracketMatch.border':            '#8C7C68A0',

            'editorFindMatch.background':           '#D9921F59',
            'editorFindMatch.border':               '#945602A6',
            'editorFindMatchHighlight.background':  '#D9921F2E',

            // Popups float ABOVE the paper, so they go lighter, not darker.
            'editorWidget.background':                       '#FFFDF9',
            'editorWidget.border':                           '#D5C8B3',
            'editorWidget.foreground':                       '#2A2016',
            'editorSuggestWidget.background':                '#FFFDF9',
            'editorSuggestWidget.border':                    '#D5C8B3',
            'editorSuggestWidget.foreground':                '#2A2016',
            'editorSuggestWidget.selectedBackground':        '#D9921F2E',
            'editorSuggestWidget.selectedForeground':        '#2A2016',
            'editorSuggestWidget.highlightForeground':       '#804900',
            'editorHoverWidget.background':                  '#FFFDF9',
            'editorHoverWidget.border':                      '#D5C8B3',
            'editorHoverWidget.foreground':                  '#2A2016',

            'scrollbar.shadow':                     '#00000000',
            'scrollbarSlider.background':           '#9C8B7466',
            'scrollbarSlider.hoverBackground':      '#8C7C6899',
            'scrollbarSlider.activeBackground':     '#7A6C5CB3',

            'focusBorder':                          '#94560266',
        },
        fallback: {
            bgBase: '#FBF8F2', textDim: '#978874', textSec: '#6A5B4B', gold: '#945602',
            error: '#AE3C26', warning: '#965500', info: '#127065', success: '#456F20',
        },
    },
};

/**
 * Define the Monaco theme for the current site theme and switch to it.
 *
 * setTheme is global, so every editor on the page (the buffer and the REPL
 * prompt) follows. Only the active theme is (re)defined: minimapColors() reads
 * the stylesheet as it is NOW, which is right for the theme on screen and
 * wrong for the other one.
 */
export function applyMonacoTheme() {
    const monaco = getMonaco();
    const theme = siteTheme();
    const def = MONACO_THEMES[theme];
    monaco.editor.defineTheme(monacoThemeName(), {
        base: def.base,
        inherit: true,
        rules: def.rules,
        colors: {
            ...def.colors,
            // Rainbow brackets — depth-colored, matching the trowel editor
            ...RAINBOW_BRACKETS[theme],
            // Minimap + overview ruler, resolved from the stylesheet
            ...minimapColors(def.fallback),
        },
    });
    monaco.editor.setTheme(monacoThemeName());
}
