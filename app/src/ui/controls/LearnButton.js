// The two buttons beside every control, each opening a menu: what moves this
// parameter when my hands are somewhere else. **learn** arms a learn when a
// controller is listening and lists the CC numbers either way; **CV** lists
// the control buses, with what writes each, and routes one onto it.

import { el, classes } from '../dom.js';
import { icon } from '../components/icons.js';
import { Select } from '../components/Select.js';
import { openMenu, closeMenu, MenuItem, MenuField } from '../components/Menu.js';
import { Domain, busCount } from '../../core/validate.js';
import { CC_MAX } from '../../core/graph.js';
import * as P from '../../protocol/generated.js';
import { busPeers, isBinding } from '../../core/patch.js';

const menuButton = ({ what, klass, glyph, onOpen }) => {
  const button = el('button', {
    type: 'button', class: classes('ghost learn', klass), title: what, 'aria-label': what,
  }, glyph);
  button.addEventListener('click', () => onOpen(button));
  return button;
};

// The learn button: an icon, its meaning in the tooltip and the accessible
// name, and the bound CC in both so neither a pointer nor a screen reader
// has to go looking for it.
export function LearnButton(app, index, at, name, binding) {
  const armed = app.editor.isArmed(index, at);
  const what = armed ? `waiting for a controller to move ${name}`
    : binding ? `CC ${binding.cc} moves ${name} - change or remove the binding`
    : `bind a controller to ${name}`;
  return menuButton({
    what, glyph: icon('midi'), klass: [binding && 'bound', armed && 'armed'],
    onOpen: (button) => openCcMenu(app, button, index, at, name, binding),
  });
}

// The CC menu: the learn at the top, the CC numbers below it.
//
// **Opening it arms the learn**, rather than offering a row that would.
// Turning a knob is the gesture the button is named for, and a menu between
// the knob and the parameter is a menu in the way. When nothing is listening
// to a controller the arm would be a promise the page cannot keep, so there
// it is a row instead, and the list binds a number with no controller at all.
function openCcMenu(app, anchor, index, at, name, binding) {
  // Before anything arms: arming re-renders the page, and the button this
  // menu is placed under goes with it.
  const box = anchor.getBoundingClientRect();
  const listening = Boolean(app.controller?.input);
  if (listening && !app.session.offline && !app.editor.isArmed(index, at)) app.editor.learn(index, at);
  const armed = app.editor.isArmed(index, at);

  // What else is already on a CC. Two bindings may share a number - one knob
  // moving two parameters is a patch, not a mistake - so this is said rather
  // than refused.
  const alsoOn = (cc) => app.state.patch.ccMap.some((m) => isBinding(m) && m.cc === cc
    && !(m.targetKind === P.CcTargetKind.CC_TARGET_NODE && m.targetIndex === index && m.param === at));
  const numbers = Array.from({ length: CC_MAX + 1 }, (_, cc) =>
    ({ value: cc, label: `CC ${cc}${alsoOn(cc) ? ' · also bound' : ''}` }));
  if (!binding) numbers.unshift({ value: '', label: 'not bound' });

  openMenu({
    at: box, kind: 'cc',
    head: armed ? `waiting for a controller to move ${name}`
      : binding ? `${name} is on CC ${binding.cc}`
      : `bind a controller to ${name}`,
    items: [
      armed
        ? MenuItem({ label: 'stop waiting', hint: 'cancel the learn', onPick: () => app.editor.cancelLearn() })
        : MenuItem({ label: 'turn a knob to bind it', hint: listening ? null : 'no controller yet',
                     onPick: () => app.editor.learn(index, at) }),
      MenuField('CC number', Select({
        options: numbers, value: binding?.cc ?? '', class: 'grow',
        'aria-label': `the CC that moves ${name}`,
        onChange: (cc) => { if (cc !== '') { closeMenu(); app.editor.bindParam(index, at, cc); } },
      })),
      binding
        ? MenuItem({ label: 'remove the binding', hint: `CC ${binding.cc}`, class: 'danger',
                     onPick: () => app.editor.clearMapping(binding.slot) })
        : null,
    ],
  });
}

// The CV button: a control signal onto this parameter, from here. The route
// it makes is the same route a drag on the canvas makes (`planBusModulation`
// and `planModulation` build the same object).
export function CvButton(app, index, at, name, route) {
  const what = route
    ? `${name} is modulated from CV ${route.buses.join(' + ')} - change or remove the route`
    : `modulate ${name} from a CV bus`;
  return menuButton({
    what, glyph: icon('cv'), klass: ['cv', route && 'bound'],
    onOpen: (button) => openBusMenu(app, button, index, at, name, route),
  });
}

// The CV buses, as a menu: the ones something writes first, each saying what,
// then the rest - a bus nothing writes is a route to silence, which is still
// occasionally what somebody wants while the source is on its way.
function openBusMenu(app, anchor, index, at, name, route) {
  const caps = app.device?.capabilities;
  if (!caps?.modRoutes) { app.say('this firmware has no modulation routes'); return; }
  const buses = [];
  for (let bus = 0; bus < busCount(caps, Domain.CV); bus++) {
    const { writers } = busPeers(app.device, app.state.patch, Domain.CV, bus);
    buses.push({ bus, writers });
  }
  buses.sort((a, b) => Number(Boolean(b.writers.length)) - Number(Boolean(a.writers.length)) || a.bus - b.bus);

  openMenu({
    at: anchor.getBoundingClientRect(), kind: 'cv',
    head: route ? `${name} reads CV ${route.buses.join(' + ')}` : `modulate ${name} from`,
    items: [
      ...buses.map(({ bus, writers }) => MenuItem({
        label: `CV bus ${bus}`,
        hint: writers.length ? `from ${writers.join(', ')}` : 'nothing writes it',
        class: route?.buses?.includes(bus) && 'chosen',
        onPick: () => app.editor.routeParam(index, at, bus),
      })),
      route ? MenuItem({ label: 'remove the route', class: 'danger',
                         onPick: () => app.editor.clearModRoute(route.slot) }) : null,
    ],
  });
}
