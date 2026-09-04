function abortError(reason = 'engine search cancelled') {
  if (reason?.name === 'AbortError') return reason;
  const error = new Error(reason instanceof Error ? reason.message : String(reason));
  error.name = 'AbortError';
  return error;
}

export class ClientEngine {
  constructor() {
    this.generation = 0;
    this.nextId = 0;
    this.pending = null;
    this.worker = null;
    this.spawn();
  }

  spawn() {
    const generation = ++this.generation;
    const worker = new Worker('/engine-worker.js', { type: 'module' });
    this.worker = worker;
    this.isReady = false;
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    worker.addEventListener('message', ({ data }) => {
      if (generation !== this.generation) return;
      if (data?.type === 'ready') {
        this.isReady = true;
        this.resolveReady();
        return;
      }
      const pending = this.pending;
      if (!pending || data?.id !== pending.id) return;
      if (data.type === 'info') {
        pending.latestInfo = data.info;
        pending.onInfo(data.info);
      } else if (data.type === 'result') {
        this.finish(pending, data.result);
      } else if (data.type === 'failure') {
        this.fail(pending, new Error(data.error || 'engine worker failed'));
      }
    });
    worker.addEventListener('error', (event) => {
      if (generation !== this.generation) return;
      const error = new Error(event.message || 'engine worker failed to load');
      const wasReady = this.isReady;
      if (!wasReady) this.rejectReady(error);
      if (this.pending) this.fail(this.pending, error);
      worker.terminate();
      if (wasReady) this.spawn();
    });
  }

  async start() {
    await this.ready;
  }

  async newGame() {
    if (this.pending) this.cancel('new game started');
    await this.ready;
    this.worker.postMessage({ type: 'new-game' });
  }

  async search({ rootFen, moves, depth, signal, onInfo = () => {} }) {
    await this.ready;
    if (signal?.aborted) throw abortError(signal.reason);
    if (this.pending) throw new Error('engine search is already active');

    let resolve;
    let reject;
    const result = new Promise((accept, decline) => {
      resolve = accept;
      reject = decline;
    });
    const pending = {
      id: ++this.nextId,
      latestInfo: undefined,
      onInfo,
      reject,
      resolve,
      signal,
    };
    pending.abort = () => this.cancel(signal?.reason);
    signal?.addEventListener('abort', pending.abort, { once: true });
    this.pending = pending;
    this.worker.postMessage({
      type: 'search', id: pending.id, rootFen, moves, depth,
    });
    return result;
  }

  cancel(reason) {
    const pending = this.pending;
    const error = abortError(reason);
    if (pending) this.fail(pending, error);
    this.worker?.terminate();
    this.spawn();
  }

  close() {
    if (this.pending) this.fail(this.pending, abortError('engine closed'));
    ++this.generation;
    this.worker?.terminate();
    this.worker = null;
    this.isReady = false;
  }

  finish(pending, result) {
    if (this.pending !== pending) return;
    this.clear(pending);
    if (!result?.ok) {
      pending.reject(new Error(result?.error || 'engine search failed'));
      return;
    }
    pending.resolve({
      move: result.move,
      info: result.info ?? pending.latestInfo,
    });
  }

  fail(pending, error) {
    if (this.pending !== pending) return;
    this.clear(pending);
    pending.reject(error);
  }

  clear(pending) {
    pending.signal?.removeEventListener('abort', pending.abort);
    this.pending = null;
  }
}

export class PlayingEngine {
  constructor() {
    this.client = new ClientEngine();
    this.gameId = null;
    this.ponder = null;
  }

  async start() {
    await this.client.start();
  }

  async newGame(gameId) {
    await this.stopPonder();
    this.gameId = gameId;
    await this.client.newGame();
  }

  async search({ gameId, rootFen, moves, depth, signal, onInfo = () => {} }) {
    if (gameId !== this.gameId) await this.newGame(gameId);

    const ponder = this.ponder;
    this.ponder = null;
    const matches = ponder && ponder.rootFen === rootFen &&
      ponder.depth === depth && ponder.moves.length === moves.length &&
      ponder.moves.every((move, index) => move === moves[index]);
    let result;
    if (matches) {
      ponder.onInfo = onInfo;
      if (ponder.latestInfo) onInfo(ponder.latestInfo);
      const abort = () => ponder.controller.abort(signal?.reason);
      signal?.addEventListener('abort', abort, { once: true });
      if (signal?.aborted) abort();
      const outcome = await ponder.outcome;
      signal?.removeEventListener('abort', abort);
      if (outcome.error) throw outcome.error;
      result = outcome.result;
    } else {
      if (ponder) await this.cancelPonder(ponder);
      result = await this.client.search({ rootFen, moves, depth, signal, onInfo });
    }

    const pondering = this.beginPonder({ gameId, rootFen, moves, depth, result });
    return { ...result, pondering };
  }

  async stopPonder() {
    const ponder = this.ponder;
    this.ponder = null;
    if (ponder) await this.cancelPonder(ponder);
  }

  close() {
    this.ponder?.controller.abort(new Error('engine closed'));
    this.ponder = null;
    this.client.close();
  }

  beginPonder({ gameId, rootFen, moves, depth, result }) {
    const pv = result.info?.pv;
    const predictedMove = pv?.[0] === result.move ? pv[1] : undefined;
    if (typeof predictedMove !== 'string') return false;

    const controller = new AbortController();
    const ponder = {
      controller,
      depth,
      latestInfo: undefined,
      moves: [...moves, result.move, predictedMove],
      onInfo: null,
      outcome: null,
      rootFen,
    };
    ponder.outcome = this.client.search({
      rootFen,
      moves: ponder.moves,
      depth,
      signal: controller.signal,
      onInfo: (info) => {
        ponder.latestInfo = info;
        ponder.onInfo?.(info);
      },
    }).then(
      (next) => ({ result: next }),
      (error) => ({ error }),
    );
    this.ponder = ponder;
    return true;
  }

  async cancelPonder(ponder) {
    ponder.controller.abort(new Error('ponder prediction was not played'));
    await ponder.outcome;
  }
}
