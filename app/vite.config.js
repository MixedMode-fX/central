// The app's build. One page in, one page out.
//
// `vite build` writes emulator/dist/index.html: the page with every module,
// every stylesheet and the WebAssembly module inlined, so it opens from a
// download with nothing serving it - the CI artifact, and what Pages
// publishes. `vite` serves the source tree with hot reload, reaching the
// module at emulator/dist/mmmc.wasm, which emulator/build.sh writes.
//
// The tests are vitest over the same source: they drive the real module in
// Node with no browser, so the environment is `node` and stylesheets are
// stubbed rather than parsed.

import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..');

export default defineConfig({
  root: here,
  base: './',
  resolve: {
    // The module, by name rather than by a relative path that says where the
    // emulator's build output happens to be.
    alias: { '@module': resolve(repo, 'emulator', 'dist') },
  },
  server: {
    port: 5173,
    strictPort: true,
    // The module is built outside app/, so the dev server is allowed to
    // reach it.
    fs: { allow: [repo] },
  },
  build: {
    outDir: resolve(repo, 'emulator', 'dist'),
    // mmmc.wasm is already there: the app is built beside it, not over it.
    emptyOutDir: false,
    target: 'es2022',
    // Everything inlined, the module included: the built page is one file.
    assetsInlineLimit: () => true,
    minify: false,
  },
  plugins: [viteSingleFile({ removeViteModuleLoader: true })],
  test: {
    environment: 'node',
    css: false,
    include: ['test/**/*.test.mjs'],
    // A test runs the module for tens of seconds of simulated time.
    testTimeout: 60_000,
    hookTimeout: 60_000,
  },
});
