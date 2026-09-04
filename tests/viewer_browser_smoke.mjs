// Optional real-browser integration test. Requires Playwright and Chromium.
// ZFS_PLAYWRIGHT_MODULE and ZFS_CHROMIUM_PATH can select an existing installation.
// ZFS_VIEWER_URL may point to a Pages preview or production: every game API
// request is intercepted in the browser, so these fixtures never reach D1.
import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import createZfsModule from '../viewer/src/generated/zfs.js';
import { validateSavedGame, savedGameSummary } from '../viewer/shared/saved-games.mjs';

const { chromium } = await import(process.env.ZFS_PLAYWRIGHT_MODULE || 'playwright');
const base = process.env.ZFS_VIEWER_URL || 'http://127.0.0.1:4187';
const output = fileURLToPath(new URL('../.runtime/design-browser/', import.meta.url));
await mkdir(output, { recursive: true });
const wasm = await createZfsModule();
const api = {
  reset: wasm.cwrap('zfs_reset', null, []),
  load: wasm.cwrap('zfs_load', 'number', ['string']),
  play: wasm.cwrap('zfs_play', 'number', ['string']),
  state: wasm.cwrap('zfs_state_json', 'string', []),
};
const state = () => JSON.parse(api.state());
api.reset();
const rootFen = state().fen;
const fixtureMoves = ['e2e4', 'e7e5', 'g1f3'];
fixtureMoves.forEach(move => assert.equal(api.play(move), 1));
const fixture = validateSavedGame('browser-fixture', {
  rootFen, finalFen: state().fen, moves: fixtureMoves, turn: state().turn,
  humanColor: 'white', depth: 9, createdAt: 1788497939681, terminal: 'resignation',
}, 1788497975434);
const games = new Map([[fixture.id, fixture]]);
const errors = [];
const browser = await chromium.launch({
  headless: true,
  ...(process.env.ZFS_CHROMIUM_PATH ? { executablePath: process.env.ZFS_CHROMIUM_PATH } : {}),
});

async function contextFor(width, height = 844) {
  const context = await browser.newContext({
    viewport: { width, height }, deviceScaleFactor: 1,
    isMobile: width < 800, hasTouch: true, serviceWorkers: 'block',
  });
  await context.route('**/api/**', async route => {
    const request = route.request();
    const url = new URL(request.url());
    let body;
    if (url.pathname === '/api/games' && request.method() === 'GET') {
      body = { games: [...games.values()].map(savedGameSummary), nextCursor: null };
    } else if (url.pathname.startsWith('/api/games/')) {
      const id = decodeURIComponent(url.pathname.slice('/api/games/'.length));
      if (request.method() === 'PUT') {
        games.set(id, validateSavedGame(id, request.postDataJSON()));
      } else {
        assert.equal(request.method(), 'GET');
      }
      body = { game: games.get(id) };
    } else {
      errors.push(`Unexpected API request: ${url.pathname}`);
      return route.abort();
    }
    await route.fulfill({ json: body });
  });
  const page = await context.newPage();
  page.on('pageerror', error => errors.push(error.message));
  page.on('dialog', dialog => dialog.accept());
  return { context, page };
}

async function ready(page, path, selector) {
  await page.goto(base + path);
  await page.locator(selector).first().waitFor();
}

async function noOverflow(page, name) {
  const size = await page.evaluate(() => ({
    document: document.documentElement.scrollWidth,
    viewport: document.documentElement.clientWidth,
  }));
  assert.ok(size.document <= size.viewport + 1, `${name}: horizontal overflow ${JSON.stringify(size)}`);
}

async function boardPosition(page, id) {
  return page.locator(`${id} cg-board`).evaluate(el => {
    const box = el.getBoundingClientRect();
    return { x: box.x + scrollX, y: box.y + scrollY, width: box.width, height: box.height, scrollY };
  });
}

async function squarePoint(page, id, square) {
  const box = await page.locator(`${id} cg-board`).boundingBox();
  return { x: box.x + (square.charCodeAt(0) - 97 + .5) * box.width / 8,
    y: box.y + (8 - Number(square[1]) + .5) * box.height / 8 };
}

async function move(page, id, uci, drag = false) {
  const from = await squarePoint(page, id, uci.slice(0, 2));
  const to = await squarePoint(page, id, uci.slice(2, 4));
  if (!drag) {
    await page.touchscreen.tap(from.x, from.y);
    await page.touchscreen.tap(to.x, to.y);
    return;
  }
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [from] });
  for (let i = 1; i <= 8; i++) {
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{
      x: from.x + (to.x - from.x) * i / 8,
      y: from.y + (to.y - from.y) * i / 8,
    }] });
  }
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await cdp.detach();
}

try {
  for (const width of [320, 390, 768, 1440]) {
    const { context, page } = await contextFor(width, width === 1440 ? 1000 : 844);
    for (const [name, path, selector] of [
      ['landing', '/', '.landing-mark'],
      ['setup', '/play', '#start-game:not([disabled])'],
      ['analysis', '/analysis', '#analysis-toggle:not([disabled])'],
      ['archive', '/games', '.archive-card'],
      ['replay', `/games/${fixture.id}`, '#detail-content:not([hidden])'],
    ]) {
      await ready(page, path, selector);
      await noOverflow(page, `${name} ${width}`);
      await page.screenshot({ path: `${output}/${name}-${width}.png`, fullPage: true });
    }
    await context.close();
  }

  const { context, page } = await contextFor(390);
  await ready(page, '/analysis', '#analysis-toggle:not([disabled])');
  assert.equal(await page.locator('#analysis-depth').inputValue(), '9');
  await page.locator('#analysis-engine-status').filter({ hasText: 'Complete' }).waitFor();
  const initial = await boardPosition(page, '#analysis-board');
  await move(page, '#analysis-board', 'g1f3');
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 1');
  await page.locator('#analysis-engine-status').filter({ hasText: 'Complete' }).waitFor();
  await move(page, '#analysis-board', 'b8c6', true);
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '2 / 2');
  const after = await boardPosition(page, '#analysis-board');
  assert.deepEqual(after, initial, 'analysis tap/drag input shifted the board or viewport');
  assert.equal(await page.locator('.history-row').count(), 1);
  assert.deepEqual(await page.locator('.history-move').allTextContents(), ['Nf3', 'Nc6']);
  await page.locator('#analysis-back').tap();
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 2');
  await page.keyboard.press('ArrowLeft');
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '0 / 2');
  await page.locator('.engine-switch').tap();
  assert.equal(await page.locator('#analysis-toggle').isChecked(), false);
  await page.locator('.position-tools summary').tap();
  await page.locator('#analysis-fen').fill('4k3/8/8/8/8/8/8/4K1N1 b - - 0 17 -');
  await page.locator('#analysis-load').tap();
  await page.evaluate(() => scrollTo(0, 0));
  await move(page, '#analysis-board', 'e8e7');
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 1');
  assert.equal(await page.locator('.move-number').first().textContent(), '17.');
  assert.equal(await page.locator('.history-move').getAttribute('aria-label'), '17. Black: Ke7');
  await page.locator('#analysis-fen').fill('4k3/P7/8/8/8/8/8/4K3 w - - 0 1 -');
  await page.locator('#analysis-load').tap();
  await page.evaluate(() => scrollTo(0, 0));
  await move(page, '#analysis-board', 'a7a8');
  await page.locator('#analysis-promotion-dialog[open]').waitFor();
  await page.getByRole('button', { name: 'Knight', exact: true }).tap();
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 1');
  assert.match(await page.locator('.history-move').textContent(), /N/);
  await page.locator('#analysis-fen').fill('7k/8/8/8/8/8/8/R3K3 w - - 0 1 a3');
  await page.locator('#analysis-load').tap();
  await page.evaluate(() => scrollTo(0, 0));
  const rook = await squarePoint(page, '#analysis-board', 'a1');
  await page.touchscreen.tap(rook.x, rook.y);
  await page.locator('square.follow-field.move-dest').waitFor();
  const background = await page.locator('square.follow-field.move-dest').evaluate(el => getComputedStyle(el).backgroundImage);
  assert.equal((background.match(/radial-gradient/g) || []).length, 2);
  await page.screenshot({ path: `${output}/follow-destination-phone.png`, fullPage: true });

  await ready(page, '/play', '#start-game:not([disabled])');
  assert.equal(await page.locator('#engine-depth').inputValue(), '9');
  await page.locator('label[for="color-white"]').tap();
  await page.locator('#start-game').tap();
  const playInitial = await boardPosition(page, '#board');
  await move(page, '#board', 'g1f3');
  await page.waitForFunction(() => document.querySelector('#ply-count').textContent === '2 / 2');
  assert.deepEqual(await boardPosition(page, '#board'), playInitial, 'play tap shifted board');
  const played = [...games.values()].find(game => game.id !== fixture.id && game.moves.length === 2);
  assert.ok(played, 'real browser moves were not saved');
  api.load(played.rootFen);
  played.moves.forEach(uci => assert.equal(api.play(uci), 1));
  const legal = state().legalMoves.find(uci => uci.length === 4);
  await move(page, '#board', legal, true);
  await page.waitForFunction(() => document.querySelector('#ply-count').textContent === '4 / 4');
  assert.deepEqual(await boardPosition(page, '#board'), playInitial, 'play drag shifted board');
  await page.screenshot({ path: `${output}/playing-phone.png`, fullPage: true });
  await page.locator('#resign').tap();
  await page.locator('#position-card.terminal').waitFor();
  await page.locator('#review-game:not([hidden])').tap();
  await page.locator('#detail-content:not([hidden])').waitFor();
  assert.match(await page.locator('#detail-outcome').textContent(), /Black wins · Resignation/);
  await page.locator('#replay-start').tap();
  await page.waitForFunction(() => document.querySelector('#replay-counter').textContent === '0 / 4');
  await page.locator('#replay-forward').tap();
  await page.locator('#replay-flip').tap();
  assert.equal(await page.locator('#replay-bottom-color').textContent(), 'Black');
  assert.match(await page.locator('#analyze-game').getAttribute('href'), /&ply=1$/);
  await page.locator('#analyze-game').tap();
  await page.locator('#analysis-toggle:not([disabled])').waitFor();
  await page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 4');
  await page.locator('#analysis-engine-status').filter({ hasText: 'Complete' }).waitFor();
  assert.equal(await page.locator('#analysis-depth-readout').textContent(), 'Depth 9');
  await page.locator('#analysis-depth').selectOption('8');
  await page.waitForFunction(() => document.querySelector('#analysis-engine-status').textContent === 'Complete' && document.querySelector('#analysis-depth-readout').textContent === 'Depth 8');
  assert.equal(await page.locator('#analysis-error').textContent(), '');
  await page.screenshot({ path: `${output}/analysis-history-phone.png`, fullPage: true });
  await context.close();

  const desktop = await contextFor(1440, 1000);
  await ready(desktop.page, '/analysis', '#analysis-toggle:not([disabled])');
  const from = await squarePoint(desktop.page, '#analysis-board', 'g1');
  const to = await squarePoint(desktop.page, '#analysis-board', 'f3');
  await desktop.page.mouse.move(from.x, from.y);
  await desktop.page.mouse.down();
  await desktop.page.mouse.move(to.x, to.y, { steps: 8 });
  await desktop.page.mouse.up();
  await desktop.page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 1');
  await desktop.page.keyboard.press('ArrowLeft');
  await desktop.page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '0 / 1');
  await desktop.page.keyboard.press('ArrowRight');
  await desktop.page.waitForFunction(() => document.querySelector('#analysis-ply-count').textContent === '1 / 1');
  await desktop.page.locator('#analysis-engine-status').filter({ hasText: 'Complete' }).waitFor();
  await noOverflow(desktop.page, 'desktop mouse/keyboard');
  await desktop.context.close();
  assert.deepEqual(errors, [], 'browser exceptions');
  console.log('PASS: four viewport sizes; tap/drag play and analysis; no board shift; saves/resignation/replay; history-aware analysis handoff; promotion; follow destination; depth and toggle. Game APIs intercepted.');
} finally {
  await browser.close();
}
