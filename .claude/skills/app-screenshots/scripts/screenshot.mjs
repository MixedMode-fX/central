#!/usr/bin/env node
// Photograph the running app. See SKILL.md for the option table.
//
// Deliberately not a test runner and not a visual-diff tool: it takes a
// picture of a real page at a real width, so that a claim about layout can be
// looked at instead of believed.

import { execSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const skillDir = dirname(fileURLToPath(import.meta.url));

// Playwright is installed beside the skill (see package.json here), never in
// the app's own dependencies — a browser has no business in the tree that
// builds the site. Self-heal on first run so nobody has to know that.
function loadPlaywright() {
  const req = createRequire(import.meta.url);
  try {
    return req('playwright');
  } catch {
    console.error('installing playwright beside the skill (first run)...');
    execSync('npm install --no-audit --no-fund', {
      cwd: skillDir,
      stdio: 'inherit',
      // The container ships a browser at /opt/pw-browsers; downloading a
      // second copy is a slow way to arrive at the same place.
      env: { ...process.env, PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD: process.env.PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD ?? '1' },
    });
    return req('playwright');
  }
}

// phone first, because that is where this workflow is read.
const WIDTHS = { phone: 390, tablet: 768, desktop: 1440 };
const HEIGHTS = { 390: 844, 768: 1024 };
const DEFAULT_HEIGHT = 900;

const opt = {
  widths: [],
  clicks: [],
  path: '/',
  name: 'shot',
  base: null,
  theme: null,
  fullPage: false,
  wait: 0,
};

const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  switch (args[i]) {
    case '--width': opt.widths.push(args[++i]); break;
    case '--path': opt.path = args[++i]; break;
    case '--click': opt.clicks.push(args[++i]); break;
    case '--name': opt.name = args[++i]; break;
    case '--base': opt.base = args[++i]; break;
    case '--theme': opt.theme = args[++i]; break;
    case '--full-page': opt.fullPage = true; break;
    case '--wait': opt.wait = Number(args[++i]); break;
    case '--help': usage(); process.exit(0); break;
    default: console.error(`unknown option: ${args[i]}`); usage(); process.exit(2);
  }
}
if (!opt.widths.length) opt.widths = ['phone'];
if (opt.theme && !['light', 'dark'].includes(opt.theme)) {
  console.error('--theme takes light or dark');
  process.exit(2);
}

function usage() {
  console.error(`
screenshot.mjs [options]
  --width phone|tablet|desktop|<px>   repeatable; default phone
  --path <path>                       page or #fragment to open (default /)
  --click <selector>                  repeatable, applied in order
  --theme light|dark                  force a colour scheme
  --full-page                         capture the whole scroll height
  --wait <ms>                         settle time before the shot
  --name <slug>                       filename prefix
  --base <url>                        override .dev/base_url
`.trim());
}

function baseUrl() {
  if (opt.base) return opt.base;
  const f = resolve('.dev/base_url');
  if (existsSync(f)) return readFileSync(f, 'utf8').trim();
  return 'http://127.0.0.1:5173';
}

async function launch() {
  const { chromium } = loadPlaywright();
  const candidates = [
    process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE,
    '/opt/pw-browsers/chromium',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/usr/bin/google-chrome',
  ].filter(Boolean);
  try { return await chromium.launch(); } catch { /* fall through to explicit paths */ }
  for (const executablePath of candidates) {
    try { return await chromium.launch({ executablePath }); } catch { /* next */ }
  }
  throw new Error('no launchable chromium found — set PLAYWRIGHT_CHROMIUM_EXECUTABLE');
}

/**
 * Wait until the page has stopped scrolling.
 *
 * Belt and braces alongside `reducedMotion`, because a page can animate its
 * own scrolling in JavaScript, where no media query reaches. Shooting mid-
 * scroll does not merely catch the wrong part of the page: a sticky header
 * over a moving document composites into a torn frame with a band of empty
 * background above it, which reads as a layout bug that is not there. Evidence
 * that invents its own faults is worse than no evidence.
 */
async function settle(page, timeout = 3000) {
  const started = Date.now();
  let last = null;
  while (Date.now() - started < timeout) {
    const y = await page.evaluate(() => Math.round(window.scrollY));
    if (y === last) break;
    last = y;
    await page.waitForTimeout(120);
  }
  // One more frame for the compositor after the last scroll lands.
  await page.waitForTimeout(150);
}

const base = baseUrl().replace(/\/$/, '');
const url = base + (opt.path.startsWith('/') || opt.path.startsWith('#') ? opt.path : `/${opt.path}`);

mkdirSync('.dev/screenshots', { recursive: true });

const browser = await launch();
const written = [];

for (const w of opt.widths) {
  const width = WIDTHS[w] ?? Number(w);
  if (!Number.isFinite(width)) {
    console.error(`unknown width: ${w}`);
    process.exitCode = 2;
    continue;
  }
  const height = HEIGHTS[width] ?? DEFAULT_HEIGHT;

  const context = await browser.newContext({
    viewport: { width, height },
    deviceScaleFactor: 2,
    // A phone-width viewport that is not touch-capable renders hover styles
    // that no phone ever shows.
    hasTouch: width <= 768,
    isMobile: width <= 768,
    // A screenshot wants the end state, never a frame from the middle of an
    // animation. Reduced motion turns `scroll-behavior: smooth` off at the
    // source, so a `--path '#anchor'` lands instantly instead of gliding.
    reducedMotion: 'reduce',
    ...(opt.theme ? { colorScheme: opt.theme } : {}),
  });
  const page = await context.newPage();
  page.on('console', (m) => { if (m.type() === 'error') console.error(`  console: ${m.text()}`); });
  page.on('pageerror', (e) => console.error(`  pageerror: ${e.message}`));

  await page.goto(url, { waitUntil: 'networkidle' });
  for (const selector of opt.clicks) {
    await page.click(selector, { timeout: 5000 });
  }
  if (opt.wait) await page.waitForTimeout(opt.wait);
  // Fonts land after first paint; a shot taken before they do is a picture of
  // the fallback stack, and every spacing judgement made from it is wrong.
  await page.evaluate(() => document.fonts.ready);
  await settle(page);

  const label = WIDTHS[w] ? w : `${width}px`;
  const file = `.dev/screenshots/${opt.name}-${label}${opt.theme ? `-${opt.theme}` : ''}.png`;
  await page.screenshot({ path: file, fullPage: opt.fullPage });
  written.push(file);
  await context.close();
}

await browser.close();

// Print every path: the next step is reading these back and attaching them to
// the conversation, and that step needs the names.
for (const f of written) console.log(f);
