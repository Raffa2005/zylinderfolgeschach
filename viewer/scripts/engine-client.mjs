import { spawn } from 'node:child_process';

function integerAfter(tokens, name) {
  const index = tokens.indexOf(name);
  if (index < 0 || index + 1 >= tokens.length) return undefined;
  const value = Number.parseInt(tokens[index + 1], 10);
  return Number.isSafeInteger(value) ? value : undefined;
}

export function parseInfoLine(line) {
  const tokens = line.trim().split(/\s+/);
  if (tokens[0] !== 'info' || !tokens.includes('depth')) return null;

  const scoreIndex = tokens.indexOf('score');
  let score;
  if (scoreIndex >= 0 && scoreIndex + 2 < tokens.length) {
    const kind = tokens[scoreIndex + 1];
    const value = Number.parseInt(tokens[scoreIndex + 2], 10);
    if ((kind === 'cp' || kind === 'mate') && Number.isSafeInteger(value)) {
      score = { kind, value };
    }
  }

  const pvIndex = tokens.indexOf('pv');
  return {
    depth: integerAfter(tokens, 'depth'),
    seldepth: integerAfter(tokens, 'seldepth'),
    score,
    nodes: integerAfter(tokens, 'nodes'),
    nps: integerAfter(tokens, 'nps'),
    hashfull: integerAfter(tokens, 'hashfull'),
    time: integerAfter(tokens, 'time'),
    pv: pvIndex >= 0 ? tokens.slice(pvIndex + 1) : [],
  };
}

function asAbortError(reason, fallback = 'engine search aborted') {
  const error = reason instanceof Error ? reason : new Error(fallback);
  error.name = 'AbortError';
  return error;
}

class EngineProcessError extends Error {
  constructor(error) {
    super(error instanceof Error ? error.message : String(error), { cause: error });
    this.name = 'EngineProcessError';
  }
}

function processError(code, signal, stderr) {
  const reason = signal
    ? `engine was terminated by ${signal}`
    : `engine exited with code ${code}`;
  const detail = stderr.trim();
  return new EngineProcessError(
    new Error(detail ? `${reason}: ${detail}` : reason),
  );
}

export class UciEngineClient {
  constructor(enginePath) {
    this.enginePath = enginePath;
    this.child = null;
    this.currentGameId = null;
    this.currentSearch = null;
    this.startup = null;
    this.startupPromise = null;
    this.stderr = '';
    this.stdout = '';
    this.closed = false;
  }

  async start() {
    if (this.closed) throw new Error('engine client is closed');
    if (this.child) return this.startupPromise;

    const child = spawn(this.enginePath, [], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
    this.child = child;
    this.currentGameId = null;
    this.stderr = '';
    this.stdout = '';
    const startupPromise = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.child === child && this.startup) child.kill('SIGKILL');
      }, 5000);
      timer.unref();
      this.startup = { phase: 'uci', reject, resolve, timer };
    });
    this.startupPromise = startupPromise;

    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => this.handleStdout(child, chunk));
    child.stderr.on('data', (chunk) => {
      if (this.child === child && this.stderr.length < 8192) {
        this.stderr += chunk.slice(0, 8192 - this.stderr.length);
      }
    });
    child.on('error', (error) => this.handleFailure(child, error));
    child.stdin.on('error', (error) => this.handleFailure(child, error));
    child.on('exit', (code, signal) => {
      this.handleFailure(child, processError(code, signal, this.stderr));
    });

    try {
      this.write('uci\n');
    } catch (error) {
      this.handleFailure(child, error);
    }
    return startupPromise;
  }

  async search({
    gameId,
    rootFen,
    moves,
    depth,
    ponder = false,
    signal,
    onInfo = () => {},
  }) {
    const request = { gameId, rootFen, moves, depth, ponder, signal, onInfo };
    for (let attempt = 0; ; ++attempt) {
      try {
        return await this.searchOnce(request);
      } catch (error) {
        if (
          attempt !== 0 ||
          signal?.aborted ||
          !(error instanceof EngineProcessError)
        ) {
          throw error;
        }
      }
    }
  }

  async searchOnce({ gameId, rootFen, moves, depth, ponder, signal, onInfo }) {
    await this.start();
    if (signal?.aborted) throw asAbortError(signal.reason);
    if (this.currentSearch) throw new Error('engine search is already active');

    if (gameId !== this.currentGameId) {
      this.write('ucinewgame\n');
      this.currentGameId = gameId;
    }

    let resolveSearch;
    let rejectSearch;
    const result = new Promise((resolve, reject) => {
      resolveSearch = resolve;
      rejectSearch = reject;
    });
    const search = {
      abortReason: null,
      abortTimer: null,
      onInfo,
      ponder,
      reject: rejectSearch,
      resolve: resolveSearch,
      result,
      signal,
    };
    search.abort = () => this.interrupt(search, signal?.reason);
    signal?.addEventListener('abort', search.abort, { once: true });
    this.currentSearch = search;

    const position = `position zfsfen ${rootFen}` +
      (moves.length === 0 ? '' : ` moves ${moves.join(' ')}`);
    try {
      this.write(`${position}\ngo${ponder ? ' ponder' : ''} depth ${depth}\n`);
    } catch (error) {
      this.finishSearch(search, false, error);
    }
    return result;
  }

  ponderHit() {
    const search = this.currentSearch;
    if (!search?.ponder || search.abortReason) {
      throw new Error('engine has no active ponder search');
    }
    search.ponder = false;
    this.write('ponderhit\n');
  }

  async close() {
    if (this.closed) return;
    this.closed = true;

    const search = this.currentSearch;
    if (search) {
      this.interrupt(search, new Error('engine client is closing'));
      await search.result.catch(() => {});
    }

    const child = this.child;
    if (!child) return;
    const exited = new Promise((resolve) => child.once('exit', resolve));
    if (child.stdin.writable) child.stdin.end('quit\n');
    const killTimer = setTimeout(() => child.kill('SIGKILL'), 500);
    killTimer.unref();
    await exited;
    clearTimeout(killTimer);
  }

  write(command) {
    if (!this.child || !this.child.stdin.writable) {
      throw new Error('engine input is not writable');
    }
    this.child.stdin.write(command);
  }

  handleStdout(child, chunk) {
    if (this.child !== child) return;
    this.stdout += chunk;
    for (;;) {
      const newline = this.stdout.indexOf('\n');
      if (newline < 0) break;
      const line = this.stdout.slice(0, newline).trimEnd();
      this.stdout = this.stdout.slice(newline + 1);
      this.handleLine(line);
    }
  }

  handleLine(line) {
    if (this.startup) {
      if (this.startup.phase === 'uci' && line === 'uciok') {
        this.startup.phase = 'ready';
        try {
          this.write('isready\n');
        } catch (error) {
          if (this.child) this.handleFailure(this.child, error);
        }
      } else if (this.startup.phase === 'ready' && line === 'readyok') {
        const startup = this.startup;
        this.startup = null;
        clearTimeout(startup.timer);
        startup.resolve();
      }
      return;
    }

    const search = this.currentSearch;
    if (!search) return;
    const info = parseInfoLine(line);
    if (info && !search.abortReason) {
      try {
        search.onInfo(info);
      } catch (error) {
        this.interrupt(search, error);
        return;
      }
    }
    if (!line.startsWith('bestmove ')) return;

    if (search.abortReason) {
      this.finishSearch(search, false, search.abortReason);
      return;
    }
    this.finishSearch(search, true, { move: line.split(/\s+/)[1] });
  }

  interrupt(search, reason) {
    if (this.currentSearch !== search || search.abortReason) return;
    search.abortReason = asAbortError(reason);
    try {
      this.write('stop\n');
    } catch (error) {
      this.finishSearch(search, false, error);
      return;
    }
    search.abortTimer = setTimeout(() => {
      if (this.currentSearch === search) this.child?.kill('SIGKILL');
    }, 500);
    search.abortTimer.unref();
  }

  finishSearch(search, succeeded, value) {
    if (this.currentSearch !== search) return;
    this.currentSearch = null;
    search.signal?.removeEventListener('abort', search.abort);
    if (search.abortTimer) clearTimeout(search.abortTimer);
    if (succeeded) search.resolve(value);
    else search.reject(value);
  }

  handleFailure(child, error) {
    if (this.child !== child) return;
    const failure = error instanceof EngineProcessError
      ? error
      : new EngineProcessError(error);
    this.child = null;
    this.currentGameId = null;
    this.stdout = '';

    if (this.startup) {
      const startup = this.startup;
      this.startup = null;
      clearTimeout(startup.timer);
      startup.reject(failure);
    }
    this.startupPromise = null;

    const search = this.currentSearch;
    if (search) {
      this.finishSearch(search, false, search.abortReason ?? failure);
    }
  }
}
