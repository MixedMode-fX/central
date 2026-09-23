// The two LEDs, the clock and the gate buses. With no display on the module,
// this *is* the missing panel: green flashes the beat, red means attention,
// and a lit gate bus is a signal moving.
//
// Painted per frame from the module's own per-pass sampling, which arrives
// in the monitor's frame (runtime/monitor.js) whichever transport the module
// is on - not from a timer reading the levels for itself. A trigger is high
// for one or two passes and a frame is sixteen, so anything polled at paint
// time shows a pattern nobody is playing; `activity` is everything that has
// been high since the last paint.

import * as P from '../../protocol/generated.js';
import { el } from '../dom.js';
import { Panel } from '../components/Panel.js';

export function Meters(app) {
  const green = el('span', { class: 'led green', title: 'green: the clock' });
  const red = el('span', { class: 'led red', title: 'red: attention' });
  const readout = el('span', { class: 'clock-readout' }, '');
  const dots = Array.from({ length: P.N_GATE_BUS }, (_, b) =>
    el('span', { class: 'gate-dot', title: `gate bus ${b}` }));

  app.live.paint(({ monitor, activity }) => {
    green.style.opacity = String(Math.max(0.08, activity.green / 255));
    red.style.opacity = String(Math.max(0.08, activity.red / 255));
    const clock = monitor.clock;
    readout.textContent = clock.running
      ? `${clock.bpm} BPM · beat ${Math.floor(clock.count / (P.MASTER_PPQN * P.CLOCK_SUBTICK)) + 1}`
      : 'clock stopped';
    dots.forEach((dot, b) => dot.classList.toggle('lit', (activity.gate & (1 << b)) !== 0));
  });

  return Panel(null, el('div', { class: 'meter-row' }, green, red, readout, el('div', { class: 'gate-dots' }, dots)));
}
