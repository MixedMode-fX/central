// Start, stop, continue, panic.
//
// One component in two places, because these are the module's controls and not
// a panel's: they are in the editor's sticky bar, above every tab, and on the
// performance surface, which is its own shell. Nothing about them changes
// between the two but the size of the buttons — what they do is
// `services/transport.js`, and neither view knows which machine is listening.
//
// **Running is painted, not rendered**, and only when the page can honestly
// know: the module in the page is asked per frame, and a module on a cable is
// not asked at all, because nothing in the protocol reports its clock and a
// lamp that guesses is worse than no lamp.

import { el, classes } from '../dom.js';
import { IconButton } from './IconButton.js';
import './Transport.css';

export function Transport(app, { compact = false, class: klass = '' } = {}) {
  const t = app.transport;
  const button = ({ icon, label, text, ...rest }) => IconButton({
    icon, label, text: compact ? null : text, ...rest,
  });
  const group = el('div', { class: classes('transport', compact && 'compact', klass), role: 'group', 'aria-label': 'transport' },
    button({ icon: 'play', label: 'start the clock from the top', class: 'start', onclick: () => t.start() }),
    button({ icon: 'stop', label: 'stop the clock', class: 'stop', onclick: () => t.stop() }),
    button({ icon: 'resume', label: 'continue from where the clock stopped', class: 'ghost', onclick: () => t.resume() }),
    // The one button whose word is worth the width it costs: an icon for
    // "everything sounding, released" is not a thing anybody recognises.
    button({ icon: 'panic', text: 'panic', class: 'ghost danger',
             label: 'panic: all notes off, on every port and channel', onclick: () => t.panic() }));
  app.live?.paint(({ monitor }) => group.classList.toggle('running', monitor.clock.running));
  return group;
}
