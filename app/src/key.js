// The key the module is in: one scale, one root, one register, for the whole
// patch.
//
// It is a page of its own because it is not a MIDI setting and never was. It
// sat under "MIDI" beside the clock and the Program Change filters for the
// same reason a spare drawer fills up - the panel that pushed GlobalSettings
// happened to live there - and a key is the most musical decision in the
// patch, not a transport detail. It is now the first thing beside the patch
// itself.
//
// Every node reads it, and reads it as two separate questions
// (src/midi/global_scale.h): its `scale` parameter says which notes it plays
// and its `key` parameter says whose root they are measured from. Both
// default to following, so a patch set to A minor is in A minor everywhere -
// including the nodes that name a mode of their own - and a node leaves the
// key only by saying so.

import { el } from './views.js';
import { KEY_SCALES, PITCH_CLASSES } from './names.js';

export function keyPanel(app) {
  if (!app.device?.capabilities) return null;
  const g = app.globals;
  const push = () => { app.edit(() => app.device.setGlobals(g), 'key'); app.render(); };

  const scale = el('select', { onchange: (e) => { g.scale = Number(e.target.value); push(); } });
  for (const s of KEY_SCALES) {
    const option = el('option', { value: String(s.value) }, s.label);
    if (s.value === g.scale) option.selected = true;
    scale.append(option);
  }

  const root = el('select', { onchange: (e) => { g.root = Number(e.target.value); push(); } });
  PITCH_CLASSES.forEach((name, pitchClass) => {
    const option = el('option', { value: String(pitchClass) }, name);
    if (pitchClass === (g.root ?? 0)) option.selected = true;
    root.append(option);
  });

  // The register the key sits in. "none" is the default and means what it
  // always meant: the key names a pitch class, and every node with an
  // absolute root keeps the octave it stored.
  const register = el('select', { onchange: (e) => { g.rootOctave = Number(e.target.value); push(); } });
  for (let octave = 0; octave <= 10; octave++) {
    const label = octave === 0
      ? 'none'
      : `${octave} (${PITCH_CLASSES[(g.root ?? 0) % 12]}${octave}, note ${Math.min(127, octave * 12 + (g.root ?? 0))})`;
    const option = el('option', { value: String(octave) }, label);
    if (octave === (g.rootOctave ?? 0)) option.selected = true;
    register.append(option);
  }

  const field = (name, control, hint) => el('div', { class: 'field' },
    el('span', { class: 'field-name' }, name), control,
    hint ? el('span', { class: 'hint' }, hint) : null);

  return el('section', { class: 'panel' },
    el('h2', {}, 'the key'),
    el('div', { class: 'fields' },
      field('scale', scale, 'which notes'),
      field('root', root, 'which of them is home'),
      field('register', register, 'where home sits; "none" leaves every node the octave it stored')),
    el('p', { class: 'hint' },
      'Every node follows this unless it says otherwise: its "scale" names notes of its own, '
      + 'its "key" takes it off this root. A node that follows plays in the register named here, '
      + 'moved by the octave its own root parameter names - so one key moves the patch and each '
      + 'node keeps its place in it.'));
}
