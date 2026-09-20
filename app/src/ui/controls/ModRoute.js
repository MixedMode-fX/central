// What is modulating a node, how much of it, and what it is doing *now*.
//
// A route is made on the canvas or from the CV button, but *how* it modulates
// is not something a drag can say, and three of its four fields decide
// whether the result is a modulation or a mess: how deep, around the set
// point or instead of it, and which way up. They live here, beside the
// parameter they move.
//
// **And the answer to "what is happening to this control" is a moving
// picture, not a form.** Every field of a route could be set correctly and
// the parameter still not move - the depth is zero, the range is one value
// wide, nothing writes the bus - and all of them look identical from a panel
// that only shows what was typed into it. So each route carries a meter of
// the target's own range with the set point, the reach and the live value on
// it, and a line of words for the cases where there is nothing to see. Both
// come from the module (`ModMatrix::state`), because the alternative is a
// second implementation of the matrix here that is right until it is not.

import * as P from '../../protocol/generated.js';
import { el } from '../dom.js';
import { IconButton } from '../components/IconButton.js';
import { Slider } from '../components/Slider.js';
import { Select } from '../components/Select.js';
import { Switch } from '../components/Switch.js';
import { Field, Fields } from '../components/Field.js';
import { Domain } from '../../core/validate.js';
import { busPeers, routesOf, targetParamName, paramDescriptorOf } from '../../core/patch.js';
import { busWords } from '../../core/graph.js';
import { paramText } from './ParamText.js';
import './ModRoute.css';

const percent = (depth) => `${Math.round((depth ?? 255) * 100 / 255)} %`;

// Nothing is drawn when nothing is modulating the node: an empty panel on
// every block would be a heading repeated thirty-two times. A route to the
// clock has no block to be beside; those live in the mod matrix.
export function ModRoutes(app, index) {
  const routes = routesOf(app.state.patch, index);
  if (!routes.length) return null;
  return el('div', { class: 'params mod-routes' },
    el('h4', {}, 'modulation'),
    routes.map(({ slot, route }) => ModRoute(app, slot, route)));
}

function ModRoute(app, slot, route) {
  const write = (changed) => app.editor.setModRoute(slot, { ...route, ...changed });
  const name = targetParamName(app.device, app.state.patch, route);
  const flag = (bit, label, hint) => Switch({
    checked: (route.flags & bit) !== 0, label, hint,
    onChange: (on) => write({ flags: on ? (route.flags | bit) : (route.flags & ~bit) }),
  });
  const reading = el('span', { class: 'param-value' }, percent(route.depth));

  // Three questions, one per row, in the order they are asked: what does it
  // reach, how far does it move it, and how is the signal read.
  return el('div', { class: 'param mod-route' },
    el('div', { class: 'param-head' },
      el('span', { class: 'param-name' }, name),
      el('span', { class: 'param-cc dom-CV' }, `CV ${route.buses.join(' + ')}`),
      IconButton({ icon: 'cut', label: `stop modulating ${name}`, class: 'ghost danger',
                   onclick: () => app.editor.clearModRoute(slot) })),
    ModMeter(app, slot, route),
    Fields(
      // Named by what they do to the control, because "offset" and
      // "absolute" are the firmware's words and neither says which one leaves
      // the knob below still working.
      Field({ label: 'moves it' }, Select({
        class: 'grow', 'aria-label': `how CV ${busWords(route.buses)} reaches ${name}`,
        value: route.flags & P.ModFlags.MOD_MODE_MASK,
        options: [{ value: P.ModMode.MOD_OFFSET, label: 'around the setting' },
                  { value: P.ModMode.MOD_ABSOLUTE, label: 'instead of the setting' }],
        onChange: (mode) => write({ flags: (route.flags & ~P.ModFlags.MOD_MODE_MASK) | mode }),
      })),
      Field({ label: 'by' }, el('div', { class: 'param-controls' },
        Slider({ class: 'slider', min: '0', max: '255', step: '1', value: String(route.depth ?? 255),
                 'aria-label': `${name} modulation depth` },
               { onInput: (v) => { reading.textContent = percent(Number(v)); },
                 onCommit: (v) => write({ depth: Number(v) }) }),
        reading)),
      Field({ label: 'reading the signal' }, el('div', { class: 'param-controls' },
        flag(P.ModFlags.MOD_BIPOLAR, 'bipolar', 'the signal swings either side of zero'),
        flag(P.ModFlags.MOD_INVERT, 'invert', 'the other way up')))));
}

// What the module says about a route, in the words of the thing that decided
// it. A status this app has never heard of falls through to its number.
const MOD_WHY = {
  [P.ModStatus.MOD_STATUS_SILENT]: 'the depth is zero, so this route is switched off',
  [P.ModStatus.MOD_STATUS_NO_TARGET]: 'the module has nothing at this target',
  [P.ModStatus.MOD_STATUS_REFUSED]: 'the module refused the value',
  [P.ModStatus.MOD_STATUS_PINNED]: 'the range is one value wide: there is nowhere to move',
  [P.ModStatus.MOD_STATUS_UNUSED]: 'the module is not running this route',
};

// The target parameter's own range, which is the scale the meter is drawn on.
function targetRange(app, route) {
  const node = app.state.patch.nodes[route.targetIndex];
  const pd = node ? paramDescriptorOf(app.device, node.algorithmId, route.param) : null;
  return pd && pd.max > pd.min ? { pd, min: pd.min, max: pd.max } : null;
}

// The meter: the target's whole range as a track, the part this route can
// reach as a band, the set point as a tick, and the live value as a bar from
// one to the other. Built here and painted per frame from what the module
// last reported, because re-rendering the page for it would fight every
// open select on the card.
export function ModMeter(app, slot, route) {
  const range = targetRange(app, route);
  const parts = {
    band: el('div', { class: 'mod-band' }),
    reach: el('div', { class: 'mod-reach' }),
    tick: el('div', { class: 'mod-centre' }),
    now: el('div', { class: 'mod-now' }),
    live: el('span', { class: 'mod-live' }, 'waiting for the module…'),
    why: el('p', { class: 'mod-why' }),
  };
  const meter = el('div', { class: 'mod-meter', 'data-mod-slot': String(slot) },
    el('div', { class: 'mod-track' }, parts.band, parts.reach, parts.tick, parts.now),
    el('div', { class: 'mod-scale' },
      el('span', {}, range ? paramText(range.pd, range.min) : ''),
      parts.live,
      el('span', {}, range ? paramText(range.pd, range.max) : '')),
    parts.why);
  app.live?.watchMod(slot);
  app.live?.paint(() => paintMeter(app, meter, parts, slot, route, range));
  return meter;
}

// The one reason the module cannot report, because it is not the matrix's:
// the matrix reads the bus and finds whatever is on it, and zero every pass
// is a perfectly valid signal. Nothing *writing* the bus is a fact about the
// patch, and the patch is here.
function noSource(app, route) {
  const { writers } = busPeers(app.device, app.state.patch, Domain.CV, route.buses);
  return writers.length ? null : `nothing writes CV ${busWords(route.buses)}`;
}

export function paintMeter(app, meter, parts, slot, route, range) {
  if (!range) return;
  const state = app.session.modLive.get(slot);
  if (!state) {
    meter.classList.add('waiting');
    parts.live.textContent = 'waiting for the module…';
    parts.why.textContent = '';
    return;
  }
  meter.classList.remove('waiting');

  // Every status but "active" has a reason, and the one the module cannot
  // give is checked first.
  const idle = state.status !== P.ModStatus.MOD_STATUS_ACTIVE;
  const why = idle ? (MOD_WHY[state.status] ?? `the module reports status ${state.status}`)
                   : noSource(app, route);
  meter.classList.toggle('idle', Boolean(why));
  parts.why.textContent = why ?? '';

  const at = (value) => `${((value - range.min) * 100) / (range.max - range.min)}%`;
  const width = (lo, hi) => `${((hi - lo) * 100) / (range.max - range.min)}%`;
  const lo = Math.max(range.min, Math.min(range.max, state.rangeLo));
  const hi = Math.max(lo, Math.min(range.max, state.rangeHi));
  const centre = Math.max(lo, Math.min(hi, state.centre));
  const value = Math.max(lo, Math.min(hi, state.value));
  parts.band.style.left = at(lo);
  parts.band.style.width = width(lo, hi);
  // The bar runs from the set point to where the modulation has taken it, so
  // its length *is* the modulation: a route doing nothing has no bar at all.
  parts.reach.style.left = at(Math.min(centre, value));
  parts.reach.style.width = width(Math.min(centre, value), Math.max(centre, value));
  parts.tick.style.left = at(centre);
  parts.now.style.left = at(value);
  parts.live.textContent = idle
    ? paramText(range.pd, value)
    // The value the node is running, and the signal that put it there.
    : `${paramText(range.pd, value)} · signal ${Math.round((state.position * 100) / P.CV_FULL)} %`;

  // And the control itself, wherever it is on the card: a modulated
  // parameter shows where it has been taken to, beside the set point.
  const readout = document.querySelector(`[data-param-live="${route.targetIndex}:${route.param}"]`);
  if (readout) {
    readout.textContent = idle ? '' : `now ${paramText(range.pd, value)}`;
    readout.classList.toggle('empty', idle);
  }
}
