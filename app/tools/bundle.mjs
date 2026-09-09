#!/usr/bin/env node
// Builds the single-file page: app/index.html with its modules and the wasm
// inlined.
//
//   node app/tools/bundle.mjs <mmmc.wasm> <out.html>
//
// **Why this exists.** The app is ES modules with no build step, which is how
// it should be developed and how it is served: a browser fetches `app.js`, the
// browser fetches its imports, and nothing has to be compiled to try a change.
// But a browser refuses to load a module from a `file://` URL, and the built
// page has to open from a download - it is the CI artifact for trying a branch
// on a machine with nothing installed, and it is the offline case the whole
// `.syx` export story is about. One file, opened from anywhere, no server.
//
// So the modules are wrapped, not transformed: each becomes a function that
// returns its exports, and an import becomes a call. Nothing is minified,
// nothing is rewritten beyond the import and export statements themselves, so
// what runs in the built page is line for line what runs from the source tree
// and a stack trace still points at real code.
//
// It understands exactly the import and export forms this app uses and throws
// on anything else, rather than guessing and producing a page that half works.

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const srcDir = join(here, '..', 'src');
const pagePath = join(here, '..', 'index.html');

// The import and export forms this app uses, and only those. A named import
// may span several lines, so these are matched against the whole source with
// `m` rather than line by line.
const IMPORT = /^import\s+([\s\S]*?)\s+from\s+'(\.\/[\w.-]+)';?$/gm;
const BARE_IMPORT = /^import\s+'(\.\/[\w.-]+)';?$/gm;
const EXPORT_LIST = /^export\s+\{([^}]*)\};?$/gm;
const EXPORT_DECL = /^export\s+(?:async\s+)?(?:function|class|const|let|var)\s+(\w+)/gm;

// One module, rewritten. Returns its source with imports and exports turned
// into calls, the names it exports, and the modules it depends on.
function transform(name) {
  const source = readFileSync(join(srcDir, name), 'utf8');
  const exported = new Set();
  const imports = [];

  let body = source.replace(BARE_IMPORT, (_, from) => {
    imports.push(from.slice(2));
    return `__require('${from.slice(2)}');`;
  });

  body = body.replace(IMPORT, (whole, what, from) => {
    const module = from.slice(2);
    imports.push(module);
    const clause = what.trim();
    if (clause.startsWith('*')) return `const ${clause.split(/\s+as\s+/)[1]} = __require('${module}');`;
    if (clause.startsWith('{')) {
      // `{ a, b as c }` is a destructuring pattern once `as` becomes `:`.
      return `const ${clause.replace(/\s+as\s+/g, ': ')} = __require('${module}');`;
    }
    throw new Error(`${name}: default imports are not used in this app: ${whole.trim()}`);
  });

  body = body.replace(EXPORT_LIST, (_, list) => {
    for (const entry of list.split(',')) {
      const trimmed = entry.trim();
      if (!trimmed) continue;
      const [local, exposed] = trimmed.split(/\s+as\s+/);
      exported.add(`${exposed ?? local}: ${local}`);
    }
    return '';
  });

  body = body.replace(EXPORT_DECL, (whole, declared) => {
    exported.add(declared);
    return whole.replace(/^export\s+/, '');
  });

  const left = body.match(/^\s*(import|export)\s/m);
  if (left) throw new Error(`${name}: cannot read ${left[1]}: ${body.slice(left.index, left.index + 60).trim()}`);

  return { name, imports, exported: [...exported], body };
}

// Depth-first from the entry point, so every module is defined before the one
// that imports it runs. A cycle would leave a module reading exports that do
// not exist yet, which is a silent `undefined` at run time - so it is an error
// here instead.
function order(entry) {
  const modules = new Map();
  const done = new Set();
  const path = [];

  const visit = (name) => {
    if (done.has(name)) return;
    if (path.includes(name)) {
      throw new Error(`import cycle: ${[...path.slice(path.indexOf(name)), name].join(' -> ')}`);
    }
    path.push(name);
    const module = modules.get(name) ?? transform(name);
    modules.set(name, module);
    for (const dependency of module.imports) visit(dependency);
    path.pop();
    done.add(name);
  };

  visit(entry);
  return [...done].map((name) => modules.get(name));
}

export function bundle(entry) {
  const modules = order(entry);
  const parts = modules.map((module) => `__define('${module.name}', (exports) => {\n`
    + `${module.body}\n`
    + `Object.assign(exports, { ${module.exported.join(', ')} });\n});`);

  return [
    "'use strict';",
    '// Built by app/tools/bundle.mjs from app/src. Each module is the file of',
    '// the same name, with its imports and exports rewritten into these two',
    '// functions and nothing else changed.',
    'const __modules = new Map();',
    'const __loaded = new Map();',
    'const __define = (name, body) => __modules.set(name, body);',
    'const __require = (name) => {',
    '  if (__loaded.has(name)) return __loaded.get(name);',
    '  const body = __modules.get(name);',
    '  if (!body) throw new Error(`no module ${name} in this build`);',
    '  const exports = {};',
    '  __loaded.set(name, exports);',
    '  body(exports);',
    '  return exports;',
    '};',
    ...parts,
    `__require('${entry}');`,
  ].join('\n');
}

// The command line. Importing this module (the bundle test does) must not
// build anything.
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const [wasmPath, outPath] = process.argv.slice(2);
  if (!wasmPath || !outPath) {
    console.error('usage: bundle.mjs <mmmc.wasm> <out.html>');
    process.exit(2);
  }

  let html = readFileSync(pagePath, 'utf8');

  const scriptTag = '<script type="module" src="./src/app.js"></script>';
  if (!html.includes(scriptTag)) throw new Error('index.html no longer loads ./src/app.js as a module');
  // Function replacements, so a `$` in the code or the base64 is not read as a
  // substitution pattern by String.replace.
  const script = `<script type="module">\n${bundle('app.js')}\n</script>`;
  html = html.replace(scriptTag, () => script);

  const marker = '/*MMMC_WASM_BASE64*/';
  if (!html.includes(marker)) throw new Error('index.html lost its wasm embed marker');
  const wasm = readFileSync(wasmPath).toString('base64');
  html = html.replace(marker, () => wasm);

  writeFileSync(outPath, html);
  console.log(`app: ${relative(process.cwd(), outPath)} `
    + `(${Buffer.byteLength(html)} bytes, one file, opens from a file:// URL)`);
}
