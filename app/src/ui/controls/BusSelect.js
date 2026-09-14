// A bus selector for one port. The options are only the buses of the right
// domain, because the editor only offers domain-compatible connections - the
// module would refuse anything else.
//
// `none` is what the "on no bus" row says, and `null` leaves it out: a jack's
// bus selector has no such row, because a jack that is on no bus is a jack
// that is *unused*, and that is its direction toggle's word to say.

import * as P from '../../protocol/generated.js';
import { busCount, domainName } from '../../core/validate.js';
import { Select, range } from '../components/Select.js';

export function BusSelect({ caps, domain, value, none = 'not connected', onChange, ...attrs }) {
  const name = domainName(domain);
  const options = range(busCount(caps, domain), (b) => `${name} bus ${b}`);
  if (none !== null) options.unshift({ value: P.NO_BUS, label: none });
  return Select({ options, value, onChange, class: `bus dom-${name}`, ...attrs });
}
