// What a setting *is*, beside the control that says what it was set to.
//
// The editor is not the only writer of the key or the clock: a Key node walks
// the root off a note bus, a CC or an NRPN reaches either of them, a tap
// moves the tempo. None of those touch the stored settings, on purpose
// (src/midi/global_key.h), so the control is right about what the patch holds
// and silent about what the module is doing - and a control that is
// confidently half-right is worse than one that says nothing.
//
// So the reading goes beside it, in the same place and the same colour a
// modulated parameter's does (`.param-live`, controls/Param.css): nothing at
// all while the two agree, which is nearly always, and the live value the
// moment they do not.
//
// **Painted, not rebuilt.** The value arrives from a poll between renders
// (services/session.js), and a card that rebuilt itself because a sequencer
// moved the key would fight every control on it - so the mark is one element
// written into per frame, as the modulation meters are.

import { el } from '../dom.js';
import { globalNow } from '../../core/globals.js';
import './LiveMark.css';

// `name` is the settings field (protocol/names.js, LIVE_GLOBALS); `say` turns
// a raw value into the words this field uses, so a scale reads "natural
// minor" rather than 3.
export function LiveMark(app, name, say = String) {
  const mark = el('span', { class: 'live-mark empty',
                            title: 'something other than this control is moving it' });
  const paint = () => {
    const now = globalNow(app, name);
    const said = now.moved ? `now ${say(now.value)}` : '';
    if (mark.textContent === said) return;
    mark.textContent = said;
    mark.classList.toggle('empty', said === '');
  };
  paint();
  app.live?.paint(paint);
  return mark;
}
