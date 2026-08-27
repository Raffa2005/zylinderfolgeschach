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

const sourceHtml = await readFile(
  path.join(viewerDirectory, 'index.html'),
  'utf8',
);
const outputHtml = sourceHtml.replace(
  '    <script type="module" src="/src/main.js"></script>',
  '    <link rel="stylesheet" href="/app.css" />\n' +
    '    <script type="module" src="/app.js"></script>',
);
if (outputHtml === sourceHtml) {
  throw new Error('index.html entry script was not found');
}
await writeFile(path.join(outputDirectory, 'index.html'), outputHtml);
