import { build } from 'esbuild';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const viewerDirectory = fileURLToPath(new URL('../', import.meta.url));
const outputDirectory = path.join(viewerDirectory, 'dist');
const generatedModule = await readFile(
  path.join(viewerDirectory, 'src/generated/zfs.js'),
);
const generatedEngine = await readFile(
  path.join(viewerDirectory, 'src/generated/kugelfisch-engine.js'),
);
const generatedModuleHash = createHash('sha256')
  .update(generatedModule)
  .digest('hex');
const generatedEngineHash = createHash('sha256')
  .update(generatedEngine)
  .digest('hex');

await mkdir(outputDirectory, { recursive: true });
await build({
  banner: { js: `/* zfs-wasm-sha256:${generatedModuleHash}; kugelfisch-engine-wasm-sha256:${generatedEngineHash} */` },
  entryPoints: {
    app: path.join(viewerDirectory, 'src/main.js'),
    'engine-worker': path.join(viewerDirectory, 'src/engine-worker.js'),
  },
  bundle: true,
  format: 'esm',
  legalComments: 'eof',
  minify: true,
  outdir: outputDirectory,
  platform: 'browser',
  sourcemap: true,
  target: 'es2022',
});

for (const [source, destination] of [
  ['index.html', 'index.html'],
  ['play.html', 'play/index.html'],
  ['games.html', 'games/index.html'],
  ['analysis.html', 'analysis/index.html'],
  ['mark.svg', 'mark.svg'],
  ['_redirects', '_redirects'],
  ['_routes.json', '_routes.json'],
]) {
  const output = path.join(outputDirectory, destination);
  await mkdir(path.dirname(output), { recursive: true });
  await writeFile(
    output,
    await readFile(path.join(viewerDirectory, source), 'utf8'),
  );
}
