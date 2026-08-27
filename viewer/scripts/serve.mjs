import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { startViewerServer } from './server-lib.mjs';

const host = '127.0.0.1';
const port = Number.parseInt(process.env.ZFS_VIEWER_PORT ?? '4173', 10);
const viewerDirectory = fileURLToPath(new URL('../', import.meta.url));
const projectDirectory = path.resolve(viewerDirectory, '..');
const root = path.join(viewerDirectory, 'dist');
const enginePath = process.env.ZFS_ENGINE_PATH ??
  path.join(projectDirectory, 'build', 'zfs_engine');

if (!Number.isInteger(port) || port < 0 || port > 65535) {
  throw new Error('ZFS_VIEWER_PORT must be an integer from 0 to 65535');
}

await startViewerServer({ root, enginePath, host, port });
