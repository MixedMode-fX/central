// The shell: the header, the tabs, what went wrong, and the tab on screen.
//
// Six tabs - patch, globals, MIDI, module, library, schema - in the order the
// work happens, and **play** as a button at the top beside *connect a module*,
// because those two answer the same question: which module am I listening
// to, the one in the page or the one on the cable.
//
// The transport - start, stop, continue and panic - is in the bar with the
// tabs rather than on any of them: it is the machine's, not a panel's, and it
// is needed while looking at whatever tab happens to be open.
//
// Play does not open a tab. It opens the performance surface, which is its
// own shell and takes the whole viewport (ui/surface/Surface.js): an
// instrument rather than an instrument panel. The panels that used to be
// behind the play button - the meters, the scope, the jacks, the listener,
// the monitor - are the **module** tab, which is where an instrument panel
// belongs.

import { el, classes } from './dom.js';
import { icon } from './components/icons.js';
import { IconButton } from './components/IconButton.js';
import { Notice } from './components/Panel.js';
import { Transport } from './components/Transport.js';
import { closeMenu } from './components/Menu.js';
import { validate, advise } from '../core/validate.js';
import { Domain } from '../core/validate.js';
import { busPeers } from '../core/patch.js';
import { drumSources } from '../runtime/audio/drums.js';
import { PatchTab } from './tabs/PatchTab.js';
import { PlayTab } from './tabs/PlayTab.js';
import { Surface } from './surface/Surface.js';
import { GlobalsTab } from './tabs/GlobalsTab.js';
import { MidiTab } from './tabs/MidiTab.js';
import { LibraryTab } from './tabs/LibraryTab.js';
import { SchemaTab } from './tabs/SchemaTab.js';

const TABS = [
  { key: 'patch', label: 'patch', icon: 'patch', view: PatchTab },
  { key: 'globals', label: 'globals', icon: 'globals', view: GlobalsTab },
  { key: 'midi', label: 'MIDI', icon: 'midi', view: MidiTab },
  { key: 'module', label: 'module', icon: 'clock', view: PlayTab },
  { key: 'library', label: 'library', icon: 'library', view: LibraryTab },
  { key: 'schema', label: 'schema', icon: 'schema', view: SchemaTab },
];

export function App(app) {
  const { state } = app;
  // A menu belongs to the page that opened it, and the page is about to be
  // replaced.
  closeMenu();
  syncNoteBuses(app);
  syncDrums(app);

  // The surface is not a tab: it is a second shell, with no header and no
  // tab bar, because a stage view that carried the editor's furniture would
  // have a third of a phone's screen left for the instrument.
  if (state.ui.tab === 'play') return Surface(app);

  const checked = app.device?.capabilities;
  const problems = checked ? validate(app.device, state.patch) : [];
  const notes = checked ? advise(app.device, state.patch) : [];
  const said = (list) => list.map((p) => `${p.where}: ${p.message}`);
  const tab = TABS.find((t) => t.key === state.ui.tab)?.view ?? PatchTab;

  return el('div', { class: 'shell' },
    Header(app),
    // The transport travels with the tabs, stuck to the top of the page: a
    // start button that scrolls away with the header is one you have to go
    // and find again, which was the whole complaint against keeping it inside
    // the module tab's clock panel.
    el('div', { class: 'bar' }, Tabs(app), Transport(app)),
    state.error ? Notice({ kind: 'error', text: state.error, onClick: app.dismissError }) : null,
    problems.length ? Notice({ kind: 'problems', title: state.diverged ? 'not sent' : 'rejected', items: said(problems) }) : null,
    notes.length ? Notice({ kind: 'notes', title: 'notes', items: said(notes) }) : null,
    tab(app));
}

function Header(app) {
  const { state, session } = app;
  const playing = state.ui.tab === 'play';
  return el('header', { class: 'top' },
    el('div', { class: 'title' },
      el('h1', {}, 'MMMC'),
      el('span', { class: 'patch-name' }, state.current.name, state.current.dirty ? ' •' : '')),
    el('div', { class: 'top-buttons' },
      IconButton({
        icon: playing ? 'edit' : 'play', text: playing ? 'edit' : 'play',
        label: playing ? 'back to editing' : 'play the module',
        class: classes(playing && 'active'), 'aria-pressed': playing ? 'true' : 'false',
        onclick: () => app.togglePlay(),
      }),
      IconButton({
        icon: 'plug', text: session.usingModule ? 'connect' : 'reconnect',
        label: session.usingModule ? 'connect a module over MIDI' : 'reconnect the module',
        onclick: () => app.connect(),
      })),
    el('p', { class: classes('status', session.offline ? 'offline' : 'online', state.diverged && 'warn') },
      state.status, el('span', { class: 'hint' }, ` · ${session.transportName}`)));
}

function Tabs(app) {
  const current = app.state.ui.tab;
  return el('nav', { class: 'tabs', role: 'tablist' },
    TABS.map((t) => el('button', {
      class: classes('tab', current === t.key && 'active'),
      role: 'tab', 'aria-selected': current === t.key ? 'true' : 'false',
      onclick: () => app.showTab(t.key),
    }, icon(t.icon), el('span', { class: 'tab-label' }, t.label))));
}

// Keep the note-bus watches in step with the patch, on every render, which
// is every edit: a connection dragged onto a new bus is a new bus to show,
// and one dragged off is one to stop reading. A bus is only read when
// something asks for it, and what a block's own roll asks for is "every bus
// this patch writes" - the block is chosen after the notes were played, so
// the bus has to have been watched all along.
function syncNoteBuses(app) {
  const wanted = new Set();
  app.watchedBuses ??= new Set();
  if (app.module && app.session.usingModule && app.device?.capabilities) {
    for (let bus = 0; bus < app.device.capabilities.noteBuses; bus++) {
      if (busPeers(app.device, app.state.patch, Domain.Note, bus).writers.length) wanted.add(bus);
    }
  }
  for (const bus of [...app.watchedBuses]) {
    if (wanted.has(bus)) continue;
    app.module?.unwatchNoteBus(bus);
    app.watchedBuses.delete(bus);
  }
  for (const bus of wanted) {
    if (app.watchedBuses.has(bus)) continue;
    app.module.watchNoteBus(bus);
    app.watchedBuses.add(bus);
  }
}

// Keep the drum voices in step with the patch, for the same reason: a drum
// machine is heard because it is *in the patch*, whichever tab is on screen.
// The mask is the other half: one patched to a MIDI output sends every hit
// twice as far as this page is concerned, and two of them is a flam nobody
// programmed.
function syncDrums(app) {
  if (!app.listener) return;
  const sources = app.module && app.session.usingModule && app.device
    ? drumSources(app.device, app.state.patch) : [];
  app.listener.setDrumSources(sources);
  const buses = new Set(sources.filter((s) => s.kind === 'note').flatMap((s) => s.buses ?? []));
  let mask = 0;
  for (const port of app.state.patch.midiOut) {
    if (port.targetMask && (port.buses ?? []).some((b) => buses.has(b))) mask |= port.targetMask;
  }
  app.listener.setDrumOutMask(mask);
}
