// One MIDI port, in full. The routing panel is a list of these and the
// canvas inspector shows the one whose block was clicked.
//
// **A port is a source and a destination, and the card says which is
// which.** The cables and the channel are the controls; the note buses are
// the canvas's to patch, and the card only reports them. They are labelled
// `from` and `to` with the signal running down the card in that order: an
// output is a note bus at the top playing cables at the bottom.
//
// Which way it faces is not a control here: turning a port round is an
// action on it, beside fanning it out and putting it away.

import { el } from '../dom.js';
import { IconButton } from '../components/IconButton.js';
import { ChannelSelect } from '../controls/ChannelSelect.js';
import { PortToggles } from '../controls/PortToggles.js';
import { BusNeighbours } from '../controls/BusNeighbours.js';
import { Domain } from '../../core/validate.js';
import { BlockKind } from '../../core/graph.js';
import './cards.css';

export function RouteCard(app, index, isOut, { header = true } = {}) {
  const caps = app.device?.capabilities;
  const port = (isOut ? app.state.patch.midiOut : app.state.patch.midiIn)[index];
  if (!caps || !port) return null;
  const what = `${isOut ? 'MIDI out' : 'MIDI in'} ${index + 1}`;
  const set = (changes) => app.editor.setMidiPort(index, isOut, changes);

  const cables = PortToggles({
    mask: isOut ? port.targetMask : port.sourceMask, label: `${what} ${isOut ? 'targets' : 'sources'}`,
    onChange: (mask) => set({ [isOut ? 'targetMask' : 'sourceMask']: mask }),
  });
  const channel = ChannelSelect({
    value: port.channel, onChange: (channel) => set({ channel }),
    omni: isOut ? 'keep each event’s channel' : undefined,
    'aria-label': isOut ? `${what} sends on` : `${what} accepts`,
  });
  // An input writes the buses, an output reads them: the far end is what its
  // notes reach or come from, and the company at this end is a fan-out.
  const wiring = BusNeighbours(app, {
    domain: Domain.Note, buses: port.buses, self: `${isOut ? 'midiOut' : 'midiIn'}:${index}`,
    writes: !isOut, unused: 'on no note bus — patch it on the canvas',
  });
  const leg = (label, ...controls) => el('div', { class: 'route-leg' },
    el('span', { class: 'leg-name' }, label),
    el('div', { class: 'leg-body' }, ...controls));

  return el('div', { class: 'route' },
    el('div', { class: 'route-head' },
      header ? el('h4', {}, `${isOut ? 'out' : 'in'} ${index + 1}`) : null,
      el('div', { class: 'route-actions' },
        // Only an output: an input reaching a second note bus is that bus in
        // its own set now, which is a drag on the canvas rather than a
        // second port to keep in step (core/graph.js).
        isOut ? IconButton({ icon: 'copy', class: 'ghost',
                             label: 'play these note buses down another cable too',
                             onclick: () => app.editor.fanOutMidiPort(index) }) : null,
        IconButton({ icon: 'flip', class: 'ghost', label: `turn ${what} round`,
                     onclick: () => app.editor.flipMidiPort(index, isOut) }),
        header ? IconButton({ icon: 'trash', class: 'ghost danger', label: `stop using ${what}`,
                              onclick: () => app.editor.removeBlock(
                                { kind: isOut ? BlockKind.MidiOut : BlockKind.MidiIn, index }) })
               : null)),
    isOut ? [leg('from', wiring), leg('to', cables, channel)]
          : [leg('from', cables, channel), leg('to', wiring)]);
}
