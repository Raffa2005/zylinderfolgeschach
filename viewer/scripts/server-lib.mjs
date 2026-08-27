import { constants as fsConstants, createReadStream } from 'node:fs';
import { access, stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import path from 'node:path';

import { UciEngineClient } from './engine-client.mjs';

export const DEFAULT_ENGINE_DEPTH = 10;

const contentTypes = new Map([
  ['.css', 'text/css; charset=utf-8'],
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.map', 'application/json; charset=utf-8'],
  ['.wasm', 'application/wasm'],
]);

class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function writeJson(response, status, value) {
  const body = JSON.stringify(value);
  response.writeHead(status, {
    'Cache-Control': 'no-store',
    'Content-Length': Buffer.byteLength(body),
    'Content-Type': 'application/json; charset=utf-8',
    'X-Content-Type-Options': 'nosniff',
  });
  response.end(body);
}

async function readJson(request) {
  const chunks = [];
  let length = 0;
  for await (const chunk of request) {
    length += chunk.length;
    if (length > 64 * 1024) throw new HttpError(413, 'request is too large');
    chunks.push(chunk);
  }
  try {
    return JSON.parse(Buffer.concat(chunks).toString('utf8'));
  } catch {
    throw new HttpError(400, 'request body must be valid JSON');
  }
}

function validateSearch(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) {
    throw new HttpError(400, 'search request must be an object');
  }
  const depth = body.depth ?? DEFAULT_ENGINE_DEPTH;
  if (!Number.isInteger(depth) || depth < 1 || depth > 100) {
    throw new HttpError(400, 'depth must be an integer from 1 to 100');
  }
  if (
    typeof body.rootFen !== 'string' ||
    body.rootFen.length === 0 ||
    body.rootFen.length > 256 ||
    /[\r\n]/.test(body.rootFen)
  ) {
    throw new HttpError(400, 'rootFen must be one bounded FEN line');
  }
  const fieldCount = body.rootFen.trim().split(/\s+/).length;
  if (fieldCount !== 6 && fieldCount !== 7) {
    throw new HttpError(400, 'rootFen must contain six or seven fields');
  }
  if (!Array.isArray(body.moves) || body.moves.length > 4096) {
    throw new HttpError(400, 'moves must be a bounded array');
  }
  if (
    typeof body.gameId !== 'string' ||
    !/^[A-Za-z0-9._-]{1,128}$/.test(body.gameId)
  ) {
    throw new HttpError(400, 'gameId must be a bounded opaque identifier');
  }
  const moves = body.moves.map((move) => {
    if (typeof move !== 'string' || !/^[a-h][1-8][a-h][1-8][qrbn]?$/.test(move)) {
      throw new HttpError(400, 'moves must contain canonical UCI moves');
    }
    return move;
  });
  return { depth, gameId: body.gameId, rootFen: body.rootFen.trim(), moves };
}

async function executableExists(enginePath) {
  try {
    await access(enginePath, fsConstants.X_OK);
    return true;
  } catch {
    return false;
  }
}

export async function startViewerServer({
  root,
  enginePath,
  host = '127.0.0.1',
  port = 4173,
  quiet = false,
}) {
  const staticRoot = path.resolve(root);
  const resolvedEngine = path.resolve(enginePath);
  const engineAvailable = await executableExists(resolvedEngine);
  const engine = engineAvailable ? new UciEngineClient(resolvedEngine) : null;
  let activeSearch;
  let closing = false;
  let searchTransition = Promise.resolve();

  const replaceSearch = async (reason) => {
    let releaseTransition;
    const previousTransition = searchTransition;
    searchTransition = new Promise((resolve) => {
      releaseTransition = resolve;
    });
    await previousTransition;
    try {
      if (closing) throw new HttpError(503, 'viewer server is closing');
      if (activeSearch) {
        activeSearch.controller.abort(new Error(reason));
        await activeSearch.done;
      }
      const controller = new AbortController();
      let finish;
      const done = new Promise((resolve) => {
        finish = resolve;
      });
      const search = { controller, done, finish };
      activeSearch = search;
      return search;
    } finally {
      releaseTransition();
    }
  };

  const server = createServer(async (request, response) => {
    try {
      const url = new URL(request.url ?? '/', `http://${host}`);
      if (url.pathname === '/api/engine/status') {
        if (request.method !== 'GET') throw new HttpError(405, 'method not allowed');
        writeJson(response, 200, {
          available: engineAvailable,
          defaultDepth: DEFAULT_ENGINE_DEPTH,
        });
        return;
      }

      if (url.pathname === '/api/engine/move') {
        if (request.method !== 'POST') throw new HttpError(405, 'method not allowed');
        if (!engineAvailable) throw new HttpError(503, 'zfs_engine is not available');
        if (!/^application\/json(?:\s*;|$)/i.test(request.headers['content-type'] ?? '')) {
          throw new HttpError(415, 'content type must be application/json');
        }
        const search = validateSearch(await readJson(request));

        const currentSearch = await replaceSearch(
          'superseded by a new engine request',
        );
        const { controller } = currentSearch;
        try {
          response.writeHead(200, {
            'Cache-Control': 'no-store',
            'Content-Type': 'application/x-ndjson; charset=utf-8',
            'X-Content-Type-Options': 'nosniff',
          });
          const sendEvent = (event) => {
            if (!response.destroyed && !response.writableEnded) {
              response.write(`${JSON.stringify(event)}\n`);
            }
          };
          response.once('close', () => {
            if (!response.writableEnded) {
              controller.abort(new Error('browser disconnected'));
            }
          });

          try {
            const result = await engine.search({
              ...search,
              signal: controller.signal,
              onInfo: (info) => sendEvent({ type: 'info', ...info }),
            });
            sendEvent({ type: 'bestmove', move: result.move });
          } catch (error) {
            if (!response.destroyed) {
              sendEvent({
                type: 'error',
                message: error instanceof Error ? error.message : String(error),
              });
            }
          }
        } finally {
          if (activeSearch === currentSearch) activeSearch = undefined;
          currentSearch.finish();
          if (!response.destroyed && !response.writableEnded) response.end();
        }
        return;
      }

      if (request.method !== 'GET' && request.method !== 'HEAD') {
        throw new HttpError(405, 'method not allowed');
      }
      const pathname = decodeURIComponent(url.pathname);
      const relative = pathname === '/' ? 'index.html' : pathname.slice(1);
      const filename = path.resolve(staticRoot, relative);
      if (filename !== staticRoot && !filename.startsWith(staticRoot + path.sep)) {
        throw new HttpError(403, 'forbidden');
      }
      const metadata = await stat(filename);
      if (!metadata.isFile()) throw new HttpError(404, 'not found');
      response.writeHead(200, {
        'Cache-Control': 'no-cache',
        'Content-Length': metadata.size,
        'Content-Type':
          contentTypes.get(path.extname(filename)) ?? 'application/octet-stream',
        'X-Content-Type-Options': 'nosniff',
      });
      if (request.method === 'HEAD') response.end();
      else createReadStream(filename).pipe(response);
    } catch (error) {
      if (response.headersSent) {
        if (!response.writableEnded) response.end();
        return;
      }
      const status = error instanceof HttpError
        ? error.status
        : error?.code === 'ENOENT'
          ? 404
          : 500;
      const message = error instanceof Error ? error.message : 'not found';
      writeJson(response, status, { error: message });
    }
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  const address = server.address();
  if (address === null || typeof address === 'string') {
    throw new Error('viewer server did not acquire a TCP address');
  }
  const url = `http://${host}:${address.port}`;
  if (!quiet) {
    console.log(`ZFS viewer: ${url}`);
    console.log(engineAvailable
      ? `Engine: ${resolvedEngine} (default depth ${DEFAULT_ENGINE_DEPTH})`
      : `Engine unavailable: ${resolvedEngine}`);
  }

  return {
    engineAvailable,
    server,
    url,
    async close() {
      closing = true;
      activeSearch?.controller.abort(new Error('viewer server is closing'));
      const closed = new Promise((resolve, reject) => {
        server.close((error) => error ? reject(error) : resolve());
      });
      server.closeIdleConnections?.();
      await closed;
      await engine?.close();
    },
  };
}
