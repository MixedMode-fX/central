// The audio rack and the USB socket: the module's, not the patch's
// (src/audio/rack.h, src/hal/iusb_host.h).
//
// Three panels. **The socket** is what is plugged into the module's USB host
// port now, and what has been since it booted: a device no driver holds is
// never told it has gone, so only a held one is said to be connected. Any of
// them that offered an audio function can be chosen. **The interface** is
// the one chosen, kept on the module so it is the one used offline, and what
// it is doing - which is not the chosen one until it has been plugged in
// again, because a USB driver claims a device as it enumerates. **The rack**
// is the slots: a plugin each, where it reads and writes, and its
// parameters.
//
// Every write goes to the module at once (services/audio.js); what is drawn
// is what the module answers, and polled while the tab is open, so a
// Launchpad moving a fader moves the slider here too.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Panel, Hint } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { Select, range } from '../components/Select.js';
import { Slider } from '../components/Slider.js';
import { IconButton } from '../components/IconButton.js';
import { paletteColour } from '../../runtime/launchpad.js';
import './Audio.css';

// Each slot's colour on a Launchpad (src/surface/launchpad.cpp, slot_colour):
// violet, cyan, orange, green.
const SLOT_PALETTE = [49, 37, 9, 21];
const LEVEL_UNITY = 101;

const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, '0');
const ids = (d) => `${hex4(d.vid)}:${hex4(d.pid)}`;

export function AudioTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  if (!app.session.rack) return Panel('audio', Hint('this module has no audio rack'));
  app.live.watchAudio();
  return el('div', {}, SocketPanel(app), InterfacePanel(app), RackPanel(app));
}

// --- the socket --------------------------------------------------------------------

const ROLES = [
  [P.UsbRole.USB_ROLE_AUDIO, 'the rack’s'],
  [P.UsbRole.USB_ROLE_AUDIO_IDLE, 'audio, unused'],
  [P.UsbRole.USB_ROLE_LAUNCHPAD, 'Launchpad'],
  [P.UsbRole.USB_ROLE_MIDI, 'MIDI'],
  [P.UsbRole.USB_ROLE_HUB, 'hub'],
];

function SocketPanel(app) {
  const list = el('div', { class: 'usb-list' });
  let shown = null;
  const paint = () => {
    const devices = app.session.usbLive;
    const chosen = app.session.audioLive?.chosen;
    const key = JSON.stringify([devices, chosen]);
    if (key === shown) return;
    shown = key;
    list.replaceChildren(...(devices === null
      ? [Hint('asking the module…')]
      : devices.length ? devices.map((d) => DeviceRow(app, d, chosen))
        : [Hint('nothing has been plugged into the USB host socket since the module started')]));
  };
  paint();
  app.live.paint(paint);
  return Panel('USB host socket', list,
    Hint('A device no driver holds is never told it has gone, so it is listed as seen rather than connected.'));
}

function DeviceRow(app, d, chosen) {
  const connected = Boolean(d.roles & P.UsbRole.USB_ROLE_CONNECTED);
  const audio = Boolean(d.roles & P.UsbRole.USB_ROLE_HAS_AUDIO);
  const isChosen = chosen && chosen.vid !== 0 && chosen.vid === d.vid && chosen.pid === d.pid;
  const roles = ROLES.filter(([bit]) => d.roles & bit).map(([, label]) => label);
  return el('div', { class: classes('usb-device', connected ? 'connected' : 'seen') },
    el('span', { class: 'usb-dot', title: connected ? 'connected' : 'seen since boot' }),
    el('div', { class: 'usb-name' },
      el('strong', {}, d.name || '(no name)'),
      el('span', { class: 'usb-ids' }, ids(d)),
      el('span', { class: 'usb-roles' }, [connected ? 'connected' : 'seen since boot', ...roles].join(' · '))),
    audio ? (isChosen
      ? el('span', { class: 'usb-chosen' }, 'chosen')
      : el('button', { onclick: () => app.audio.choose(d) }, 'use for audio')) : null);
}

// --- the interface -----------------------------------------------------------------

const STATE_NAMES = ['no interface', 'configuring', 'streaming', 'failed'];

function InterfacePanel(app) {
  const line = el('p', { class: 'audio-state' });
  const chosenLine = el('p', { class: 'audio-chosen' });
  const anyButton = el('button', { onclick: () => app.audio.choose(null) }, 'any interface');
  const paint = () => {
    const st = app.session.audioLive;
    if (!st) { line.textContent = 'asking the module…'; chosenLine.textContent = ''; return; }
    const chosen = st.chosen.vid ? `${st.chosen.name || '(no name)'} (${ids(st.chosen)})` : 'any interface';
    chosenLine.textContent = `chosen: ${chosen}`;
    anyButton.hidden = !st.chosen.vid;
    let text = STATE_NAMES[st.state] ?? `state ${st.state}`;
    if (st.held.vid) text = `${st.held.name || ids(st.held)}: ${text}`;
    if (st.state === P.AudioState.AUDIO_STREAMING) {
      text += ` · ${st.rate} Hz · ${st.inChannels} in / ${st.outChannels} out`;
    }
    if (st.state === P.AudioState.AUDIO_FAILED && st.failure) text += `: ${st.failure}`;
    const waiting = st.chosen.vid && st.held.vid && (st.chosen.vid !== st.held.vid || st.chosen.pid !== st.held.pid);
    if (waiting) text += ' — unplug it and plug the chosen one in to switch';
    line.textContent = text;
    line.classList.toggle('streaming', st.state === P.AudioState.AUDIO_STREAMING);
    line.classList.toggle('failed', st.state === P.AudioState.AUDIO_FAILED);
  };
  paint();
  app.live.paint(paint);
  return Panel('audio interface', chosenLine, line, anyButton,
    Hint('The module keeps the chosen interface, so it is the one used with no app attached. '
       + 'A driver claims a device as it is plugged in: a new choice takes effect the next time one is.'));
}

// --- the rack ----------------------------------------------------------------------

function RackPanel(app) {
  const { rack } = app.session;
  return Panel('rack',
    Hint('Each slot runs a plugin with a low cut in front of it and a level behind it. A side reads an input '
       + 'or an earlier slot, and slots writing the same output are summed: a tape slot read by a reverb '
       + 'slot through its low cut is a send. On a Launchpad the audio page plays them a slot at a time.'),
    el('div', { class: 'rack' }, rack.slots.map((slot, index) => SlotCard(app, index, slot))));
}

function sourceOptions(app, index) {
  const channels = app.device.capabilities.rackChannels ?? P.RACK_CHANNELS;
  const options = [{ value: P.RackSource.RACK_SOURCE_NONE, label: 'nothing' }];
  for (let c = 0; c < channels; c++) options.push({ value: P.RackSource.RACK_SOURCE_INPUT + c, label: `input ${c + 1}` });
  for (let s = 0; s < index; s++) {
    options.push({ value: P.RackSource.RACK_SOURCE_SLOT + s * 2, label: `slot ${s + 1} left` });
    options.push({ value: P.RackSource.RACK_SOURCE_SLOT + s * 2 + 1, label: `slot ${s + 1} right` });
  }
  return options;
}

function outOptions(app) {
  const channels = app.device.capabilities.rackChannels ?? P.RACK_CHANNELS;
  return [{ value: 0, label: 'nowhere' }, ...range(channels + 1, (c) => `output ${c}`, 1)];
}

function SlotCard(app, index, slot) {
  const plugins = app.device.plugins ?? [];
  const plugin = plugins[slot.plugin];
  const colour = paletteColour(SLOT_PALETTE[index] ?? 0);
  const set = (changes) => app.audio.setSlot(index, changes);
  const routed = slot.plugin !== P.RackPlugin.RACK_PLUGIN_NONE;
  const side = (key, which, options, label) => Select({
    options, value: slot[key][which], 'aria-label': `slot ${index + 1}: ${label}`,
    onChange: (v) => { const next = [...slot[key]]; next[which] = v; set({ [key]: next }); },
  });
  return el('div', { class: classes('rack-slot', !routed && 'empty'), style: `--slot: ${colour}` },
    el('div', { class: 'rack-head' },
      el('span', { class: 'rack-number' }, `${index + 1}`),
      Select({
        class: 'grow', 'aria-label': `slot ${index + 1}: plugin`,
        options: plugins.filter(Boolean).map((p) => ({ value: p.id, label: p.name, hint: p.summary })),
        value: slot.plugin, onChange: (id) => set({ plugin: id }),
      }),
      routed ? IconButton({ icon: 'erase', label: `slot ${index + 1}: empty the plugin, so a tail stops`,
                            onclick: () => app.audio.clear(index) }) : null),
    plugin && routed ? el('p', { class: 'rack-summary' }, plugin.summary) : null,
    routed ? Fields(
      Field({ label: 'reads' }, side('source', 0, sourceOptions(app, index), 'left source'),
                                side('source', 1, sourceOptions(app, index), 'right source')),
      Field({ label: 'writes' }, side('out', 0, outOptions(app), 'left output'),
                                 side('out', 1, outOptions(app), 'right output'))) : null,
    routed && plugin ? el('div', { class: 'rack-params' },
      plugin.params.map((p, param) => ParamRow(app, index, plugin, param))) : null);
}

// What a value means, for the two every slot has; a plugin's own are shown
// as the fader's 0..127, which is what the module and a Launchpad move.
function shown(plugin, param, value) {
  const own = plugin.params.length - 2;
  if (param === own) return value === 0 ? 'off' : `${Math.round(40 * 50 ** (value / 127))} Hz`;
  if (param === own + 1) {
    if (value === 0) return '−∞ dB';
    const db = 60 * Math.log10(value / LEVEL_UNITY);
    return `${db >= 0 ? '+' : ''}${db.toFixed(1)} dB`;
  }
  return String(value);
}

function ParamRow(app, index, plugin, param) {
  const value = () => app.session.rack.slots[index]?.params[param] ?? 0;
  const readout = el('span', { class: 'rack-value' }, shown(plugin, param, value()));
  const slider = Slider({ min: '0', max: '127', step: '1', value: String(value()),
                          'aria-label': `slot ${index + 1}: ${plugin.params[param].name}` }, {
    onInput: (v) => { readout.textContent = shown(plugin, param, Number(v)); },
    onCommit: (v) => app.audio.setParam(index, param, Number(v)),
  });
  // A Launchpad moves the rack too: follow the module, unless a finger is on
  // this slider.
  app.live.paint(() => {
    if (document.activeElement === slider) return;
    const v = value();
    if (slider.value !== String(v)) { slider.value = String(v); readout.textContent = shown(plugin, param, v); }
  });
  return el('label', { class: 'rack-param' },
    el('span', { class: 'rack-param-name' }, plugin.params[param].name), slider, readout);
}
