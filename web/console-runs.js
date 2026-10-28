// console-runs.js -- the console transcript as data (security-audit-plan W-2).
//
// The transcript is stored as DATA and rebuilt with createElement/textContent,
// so nothing read back out of localStorage is ever parsed as markup. It used
// to be the lines' HTML, re-inserted with insertAdjacentHTML on load -- which
// made any one-time injection into the console a permanent one.
//
// A line is a list of runs. A run is a string (text), or [tag, className, runs]
// for a tag in CONSOLE_TAGS; `br` carries no runs. Anything else in the markup
// appendToConsole was handed (a link in an error message, say) is kept as its
// text. Attributes other than class are not kept, which is why the flush <pre>
// blocks are a class (console-flush) rather than an inline style.

const CONSOLE_TAGS = new Set(['span', 'pre', 'strong', 'em', 'code', 'br']);

// Deeper than anything appendToConsole writes (two levels today); what the
// renderer does with a record nested past it is flatten it to text.
const CONSOLE_MAX_DEPTH = 8;

/** The runs of a parsed console line. */
export function consoleRuns(nodes) {
    const runs = [];
    for (const n of nodes) {
        if (n.nodeType === Node.TEXT_NODE) {
            runs.push(n.data);
        } else if (n.nodeType === Node.ELEMENT_NODE) {
            const tag = n.localName;
            if (tag === 'br') runs.push(['br', '', []]);
            else if (CONSOLE_TAGS.has(tag)) runs.push([tag, n.className, consoleRuns(n.childNodes)]);
            else runs.push(n.textContent);
        }
    }
    return runs;
}

/** Text of a run list, for a record nested too deep to rebuild. */
function consoleRunsText(runs) {
    if (typeof runs === 'string') return runs;
    if (!Array.isArray(runs)) return '';
    return runs.map((r) => (typeof r === 'string' ? r
        : Array.isArray(r) ? consoleRunsText(r[2]) : '')).join('');
}

/**
 * Append a stored run list under `parent`. The record came out of storage, so
 * it is validated as it is walked rather than trusted: an unknown tag or a
 * malformed run contributes nothing, and nothing is ever parsed as HTML.
 */
export function renderConsoleRuns(runs, parent, depth = 0) {
    if (!Array.isArray(runs)) return;
    for (const r of runs) {
        if (typeof r === 'string') {
            parent.appendChild(document.createTextNode(r));
        } else if (Array.isArray(r) && typeof r[0] === 'string' && CONSOLE_TAGS.has(r[0])) {
            if (depth >= CONSOLE_MAX_DEPTH) {
                parent.appendChild(document.createTextNode(consoleRunsText(r[2])));
                continue;
            }
            const el = document.createElement(r[0]);
            if (typeof r[1] === 'string' && r[1]) el.className = r[1];
            if (r[0] !== 'br') renderConsoleRuns(r[2], el, depth + 1);
            parent.appendChild(el);
        }
    }
}
