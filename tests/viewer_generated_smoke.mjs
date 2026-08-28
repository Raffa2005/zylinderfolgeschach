import fs from 'node:fs/promises';
import { createHash } from 'node:crypto';

import createZfsModule from '../viewer/src/generated/zfs.js';
import createKugelfischEngine from '../viewer/src/generated/kugelfisch-engine.js';

const module = await createZfsModule();
const reset = module.cwrap('zfs_reset', null, []);
const load = module.cwrap('zfs_load', 'number', ['string']);
const play = module.cwrap('zfs_play', 'number', ['string']);
const lineSan = module.cwrap('zfs_line_san', 'string', ['string']);
const state = module.cwrap('zfs_state_json', 'string', []);

function requireCondition(condition, message) {
  if (!condition) throw new Error(message);
}

reset();
requireCondition(lineSan('e2e4 e7e5 g1f3') === 'e4 e5 Nf3',
                 'generated WASM returned incorrect SAN');

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

const engineModule = await createKugelfischEngine();
const engineSearch = engineModule.cwrap(
  'zfs_engine_search', 'string', ['string', 'string', 'number'],
);
const engineNewGame = engineModule.cwrap('zfs_engine_new_game', null, []);
const forcedSearch = JSON.parse(engineSearch(
  '7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3', '', 2,
));
requireCondition(
  forcedSearch.ok === true && forcedSearch.move === 'a1a3' &&
    forcedSearch.info.depth === 2 && forcedSearch.info.pv[0] === 'a1a3',
  'generated search WASM failed its forced-follow search',
);

engineNewGame();
const provenMate = JSON.parse(engineSearch(
  '8/8/8/8/8/K7/2Q5/k7 w - - 0 1 -', '', 10,
));
requireCondition(
  provenMate.ok === true && provenMate.move === 'c2c1' &&
    provenMate.info.score.kind === 'mate' && provenMate.info.depth === 1,
  'generated search WASM continued beyond a proven root mate',
);

const archive = JSON.parse(await fs.readFile(
  new URL('../viewer/src/selfplay-games.json', import.meta.url),
  'utf8',
));
requireCondition(archive.schema === 1, 'unsupported self-play archive schema');
requireCondition(archive.games.length === archive.summary.games,
                 'self-play archive summary has the wrong game count');
const terminations = new Map();
for (const game of archive.games) {
  reset();
  requireCondition(game.moves.length === game.plies,
                   `${game.id} has the wrong recorded ply count`);
  for (const move of game.moves) {
    requireCondition(play(move) === 1,
                     `${game.id} contains illegal archive move ${move}`);
  }
  const final = JSON.parse(state());
  requireCondition(final.historyCursor === game.plies,
                   `${game.id} did not replay to its final ply`);
  const expectedTerminal = game.termination === 'ply-cap'
    ? 'ongoing'
    : game.termination;
  requireCondition(final.terminal === expectedTerminal,
                   `${game.id} ended as ${final.terminal}, expected ${expectedTerminal}`);
  if (game.termination === 'checkmate') {
    const loser = game.result === '1-0' ? 'black' : 'white';
    requireCondition(final.turn === loser,
                     `${game.id} checkmated the wrong recorded color`);
  } else {
    requireCondition(game.result === '1/2-1/2',
                     `${game.id} has a decisive non-checkmate result`);
  }
  terminations.set(
    game.termination,
    (terminations.get(game.termination) ?? 0) + 1,
  );
}
for (const [termination, count] of Object.entries(archive.summary.terminations)) {
  requireCondition(terminations.get(termination) === count,
                   `self-play archive miscounts ${termination}`);
}

const generated = await fs.readFile(
  new URL('../viewer/src/generated/zfs.js', import.meta.url),
);
const distribution = await fs.readFile(
  new URL('../viewer/dist/app.js', import.meta.url),
  'utf8',
);
const engineGenerated = await fs.readFile(
  new URL('../viewer/src/generated/kugelfisch-engine.js', import.meta.url),
);
const engineDistribution = await fs.readFile(
  new URL('../viewer/dist/engine-worker.js', import.meta.url),
  'utf8',
);
const cloudflareRoutes = JSON.parse(await fs.readFile(
  new URL('../viewer/dist/_routes.json', import.meta.url),
  'utf8',
));
const cloudflareRedirects = await fs.readFile(
  new URL('../viewer/dist/_redirects', import.meta.url),
  'utf8',
);
const playPage = await fs.readFile(
  new URL('../viewer/dist/play/index.html', import.meta.url),
  'utf8',
);
const analysisPage = await fs.readFile(
  new URL('../viewer/dist/analysis/index.html', import.meta.url),
  'utf8',
);
const generatedHash = createHash('sha256').update(generated).digest('hex');
const engineHash = createHash('sha256').update(engineGenerated).digest('hex');
requireCondition(
  distribution.includes(`zfs-wasm-sha256:${generatedHash}`),
  'distribution embeds a stale generated WASM module',
);
requireCondition(
  engineDistribution.includes(`kugelfisch-engine-wasm-sha256:${engineHash}`),
  'distribution embeds a stale search WASM module',
);
requireCondition(distribution.includes('threefold'),
                 'distribution lacks threefold rendering');
requireCondition(distribution.includes('fifty-move'),
                 'distribution lacks fifty-move rendering');
requireCondition(distribution.includes('resignation'),
                 'distribution lacks resignation handling');
requireCondition(distribution.includes('/api/games'),
                 'distribution lacks saved-game support');
requireCondition(distribution.includes('/engine-worker.js'),
                 'distribution lacks the client engine worker');
requireCondition(!distribution.includes('/api/engine/'),
                 'distribution still depends on the server engine');
requireCondition(!distribution.includes('scrollIntoView'),
                 'distribution can still scroll the page when history changes');
requireCondition(
  /id="engine-depth"[^>]*>.*value="9" selected/.test(playPage) &&
    /id="analysis-depth"[^>]*>.*value="9" selected/.test(analysisPage),
  'fresh browser sessions do not default to depth 9',
);
requireCondition(
  cloudflareRedirects.trim() ===
    '/games/:id/ /games/ 200\n/games/:id /games/ 200',
  'Cloudflare does not rewrite saved-game detail routes',
);
requireCondition(
  cloudflareRoutes.version === 1 &&
    cloudflareRoutes.include.length === 1 &&
    cloudflareRoutes.include[0] === '/api/*',
  'Cloudflare routes invoke Functions outside the saved-game API',
);

console.log('generated viewer runtime passed');
