// What was clicked on the canvas, in full. A node gets its card - every
// parameter, its grid, its bus selectors, and the signals it read and wrote;
// a jack and a MIDI port get theirs, signals included; an arrow gets the two
// ends it joins.
//
// The panel folds. Its bar carries the block's name, what can be done to the
// block whole - copy it, make another one, remove it - a chevron that folds
// the body away and a cross that puts the panel away by deselecting. So on a
// phone, where the panel is below a canvas that fills the screen, the card can
// be folded to a bar without losing the selection it belongs to, and the
// commands the keyboard shortcuts reach are buttons for a screen that has no
// keyboard.

import { el, classes } from '../dom.js';
import { IconButton } from '../components/IconButton.js';
import { NodeCard } from '../panels/NodeCard.js';
import { JackCard } from '../panels/JackCard.js';
import { RouteCard } from '../panels/RouteCard.js';
import { BlockKind, planDisconnect } from '../../core/graph.js';
import { domainName } from '../../core/validate.js';
import { underBlock } from '../../core/layout.js';
import { geometry, shortcut } from './Canvas.js';

const portLabel = (block, at, isOutlet) =>
  (isOutlet ? block.outlets : block.inlets).find((p) => p.at === at)?.name ?? '';

export function Inspector(app, geom) {
  const { ui } = app.state;
  const selected = ui.canvas.selected;
  if (!selected) return null;
  const open = ui.opened.has('details');
  const deselect = () => { ui.canvas.selected = null; app.render(); };

  const panel = (title, { before = [], after = [] }, ...body) => el('section', {
    class: classes('panel details', !open && 'folded'),
  },
    el('div', { class: 'details-bar' },
      IconButton({ icon: open ? 'down' : 'up', class: 'ghost',
                   label: open ? 'fold the details away' : 'unfold the details',
                   'aria-expanded': open ? 'true' : 'false',
                   onclick: () => { if (open) ui.opened.delete('details'); else ui.opened.add('details'); app.render(); } }),
      ...before,
      el('h2', { class: 'details-title' }, title),
      ...after,
      IconButton({ icon: 'close', class: 'ghost details-close', label: 'close the details', onclick: deselect })),
    open ? body : null);

  if (selected.kind === 'arrow') {
    const arrow = geom.arrows.find((a) => a.id === selected.id);
    if (!arrow) return el('p', { class: 'hint' }, 'gone');
    const from = geom.blocks.find((b) => b.id === arrow.from.blockId);
    const to = geom.blocks.find((b) => b.id === arrow.to.blockId);
    const domain = domainName(arrow.domain);
    const cut = () => {
      const fresh = geometry(app);
      const same = fresh.arrows.find((a) => a.id === arrow.id);
      app.editor.applyPlan(same ? planDisconnect(fresh.blocks, same) : { ok: false, why: 'already gone' });
    };
    return panel(`${domain} bus ${arrow.bus}`,
      { before: [el('span', { class: `chip dom-${domain}` }, domain)],
        after: [IconButton({ icon: 'cut', label: 'disconnect this arrow', class: 'ghost danger', onclick: cut })] },
      el('p', {}, `${from?.title ?? '?'} ${from ? portLabel(from, arrow.from.at, true) : ''} → `
                + `${to?.title ?? '?'} ${to ? portLabel(to, arrow.to.at, false) : ''}`),
      arrow.writers > 1 || arrow.readers > 1
        ? el('p', { class: 'hint' }, `${arrow.writers} writing · ${arrow.readers} reading`) : null);
  }

  const block = geom.blocks.find((b) => b.id === selected.id);
  if (!block) return el('p', { class: 'hint' }, 'gone');
  const remove = IconButton({ icon: 'trash', label: `remove ${block.title} (Delete)`, class: 'ghost danger',
                              onclick: () => app.editor.removeBlock(block) });
  if (block.kind === BlockKind.Node) {
    const d = app.device.byId.get(app.state.patch.nodes[block.index]?.algorithmId);
    const copy = IconButton({ icon: 'copy', label: `copy ${block.title} (${shortcut('c')})`, class: 'ghost',
                              onclick: () => app.editor.copyBlock(block) });
    const duplicate = IconButton({
      icon: 'duplicate', label: `another ${block.title} with the same settings (${shortcut('d')})`, class: 'ghost',
      onclick: () => app.editor.duplicateBlock(block, underBlock(geom.positions, block)),
    });
    return panel(block.title,
      { before: [el('span', { class: 'index' }, block.index)],
        after: [d?.wantsTick ? el('span', { class: 'tag' }, 'clocked') : null, copy, duplicate, remove] },
      NodeCard(app, block.index, { header: false }));
  }
  if (block.kind === BlockKind.Jack) return panel(block.title, { after: [remove] }, JackCard(app, block.index));
  // The port's own head is left out: the panel's bar already carries its
  // name and its remove button.
  return panel(block.title, { after: [remove] },
               RouteCard(app, block.index, block.kind === BlockKind.MidiOut, { header: false }));
}
