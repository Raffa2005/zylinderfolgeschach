import { Chessground } from '@lichess-org/chessground';
import '@lichess-org/chessground/assets/chessground.base.css';
import '@lichess-org/chessground/assets/chessground.brown.css';
import '@lichess-org/chessground/assets/chessground.cburnett.css';
import createZfsModule from './generated/zfs.js';
import { ClientEngine, PlayingEngine } from './client-engine.js';
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
  lineSan: module.cwrap('zfs_line_san', 'string', ['string']),
};

const MOVE_PATTERN = /^[a-h][1-8][a-h][1-8][qrbn]?$/;

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

function newId(prefix = 'game') {
  if (globalThis.crypto?.randomUUID) return `${prefix}-${crypto.randomUUID()}`;
  return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function randomColor() {
  if (globalThis.crypto?.getRandomValues) {
    const value = new Uint8Array(1);
    crypto.getRandomValues(value);
    return value[0] & 1 ? 'white' : 'black';
  }
  return Math.random() < .5 ? 'white' : 'black';
}

function sideName(color) {
  return color === 'white' ? 'White' : 'Black';
}

function terminalText(state) {
  if (state.terminal === 'checkmate') {
    const winner = state.turn === 'white' ? 'black' : 'white';
    return `Checkmate · ${sideName(winner)} wins`;
  }
  const labels = {
    'fifty-move': 'Draw · 50-move rule',
    stalemate: 'Draw · Stalemate',
    threefold: 'Draw · Repetition',
  };
  return labels[state.terminal] ?? `${sideName(state.turn)} to move`;
}

function resignationText(humanColor) {
  const winner = humanColor === 'white' ? 'black' : 'white';
  return `Resignation · ${sideName(winner)} wins`;
}

function followText(state) {
  if (state.followForced) return `Follow ${state.follow}`;
  return 'Free to move';
}

function formatScore(score, turn) {
  if (!score) return '—';
  const value = turn === 'black' ? -score.value : score.value;
  if (score.kind === 'mate') {
    return value < 0 ? `−M${Math.abs(value)}` : `+M${value}`;
  }
  const pawns = value / 100;
  return `${pawns >= 0 ? '+' : ''}${pawns.toFixed(2)}`;
}

function readableDate(value) {
  return new Intl.DateTimeFormat(undefined, {
    year: 'numeric', month: 'short', day: 'numeric',
    hour: '2-digit', minute: '2-digit',
  }).format(new Date(value));
}

function shouldIgnoreArrowKey(event) {
  if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey ||
      event.shiftKey) return true;
  const target = event.target;
  return target instanceof HTMLElement &&
    (target.isContentEditable || target.matches('input, textarea, select'));
}

function createBoard(element, state, afterMove) {
  return Chessground(element, {
    fen: state.board,
    orientation: 'white',
    turnColor: state.turn,
    coordinates: true,
    animation: { enabled: true, duration: 160 },
    movable: {
      color: afterMove ? state.turn : undefined,
      free: false,
      dests: afterMove ? destinations(state.legalMoves) : new Map(),
      showDests: Boolean(afterMove),
      rookCastle: true,
      events: afterMove ? { after: afterMove } : {},
    },
    draggable: { enabled: Boolean(afterMove), showGhost: true },
    selectable: { enabled: Boolean(afterMove) },
    highlight: { lastMove: true, check: true },
    drawable: { enabled: true, visible: true, eraseOnClick: false },
    disableContextMenu: true,
  });
}

function boardUpdate(state, movable, orientation, engineMove) {
  const lastMove = state.history[state.historyCursor - 1];
  const arrow = MOVE_PATTERN.test(engineMove ?? '') &&
    state.legalMoves.includes(engineMove)
    ? [{ orig: engineMove.slice(0, 2), dest: engineMove.slice(2, 4), brush: 'paleBlue' }]
    : [];
  return {
    fen: state.board,
    orientation,
    turnColor: state.turn,
    check: state.inCheck ? state.turn : false,
    lastMove: lastMove ? [lastMove.slice(0, 2), lastMove.slice(2, 4)] : [],
    highlight: {
      custom: state.follow === '-'
        ? new Map()
        : new Map([[state.follow, 'follow-field']]),
    },
    movable: {
      color: movable ? state.turn : undefined,
      dests: movable ? destinations(state.legalMoves) : new Map(),
    },
    drawable: { autoShapes: arrow },
  };
}

const renderedHistories = new WeakMap();

function renderMoveHistory(container, state, onSelect) {
  // Iterative engine updates must not rebuild the list or disturb its focus.
  const key = `${state.fen}|${state.historyCursor}|${state.history.join(' ')}`;
  if (renderedHistories.get(container) === key) return;
  renderedHistories.set(container, key);
  container.replaceChildren();
  container.classList.toggle('empty', state.history.length === 0);
  if (state.history.length === 0) {
    container.textContent = 'No moves yet';
    return;
  }
  const fullmove = Number(state.fen.split(/\s+/)[5]);
  const firstPly = (fullmove - 1) * 2 + (state.turn === 'black' ? 1 : 0) - state.historyCursor;
  let current;
  let row;
  state.history.forEach((move, index) => {
    const absolutePly = firstPly + index;
    const black = absolutePly % 2 === 1;
    const number = Math.floor(absolutePly / 2) + 1;
    if (!row || !black) {
      row = document.createElement('div');
      row.className = 'history-row';
      const label = document.createElement('span');
      label.className = 'move-number';
      label.textContent = `${number}.`;
      row.append(label);
      if (black) {
        const empty = document.createElement('span');
        empty.className = 'move-number';
        empty.textContent = '…';
        row.append(empty);
      }
      container.append(row);
    }
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'history-move';
    if (index >= state.historyCursor) button.classList.add('future');
    if (index === state.historyCursor - 1) {
      button.classList.add('current');
      button.setAttribute('aria-current', 'step');
      current = button;
    }
    button.textContent = state.sanHistory?.[index] ?? move;
    button.title = move;
    button.setAttribute('aria-label', `${number}. ${black ? 'Black' : 'White'}: ${button.textContent}`);
    button.addEventListener('click', () => onSelect(index + 1));
    row.append(button);
  });
  if (!current) return;
  const top = current.offsetTop;
  const bottom = top + current.offsetHeight;
  if (top < container.scrollTop) {
    container.scrollTop = top;
  } else if (bottom > container.scrollTop + container.clientHeight) {
    container.scrollTop = bottom - container.clientHeight;
  }
}

function navigateState(state, cursor) {
  if (cursor < 0 || cursor > state.history.length) return false;
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
  return accepted;
}

function choosePromotion(dialog, choices, cancel, candidates) {
  const names = { q: 'Queen', r: 'Rook', b: 'Bishop', n: 'Knight' };
  choices.replaceChildren();
  return new Promise((resolve) => {
    let settled = false;
    const finish = (move) => {
      if (settled) return;
      settled = true;
      resolve(move);
    };
    for (const suffix of ['q', 'r', 'b', 'n']) {
      const move = candidates.find((candidate) => candidate.endsWith(suffix));
      if (!move) continue;
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = names[suffix];
      button.addEventListener('click', () => {
        dialog.close();
        finish(move);
      });
      choices.append(button);
    }
    cancel.onclick = () => dialog.close();
    dialog.addEventListener('close', () => finish(null), { once: true });
    dialog.showModal();
  });
}

async function initializePlay() {
  const element = (id) => document.getElementById(id);
  const elements = {
    back: element('back'), board: element('board'), bottomMeta: element('bottom-player-meta'),
    bottomName: element('bottom-player'), bottomState: element('bottom-player-state'),
    cancelPromotion: element('cancel-promotion'), depth: element('engine-depth'),
    engineStatus: element('engine-status'), error: element('error'), flip: element('flip'),
    forward: element('forward'), gameView: element('game-view'), history: element('history'),
    live: element('live-position'), newGame: element('new-game'), ply: element('ply-count'),
    positionCard: element('position-card'),
    promotionChoices: element('promotion-choices'), promotionDialog: element('promotion-dialog'),
    setupForm: element('setup-form'), setupMessage: element('setup-message'),
    resign: element('resign'), review: element('review-game'), setupView: element('setup-view'),
    start: element('start-game'), startPosition: element('start-position'),
    status: element('status'), statusDetail: element('status-detail'),
    topMeta: element('top-player-meta'), topName: element('top-player'),
    topState: element('top-player-state'), turnDot: element('turn-dot'),
  };
  api.reset();
  let state = readState();
  let rootFen = state.fen;
  let ground;
  let orientation = 'white';
  let inputLocked = false;
  let adjudication = null;
  let saveChain = Promise.resolve(false);
  const playingEngine = new PlayingEngine();
  const engine = {
    available: false, controller: null, createdAt: 0, depth: 9, enabled: false,
    gameId: null, humanColor: 'white', message: 'Ready', pondering: false,
    requestSerial: 0, thinking: false,
  };

  function ensureBoard() {
    if (!ground) ground = createBoard(elements.board, state, onBoardMove);
    return ground;
  }

  function canMove() {
    return !inputLocked && !engine.thinking && !adjudication &&
      state.terminal === 'ongoing' &&
      state.historyCursor === state.history.length &&
      (!engine.enabled || state.turn === engine.humanColor);
  }

  function renderPlayers() {
    const topColor = orientation === 'white' ? 'black' : 'white';
    const bottomColor = orientation;
    const set = (name, meta, status, color) => {
      const human = color === engine.humanColor;
      name.textContent = human ? 'You' : 'Kugelfisch';
      meta.textContent = sideName(color);
      if (adjudication || state.terminal !== 'ongoing') status.textContent = 'Finished';
      else if (state.turn !== color) status.textContent = 'Waiting';
      else if (!human && engine.thinking) status.textContent = 'Thinking';
      else status.textContent = 'To move';
      status.classList.toggle('active', !adjudication && state.terminal === 'ongoing' && state.turn === color);
    };
    set(elements.topName, elements.topMeta, elements.topState, topColor);
    set(elements.bottomName, elements.bottomMeta, elements.bottomState, bottomColor);
  }

  function render() {
    if (!elements.gameView.hidden) ensureBoard().set(boardUpdate(state, canMove(), orientation));
    const reviewing = state.historyCursor !== state.history.length;
    const terminal = (adjudication || state.terminal !== 'ongoing') && !reviewing;
    elements.status.textContent = reviewing
      ? 'Reviewing game'
      : adjudication === 'resignation'
        ? resignationText(engine.humanColor)
        : state.terminal === 'ongoing'
        ? `${sideName(state.turn)} to move`
        : terminalText(state);
    elements.statusDetail.textContent = terminal
      ? ''
      : `${state.inCheck ? 'Check · ' : ''}${followText(state)}`;
    elements.positionCard.classList.toggle('terminal', terminal);
    elements.turnDot.className = `turn-dot ${state.turn}`;
    elements.engineStatus.textContent = engine.pondering ? 'Your move' : engine.message;
    elements.ply.textContent = `${state.historyCursor} / ${state.history.length}`;
    renderMoveHistory(elements.history, state, navigateTo);
    elements.startPosition.disabled = inputLocked || state.historyCursor === 0;
    elements.back.disabled = inputLocked || state.historyCursor === 0;
    elements.forward.disabled = inputLocked || state.historyCursor === state.history.length;
    elements.live.disabled = inputLocked || state.historyCursor === state.history.length;
    elements.resign.disabled = inputLocked || Boolean(adjudication) ||
      state.terminal !== 'ongoing' || !engine.gameId ||
      state.historyCursor !== state.history.length;
    elements.review.hidden = !terminal || !engine.gameId;
    elements.review.href = `/games/${encodeURIComponent(engine.gameId ?? '')}`;
    renderPlayers();
  }

  function sync() {
    state = readState();
    render();
  }

  function snapshot() {
    return {
      rootFen, finalFen: state.fen,
      moves: state.history.slice(0, state.historyCursor),
      humanColor: engine.humanColor, depth: engine.depth,
      turn: state.turn, terminal: adjudication ?? state.terminal,
      createdAt: engine.createdAt,
    };
  }

  function saveGame() {
    if (!engine.gameId || !engine.createdAt) return;
    const id = engine.gameId;
    const body = JSON.stringify(snapshot());
    saveChain = saveChain.then(async () => {
      const response = await fetch(`/api/games/${encodeURIComponent(id)}`, {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body,
      });
      if (!response.ok) {
        const payload = await response.json().catch(() => ({}));
        throw new Error(payload.error || `could not save game (HTTP ${response.status})`);
      }
      return true;
    }).catch((error) => {
      elements.error.textContent = error instanceof Error ? error.message : String(error);
      return false;
    });
  }

  function cancelSearch() {
    ++engine.requestSerial;
    engine.controller?.abort();
    engine.controller = null;
    engine.thinking = false;
  }

  function stopGame(message) {
    cancelSearch();
    void playingEngine.stopPonder();
    engine.enabled = false;
    engine.pondering = false;
    engine.message = message;
  }

  async function pauseSearch(message) {
    cancelSearch();
    engine.pondering = false;
    engine.message = message;
    await playingEngine.stopPonder();
  }

  async function connect() {
    try {
      await playingEngine.start();
      engine.available = true;
    } catch (error) {
      engine.available = false;
      elements.setupMessage.textContent = error instanceof Error ? error.message : String(error);
    }
    elements.setupMessage.textContent = engine.available
      ? 'Engine ready on this device.'
      : elements.setupMessage.textContent;
    elements.start.disabled = !engine.available;
  }

  function beginGame(event) {
    event.preventDefault();
    if (!engine.available) return;
    const requested = new FormData(elements.setupForm).get('color');
    api.reset();
    state = readState();
    rootFen = state.fen;
    adjudication = null;
    engine.depth = Number(elements.depth.value);
    engine.humanColor = requested === 'random' ? randomColor() : requested;
    engine.gameId = newId();
    engine.createdAt = Date.now();
    engine.enabled = true;
    engine.pondering = false;
    engine.message = state.turn === engine.humanColor ? 'Your move' : 'Thinking';
    orientation = engine.humanColor;
    elements.error.textContent = '';
    elements.setupView.hidden = true;
    elements.gameView.hidden = false;
    document.body.classList.add('active-game');
    render();
    saveGame();
    void maybeEngineMove();
  }

  function returnToSetup() {
    if (engine.enabled && state.terminal === 'ongoing' &&
        !window.confirm('Leave this game and start a new one? Your moves are saved.')) return;
    stopGame('Ready');
    api.reset();
    state = readState();
    rootFen = state.fen;
    adjudication = null;
    elements.gameView.hidden = true;
    elements.setupView.hidden = false;
    document.body.classList.remove('active-game');
    elements.error.textContent = '';
  }

  async function maybeEngineMove() {
    if (!engine.enabled || adjudication || engine.thinking ||
        state.turn === engine.humanColor ||
        state.terminal !== 'ongoing') {
      if (engine.enabled && state.terminal !== 'ongoing') {
        void playingEngine.stopPonder();
        engine.enabled = false;
        engine.message = 'Game over';
      } else if (engine.enabled && !engine.thinking) {
        engine.message = engine.pondering ? 'Pondering' : 'Your move';
      }
      render();
      return;
    }
    const controller = new AbortController();
    const requestSerial = ++engine.requestSerial;
    engine.controller = controller;
    engine.thinking = true;
    engine.pondering = false;
    engine.message = 'Thinking';
    render();
    try {
      const result = await playingEngine.search({
        gameId: engine.gameId,
        rootFen,
        moves: state.history.slice(0, state.historyCursor),
        depth: engine.depth,
        signal: controller.signal,
        onInfo: () => {},
      });
      const bestMove = result.move;
      if (requestSerial !== engine.requestSerial || !engine.enabled) return;
      if (!bestMove || bestMove === '0000' || !api.play(bestMove)) {
        throw new Error(`engine returned an invalid move: ${bestMove ?? 'none'}`);
      }
      engine.controller = null;
      engine.thinking = false;
      engine.pondering = result.pondering;
      engine.message = result.pondering ? 'Pondering' : 'Your move';
      sync();
      saveGame();
      void maybeEngineMove();
    } catch (error) {
      if (requestSerial !== engine.requestSerial) return;
      engine.controller = null;
      engine.thinking = false;
      if (error?.name === 'AbortError') return;
      engine.enabled = false;
      engine.message = 'Engine stopped';
      elements.error.textContent = error instanceof Error ? error.message : String(error);
      render();
    }
  }

  async function onBoardMove(origin, destination) {
    if (!canMove()) return sync();
    inputLocked = true;
    const candidates = state.legalMoves.filter((move) => move.startsWith(origin + destination));
    let move = candidates[0];
    if (candidates.length > 1) {
      move = await choosePromotion(elements.promotionDialog, elements.promotionChoices,
        elements.cancelPromotion, candidates);
    }
    inputLocked = false;
    if (!move) return sync();
    if (!api.play(move)) {
      elements.error.textContent = api.error();
      return sync();
    }
    engine.pondering = false;
    elements.error.textContent = '';
    sync();
    saveGame();
    void maybeEngineMove();
  }

  async function navigateTo(cursor) {
    if (inputLocked) return;
    inputLocked = true;
    if (engine.enabled) await pauseSearch('Reviewing game');
    const accepted = navigateState(state, cursor);
    state = readState();
    inputLocked = false;
    elements.error.textContent = accepted ? '' : api.error();
    render();
    if (accepted && engine.enabled && state.historyCursor === state.history.length) {
      void maybeEngineMove();
    }
  }

  elements.setupForm.addEventListener('submit', beginGame);
  elements.newGame.addEventListener('click', returnToSetup);
  elements.review.addEventListener('click', async (event) => {
    if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    event.preventDefault();
    const target = elements.review.href;
    if (await saveChain) location.assign(target);
  });
  elements.resign.addEventListener('click', () => {
    if (elements.resign.disabled || !window.confirm('Resign this game?')) return;
    adjudication = 'resignation';
    stopGame('Game over');
    saveGame();
    render();
  });
  elements.flip.addEventListener('click', () => {
    orientation = orientation === 'white' ? 'black' : 'white';
    ensureBoard().toggleOrientation();
    renderPlayers();
  });
  elements.startPosition.addEventListener('click', () => navigateTo(0));
  elements.back.addEventListener('click', () => navigateTo(state.historyCursor - 1));
  elements.forward.addEventListener('click', () => navigateTo(state.historyCursor + 1));
  elements.live.addEventListener('click', () => navigateTo(state.history.length));
  document.addEventListener('keydown', (event) => {
    if (shouldIgnoreArrowKey(event) || elements.promotionDialog.open) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      navigateTo(state.historyCursor + (event.key === 'ArrowLeft' ? -1 : 1));
    }
  });
  window.addEventListener('pagehide', () => playingEngine.close());
  await connect();
}

async function initializeGames() {
  const element = (id) => document.getElementById(id);
  const gameId = location.pathname.match(/^\/games\/([A-Za-z0-9._-]{1,128})\/?$/)?.[1];
  const elements = {
    analyze: element('analyze-game'), archiveEmpty: element('archive-empty'),
    archiveError: element('archive-error'), archiveGrid: element('archive-grid'),
    archiveLoading: element('archive-loading'), archivePager: element('archive-pager'),
    archiveView: element('archive-view'), detailContent: element('detail-content'),
    detailError: element('detail-error'), detailHeading: element('detail-heading'),
    detailLoading: element('detail-loading'), detailMeta: element('detail-meta'),
    detailMoves: element('detail-moves'), detailPlayers: element('detail-players'),
    detailPly: element('detail-ply-count'), detailResult: element('detail-result'),
    detailView: element('detail-view'), loadMore: element('load-more'),
    outcome: element('detail-outcome'), replayFlip: element('replay-flip'),
    topPlayer: element('replay-top-player'), topColor: element('replay-top-color'),
    bottomPlayer: element('replay-bottom-player'), bottomColor: element('replay-bottom-color'),
    replayBack: element('replay-back'), replayBoard: element('replay-board'),
    replayCounter: element('replay-counter'), replayEnd: element('replay-end'),
    replayForward: element('replay-forward'), replayStart: element('replay-start'),
  };
  let cursor = null;
  let loading = false;
  let state;
  let ground;
  let savedGame;
  let orientation = 'white';

  function resultLabel(game) {
    return game.status === 'completed' ? game.result : 'In progress';
  }

  function archiveCard(game) {
    const link = document.createElement('a');
    link.className = 'archive-card';
    link.href = `/games/${encodeURIComponent(game.id)}`;
    const result = document.createElement('strong');
    result.textContent = resultLabel(game);
    const date = document.createElement('span');
    date.textContent = readableDate(game.updatedAt);
    const head = document.createElement('div');
    head.className = 'archive-card-head';
    head.append(result, date);
    const preview = document.createElement('div');
    preview.className = 'archive-board board';
    const details = document.createElement('div');
    details.className = 'archive-card-details';
    const white = document.createElement('span');
    white.textContent = game.white;
    const black = document.createElement('span');
    black.textContent = game.black;
    const meta = document.createElement('small');
    meta.textContent = `${Math.ceil(game.plies / 2)} moves · Depth ${game.depth}`;
    details.append(white, black, meta);
    const body = document.createElement('div');
    body.className = 'archive-card-body';
    body.append(preview, details);
    link.append(head, body);
    createBoard(preview, {
      board: game.finalFen.split(/\s+/)[0], turn: 'white', legalMoves: [],
    });
    return link;
  }

  async function loadPage(reset) {
    if (loading) return;
    loading = true;
    if (reset) {
      cursor = null;
      elements.archiveGrid.replaceChildren();
      elements.archiveLoading.hidden = false;
      elements.archiveError.textContent = '';
    }
    try {
      const params = new URLSearchParams({ limit: '20' });
      if (cursor) params.set('cursor', cursor);
      const response = await fetch(`/api/games?${params}`, { cache: 'no-store' });
      if (!response.ok) throw new Error(`could not load games (HTTP ${response.status})`);
      const page = await response.json();
      page.games.forEach((game) => elements.archiveGrid.append(archiveCard(game)));
      cursor = page.nextCursor;
      const empty = elements.archiveGrid.childElementCount === 0;
      elements.archiveGrid.hidden = empty;
      elements.archiveEmpty.hidden = !empty;
      elements.archivePager.hidden = !cursor;
    } catch (error) {
      elements.archiveError.textContent = error instanceof Error ? error.message : String(error);
    } finally {
      elements.archiveLoading.hidden = true;
      loading = false;
    }
  }

  function renderReplay() {
    ground.set(boardUpdate(state, false, orientation));
    const whiteBelow = orientation === 'white';
    elements.topPlayer.textContent = whiteBelow ? savedGame.black : savedGame.white;
    elements.bottomPlayer.textContent = whiteBelow ? savedGame.white : savedGame.black;
    elements.topColor.textContent = whiteBelow ? 'Black' : 'White';
    elements.bottomColor.textContent = whiteBelow ? 'White' : 'Black';
    elements.analyze.href = `/analysis?game=${encodeURIComponent(gameId)}&ply=${state.historyCursor}`;
    elements.replayCounter.textContent = `${state.historyCursor} / ${state.history.length}`;
    elements.detailPly.textContent = `${state.historyCursor} / ${state.history.length}`;
    renderMoveHistory(elements.detailMoves, state, navigateReplay);
    elements.replayStart.disabled = state.historyCursor === 0;
    elements.replayBack.disabled = state.historyCursor === 0;
    elements.replayForward.disabled = state.historyCursor === state.history.length;
    elements.replayEnd.disabled = state.historyCursor === state.history.length;
  }

  function navigateReplay(cursorValue) {
    if (!navigateState(state, cursorValue)) elements.detailError.textContent = api.error();
    state = readState();
    renderReplay();
  }

  async function loadDetail() {
    try {
      const response = await fetch(`/api/games/${encodeURIComponent(gameId)}`, { cache: 'no-store' });
      if (!response.ok) throw new Error(`could not load game (HTTP ${response.status})`);
      const { game } = await response.json();
      savedGame = game;
      if (!api.load(game.rootFen)) throw new Error(api.error());
      for (const move of game.moves) {
        if (!MOVE_PATTERN.test(move) || !api.play(move)) {
          throw new Error(`saved game contains illegal move ${move}`);
        }
      }
      state = readState();
      elements.detailHeading.textContent = 'Game review';
      elements.detailResult.textContent = resultLabel(game);
      const reasons = { resignation: 'Resignation', checkmate: 'Checkmate', threefold: 'Repetition', 'fifty-move': '50-move rule', stalemate: 'Stalemate' };
      const outcome = game.result === '1-0' ? 'White wins' : game.result === '0-1' ? 'Black wins' : 'Draw';
      elements.outcome.textContent = game.status === 'completed'
        ? `${outcome} · ${reasons[game.termination] ?? game.termination}`
        : 'Saved position';
      elements.detailPlayers.replaceChildren();
      for (const [name, color] of [[game.white, 'White'], [game.black, 'Black']]) {
        const row = document.createElement('span');
        const side = document.createElement('small');
        row.textContent = name;
        side.textContent = color;
        row.append(side);
        elements.detailPlayers.append(row);
      }
      elements.detailMeta.textContent = `${Math.ceil(game.moves.length / 2)} moves · Depth ${game.depth} · ${readableDate(game.updatedAt)}`;
      elements.detailLoading.hidden = true;
      elements.detailContent.hidden = false;
      ground = createBoard(elements.replayBoard, state);
      renderReplay();
    } catch (error) {
      elements.detailLoading.hidden = true;
      elements.detailError.textContent = error instanceof Error ? error.message : String(error);
    }
  }

  elements.loadMore.addEventListener('click', () => loadPage(false));
  elements.replayStart.addEventListener('click', () => navigateReplay(0));
  elements.replayBack.addEventListener('click', () => navigateReplay(state.historyCursor - 1));
  elements.replayForward.addEventListener('click', () => navigateReplay(state.historyCursor + 1));
  elements.replayEnd.addEventListener('click', () => navigateReplay(state.history.length));
  elements.replayFlip.addEventListener('click', () => {
    if (!ground) return;
    orientation = orientation === 'white' ? 'black' : 'white';
    renderReplay();
  });
  document.addEventListener('keydown', (event) => {
    if (!gameId || !state || shouldIgnoreArrowKey(event)) return;
    const target = {
      ArrowLeft: state.historyCursor - 1, ArrowRight: state.historyCursor + 1,
      Home: 0, End: state.history.length,
    }[event.key];
    if (target === undefined) return;
    event.preventDefault();
    navigateReplay(target);
  });
  if (gameId) {
    elements.archiveView.hidden = true;
    elements.detailView.hidden = false;
    await loadDetail();
  } else {
    elements.archiveView.hidden = false;
    elements.detailView.hidden = true;
    await loadPage(true);
  }
}

async function initializeAnalysis() {
  const element = (id) => document.getElementById(id);
  const elements = {
    back: element('analysis-back'), board: element('analysis-board'),
    cancelPromotion: element('analysis-cancel-promotion'),
    copy: element('analysis-copy'), depth: element('analysis-depth'),
    depthReadout: element('analysis-depth-readout'), end: element('analysis-end'),
    engineStatus: element('analysis-engine-status'),
    error: element('analysis-error'), fen: element('analysis-fen'), flip: element('analysis-flip'),
    forward: element('analysis-forward'), history: element('analysis-history'),
    load: element('analysis-load'), nodes: element('analysis-nodes'),
    ply: element('analysis-ply-count'), promotionChoices: element('analysis-promotion-choices'),
    promotionDialog: element('analysis-promotion-dialog'), pv: element('analysis-pv'),
    positionCard: element('analysis-position-card'),
    reset: element('analysis-reset'), score: element('analysis-score'),
    source: element('analysis-source'), start: element('analysis-start'),
    status: element('analysis-status'), statusDetail: element('analysis-status-detail'),
    toggle: element('analysis-toggle'), turnDot: element('analysis-turn-dot'),
  };
  api.reset();
  let state = readState();
  let rootFen = state.fen;
  let orientation = 'white';
  let inputLocked = false;
  let available = false;
  let controller;
  let serial = 0;
  let analysis;
  let analysisChain = Promise.resolve();
  let message = 'Loading engine';
  const analysisEngine = new ClientEngine();
  const ground = createBoard(elements.board, state, onBoardMove);

  function canMove() {
    return !inputLocked && state.terminal === 'ongoing';
  }

  function render() {
    ground.set(boardUpdate(state, canMove(), orientation, analysis?.pv?.[0]));
    elements.status.textContent = state.terminal === 'ongoing'
      ? `${sideName(state.turn)} to move`
      : terminalText(state);
    const terminal = state.terminal !== 'ongoing';
    elements.statusDetail.textContent = terminal
      ? ''
      : `${state.inCheck ? 'Check · ' : ''}${followText(state)}`;
    elements.positionCard.classList.toggle('terminal', terminal);
    elements.turnDot.className = `turn-dot ${state.turn}`;
    if (document.activeElement !== elements.fen) elements.fen.value = state.fen;
    elements.engineStatus.textContent = terminal ? 'Game over' : message;
    elements.score.textContent = formatScore(analysis?.score, state.turn);
    elements.depthReadout.textContent = analysis?.depth === undefined ? 'Depth —' : `Depth ${analysis.depth}`;
    elements.nodes.textContent = analysis?.nodes === undefined ? '— nodes' : `${analysis.nodes.toLocaleString()} nodes`;
    const uci = analysis?.pv?.join(' ') ?? '';
    const san = uci ? api.lineSan(uci) : '';
    elements.pv.textContent = san || uci || 'No line.';
    elements.toggle.disabled = !available;
    elements.ply.textContent = `${state.historyCursor} / ${state.history.length}`;
    renderMoveHistory(elements.history, state, navigateTo);
    elements.start.disabled = inputLocked || state.historyCursor === 0;
    elements.back.disabled = inputLocked || state.historyCursor === 0;
    elements.forward.disabled = inputLocked || state.historyCursor === state.history.length;
    elements.end.disabled = inputLocked || state.historyCursor === state.history.length;
  }

  function cancelAnalysis(
    nextMessage = elements.toggle.checked ? 'Ready' : 'Paused',
  ) {
    ++serial;
    controller?.abort();
    controller = undefined;
    message = nextMessage;
  }

  function changedPosition() {
    cancelAnalysis();
    analysis = undefined;
    state = readState();
    render();
    scheduleAnalysis();
  }

  async function runAnalysis() {
    if (!available || !elements.toggle.checked || controller ||
        state.terminal !== 'ongoing') return;
    const searchController = new AbortController();
    const requestSerial = ++serial;
    controller = searchController;
    analysis = undefined;
    message = 'Analyzing';
    elements.error.textContent = '';
    render();
    try {
      const result = await analysisEngine.search({
        rootFen,
        moves: state.history.slice(0, state.historyCursor),
        depth: Number(elements.depth.value),
        signal: searchController.signal,
        onInfo: (info) => {
          if (requestSerial === serial) {
            analysis = info;
            render();
          }
        },
      });
      if (requestSerial !== serial) return;
      analysis = result.info ?? analysis;
      controller = undefined;
      message = 'Complete';
      render();
    } catch (error) {
      if (requestSerial !== serial) return;
      controller = undefined;
      if (error?.name === 'AbortError') return;
      message = 'Stopped';
      elements.error.textContent = error instanceof Error ? error.message : String(error);
      render();
    }
  }

  function scheduleAnalysis() {
    if (!available || !elements.toggle.checked ||
        state.terminal !== 'ongoing') return;
    const scheduledSerial = serial;
    analysisChain = analysisChain.then(() => {
      if (scheduledSerial !== serial || !elements.toggle.checked ||
          state.terminal !== 'ongoing') return undefined;
      return runAnalysis();
    });
  }

  function restartAnalysis() {
    cancelAnalysis();
    analysis = undefined;
    render();
    scheduleAnalysis();
  }

  async function onBoardMove(origin, destination) {
    if (!canMove()) return render();
    inputLocked = true;
    const candidates = state.legalMoves.filter((move) => move.startsWith(origin + destination));
    let move = candidates[0];
    if (candidates.length > 1) {
      move = await choosePromotion(elements.promotionDialog, elements.promotionChoices,
        elements.cancelPromotion, candidates);
    }
    inputLocked = false;
    if (!move) return render();
    if (!api.play(move)) elements.error.textContent = api.error();
    else elements.error.textContent = '';
    changedPosition();
  }

  function navigateTo(cursorValue) {
    if (inputLocked) return;
    cancelAnalysis();
    inputLocked = true;
    const accepted = navigateState(state, cursorValue);
    state = readState();
    inputLocked = false;
    analysis = undefined;
    elements.error.textContent = accepted ? '' : api.error();
    render();
    scheduleAnalysis();
  }

  function loadRoot(fen) {
    cancelAnalysis();
    if (!api.load(fen)) {
      elements.error.textContent = api.error();
      elements.fen.value = fen;
      return false;
    }
    state = readState();
    rootFen = state.fen;
    analysis = undefined;
    elements.error.textContent = '';
    render();
    scheduleAnalysis();
    return true;
  }

  async function loadSavedGame(id) {
    const response = await fetch(`/api/games/${encodeURIComponent(id)}`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`could not load saved game (HTTP ${response.status})`);
    const { game } = await response.json();
    if (!loadRoot(game.rootFen)) return;
    for (const move of game.moves) {
      if (!MOVE_PATTERN.test(move) || !api.play(move)) {
        throw new Error(`saved game contains illegal move ${move}`);
      }
    }
    state = readState();
    const requestedPly = new URLSearchParams(location.search).get('ply');
    if (requestedPly !== null && /^\d+$/.test(requestedPly)) {
      const cursor = Math.min(Number(requestedPly), state.history.length);
      if (!navigateState(state, cursor)) throw new Error(api.error());
      state = readState();
    }
    elements.source.hidden = false;
    elements.source.replaceChildren();
    elements.source.append('Loaded ');
    const link = document.createElement('a');
    link.href = `/games/${encodeURIComponent(game.id)}`;
    link.textContent = `${game.white} vs ${game.black}`;
    elements.source.append(link, '.');
    restartAnalysis();
  }

  elements.toggle.addEventListener('change', () => {
    if (elements.toggle.checked) {
      message = 'Ready';
      scheduleAnalysis();
    } else {
      cancelAnalysis('Paused');
      render();
    }
  });
  elements.depth.addEventListener('change', restartAnalysis);
  elements.flip.addEventListener('click', () => {
    orientation = orientation === 'white' ? 'black' : 'white';
    ground.toggleOrientation();
  });
  elements.reset.addEventListener('click', () => {
    api.reset();
    state = readState();
    rootFen = state.fen;
    elements.source.hidden = true;
    changedPosition();
  });
  elements.load.addEventListener('click', () => loadRoot(elements.fen.value.trim()));
  elements.copy.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(elements.fen.value);
      elements.copy.textContent = 'Copied';
      setTimeout(() => { elements.copy.textContent = 'Copy'; }, 1000);
    } catch {
      elements.fen.select();
    }
  });
  elements.start.addEventListener('click', () => navigateTo(0));
  elements.back.addEventListener('click', () => navigateTo(state.historyCursor - 1));
  elements.forward.addEventListener('click', () => navigateTo(state.historyCursor + 1));
  elements.end.addEventListener('click', () => navigateTo(state.history.length));
  document.addEventListener('keydown', (event) => {
    if (shouldIgnoreArrowKey(event) || elements.promotionDialog.open) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      navigateTo(state.historyCursor + (event.key === 'ArrowLeft' ? -1 : 1));
    }
  });
  window.addEventListener('pagehide', () => analysisEngine.close());
  try {
    await analysisEngine.start();
    available = true;
    message = 'Ready';
  } catch (error) {
    elements.error.textContent = error instanceof Error ? error.message : String(error);
    message = 'Engine unavailable';
  }
  render();
  const savedGame = new URLSearchParams(location.search).get('game');
  if (savedGame) {
    try {
      await loadSavedGame(savedGame);
    } catch (error) {
      elements.error.textContent = error instanceof Error ? error.message : String(error);
    }
  } else {
    scheduleAnalysis();
  }
}

if (document.body.dataset.page === 'play') {
  await initializePlay();
} else if (document.body.dataset.page === 'games') {
  await initializeGames();
} else if (document.body.dataset.page === 'analysis') {
  await initializeAnalysis();
}
