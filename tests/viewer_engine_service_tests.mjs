import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  parseInfoLine,
  UciEngineClient,
} from '../viewer/scripts/engine-client.mjs';
import {
  DEFAULT_ENGINE_DEPTH,
  startViewerServer,
} from '../viewer/scripts/server-lib.mjs';

const enginePath = process.argv[2];
assert.ok(enginePath, 'expected the zfs_engine path as argv[2]');

assert.deepEqual(
  parseInfoLine(
    'info depth 7 seldepth 11 score cp -42 nodes 1234 nps 9999 ' +
      'hashfull 12 time 8 pv e2e4 a7a5',
  ),
  {
    depth: 7,
    seldepth: 11,
    score: { kind: 'cp', value: -42 },
    nodes: 1234,
    nps: 9999,
    hashfull: 12,
    time: 8,
    pv: ['e2e4', 'a7a5'],
  },
);
assert.equal(parseInfoLine('id name Kugelfisch'), null);

const recoveringEngine = new UciEngineClient(enginePath);
let killedEngine = false;
try {
  const recovered = await recoveringEngine.search({
    gameId: 'process-recovery',
    rootFen:
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -',
    moves: [],
    depth: 8,
    onInfo: () => {
      if (killedEngine) return;
      killedEngine = true;
      recoveringEngine.child.kill('SIGKILL');
    },
  });
  assert.equal(killedEngine, true);
  assert.match(recovered.move, /^[a-h][1-8][a-h][1-8][qrbn]?$/);
} finally {
  await recoveringEngine.close();
}

const testsDirectory = path.dirname(fileURLToPath(import.meta.url));
const service = await startViewerServer({
  root: path.resolve(testsDirectory, '../viewer/dist'),
  enginePath,
  host: '127.0.0.1',
  port: 0,
  quiet: true,
});

try {
  const statusResponse = await fetch(`${service.url}/api/engine/status`);
  assert.equal(statusResponse.status, 200);
  assert.deepEqual(await statusResponse.json(), {
    available: true,
    defaultDepth: DEFAULT_ENGINE_DEPTH,
  });

  const pageResponse = await fetch(`${service.url}/`);
  assert.equal(pageResponse.status, 200);
  const page = await pageResponse.text();
  assert.match(page, /The board wraps/);
  assert.doesNotMatch(page, /Edge-case presets/);

  const playResponse = await fetch(`${service.url}/play`);
  assert.equal(playResponse.status, 200);
  assert.match(await playResponse.text(), /Play against Kugelfisch/);

  const gamesResponse = await fetch(`${service.url}/games/`);
  assert.equal(gamesResponse.status, 200);
  assert.match(await gamesResponse.text(), /Self-play archive/);

  const rulesResponse = await fetch(`${service.url}/rules`);
  assert.equal(rulesResponse.status, 200);
  assert.match(await rulesResponse.text(), /Follow when it is legal/);

  const invalidResponse = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'invalid-depth',
      rootFen: '7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3',
      moves: [],
      depth: 0,
    }),
  });
  assert.equal(invalidResponse.status, 400);

  const unsafeContentType = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: '{}',
  });
  assert.equal(unsafeContentType.status, 415);

  const unsafeStopContentType = await fetch(`${service.url}/api/engine/stop`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: '{}',
  });
  assert.equal(unsafeStopContentType.status, 415);

  const searchResponse = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'forced-move',
      rootFen: '7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3',
      moves: [],
      depth: 2,
    }),
  });
  assert.equal(searchResponse.status, 200);
  const events = (await searchResponse.text())
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line));
  assert.ok(events.some((event) => event.type === 'info' && event.depth === 2));
  assert.deepEqual(events.at(-1), { type: 'bestmove', move: 'a1a3' });
  const ponderEvent = events.find((event) => event.type === 'ponder');
  assert.match(ponderEvent?.move ?? '', /^[a-h][1-8][a-h][1-8][qrbn]?$/);

  const ponderHitResponse = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'forced-move',
      rootFen: '7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3',
      moves: ['a1a3', ponderEvent.move],
      depth: 2,
    }),
  });
  const ponderHitEvents = (await ponderHitResponse.text())
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line));
  assert.ok(ponderHitEvents.some((event) => event.type === 'ponderhit'));
  assert.ok(ponderHitEvents.some((event) => event.type === 'ponder'));
  assert.equal(ponderHitEvents.at(-1).type, 'bestmove');

  const stopResponse = await fetch(`${service.url}/api/engine/stop`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ gameId: 'forced-move' }),
  });
  assert.deepEqual(await stopResponse.json(), { stopped: true });

  const repetitionResponse = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'repetition-history',
      rootFen: '4k3/8/8/8/8/8/8/4K1N1 w - - 0 1 -',
      moves: [
        'g1f3', 'e8e7', 'f3g1', 'e7e8',
        'g1f3', 'e8e7', 'f3g1', 'e7e8',
      ],
      depth: 2,
    }),
  });
  const repetitionEvents = (await repetitionResponse.text())
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line));
  assert.deepEqual(repetitionEvents.at(-1), {
    type: 'bestmove',
    move: '0000',
  });

  const slowResponse = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'slow-search',
      rootFen:
        'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -',
      moves: [],
      depth: 100,
    }),
  });
  const interruptedEvents = slowResponse.text().then((body) =>
    body.trim().split('\n').map((line) => JSON.parse(line)),
  );
  const replacementResponse = await fetch(`${service.url}/api/engine/move`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'replacement-search',
      rootFen: '7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3',
      moves: [],
      depth: 1,
    }),
  });
  const replacementEvents = (await replacementResponse.text())
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line));
  assert.deepEqual(replacementEvents.at(-1), {
    type: 'bestmove',
    move: 'a1a3',
  });
  assert.ok(
    (await interruptedEvents).some((event) =>
      event.type === 'error' && /superseded/.test(event.message),
    ),
  );
} finally {
  await service.close();
}

console.log('viewer engine service tests passed');
