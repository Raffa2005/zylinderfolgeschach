import { mkdir, readFile, rename, writeFile } from 'node:fs/promises';
import path from 'node:path';

import {
  classifySavedGameUpdate,
  MAX_SAVED_GAMES,
  SavedGameError,
  savedGameSummary,
} from '../shared/saved-games.mjs';

const SCHEMA = 1;

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
    if (data.games.length > MAX_SAVED_GAMES) {
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
      games: page.map(savedGameSummary),
      nextCursor: next < games.length ? String(next) : null,
    };
  }

  get(id) {
    return this.games.get(id);
  }

  async put(game) {
    const operation = this.writeChain.then(async () => {
      const previous = this.games.get(game.id);
      const updateKind = classifySavedGameUpdate(previous, game);
      if (updateKind === 'unchanged') return previous;
      if (updateKind === 'insert' && this.games.size >= MAX_SAVED_GAMES) {
        throw new SavedGameError(507, 'saved-game database is full');
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
