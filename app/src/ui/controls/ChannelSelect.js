// A channel selector: 0 is omni, which is a word rather than a number nobody
// can be expected to know. A channel is a MIDI control wherever it is edited.

import { Select, range } from '../components/Select.js';
import { channelLabel } from '../../protocol/names.js';

export function ChannelSelect({ value, onChange, omni = channelLabel(0), ...attrs }) {
  return Select({
    options: range(17, (c) => (c === 0 ? omni : channelLabel(c))),
    value, onChange, ...attrs,
  });
}
