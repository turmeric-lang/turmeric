const fs = require('fs'), path = require('path');
const M = require(path.resolve(__dirname, 'build/turmeric-jit.js'));
const src = fs.readFileSync(process.argv[2], 'utf8');
M({ print: s => console.log('[out] ' + s), printErr: s => { if (!/warning|no end of line/.test(s)) console.log('[err] ' + s); } }).then(mod => {
  const jc = mod.cwrap('turi_wasm_jit_c', 'number', ['string', 'number']);
  for (let i = 0; i < (parseInt(process.argv[3] || '1')); i++) console.log('run', i, '-> rc', jc(src, 1));
});
