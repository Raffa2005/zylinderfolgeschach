import { Chessground } from '@lichess-org/chessground';
import '@lichess-org/chessground/assets/chessground.base.css';
import '@lichess-org/chessground/assets/chessground.brown.css';
import '@lichess-org/chessground/assets/chessground.cburnett.css';
import createZfsModule from './generated/zfs.js';
import './style.css';

const module = await createZfsModule();
const api = {
  reset: module.cwrap('zfs_reset', null, []),
  load: module.cwrap('zfs_load', 'number', ['string']),
  play: module.cwrap('zfs_play', 'number', ['string']),
  back: module.cwrap('zfs_back', 'number', []),
  forward: module.cwrap('zfs_forward', 'number', []),
  error: module.cwrap('zfs_last_error', 'string', []),
  state: module.cwrap('zfs_state_json', 'string', []),
};

const elements = {
  back: document.querySelector('#back'),
  badges: document.querySelector('#badges'),
  board: document.querySelector('#board'),
  copyFen: document.querySelector('#copy-fen'),
  engineConnection: document.querySelector('#engine-connection'),
  engineDepth: document.querySelector('#engine-depth'),
  engineDepthReadout: document.querySelector('#engine-depth-readout'),
  engineNodes: document.querySelector('#engine-nodes'),
  enginePv: document.querySelector('#engine-pv'),
  engineScore: document.querySelector('#engine-score'),
  engineStatus: document.querySelector('#engine-status'),
  engineToggle: document.querySelector('#engine-toggle'),
  error: document.querySelector('#error'),
  fen: document.querySelector('#fen'),
  flip: document.querySelector('#flip'),
  forward: document.querySelector('#forward'),
  history: document.querySelector('#history'),
  humanColor: document.querySelector('#human-color'),
  legalMoves: document.querySelector('#legal-moves'),
  loadFen: document.querySelector('#load-fen'),
  moveCount: document.querySelector('#move-count'),
  plyCount: document.querySelector('#ply-count'),
  promotionChoices: document.querySelector('#promotion-choices'),
  promotionDialog: document.querySelector('#promotion-dialog'),
  reset: document.querySelector('#reset'),
  status: document.querySelector('#status'),
  turnDot: document.querySelector('#turn-dot'),
};

let state = readState();
let rootFen = state.fen;
let inputLocked = false;
const engine = {
  analysis: null,
  available: false,
  controller: null,
  depth: 10,
  enabled: false,
  gameId: null,
  humanColor: 'white',
  message: 'Waiting for engine service',
  requestSerial: 0,
  thinking: false,
};

const ground = Chessground(elements.board, {
  fen: state.board,
  orientation: 'white',
  turnColor: state.turn,
  coordinates: true,
  animation: { enabled: true, duration: 180 },
  movable: {
    color: state.turn,
    free: false,
    dests: destinations(state.legalMoves),
    showDests: true,
    rookCastle: true,
    events: { after: onBoardMove },
  },
  draggable: { enabled: true, showGhost: true },
  selectable: { enabled: true },
  highlight: { lastMove: true, check: true },
  drawable: { enabled: true, visible: true, eraseOnClick: false },
  disableContextMenu: true,
});

render();
void connectEngine();

function readState() {
  return JSON.parse(api.state());
}

function destinations(moves) {
  const result = new Map();
  for (const move of moves) {
    const origin = move.slice(0, 2);
    const destination = move.slice(2, 4);
    const existing = result.get(origin);
    if (existing) {
      if (!existing.includes(destination)) existing.push(destination);
    } else {
      result.set(origin, [destination]);
    }
  }
  return result;
}

function setError(message = '') {
  elements.error.textContent = message;
}

function badge(text, className = '') {
  const item = document.createElement('span');
  item.className = `badge ${className}`.trim();
  item.textContent = text;
  return item;
}

function canHumanMove() {
  return !inputLocked &&
    !engine.thinking &&
    state.terminal === 'ongoing' &&
    (!engine.enabled || state.turn === engine.humanColor);
}

function render() {
  const lastMove = state.history[state.historyCursor - 1];
  const humanMayMove = canHumanMove();
  ground.set({
    fen: state.board,
    turnColor: state.turn,
    check: state.inCheck ? state.turn : false,
    lastMove: lastMove ? [lastMove.slice(0, 2), lastMove.slice(2, 4)] : [],
    movable: {
      color: humanMayMove ? state.turn : undefined,
      dests: humanMayMove ? destinations(state.legalMoves) : new Map(),
    },
    drawable: {
      autoShapes:
        state.follow === '-'
          ? []
          : [{ orig: state.follow, brush: 'yellow' }],
    },
  });

  elements.fen.value = state.fen;
  elements.turnDot.className = `turn-dot ${state.turn}`;
  elements.badges.replaceChildren();
  if (state.follow !== '-') {
    elements.badges.append(
      badge(
        `follow ${state.follow}`,
        state.followForced ? 'forced' : 'fallback',
      ),
    );
  } else {
    elements.badges.append(badge('no follow field'));
  }
  if (state.enPassant !== '-') {
    elements.badges.append(badge(`en passant ${state.enPassant}`));
  }
  if (state.inCheck) elements.badges.append(badge('check', 'danger'));

  const side = state.turn === 'white' ? 'White' : 'Black';
  if (state.terminal === 'checkmate') {
    elements.status.textContent = `${side} is checkmated`;
  } else if (state.terminal === 'stalemate') {
    elements.status.textContent = 'Stalemate';
  } else if (state.terminal === 'threefold') {
    elements.status.textContent = 'Draw by threefold repetition';
  } else if (state.terminal === 'fifty-move') {
    elements.status.textContent = 'Draw by the 50-move rule';
  } else if (engine.thinking) {
    elements.status.textContent = `${side} to move — engine thinking`;
  } else {
    elements.status.textContent = `${side} to move`;
  }

  elements.moveCount.textContent = String(state.legalMoves.length);
  elements.legalMoves.replaceChildren();
  for (const move of state.legalMoves) {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'move-chip';
    item.textContent = move;
    item.title = `Play ${move}`;
    item.disabled = !humanMayMove;
    item.addEventListener('click', () => play(move));
    elements.legalMoves.append(item);
  }
  if (state.legalMoves.length === 0) {
    elements.legalMoves.textContent = 'No legal moves';
    elements.legalMoves.classList.add('empty');
  } else {
    elements.legalMoves.classList.remove('empty');
  }

  elements.plyCount.textContent =
    `${state.historyCursor}/${state.history.length}`;
  elements.history.replaceChildren();
  elements.history.classList.toggle('empty', state.history.length === 0);
  if (state.history.length === 0) {
    elements.history.textContent = 'No moves yet';
  } else {
    state.history.forEach((move, index) => {
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'history-move';
      if (index >= state.historyCursor) item.classList.add('future');
      if (index === state.historyCursor - 1) item.classList.add('current');
      item.textContent = `${index + 1}. ${move}`;
      item.title = `Go to position after ${move}`;
      item.addEventListener('click', () => navigateTo(index + 1));
      elements.history.append(item);
    });
  }
  elements.back.disabled = inputLocked || state.historyCursor === 0;
  elements.forward.disabled =
    inputLocked || state.historyCursor === state.history.length;
  renderEngine();
}

function formatScore(score) {
  if (!score) return '—';
  if (score.kind === 'mate') {
    return score.value < 0
      ? `Engine -M${Math.abs(score.value)}`
      : `Engine +M${score.value}`;
  }
  const value = score.value / 100;
  return `Engine ${value >= 0 ? '+' : ''}${value.toFixed(2)}`;
}

function renderEngine() {
  elements.engineConnection.textContent = engine.available ? 'Online' : 'Offline';
  elements.engineConnection.className =
    `connection ${engine.available ? 'online' : 'offline'}`;
  elements.engineToggle.disabled = !engine.available || inputLocked;
  elements.engineToggle.textContent = engine.enabled
    ? 'Stop engine game'
    : 'Play from this position';
  elements.humanColor.disabled = engine.enabled || engine.thinking;
  elements.engineDepth.disabled = engine.enabled || engine.thinking;
  elements.engineStatus.textContent = engine.message;

  const analysis = engine.analysis;
  elements.engineScore.textContent = formatScore(analysis?.score);
  elements.engineDepthReadout.textContent = analysis?.depth === undefined
    ? 'Depth —'
    : `Depth ${analysis.depth}`;
  elements.engineNodes.textContent = analysis?.nodes === undefined
    ? '— nodes'
    : `${analysis.nodes.toLocaleString()} nodes`;
  elements.enginePv.textContent = analysis?.pv?.length
    ? analysis.pv.join(' ')
    : 'No analysis yet';
}

async function connectEngine() {
  try {
    const response = await fetch('/api/engine/status', { cache: 'no-store' });
    if (!response.ok) throw new Error(`engine service returned HTTP ${response.status}`);
    const status = await response.json();
    engine.available = status.available === true;
    if (Number.isInteger(status.defaultDepth)) {
      engine.depth = status.defaultDepth;
      elements.engineDepth.value = String(status.defaultDepth);
    }
    engine.message = engine.available
      ? `Ready at depth ${engine.depth}`
      : 'Build zfs_engine, then restart this server';
  } catch (error) {
    engine.available = false;
    engine.message = error instanceof Error ? error.message : String(error);
  }
  renderEngine();
}

function sync() {
  state = readState();
  render();
}

async function onBoardMove(origin, destination) {
  if (!canHumanMove()) {
    sync();
    return;
  }
  inputLocked = true;
  const prefix = origin + destination;
  const candidates = state.legalMoves.filter((move) => move.startsWith(prefix));
  if (candidates.length === 0) {
    inputLocked = false;
    sync();
    return;
  }

  let move = candidates[0];
  if (candidates.length > 1) move = await choosePromotion(candidates);
  inputLocked = false;
  if (move) commitMove(move);
  else sync();
}

function play(move) {
  if (!canHumanMove()) return;
  commitMove(move);
}

function commitMove(move) {
  const accepted = api.play(move);
  if (!accepted) setError(api.error());
  else setError();
  sync();
  if (accepted) void maybeStartEngineMove();
}

function cancelSearch() {
  ++engine.requestSerial;
  engine.controller?.abort();
  engine.controller = null;
  engine.thinking = false;
}

function stopEngineGame(message = 'Engine game stopped') {
  cancelSearch();
  engine.enabled = false;
  engine.message = message;
  render();
}

function selectedDepth() {
  const value = Number(elements.engineDepth.value);
  return Number.isInteger(value) && value >= 1 && value <= 100 ? value : null;
}

function newGameId() {
  if (globalThis.crypto?.randomUUID) return globalThis.crypto.randomUUID();
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function startEngineGame() {
  const depth = selectedDepth();
  if (depth === null) {
    setError('Depth must be an integer from 1 to 100.');
    return;
  }
  if (state.terminal !== 'ongoing') {
    setError('The current game is already over.');
    return;
  }

  cancelSearch();
  engine.depth = depth;
  engine.humanColor = elements.humanColor.value;
  engine.analysis = null;
  engine.enabled = true;
  engine.gameId = newGameId();
  engine.message = state.turn === engine.humanColor
    ? 'Your move'
    : `Thinking to depth ${engine.depth}`;
  setError();
  ground.set({ orientation: engine.humanColor });
  render();
  void maybeStartEngineMove();
}

async function readEngineEvents(response, serial) {
  if (!response.body) throw new Error('engine response has no body');
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let pending = '';
  let bestMove;

  const consume = (line) => {
    if (line.trim() === '') return;
    const event = JSON.parse(line);
    if (event.type === 'error') throw new Error(event.message || 'engine search failed');
    if (event.type === 'bestmove') bestMove = event.move;
    if (event.type === 'info' && serial === engine.requestSerial) {
      engine.analysis = event;
      engine.message = `Thinking to depth ${engine.depth}`;
      renderEngine();
    }
  };

  for (;;) {
    const { done, value } = await reader.read();
    pending += decoder.decode(value, { stream: !done });
    let newline;
    while ((newline = pending.indexOf('\n')) >= 0) {
      consume(pending.slice(0, newline));
      pending = pending.slice(newline + 1);
    }
    if (done) break;
  }
  consume(pending);
  return bestMove;
}

async function maybeStartEngineMove() {
  if (
    !engine.enabled ||
    engine.thinking ||
    state.terminal !== 'ongoing' ||
    state.turn === engine.humanColor
  ) {
    if (engine.enabled && state.terminal !== 'ongoing') {
      engine.message = 'Game over';
      renderEngine();
    } else if (engine.enabled && !engine.thinking) {
      engine.message = 'Your move';
      renderEngine();
    }
    return;
  }

  const controller = new AbortController();
  const serial = ++engine.requestSerial;
  engine.controller = controller;
  engine.thinking = true;
  engine.analysis = null;
  engine.message = `Thinking to depth ${engine.depth}`;
  render();

  try {
    const response = await fetch('/api/engine/move', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        gameId: engine.gameId,
        rootFen,
        moves: state.history.slice(0, state.historyCursor),
        depth: engine.depth,
      }),
      signal: controller.signal,
    });
    if (!response.ok) {
      const body = await response.json().catch(() => ({}));
      throw new Error(body.error || `engine service returned HTTP ${response.status}`);
    }
    const bestMove = await readEngineEvents(response, serial);
    if (serial !== engine.requestSerial || !engine.enabled) return;
    if (!bestMove || bestMove === '0000') {
      throw new Error('engine returned no move for an ongoing position');
    }
    if (!api.play(bestMove)) {
      throw new Error(`engine returned illegal move ${bestMove}: ${api.error()}`);
    }

    engine.controller = null;
    engine.thinking = false;
    engine.message = 'Your move';
    setError();
    sync();
    void maybeStartEngineMove();
  } catch (error) {
    if (serial !== engine.requestSerial) return;
    engine.controller = null;
    engine.thinking = false;
    if (error instanceof DOMException && error.name === 'AbortError') return;
    engine.enabled = false;
    engine.message = error instanceof Error ? error.message : String(error);
    setError(`Engine stopped: ${engine.message}`);
    render();
  }
}

function navigate(direction) {
  if (inputLocked) return;
  if (direction < 0 && state.historyCursor === 0) return;
  if (direction > 0 && state.historyCursor === state.history.length) return;

  if (engine.enabled) stopEngineGame('Engine game stopped for navigation');
  inputLocked = true;
  const accepted = direction < 0 ? api.back() : api.forward();
  if (!accepted) setError(api.error());
  else setError();
  state = readState();
  inputLocked = false;
  render();
}

function navigateTo(cursor) {
  if (inputLocked || cursor < 0 || cursor > state.history.length) return;
  if (engine.enabled) stopEngineGame('Engine game stopped for navigation');
  inputLocked = true;
  let accepted = true;
  let current = state.historyCursor;
  while (accepted && current > cursor) {
    accepted = api.back();
    if (accepted) --current;
  }
  while (accepted && current < cursor) {
    accepted = api.forward();
    if (accepted) ++current;
  }
  if (!accepted) setError(api.error());
  else setError();
  state = readState();
  inputLocked = false;
  render();
}

function choosePromotion(candidates) {
  const names = { q: 'Queen', r: 'Rook', b: 'Bishop', n: 'Knight' };
  elements.promotionChoices.replaceChildren();
  return new Promise((resolve) => {
    let settled = false;
    const finish = (choice) => {
      if (settled) return;
      settled = true;
      resolve(choice);
    };
    for (const suffix of ['q', 'r', 'b', 'n']) {
      const move = candidates.find((candidate) => candidate.endsWith(suffix));
      if (!move) continue;
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = names[suffix];
      button.addEventListener('click', () => {
        elements.promotionDialog.close();
        finish(move);
      });
      elements.promotionChoices.append(button);
    }
    elements.promotionDialog.addEventListener('close', () => finish(null), {
      once: true,
    });
    elements.promotionDialog.showModal();
  });
}

elements.back.addEventListener('click', () => navigate(-1));
elements.forward.addEventListener('click', () => navigate(1));

elements.reset.addEventListener('click', () => {
  cancelSearch();
  api.reset();
  state = readState();
  rootFen = state.fen;
  engine.analysis = null;
  engine.gameId = newGameId();
  engine.message = engine.enabled ? 'Starting new game' : 'Ready';
  setError();
  render();
  void maybeStartEngineMove();
});

elements.flip.addEventListener('click', () => ground.toggleOrientation());

elements.loadFen.addEventListener('click', () => {
  const fen = elements.fen.value.trim();
  stopEngineGame('Position loaded; engine game stopped');
  if (!api.load(fen)) {
    setError(api.error());
    elements.fen.value = fen;
    return;
  }
  state = readState();
  rootFen = state.fen;
  engine.analysis = null;
  setError();
  render();
});

elements.engineToggle.addEventListener('click', () => {
  if (engine.enabled) stopEngineGame();
  else startEngineGame();
});

elements.copyFen.addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(elements.fen.value);
    elements.copyFen.textContent = 'Copied';
    window.setTimeout(() => {
      elements.copyFen.textContent = 'Copy';
    }, 1000);
  } catch {
    elements.fen.select();
  }
});

document.addEventListener('keydown', (event) => {
  if (
    event.defaultPrevented ||
    event.altKey ||
    event.ctrlKey ||
    event.metaKey ||
    event.shiftKey ||
    elements.promotionDialog.open
  ) {
    return;
  }
  const target = event.target;
  if (
    target instanceof HTMLElement &&
    (target.isContentEditable || target.matches('input, textarea, select'))
  ) {
    return;
  }
  if (event.key === 'ArrowLeft') {
    event.preventDefault();
    navigate(-1);
  } else if (event.key === 'ArrowRight') {
    event.preventDefault();
    navigate(1);
  }
});
