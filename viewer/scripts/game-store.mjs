import { mkdir, readFile, rename, writeFile } from 'node:fs/promises';
import path from 'node:path';

const SCHEMA = 1;
const MAX_GAMES = 5000;

export class GameStore {
  static async open(filename) {
    const store = new GameStore(path.resolve(filename));
    await store.load();
    return store;
  }

  constructor(filename) {
    this.filename = filename;
    this.games = new Map();
    this.writeChain = Promise.resolve();
  }

  async load() {
    let source;
    try {
      source = await readFile(this.filename, 'utf8');
    } catch (error) {
      if (error?.code === 'ENOENT') return;
      throw error;
    }
    const data = JSON.parse(source);
    if (data?.schema !== SCHEMA || !Array.isArray(data.games)) {
      throw new Error('saved-game database has an unsupported schema');
    }
    if (data.games.length > MAX_GAMES) {
      throw new Error('saved-game database exceeds its record limit');
    }
    for (const game of data.games) {
      if (!game || typeof game.id !== 'string' || this.games.has(game.id)) {
        throw new Error('saved-game database contains an invalid record');
      }
      this.games.set(game.id, game);
    }
  }

  list(limit, cursor) {
    const games = [...this.games.values()].sort((left, right) =>
      right.updatedAt - left.updatedAt || right.id.localeCompare(left.id),
    );
    const start = cursor ?? 0;
    const page = games.slice(start, start + limit);
    const next = start + page.length;
    return {
      games: page.map((game) => ({
        id: game.id,
        createdAt: game.createdAt,
        updatedAt: game.updatedAt,
        status: game.status,
        result: game.result,
        termination: game.termination,
        white: game.white,
        black: game.black,
        humanColor: game.humanColor,
        depth: game.depth,
        plies: game.moves.length,
        finalFen: game.finalFen,
      })),
      nextCursor: next < games.length ? String(next) : null,
    };
  }

  get(id) {
    return this.games.get(id);
  }

  async put(game) {
    const operation = this.writeChain.then(async () => {
      const previous = this.games.get(game.id);
      if (previous) {
        const sameGame =
          previous.rootFen === game.rootFen &&
          previous.humanColor === game.humanColor &&
          previous.depth === game.depth &&
          previous.createdAt === game.createdAt;
        const extendsLine =
          previous.moves.length <= game.moves.length &&
          previous.moves.every((move, index) => move === game.moves[index]);
        const repeatsCompleted = sameGame && previous.status === 'completed' &&
          previous.finalFen === game.finalFen &&
          previous.turn === game.turn &&
          previous.termination === game.termination &&
          previous.moves.length === game.moves.length &&
          previous.moves.every((move, index) => move === game.moves[index]);
        if (repeatsCompleted) return previous;
        if (!sameGame || !extendsLine || previous.status === 'completed') {
          const error = new Error('saved game update conflicts with its record');
          error.code = 'GAME_CONFLICT';
          throw error;
        }
      } else if (this.games.size >= MAX_GAMES) {
        const error = new Error('saved-game database is full');
        error.code = 'GAME_LIMIT';
        throw error;
      }

      const nextGames = new Map(this.games);
      nextGames.set(game.id, game);
      await this.persist(nextGames);
      this.games = nextGames;
      return game;
    });
    this.writeChain = operation.catch(() => {});
    return operation;
  }

  async persist(games) {
    const directory = path.dirname(this.filename);
    await mkdir(directory, { recursive: true });
    const temporary = `${this.filename}.${process.pid}.tmp`;
    const data = {
      schema: SCHEMA,
      games: [...games.values()].sort((left, right) =>
        left.createdAt - right.createdAt || left.id.localeCompare(right.id),
      ),
    };
    await writeFile(temporary, `${JSON.stringify(data)}\n`, {
      encoding: 'utf8',
      mode: 0o600,
    });
    await rename(temporary, this.filename);
  }
}
