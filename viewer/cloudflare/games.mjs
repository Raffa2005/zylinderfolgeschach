import {
  classifySavedGameUpdate,
  MAX_SAVED_GAMES,
  parseSavedGamePage,
  SavedGameError,
  savedGameSummary,
  validateSavedGame,
  validateSavedGameId,
} from '../shared/saved-games.mjs';

const MAX_BODY_BYTES = 64 * 1024;
const GAME_COLUMNS = `
  id,
  root_fen AS rootFen,
  final_fen AS finalFen,
  moves,
  human_color AS humanColor,
  depth,
  turn,
  status,
  result,
  termination,
  white,
  black,
  created_at AS createdAt,
  updated_at AS updatedAt,
  revision
`;

function json(status, value, extraHeaders = {}) {
  return Response.json(value, {
    status,
    headers: {
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
      ...extraHeaders,
    },
  });
}

function methodNotAllowed(allow) {
  return json(405, { error: 'method not allowed' }, { Allow: allow });
}

async function readJson(request) {
  if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get('content-type') ?? '')) {
    throw new SavedGameError(415, 'content type must be application/json');
  }
  const declaredLength = request.headers.get('content-length');
  if (declaredLength !== null && /^\d+$/.test(declaredLength) &&
      Number(declaredLength) > MAX_BODY_BYTES) {
    throw new SavedGameError(413, 'request is too large');
  }
  if (!request.body) throw new SavedGameError(400, 'request body must be valid JSON');

  const reader = request.body.getReader();
  const decoder = new TextDecoder();
  let length = 0;
  let source = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    length += value.byteLength;
    if (length > MAX_BODY_BYTES) {
      await reader.cancel();
      throw new SavedGameError(413, 'request is too large');
    }
    source += decoder.decode(value, { stream: true });
  }
  source += decoder.decode();
  try {
    return JSON.parse(source);
  } catch {
    throw new SavedGameError(400, 'request body must be valid JSON');
  }
}

function gameFromRow(row) {
  let moves;
  try {
    moves = JSON.parse(row.moves);
  } catch {
    throw new Error(`saved game ${row.id} has invalid move storage`);
  }
  if (!Array.isArray(moves)) {
    throw new Error(`saved game ${row.id} has invalid move storage`);
  }
  return {
    id: row.id,
    rootFen: row.rootFen,
    finalFen: row.finalFen,
    moves,
    humanColor: row.humanColor,
    depth: row.depth,
    turn: row.turn,
    status: row.status,
    result: row.result,
    termination: row.termination,
    white: row.white,
    black: row.black,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
  };
}

function gameValues(game) {
  return [
    game.id,
    game.rootFen,
    game.finalFen,
    JSON.stringify(game.moves),
    game.humanColor,
    game.depth,
    game.turn,
    game.status,
    game.result,
    game.termination,
    game.white,
    game.black,
    game.createdAt,
    game.updatedAt,
  ];
}

async function selectGame(database, id) {
  return database.prepare(
    `SELECT ${GAME_COLUMNS} FROM games WHERE id = ?1`,
  ).bind(id).first();
}

async function insertGame(database, game) {
  return database.prepare(`
    INSERT INTO games (
      id, root_fen, final_fen, moves, human_color, depth, turn, status,
      result, termination, white, black, created_at, updated_at
    ) VALUES (
      ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14
    )
  `).bind(...gameValues(game)).run();
}

async function updateGame(database, game, revision) {
  return database.prepare(`
    UPDATE games SET
      root_fen = ?2,
      final_fen = ?3,
      moves = ?4,
      human_color = ?5,
      depth = ?6,
      turn = ?7,
      status = ?8,
      result = ?9,
      termination = ?10,
      white = ?11,
      black = ?12,
      created_at = ?13,
      updated_at = ?14,
      revision = revision + 1
    WHERE id = ?1 AND revision = ?15
  `).bind(...gameValues(game), revision).run();
}

async function saveGame(database, game) {
  for (let attempt = 0; attempt < 2; ++attempt) {
    const previousRow = await selectGame(database, game.id);
    const previous = previousRow ? gameFromRow(previousRow) : undefined;
    const operation = classifySavedGameUpdate(previous, game);
    if (operation === 'unchanged') return previous;

    if (operation === 'insert') {
      const count = await database.prepare(
        'SELECT COUNT(*) AS count FROM games',
      ).first('count');
      if (count >= MAX_SAVED_GAMES) {
        throw new SavedGameError(507, 'saved-game database is full');
      }
      try {
        await insertGame(database, game);
        return game;
      } catch (error) {
        if (!await selectGame(database, game.id)) throw error;
      }
    } else {
      const result = await updateGame(database, game, previousRow.revision);
      if ((result.meta?.changes ?? 0) === 1) return game;
    }
  }
  throw new SavedGameError(409, 'saved game update conflicts with its record');
}

async function listGames(database, url) {
  const { cursor, limit } = parseSavedGamePage(url);
  const page = await database.prepare(`
    SELECT ${GAME_COLUMNS}
    FROM games
    ORDER BY updated_at DESC, id DESC
    LIMIT ?1 OFFSET ?2
  `).bind(limit + 1, cursor).all();
  const rows = page.results ?? [];
  const games = rows.slice(0, limit).map(gameFromRow);
  return {
    games: games.map(savedGameSummary),
    nextCursor: rows.length > limit ? String(cursor + limit) : null,
  };
}

function databaseFrom(context) {
  if (!context.env?.DB) {
    throw new SavedGameError(503, 'saved-game database is unavailable');
  }
  return context.env.DB;
}

export async function handleGameList(context) {
  if (context.request.method !== 'GET') return methodNotAllowed('GET');
  try {
    return json(200, await listGames(
      databaseFrom(context),
      new URL(context.request.url),
    ));
  } catch (error) {
    return handleError(error);
  }
}

export async function handleGame(context) {
  try {
    const id = validateSavedGameId(context.params.id);
    const database = databaseFrom(context);
    if (context.request.method === 'GET') {
      const row = await selectGame(database, id);
      if (!row) throw new SavedGameError(404, 'saved game does not exist');
      return json(200, { game: gameFromRow(row) });
    }
    if (context.request.method !== 'PUT') return methodNotAllowed('GET, PUT');
    const game = validateSavedGame(id, await readJson(context.request));
    return json(200, { game: await saveGame(database, game) });
  } catch (error) {
    return handleError(error);
  }
}

function handleError(error) {
  if (error instanceof SavedGameError) {
    return json(error.status, { error: error.message });
  }
  console.error('saved-game request failed', error);
  return json(500, { error: 'saved-game request failed' });
}
