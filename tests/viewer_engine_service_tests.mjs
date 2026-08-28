import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
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
assert.equal(DEFAULT_ENGINE_DEPTH, 9);

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
const temporaryDirectory = await mkdtemp(path.join(os.tmpdir(), 'kugelfisch-viewer-'));
const gamesPath = path.join(temporaryDirectory, 'games.json');
const service = await startViewerServer({
  root: path.resolve(testsDirectory, '../viewer/dist'),
  enginePath,
  gamesPath,
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
  assert.match(page, /<h1>Kugelfisch<\/h1>/);
  assert.match(page, /Zylinderfolgeschach-Engine/);
  assert.doesNotMatch(page, /Rules|theme-toggle|The board wraps/);

  const markResponse = await fetch(`${service.url}/mark.svg`);
  assert.equal(markResponse.status, 200);
  assert.equal(markResponse.headers.get('content-type'), 'image/svg+xml');
  assert.match(await markResponse.text(), /<svg/);

  const playResponse = await fetch(`${service.url}/play`);
  assert.equal(playResponse.status, 200);
  const playPage = await playResponse.text();
  assert.match(playPage, /<h1>New game<\/h1>/);
  assert.match(playPage, /id="resign"/);
  assert.match(playPage, /value="9" selected>Depth 9/);
  assert.doesNotMatch(playPage, /engine-score|engine-pv|analysis-fen|ZFS-FEN/);
  assert.doesNotMatch(playPage, /Online|Offline|Connecting/);

  const workerResponse = await fetch(`${service.url}/engine-worker.js`);
  assert.equal(workerResponse.status, 200);
  assert.match(workerResponse.headers.get('content-type'), /^text\/javascript/);
  assert.match(await workerResponse.text(), /zfs_engine_search/);

  const gamesResponse = await fetch(`${service.url}/games/`);
  assert.equal(gamesResponse.status, 200);
  const gamesPage = await gamesResponse.text();
  assert.match(gamesPage, /No saved games/);
  assert.doesNotMatch(gamesPage, /Self-play archive/);

  const rulesResponse = await fetch(`${service.url}/rules`);
  assert.equal(rulesResponse.status, 404);

  const analysisResponse = await fetch(`${service.url}/analysis`);
  assert.equal(analysisResponse.status, 200);
  const analysisPage = await analysisResponse.text();
  assert.match(analysisPage, /analysis-score/);
  assert.match(analysisPage, /analysis-pv/);
  assert.match(analysisPage, /analysis-fen/);
  assert.match(analysisPage, /id="analysis-toggle" type="checkbox" checked/);
  assert.match(analysisPage, /value="9" selected>Depth 9/);
  assert.doesNotMatch(analysisPage, /Online|Offline|Connecting/);

  const initialGame = {
    rootFen:
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -',
    finalFen:
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 -',
    moves: [],
    humanColor: 'white',
    depth: 10,
    turn: 'white',
    terminal: 'ongoing',
    createdAt: 1_700_000_000_000,
  };
  const createGameResponse = await fetch(`${service.url}/api/games/test-game`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(initialGame),
  });
  assert.equal(createGameResponse.status, 200);
  const createdGame = (await createGameResponse.json()).game;
  assert.equal(createdGame.id, 'test-game');
  assert.equal(createdGame.status, 'active');
  assert.equal(createdGame.result, '*');

  const listGamesResponse = await fetch(`${service.url}/api/games?limit=1`);
  assert.equal(listGamesResponse.status, 200);
  const gamePage = await listGamesResponse.json();
  assert.equal(gamePage.games.length, 1);
  assert.equal(gamePage.games[0].id, 'test-game');
  assert.equal(gamePage.games[0].plies, 0);
  assert.equal(gamePage.nextCursor, null);

  const advancedGame = {
    ...initialGame,
    finalFen:
      'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1 e2',
    moves: ['e2e4'],
    turn: 'black',
  };
  const advanceGameResponse = await fetch(`${service.url}/api/games/test-game`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(advancedGame),
  });
  assert.equal(advanceGameResponse.status, 200);

  const savedGameResponse = await fetch(`${service.url}/api/games/test-game`);
  assert.equal(savedGameResponse.status, 200);
  assert.deepEqual((await savedGameResponse.json()).game.moves, ['e2e4']);

  const replayPageResponse = await fetch(`${service.url}/games/test-game`);
  assert.equal(replayPageResponse.status, 200);
  assert.match(await replayPageResponse.text(), /replay-board/);

  const conflictResponse = await fetch(`${service.url}/api/games/test-game`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(initialGame),
  });
  assert.equal(conflictResponse.status, 409);

  const resignationResponse = await fetch(`${service.url}/api/games/resigned-game`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      ...initialGame,
      humanColor: 'black',
      terminal: 'resignation',
    }),
  });
  assert.equal(resignationResponse.status, 200);
  const resignedGame = (await resignationResponse.json()).game;
  assert.equal(resignedGame.status, 'completed');
  assert.equal(resignedGame.result, '1-0');

  const persistedGames = JSON.parse(await readFile(gamesPath, 'utf8'));
  assert.equal(persistedGames.schema, 1);
  assert.equal(
    persistedGames.games.find((game) => game.id === 'test-game').moves[0],
    'e2e4',
  );

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

  const analysisSearchResponse = await fetch(`${service.url}/api/engine/analyze`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      gameId: 'analysis-search',
      rootFen: '7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3',
      moves: [],
      depth: 2,
    }),
  });
  assert.equal(analysisSearchResponse.status, 200);
  const analysisEvents = (await analysisSearchResponse.text())
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line));
  assert.ok(analysisEvents.some((event) => event.type === 'info'));
  assert.ok(!analysisEvents.some((event) => event.type === 'ponder'));
  assert.deepEqual(analysisEvents.at(-1), {
    type: 'bestmove',
    move: 'a1a3',
  });

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
  await rm(temporaryDirectory, { recursive: true });
}

console.log('viewer engine service tests passed');
