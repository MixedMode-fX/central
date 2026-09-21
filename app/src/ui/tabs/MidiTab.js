// MIDI: what comes in, what goes out, and the gear plugged into this
// computer - a controller playing the module, and the outputs the module
// plays a synth through. What a controller *moves* is not here: a binding is
// part of the patch, so it lives in the mod matrix. Nor is what the module
// counts time by, or what a Program Change does to it: those belong to the
// whole module rather than to a cable, and they are on the globals tab with
// the key (tabs/GlobalsTab.js). What is left is the room - the cables and
// the channels, none of which a patch travels with.

import { el } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { Select } from '../components/Select.js';
import { IconButton } from '../components/IconButton.js';
import { RouteCard } from '../panels/RouteCard.js';
import { MUSICAL_PORTS } from '../../protocol/names.js';
import { describeSupport } from '../../runtime/webmidi.js';
import { cablesOut } from '../../runtime/midiout.js';
import '../panels/cards.css';

export function MidiTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  return el('div', {}, ControllerPanel(app), OutputPanel(app), RoutingPanel(app));
}

// --- routing ---------------------------------------------------------------

// The ports a patch is using, and a way to take one more into use. An unused
// port is not drawn: eight cards, six of them empty, was most of a phone
// screen spent saying "no".
function RouteSide(app, isOut) {
  const caps = app.device.capabilities;
  const slots = (isOut ? app.state.patch.midiOut : app.state.patch.midiIn).slice(0, isOut ? caps.midiOut : caps.midiIn);
  const used = slots.map((port, index) => ({ port, index }))
    .filter(({ port }) => (isOut ? port.targetMask : port.sourceMask));

  return el('div', { class: 'route-side' },
    el('div', { class: 'route-side-head' },
      el('h3', {}, isOut ? 'outputs' : 'inputs'),
      el('span', { class: 'hint' }, `${used.length}/${slots.length}`),
      used.length < slots.length
        ? IconButton({ icon: 'plus', class: 'ghost', label: `add a MIDI ${isOut ? 'output' : 'input'}`,
                       onclick: () => app.editor.addMidiPort(isOut) })
        : null),
    used.length
      ? used.map(({ index }) => RouteCard(app, index, isOut))
      : Hint(isOut ? 'nothing is played out' : 'nothing is taken in'));
}

export function RoutingPanel(app) {
  return Panel('MIDI routing', el('div', { class: 'routes' }, RouteSide(app, false), RouteSide(app, true)));
}

// --- the gear on this computer ----------------------------------------------

// A controller plugged into the *computer*, playing the module in the page.
// The keys and knobs on the desk reach the patch being edited, on a chosen
// port and its own channel, through the same MIDI input a cable would use -
// so learn works from it, with no module in the room.
export function ControllerPanel(app) {
  const controller = app.controller;
  const title = 'external controller';
  const gate = deviceGate(app, controller?.access, 'plug it into the module on the cable');
  if (gate) return Panel(title, gate);

  return Panel(title,
    Fields(
      Field({ label: 'play the module from' }, Select({
        class: 'grow', value: controller.inputId,
        options: [{ value: '', label: 'nothing' },
                  ...controller.inputs.map((port) => ({ value: port.id, label: port.name }))],
        onChange: (id) => { controller.listenTo(id); app.render(); },
      })),
      Field({ label: 'arriving on' }, Select({
        options: MUSICAL_PORTS, value: controller.port,
        onChange: (mask) => { controller.setPort(mask); app.render(); },
      }))));
}

// Where what the module plays leaves this computer: one of the browser's MIDI
// outputs per cable the patch plays to, so a synth on the desk is played by
// the patch on screen.
//
// A row per cable *in use*, from the patch rather than from the eight the
// firmware has: a list of eight, six of them for cables nothing is played on,
// is the panel saying "no" six times - and the row appears as soon as a MIDI
// out node names the cable, which is where the question comes up.
export function OutputPanel(app) {
  const outputs = app.outputs;
  const title = 'external MIDI out';
  const gate = deviceGate(app, outputs?.access, 'the module plays out of its own sockets');
  if (gate) return Panel(title, gate);

  const cables = cablesOut(app.state.patch, app.device.capabilities, app.state.globals);
  if (!cables.length) return Panel(title, Hint('this patch plays nothing out'));

  const ports = outputs.outputs;
  return Panel(title,
    Fields(...cables.map((cable) => Field({
      label: cable.label,
      hint: outputs.routeOf(cable.value) && !outputs.output(cable.value) ? 'not plugged in' : null,
    }, Select({
      class: 'grow', 'aria-label': `what ${cable.label} plays out of`,
      value: outputs.routeOf(cable.value),
      // A route survives the port going away, so the name it holds is offered
      // even when nothing on this computer answers to it: unplugging a synth
      // for an hour is not a reason to make the user route it again.
      options: portOptions(ports, outputs.routeOf(cable.value)),
      onChange: (name) => { outputs.route(cable.value, name); app.render(); },
    })))),
    Hint('a cable with no port goes nowhere; the play tab hears it either way'));
}

const portOptions = (ports, chosen) => {
  const names = ports.map((port) => port.name);
  if (chosen && !names.includes(chosen)) names.push(chosen);
  return [{ value: '', label: 'nothing' }, ...names.map((name) => ({ value: name, label: name }))];
};

// What stands between a panel and its controls: a module on the cable has its
// own sockets and needs none of this, a browser without Web MIDI reaches
// nothing, and the devices are asked for once for both panels.
function deviceGate(app, access, offline) {
  if (!app.session.usingModule) return Hint(offline);
  // An access in hand is the only proof of support that matters; the browser
  // is only asked what it can do when there is nothing to show for it.
  if (access) return null;
  const support = describeSupport();
  return support.ok
    ? Row(el('button', { onclick: () => app.connectMidi() }, 'find my MIDI devices'))
    : Hint(support.reason);
}
