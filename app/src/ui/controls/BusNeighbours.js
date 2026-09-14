// Who else is on a port's bus, in words. A bus *is* the connection, so the
// thing a patch cable would have shown - what this port is actually connected
// to - has to be said, or the patch is a list of numbers that happen to
// match.
//
// `self` is the asking port's own peer id (core/patch.js), left out of its
// own answer, and `writes` says which side of the bus it is on: the peers on
// the other side are where its signal goes or comes from, and the peers on
// its own side are company - a fan-out, or a merge - said as "with".

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { busPeers } from '../../core/patch.js';

export function BusNeighbours(app, { domain, bus, self, writes, unused = null }) {
  if (bus === P.NO_BUS) return unused ? el('span', { class: 'wire-note empty' }, unused) : null;
  const { writers, readers } = busPeers(app.device, app.state.patch, domain, bus, self);
  const across = writes ? readers : writers;
  const beside = writes ? writers : readers;
  const parts = [];
  if (across.length) parts.push(`${writes ? 'to' : 'from'} ${across.join(', ')}`);
  // An inlet nobody writes is the warning `advise` raises: the node will
  // read silence. An outlet nobody reads is ordinary - a spare drum lane, an
  // output waiting for a jack - so it is said, not flagged.
  else parts.push(writes ? 'no reader' : 'no writer');
  if (beside.length) parts.push(`with ${beside.join(', ')}`);
  return el('span', { class: classes('wire-note', !across.length && !writes && 'empty') },
            parts.join(' · '));
}
