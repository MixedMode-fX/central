// A control per parameter, drawn from the descriptor: a range for a number, a
// list for an enum, a checkbox for a boolean. An editor cannot draw a control
// for a parameter whose range and meaning it does not know.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Slider } from '../components/Slider.js';
import { Select, range } from '../components/Select.js';
import { Switch } from '../components/Switch.js';
import { NumberField } from '../components/NumberField.js';
import { bindingTo, routeTo, effectiveValue } from '../../core/patch.js';
import { paramText } from './ParamText.js';
import { paramSections } from './ParamSections.js';
import { LearnButton, CvButton } from './LearnButton.js';
import { inertReason } from '../panels/algorithms.js';
import './Param.css';

// The card's parameters, by section. One section needs no heading: the
// heading says what the section is as opposed to the others.
export function ParamSections(app, index) {
  const node = app.state.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  if (!d.params) return el('div', { class: 'params loading' }, 'reading parameters…');
  const sections = paramSections(d.params);
  if (!sections.length) return null;
  const titled = sections.length > 1;
  return el('div', { class: 'param-sections' }, sections.map((section) =>
    el('div', { class: `param-section sec-${section.key.replace(/[^a-z0-9]+/gi, '-')}` },
      titled ? el('h4', {}, section.label) : null,
      el('div', { class: 'params' },
        section.params.map(({ at, pd }) => ParamControl(app, index, at, pd))))));
}

export function ParamControl(app, index, at, pd) {
  const value = app.state.patch.nodes[index].params[at];
  const write = (v) => app.editor.setParam(index, at, v);

  // An enum or a boolean says what it is set to in the control, so its row
  // needs no value beside the name and the name fits on the same line. A
  // number keeps its own row: a slider narrowed to make room for a label is
  // a slider that cannot be set.
  const inline = pd.kind === P.ParamKind.PARAM_ENUM || pd.kind === P.ParamKind.PARAM_BOOL;
  const controls = [];
  if (pd.kind === P.ParamKind.PARAM_ENUM) {
    controls.push(Select({
      class: 'grow', value: effectiveValue(pd, value), onChange: write,
      options: range(pd.max + 1, (v) => pd.options[v - pd.min] ?? String(v), pd.min),
    }));
  } else if (pd.kind === P.ParamKind.PARAM_BOOL) {
    controls.push(Switch({ checked: value !== 0, label: value ? 'on' : 'off', onChange: (on) => write(on ? 1 : 0) }));
  } else {
    // A slider *and* a number field: a slider is hopeless for a precise value
    // on a phone, a number field alone loses the sweep. A centred parameter
    // is driven in the terms it is written in - -12, 0, +12 - and the bias is
    // put back on at the write (`PARAM_CENTRE`, src/node/param.h).
    const bias = pd.kind === P.ParamKind.PARAM_CENTRED ? P.PARAM_CENTRE : 0;
    const lo = pd.min - bias;
    const hi = pd.max - bias;
    const shown = effectiveValue(pd, value) - bias;
    const number = NumberField({
      value: shown, min: lo, max: hi, 'aria-label': `${pd.name}, as a number`,
      onChange: (v) => { slider.value = String(v); write(v + bias); },
    });
    const slider = Slider({
      class: 'slider', min: String(lo), max: String(hi), step: '1',
      value: String(shown), 'aria-label': pd.name,
    }, {
      onInput: (v) => { number.value = v; },
      onCommit: (v) => write(Number(v) + bias),
    });
    controls.push(slider, number);
  }

  const patch = app.state.patch;
  const binding = bindingTo(patch, index, at);
  const route = routeTo(patch, index, at);
  // Where modulation has taken the parameter, beside the set point the
  // control shows. Painted by the route's meter (ModRoute.js), which is what
  // knows the live value.
  const live = route ? el('span', { class: 'param-live empty', 'data-param-live': `${index}:${at}` }) : null;
  // Dimmed and captioned, never disabled: the setting is still real and it
  // will do something again the moment the key or the patch says so.
  const inert = inertReason(app, index, pd);
  return el('div', { class: classes('param', inline && 'inline', inert && 'inert') },
    el('div', { class: 'param-head' },
      el('span', { class: 'param-name' }, pd.name),
      binding ? el('span', { class: 'param-cc' }, `CC ${binding.cc}`) : null,
      route ? el('span', { class: 'param-cc dom-CV' }, `CV ${route.buses.join(' + ')}`) : null,
      live,
      inline ? null : el('span', { class: 'param-value' }, paramText(pd, value))),
    el('div', { class: 'param-controls' }, controls,
      LearnButton(app, index, at, pd.name, binding),
      CvButton(app, index, at, pd.name, route)),
    inert ? el('p', { class: 'param-inert' }, inert) : null);
}
