export const MAX_SAVED_GAMES = 5000;
export const SAVED_GAME_ID_PATTERN = /^[A-Za-z0-9._-]{1,128}$/;

const MOVE_PATTERN = /^[a-h][1-8][a-h][1-8][qrbn]?$/;
const TERMINATIONS = new Set([
  'ongoing',
  'checkmate',
  'stalemate',
  'threefold',
  'fifty-move',
  'resignation',
]);

export class SavedGameError extends Error {
  constructor(status, message) {
    super(message);
    this.name = 'SavedGameError';
    this.status = status;
  }
}

function badRequest(message) {
  throw new SavedGameError(400, message);
}

function boundedFen(value, name) {
  if (
    typeof value !== 'string' ||
    value.length === 0 ||
    value.length > 256 ||
    /[\r\n]/.test(value)
  ) {
    badRequest(`${name} must be one bounded FEN line`);
  }
  const normalized = value.trim();
  const fields = normalized.split(/\s+/).length;
  if (fields !== 6 && fields !== 7) {
    badRequest(`${name} must contain six or seven fields`);
  }
  return normalized;
}

export function validateSavedGameId(id) {
  if (typeof id !== 'string' || !SAVED_GAME_ID_PATTERN.test(id)) {
    badRequest('saved-game id is invalid');
  }
  return id;
}

export function validateSavedGame(id, body, updatedAt = Date.now()) {
  validateSavedGameId(id);
  if (body === null || typeof body !== 'object' || Array.isArray(body)) {
    badRequest('saved game must be an object');
  }
  const humanColor = body.humanColor;
  if (humanColor !== 'white' && humanColor !== 'black') {
    badRequest('humanColor must be white or black');
  }
  if (!Number.isInteger(body.depth) || body.depth < 1 || body.depth > 100) {
    badRequest('depth must be an integer from 1 to 100');
  }
  if (!Number.isSafeInteger(body.createdAt) || body.createdAt < 0) {
    badRequest('createdAt must be a non-negative timestamp');
  }
  if (!Array.isArray(body.moves) || body.moves.length > 4096) {
    badRequest('moves must be a bounded array');
  }
  const moves = body.moves.map((move) => {
    if (typeof move !== 'string' || !MOVE_PATTERN.test(move)) {
      badRequest('moves must contain canonical UCI moves');
    }
    return move;
  });
  const terminal = body.terminal;
  if (!TERMINATIONS.has(terminal)) {
    badRequest('terminal has an unsupported value');
  }
  if (body.turn !== 'white' && body.turn !== 'black') {
    badRequest('turn must be white or black');
  }
  if (!Number.isSafeInteger(updatedAt) || updatedAt < 0) {
    throw new TypeError('updatedAt must be a non-negative safe integer');
  }

  const completed = terminal !== 'ongoing';
  const result = terminal === 'resignation'
    ? humanColor === 'white' ? '0-1' : '1-0'
    : terminal === 'checkmate'
      ? body.turn === 'white' ? '0-1' : '1-0'
      : completed ? '1/2-1/2' : '*';
  return {
    id,
    rootFen: boundedFen(body.rootFen, 'rootFen'),
    finalFen: boundedFen(body.finalFen, 'finalFen'),
    moves,
    humanColor,
    depth: body.depth,
    turn: body.turn,
    status: completed ? 'completed' : 'active',
    result,
    termination: terminal,
    white: humanColor === 'white' ? 'You' : 'Kugelfisch',
    black: humanColor === 'black' ? 'You' : 'Kugelfisch',
    createdAt: body.createdAt,
    updatedAt,
  };
}

export function classifySavedGameUpdate(previous, game) {
  if (!previous) return 'insert';
  const sameGame =
    previous.rootFen === game.rootFen &&
    previous.humanColor === game.humanColor &&
    previous.depth === game.depth &&
    previous.createdAt === game.createdAt;
  const extendsLine =
    previous.moves.length <= game.moves.length &&
    previous.moves.every((move, index) => move === game.moves[index]);
  const repeatsCompleted =
    sameGame &&
    previous.status === 'completed' &&
    previous.finalFen === game.finalFen &&
    previous.turn === game.turn &&
    previous.termination === game.termination &&
    previous.moves.length === game.moves.length &&
    extendsLine;
  if (repeatsCompleted) return 'unchanged';
  if (!sameGame || !extendsLine || previous.status === 'completed') {
    throw new SavedGameError(409, 'saved game update conflicts with its record');
  }
  return 'update';
}

export function parseSavedGamePage(url) {
  for (const name of url.searchParams.keys()) {
    if (name !== 'limit' && name !== 'cursor') {
      badRequest('unsupported game-list parameter');
    }
  }
  const rawLimit = url.searchParams.get('limit') ?? '20';
  const rawCursor = url.searchParams.get('cursor');
  if (!/^\d+$/.test(rawLimit)) badRequest('game-list limit is invalid');
  const limit = Number(rawLimit);
  if (!Number.isInteger(limit) || limit < 1 || limit > 100) {
    badRequest('game-list limit must be from 1 to 100');
  }
  let cursor = 0;
  if (rawCursor !== null) {
    if (!/^\d+$/.test(rawCursor)) badRequest('game-list cursor is invalid');
    cursor = Number(rawCursor);
    if (!Number.isSafeInteger(cursor)) badRequest('game-list cursor is invalid');
  }
  return { cursor, limit };
}

export function savedGameSummary(game) {
  return {
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
  };
}
