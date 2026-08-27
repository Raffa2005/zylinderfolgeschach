import fs from 'node:fs/promises';
import { createHash } from 'node:crypto';

import createZfsModule from '../viewer/src/generated/zfs.js';

const module = await createZfsModule();
const load = module.cwrap('zfs_load', 'number', ['string']);
const play = module.cwrap('zfs_play', 'number', ['string']);
const state = module.cwrap('zfs_state_json', 'string', []);

function requireCondition(condition, message) {
  if (!condition) throw new Error(message);
}

requireCondition(
  load(
    'rnbqkb1r/ppp1p2p/5p2/3p2p1/Q7/2P2N2/PP1PPPPP/' +
      'RNB1KB1R b KQkq - 1 7 g4',
  ) === 1,
  'generated WASM rejected antipodal-mate fixture',
);
const antipodalMate = JSON.parse(state());
requireCondition(
  antipodalMate.terminal === 'checkmate' && antipodalMate.legalMoves.length === 0,
  'generated WASM missed antipodal double-route mate',
);

requireCondition(
  load('4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -') === 1,
  'generated WASM rejected repetition fixture',
);
for (const move of [
  'g1f3',
  'e8e7',
  'f3g1',
  'e7e8',
  'g1f3',
  'e8e7',
  'f3g1',
  'e7e8',
]) {
  requireCondition(play(move) === 1, `generated WASM rejected ${move}`);
}
const repeated = JSON.parse(state());
requireCondition(repeated.terminal === 'threefold', 'generated WASM missed threefold');
requireCondition(repeated.legalMoves.length === 0, 'draw still exposes legal moves');

requireCondition(
  load('4k3/8/8/8/8/8/8/4K1N1 w - - 99 1 -') === 1,
  'generated WASM rejected fifty-move fixture',
);
requireCondition(play('g1f3') === 1, 'generated WASM rejected quiet move');
requireCondition(JSON.parse(state()).terminal === 'fifty-move',
                 'generated WASM missed fifty-move draw');

const generated = await fs.readFile(
  new URL('../viewer/src/generated/zfs.js', import.meta.url),
);
const distribution = await fs.readFile(
  new URL('../viewer/dist/app.js', import.meta.url),
  'utf8',
);
const generatedHash = createHash('sha256').update(generated).digest('hex');
requireCondition(
  distribution.includes(`zfs-wasm-sha256:${generatedHash}`),
  'distribution embeds a stale generated WASM module',
);
requireCondition(distribution.includes('threefold'),
                 'distribution lacks threefold rendering');
requireCondition(distribution.includes('fifty-move'),
                 'distribution lacks fifty-move rendering');

console.log('generated viewer runtime passed');
