// node bisect.js build/jit-tu.c -- smallest prefix (cut at a column-0 '}') c2mir rejects
const fs = require('fs'), path = require('path');
const M = require(path.resolve(__dirname, 'build/turmeric-jit.js'));
const lines = fs.readFileSync(process.argv[2], 'utf8').split('\n');
const cuts = []; lines.forEach((l, i) => { if (l === '}' || l.startsWith('} ')) cuts.push(i + 1); });
M({ print: () => {}, printErr: s => { if (/error|incompatible|repeated/.test(s)) lastErr = s; } }).then(mod => {
  const jc = mod.cwrap('turi_wasm_jit_c', 'number', ['string', 'number']);
  const fails = n => { lastErr = ''; const src = lines.slice(0, n).join('\n') + '\nint main(void){return 0;}\n'; const rc = jc(src, 0); return rc !== 0 && /incompatible/.test(lastErr); };
  let lo = 0, hi = cuts.length - 1, ans = -1;
  if (!fails(cuts[hi])) { console.log('full prefix does not fail with "incompatible"; last err:', lastErr); process.exit(1); }
  while (lo <= hi) { const mid = (lo + hi) >> 1; if (fails(cuts[mid])) { ans = cuts[mid]; hi = mid - 1; } else lo = mid + 1; }
  console.log('first failing cut at line', ans, ':', lastErr);
  const prev = cuts[cuts.indexOf(ans) - 1];
  console.log('--- the function between', prev, 'and', ans); console.log(lines.slice(prev, ans).join('\n'));
});
var lastErr = '';
