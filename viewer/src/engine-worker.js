import createKugelfischEngine from './generated/kugelfisch-engine.js';

const module = await createKugelfischEngine();
const newGame = module.cwrap('zfs_engine_new_game', null, []);
const search = module.cwrap(
  'zfs_engine_search', 'string', ['string', 'string', 'number'],
);

self.addEventListener('message', ({ data }) => {
  if (data?.type === 'new-game') {
    newGame();
    return;
  }
  if (data?.type !== 'search') return;

  self.kugelfischSearchId = data.id;
  try {
    const result = JSON.parse(search(
      data.rootFen,
      data.moves.join(' '),
      data.depth,
    ));
    self.postMessage({ type: 'result', id: data.id, result });
  } catch (error) {
    self.postMessage({
      type: 'failure',
      id: data.id,
      error: error instanceof Error ? error.message : String(error),
    });
  } finally {
    self.kugelfischSearchId = undefined;
  }
});

self.postMessage({ type: 'ready' });
