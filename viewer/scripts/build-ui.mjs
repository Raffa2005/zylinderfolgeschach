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
const generatedModuleHash = createHash('sha256')
  .update(generatedModule)
  .digest('hex');

await mkdir(outputDirectory, { recursive: true });
await build({
  banner: { js: `/* zfs-wasm-sha256:${generatedModuleHash} */` },
  entryPoints: [path.join(viewerDirectory, 'src/main.js')],
  bundle: true,
  format: 'esm',
  legalComments: 'eof',
  minify: true,
  outfile: path.join(outputDirectory, 'app.js'),
  platform: 'browser',
  sourcemap: true,
  target: 'es2022',
});

await build({
  entryPoints: [path.join(viewerDirectory, 'src/site.js')],
  bundle: true,
  format: 'esm',
  legalComments: 'eof',
  minify: true,
  outfile: path.join(outputDirectory, 'site.js'),
  platform: 'browser',
  sourcemap: true,
  target: 'es2022',
});

for (const [source, destination] of [
  ['index.html', 'index.html'],
  ['play.html', 'play/index.html'],
  ['games.html', 'games/index.html'],
  ['rules.html', 'rules/index.html'],
]) {
  const output = path.join(outputDirectory, destination);
  await mkdir(path.dirname(output), { recursive: true });
  await writeFile(
    output,
    await readFile(path.join(viewerDirectory, source), 'utf8'),
  );
}
