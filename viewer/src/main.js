import { Chessground } from '@lichess-org/chessground';
import '@lichess-org/chessground/assets/chessground.base.css';
import '@lichess-org/chessground/assets/chessground.brown.css';
import '@lichess-org/chessground/assets/chessground.cburnett.css';
import createZfsModule from './generated/zfs.js';
import selfplayArchive from './selfplay-games.json';
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

function newGameId() {
  if (globalThis.crypto?.randomUUID) return globalThis.crypto.randomUUID();
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function randomColor() {
  if (globalThis.crypto?.getRandomValues) {
    const value = new Uint8Array(1);
    globalThis.crypto.getRandomValues(value);
    return value[0] & 1 ? 'white' : 'black';
  }
  return Math.random() < 0.5 ? 'white' : 'black';
}

function sideName(color) {
  return color === 'white' ? 'White' : 'Black';
}

function terminalText(terminal, turn) {
  const labels = {
    'fifty-move': 'Draw by the 50-move rule',
    stalemate: 'Stalemate',
    threefold: 'Draw by threefold repetition',
  };
  if (terminal === 'checkmate') return `${sideName(turn)} is checkmated`;
  return labels[terminal] ?? `${sideName(turn)} to move`;
}

function terminationText(termination) {
  const labels = {
    checkmate: 'Checkmate',
    'fifty-move': '50-move draw',
    'ply-cap': `${selfplayArchive.limit.maxPlies}-ply safety cap`,
    stalemate: 'Stalemate',
    threefold: 'Threefold repetition',
  };
  return labels[termination] ?? termination;
}

function formatScore(score) {
  if (!score) return '—';
  if (score.kind === 'mate') {
    return score.value < 0
      ? `−M${Math.abs(score.value)}`
      : `+M${score.value}`;
  }
  const value = score.value / 100;
  return `${value >= 0 ? '+' : ''}${value.toFixed(2)}`;
}

function followDetail(state) {
  if (state.followForced) return `Follow ${state.follow} is compulsory`;
  if (state.follow !== '-') return `No legal follower can reach ${state.follow}`;
  return 'No active follow field';
}

function shouldIgnoreArrowKey(event) {
  if (
    event.defaultPrevented ||
    event.altKey ||
    event.ctrlKey ||
    event.metaKey ||
    event.shiftKey
  ) {
    return true;
  }
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
    animation: { enabled: true, duration: 170 },
    movable: {
      color: afterMove ? state.turn : undefined,
      free: false,
      dests: afterMove ? destinations(state.legalMoves) : new Map(),
      showDests: true,
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

async function initializePlay() {
  const elements = {
    back: document.querySelector('#back'),
    board: document.querySelector('#board'),
    bottomPlayer: document.querySelector('#bottom-player'),
    bottomPlayerMeta: document.querySelector('#bottom-player-meta'),
    bottomPlayerState: document.querySelector('#bottom-player-state'),
    cancelPromotion: document.querySelector('#cancel-promotion'),
    closeRules: document.querySelector('#close-rules'),
    copyFen: document.querySelector('#copy-fen'),
    depth: document.querySelector('#engine-depth'),
    engineDepth: document.querySelector('#engine-depth-readout'),
    engineNodes: document.querySelector('#engine-nodes'),
    enginePv: document.querySelector('#engine-pv'),
    engineScore: document.querySelector('#engine-score'),
    engineState: document.querySelector('#engine-state'),
    engineStatus: document.querySelector('#engine-status'),
    error: document.querySelector('#error'),
    fen: document.querySelector('#fen'),
    flip: document.querySelector('#flip'),
    forward: document.querySelector('#forward'),
    gameView: document.querySelector('#game-view'),
    history: document.querySelector('#history'),
    livePosition: document.querySelector('#live-position'),
    loadFen: document.querySelector('#load-fen'),
    newGame: document.querySelector('#new-game'),
    plyCount: document.querySelector('#ply-count'),
    promotionChoices: document.querySelector('#promotion-choices'),
    promotionDialog: document.querySelector('#promotion-dialog'),
    rulesButton: document.querySelector('#rules-button'),
    rulesDialog: document.querySelector('#rules-dialog'),
    setupConnection: document.querySelector('#setup-connection'),
    setupForm: document.querySelector('#setup-form'),
    setupMessage: document.querySelector('#setup-message'),
    setupView: document.querySelector('#setup-view'),
    startGame: document.querySelector('#start-game'),
    startPosition: document.querySelector('#start-position'),
    status: document.querySelector('#status'),
    statusDetail: document.querySelector('#status-detail'),
    topPlayer: document.querySelector('#top-player'),
    topPlayerMeta: document.querySelector('#top-player-meta'),
    topPlayerState: document.querySelector('#top-player-state'),
    turnDot: document.querySelector('#turn-dot'),
  };

  api.reset();
  let state = readState();
  let rootFen = state.fen;
  let ground;
  let inputLocked = false;
  let orientation = 'white';
  const engine = {
    analysis: null,
    available: false,
    controller: null,
    depth: 10,
    enabled: false,
    gameId: null,
    humanColor: 'white',
    message: 'Waiting for the engine service',
    ponderMove: null,
    requestSerial: 0,
    thinking: false,
  };

  function setError(message = '') {
    elements.error.textContent = message;
  }

  function ensureBoard() {
    if (!ground) ground = createBoard(elements.board, state, onBoardMove);
    return ground;
  }

  function canHumanMove() {
    return !inputLocked &&
      !engine.thinking &&
      state.terminal === 'ongoing' &&
      (!engine.enabled || state.turn === engine.humanColor);
  }

  function renderPlayers() {
    const topColor = engine.humanColor === 'white' ? 'black' : 'white';
    const bottomColor = engine.humanColor;
    const setPlayer = (name, meta, status, color) => {
      const human = color === engine.humanColor;
      name.textContent = human ? 'You' : 'Kugelfisch';
      meta.textContent = human
        ? sideName(color)
        : `${sideName(color)} · depth ${engine.depth}`;
      if (state.terminal !== 'ongoing') status.textContent = 'Finished';
      else if (state.turn !== color) status.textContent = 'Waiting';
      else if (!human && engine.thinking) status.textContent = 'Thinking';
      else if (human && engine.ponderMove) status.textContent = 'Pondering';
      else status.textContent = 'To move';
      status.classList.toggle('active', state.turn === color);
    };
    setPlayer(
      elements.topPlayer,
      elements.topPlayerMeta,
      elements.topPlayerState,
      topColor,
    );
    setPlayer(
      elements.bottomPlayer,
      elements.bottomPlayerMeta,
      elements.bottomPlayerState,
      bottomColor,
    );
  }

  function renderHistory() {
    elements.plyCount.textContent =
      `${state.historyCursor} / ${state.history.length}`;
    elements.history.replaceChildren();
    elements.history.classList.toggle('empty', state.history.length === 0);
    if (state.history.length === 0) {
      elements.history.textContent = 'No moves yet';
    } else {
      let currentMove;
      state.history.forEach((move, index) => {
        const item = document.createElement('button');
        item.type = 'button';
        item.className = 'history-move';
        if (index >= state.historyCursor) item.classList.add('future');
        if (index === state.historyCursor - 1) {
          item.classList.add('current');
          currentMove = item;
        }
        item.textContent = `${index + 1}. ${move}`;
        item.addEventListener('click', () => navigateTo(index + 1));
        elements.history.append(item);
      });
      currentMove?.scrollIntoView({ block: 'nearest' });
    }
    elements.startPosition.disabled = inputLocked || state.historyCursor === 0;
    elements.back.disabled = inputLocked || state.historyCursor === 0;
    elements.forward.disabled =
      inputLocked || state.historyCursor === state.history.length;
    elements.livePosition.disabled =
      inputLocked || state.historyCursor === state.history.length;
  }

  function renderEngine() {
    elements.engineState.textContent = engine.available ? 'Online' : 'Offline';
    elements.engineState.className =
      `connection ${engine.available ? 'online' : 'offline'}`;
    elements.engineStatus.textContent = engine.message;
    elements.engineScore.textContent = formatScore(engine.analysis?.score);
    elements.engineDepth.textContent = engine.analysis?.depth === undefined
      ? 'Depth —'
      : `Depth ${engine.analysis.depth}`;
    elements.engineNodes.textContent = engine.analysis?.nodes === undefined
      ? '— nodes'
      : `${engine.analysis.nodes.toLocaleString()} nodes`;
    elements.enginePv.textContent = engine.analysis?.pv?.length
      ? engine.analysis.pv.join(' ')
      : 'No analysis yet';
  }

  function render() {
    if (!elements.gameView.hidden) {
      const board = ensureBoard();
      const lastMove = state.history[state.historyCursor - 1];
      const humanMayMove = canHumanMove();
      board.set({
        fen: state.board,
        orientation,
        turnColor: state.turn,
        check: state.inCheck ? state.turn : false,
        lastMove: lastMove ? [lastMove.slice(0, 2), lastMove.slice(2, 4)] : [],
        movable: {
          color: humanMayMove ? state.turn : undefined,
          dests: humanMayMove ? destinations(state.legalMoves) : new Map(),
        },
        drawable: {
          autoShapes: state.follow === '-'
            ? []
            : [{ orig: state.follow, brush: 'yellow' }],
        },
      });
    }

    elements.fen.value = state.fen;
    elements.turnDot.className = `turn-dot ${state.turn}`;
    const side = sideName(state.turn);
    if (state.terminal !== 'ongoing') {
      elements.status.textContent = terminalText(state.terminal, state.turn);
    } else if (engine.thinking) {
      elements.status.textContent = `${side} to move · Kugelfisch is thinking`;
    } else {
      elements.status.textContent = `${side} to move`;
    }
    elements.statusDetail.textContent = state.inCheck
      ? `Check · ${followDetail(state)}`
      : followDetail(state);
    renderPlayers();
    renderHistory();
    renderEngine();
  }

  function sync() {
    state = readState();
    render();
  }

  async function connectEngine() {
    try {
      const response = await fetch('/api/engine/status', { cache: 'no-store' });
      if (!response.ok) {
        throw new Error(`engine service returned HTTP ${response.status}`);
      }
      const status = await response.json();
      engine.available = status.available === true;
      if (Number.isInteger(status.defaultDepth)) {
        engine.depth = status.defaultDepth;
        const option = [...elements.depth.options].find(
          (candidate) => Number(candidate.value) === status.defaultDepth,
        );
        if (option) elements.depth.value = option.value;
      }
      engine.message = engine.available
        ? `Ready at depth ${engine.depth}`
        : 'Native engine is unavailable';
    } catch (error) {
      engine.available = false;
      engine.message = error instanceof Error ? error.message : String(error);
    }
    elements.startGame.disabled = !engine.available;
    elements.setupConnection.textContent = engine.available ? 'Online' : 'Offline';
    elements.setupConnection.className =
      `connection ${engine.available ? 'online' : 'offline'}`;
    elements.setupMessage.textContent = engine.available
      ? 'The server engine is ready.'
      : engine.message;
    renderEngine();
  }

  function cancelSearch() {
    ++engine.requestSerial;
    engine.controller?.abort();
    engine.controller = null;
    engine.thinking = false;
  }

  function notifyEngineStop(gameId) {
    if (!gameId) return;
    void fetch('/api/engine/stop', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ gameId }),
      keepalive: true,
    }).catch(() => {});
  }

  function stopEngineGame(message = 'Engine game stopped') {
    const gameId = engine.gameId;
    cancelSearch();
    notifyEngineStop(gameId);
    engine.enabled = false;
    engine.ponderMove = null;
    engine.message = message;
    render();
  }

  function beginGame(event) {
    event.preventDefault();
    if (!engine.available) return;
    const requestedColor = new FormData(elements.setupForm).get('color');
    const depth = Number(elements.depth.value);
    if (!Number.isInteger(depth) || depth < 1 || depth > 100) {
      elements.setupMessage.textContent = 'Choose a valid search depth.';
      return;
    }

    api.reset();
    state = readState();
    rootFen = state.fen;
    engine.depth = depth;
    engine.humanColor = requestedColor === 'random'
      ? randomColor()
      : requestedColor;
    orientation = engine.humanColor;
    engine.analysis = null;
    engine.enabled = true;
    engine.gameId = newGameId();
    engine.ponderMove = null;
    engine.message = state.turn === engine.humanColor
      ? 'Your move'
      : `Thinking to depth ${engine.depth}`;
    elements.setupView.hidden = true;
    elements.gameView.hidden = false;
    document.body.classList.add('active-game');
    setError();
    render();
    void maybeStartEngineMove();
  }

  function returnToSetup() {
    stopEngineGame('Ready for a new game');
    api.reset();
    state = readState();
    rootFen = state.fen;
    engine.analysis = null;
    elements.gameView.hidden = true;
    elements.setupView.hidden = false;
    document.body.classList.remove('active-game');
    elements.setupMessage.textContent = 'The server engine is ready.';
    window.scrollTo({ top: 0, behavior: 'smooth' });
  }

  async function readEngineEvents(response, serial) {
    if (!response.body) throw new Error('engine response has no body');
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let pending = '';
    let bestMove;
    let ponderMove;

    const consume = (line) => {
      if (line.trim() === '') return;
      const event = JSON.parse(line);
      if (event.type === 'error') {
        throw new Error(event.message || 'engine search failed');
      }
      if (event.type === 'bestmove') bestMove = event.move;
      if (event.type === 'ponder') ponderMove = event.move;
      if (event.type === 'ponderhit' && serial === engine.requestSerial) {
        engine.message = 'Ponder hit · finishing the reply';
        renderEngine();
      }
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
    return { bestMove, ponderMove };
  }

  async function maybeStartEngineMove() {
    if (
      !engine.enabled ||
      engine.thinking ||
      state.terminal !== 'ongoing' ||
      state.turn === engine.humanColor
    ) {
      if (engine.enabled && state.terminal !== 'ongoing') {
        notifyEngineStop(engine.gameId);
        engine.enabled = false;
        engine.ponderMove = null;
        engine.message = 'Game over';
      } else if (engine.enabled && !engine.thinking) {
        engine.message = engine.ponderMove
          ? `Your move · pondering ${engine.ponderMove}`
          : 'Your move';
      }
      render();
      return;
    }

    const controller = new AbortController();
    const serial = ++engine.requestSerial;
    engine.controller = controller;
    engine.thinking = true;
    engine.ponderMove = null;
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
        throw new Error(
          body.error || `engine service returned HTTP ${response.status}`,
        );
      }
      const { bestMove, ponderMove } = await readEngineEvents(response, serial);
      if (serial !== engine.requestSerial || !engine.enabled) return;
      if (!bestMove || bestMove === '0000') {
        throw new Error('engine returned no move for an ongoing position');
      }
      if (!api.play(bestMove)) {
        throw new Error(`engine returned illegal move ${bestMove}: ${api.error()}`);
      }

      engine.controller = null;
      engine.thinking = false;
      engine.ponderMove = ponderMove ?? null;
      engine.message = engine.ponderMove
        ? `Your move · pondering ${engine.ponderMove}`
        : 'Your move';
      setError();
      sync();
      void maybeStartEngineMove();
    } catch (error) {
      if (serial !== engine.requestSerial) return;
      engine.controller = null;
      engine.thinking = false;
      if (error instanceof DOMException && error.name === 'AbortError') return;
      engine.enabled = false;
      engine.ponderMove = null;
      engine.message = error instanceof Error ? error.message : String(error);
      setError(`Engine stopped: ${engine.message}`);
      render();
    }
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

  function commitMove(move) {
    if (!canHumanMove()) return;
    if (!api.play(move)) {
      setError(api.error());
      sync();
      return;
    }
    engine.ponderMove = null;
    setError();
    sync();
    void maybeStartEngineMove();
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

  function navigateTo(cursor) {
    if (inputLocked || cursor < 0 || cursor > state.history.length) return;
    if (engine.enabled) stopEngineGame('Engine stopped for move review');
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
    setError(accepted ? '' : api.error());
    state = readState();
    inputLocked = false;
    render();
  }

  elements.setupForm.addEventListener('submit', beginGame);
  elements.newGame.addEventListener('click', returnToSetup);
  elements.flip.addEventListener('click', () => {
    orientation = orientation === 'white' ? 'black' : 'white';
    ensureBoard().toggleOrientation();
  });
  elements.back.addEventListener('click', () => navigateTo(state.historyCursor - 1));
  elements.forward.addEventListener('click', () => navigateTo(state.historyCursor + 1));
  elements.startPosition.addEventListener('click', () => navigateTo(0));
  elements.livePosition.addEventListener('click', () => navigateTo(state.history.length));
  elements.rulesButton.addEventListener('click', () => elements.rulesDialog.showModal());
  elements.closeRules.addEventListener('click', () => elements.rulesDialog.close());
  elements.cancelPromotion.addEventListener('click', () => elements.promotionDialog.close());

  elements.loadFen.addEventListener('click', () => {
    const fen = elements.fen.value.trim();
    const oldGameId = engine.gameId;
    cancelSearch();
    notifyEngineStop(oldGameId);
    if (!api.load(fen)) {
      setError(api.error());
      elements.fen.value = fen;
      return;
    }
    state = readState();
    rootFen = state.fen;
    engine.analysis = null;
    engine.enabled = true;
    engine.gameId = newGameId();
    engine.ponderMove = null;
    engine.message = state.turn === engine.humanColor
      ? 'Your move'
      : `Thinking to depth ${engine.depth}`;
    setError();
    render();
    void maybeStartEngineMove();
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
    if (shouldIgnoreArrowKey(event) || elements.promotionDialog.open) return;
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      navigateTo(state.historyCursor - 1);
    } else if (event.key === 'ArrowRight') {
      event.preventDefault();
      navigateTo(state.historyCursor + 1);
    }
  });

  window.addEventListener('pagehide', () => {
    if (!engine.gameId) return;
    const body = new Blob([JSON.stringify({ gameId: engine.gameId })], {
      type: 'application/json',
    });
    navigator.sendBeacon?.('/api/engine/stop', body);
  });

  await connectEngine();
}

function initializeGames() {
  const elements = {
    archiveCount: document.querySelector('#archive-count'),
    archiveFollow: document.querySelector('#archive-follow'),
    archiveGame: document.querySelector('#archive-game'),
    archiveIndex: document.querySelector('#archive-index'),
    archiveMeta: document.querySelector('#archive-meta'),
    archiveNext: document.querySelector('#archive-next'),
    archivePrevious: document.querySelector('#archive-previous'),
    archiveRestart: document.querySelector('#archive-restart'),
    archiveStatus: document.querySelector('#archive-status'),
    archiveSummary: document.querySelector('#archive-summary'),
    back: document.querySelector('#back'),
    board: document.querySelector('#board'),
    endPosition: document.querySelector('#end-position'),
    error: document.querySelector('#error'),
    flip: document.querySelector('#flip'),
    forward: document.querySelector('#forward'),
    history: document.querySelector('#history'),
    plyCount: document.querySelector('#ply-count'),
    startPosition: document.querySelector('#start-position'),
  };

  if (selfplayArchive.schema !== 1 || !selfplayArchive.games.length) {
    throw new Error('the bundled self-play archive is invalid');
  }
  api.reset();
  let state = readState();
  let activeGame = selfplayArchive.games[0];
  let inputLocked = false;
  const ground = createBoard(elements.board, state);

  function selectedIndex() {
    const index = selfplayArchive.games.findIndex(
      (game) => game.id === elements.archiveGame.value,
    );
    return index < 0 ? 0 : index;
  }

  function renderMeta() {
    const game = activeGame;
    const heading = document.createElement('div');
    heading.className = 'archive-result';
    heading.textContent = `${game.result} · ${terminationText(game.termination)}`;
    const players = document.createElement('p');
    players.textContent = `White: ${game.white} · Black: ${game.black}`;
    const details = document.createElement('p');
    details.textContent =
      `Pair ${game.pair}, game ${game.game}, opening ${game.openingLine} · ` +
      `${game.plies} plies · champion ${game.candidateOutcome}`;
    const work = document.createElement('p');
    work.textContent =
      `Recorded nodes — champion ${game.candidateNodes.toLocaleString()}, ` +
      `ZFS-0 ${game.baselineNodes.toLocaleString()}`;
    elements.archiveMeta.replaceChildren(heading, players, details, work);
  }

  function renderHistory() {
    elements.plyCount.textContent =
      `${state.historyCursor} / ${state.history.length}`;
    elements.history.replaceChildren();
    elements.history.classList.toggle('empty', state.history.length === 0);
    let currentMove;
    state.history.forEach((move, index) => {
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'history-move';
      if (index >= state.historyCursor) item.classList.add('future');
      if (index === state.historyCursor - 1) {
        item.classList.add('current');
        currentMove = item;
      }
      item.textContent = `${index + 1}. ${move}`;
      item.addEventListener('click', () => navigateTo(index + 1));
      elements.history.append(item);
    });
    currentMove?.scrollIntoView({ block: 'nearest' });
  }

  function render() {
    const lastMove = state.history[state.historyCursor - 1];
    ground.set({
      fen: state.board,
      turnColor: state.turn,
      check: state.inCheck ? state.turn : false,
      lastMove: lastMove ? [lastMove.slice(0, 2), lastMove.slice(2, 4)] : [],
      movable: { color: undefined, dests: new Map() },
      drawable: {
        autoShapes: state.follow === '-'
          ? []
          : [{ orig: state.follow, brush: 'yellow' }],
      },
    });
    elements.archiveStatus.textContent = state.terminal === 'ongoing'
      ? `${sideName(state.turn)} to move`
      : terminalText(state.terminal, state.turn);
    elements.archiveFollow.textContent = state.inCheck
      ? `Check · ${followDetail(state)}`
      : followDetail(state);
    const index = selectedIndex();
    elements.archiveIndex.textContent =
      `${index + 1} / ${selfplayArchive.games.length}`;
    elements.archivePrevious.disabled = index === 0 || inputLocked;
    elements.archiveNext.disabled =
      index === selfplayArchive.games.length - 1 || inputLocked;
    elements.startPosition.disabled = inputLocked || state.historyCursor === 0;
    elements.back.disabled = inputLocked || state.historyCursor === 0;
    elements.forward.disabled =
      inputLocked || state.historyCursor === state.history.length;
    elements.endPosition.disabled =
      inputLocked || state.historyCursor === state.history.length;
    renderHistory();
    renderMeta();
  }

  function loadGame(game) {
    if (inputLocked) return;
    inputLocked = true;
    api.reset();
    let failure = '';
    for (const move of game.moves) {
      if (!api.play(move)) {
        failure = `Archive move ${move} was rejected: ${api.error()}`;
        break;
      }
    }
    const completed = readState();
    if (!failure && completed.history.length !== game.plies) {
      failure = 'Archive replay produced the wrong history length.';
    }
    for (let cursor = completed.historyCursor; !failure && cursor > 0; --cursor) {
      if (!api.back()) failure = `Could not rewind archive: ${api.error()}`;
    }
    if (failure) {
      api.reset();
      elements.error.textContent = failure;
    } else {
      elements.error.textContent = '';
      activeGame = game;
      elements.archiveGame.value = game.id;
    }
    state = readState();
    inputLocked = false;
    render();
  }

  function navigateTo(cursor) {
    if (inputLocked || cursor < 0 || cursor > state.history.length) return;
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
    elements.error.textContent = accepted ? '' : api.error();
    state = readState();
    inputLocked = false;
    render();
  }

  selfplayArchive.games.forEach((game, index) => {
    const option = document.createElement('option');
    option.value = game.id;
    option.textContent =
      `${String(index + 1).padStart(2, '0')} · ${game.result} · ` +
      `${terminationText(game.termination)} · ${game.plies} plies`;
    elements.archiveGame.append(option);
  });
  const summary = selfplayArchive.summary;
  elements.archiveCount.textContent = `${summary.games} games`;
  elements.archiveSummary.textContent =
    `${selfplayArchive.description} Every move is replayed through the ` +
    'production rules core before it is shown.';

  elements.archiveGame.addEventListener('change', () => {
    loadGame(selfplayArchive.games[selectedIndex()]);
  });
  elements.archiveRestart.addEventListener('click', () => loadGame(activeGame));
  elements.archivePrevious.addEventListener('click', () => {
    loadGame(selfplayArchive.games[Math.max(0, selectedIndex() - 1)]);
  });
  elements.archiveNext.addEventListener('click', () => {
    loadGame(selfplayArchive.games[
      Math.min(selfplayArchive.games.length - 1, selectedIndex() + 1)
    ]);
  });
  elements.startPosition.addEventListener('click', () => navigateTo(0));
  elements.back.addEventListener('click', () => navigateTo(state.historyCursor - 1));
  elements.forward.addEventListener('click', () => navigateTo(state.historyCursor + 1));
  elements.endPosition.addEventListener('click', () => navigateTo(state.history.length));
  elements.flip.addEventListener('click', () => ground.toggleOrientation());
  document.addEventListener('keydown', (event) => {
    if (shouldIgnoreArrowKey(event)) return;
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      navigateTo(state.historyCursor - 1);
    } else if (event.key === 'ArrowRight') {
      event.preventDefault();
      navigateTo(state.historyCursor + 1);
    }
  });

  loadGame(activeGame);
}

if (document.body.dataset.page === 'play') {
  await initializePlay();
} else if (document.body.dataset.page === 'games') {
  initializeGames();
}
