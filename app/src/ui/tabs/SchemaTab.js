// The patch format, as a prompt: what a person actually needs from this page
// is not a schema but a message they can paste somewhere and get a patch back
// from. The schema (`core/schema.js`) is read from the module in front of
// you, so it describes that module, including firmware the app has never
// heard of.

import { el } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Switch } from '../components/Switch.js';
import { Disclosure, remembered } from '../components/Disclosure.js';
import { promptText, schemaText } from '../../core/schema.js';

async function copy(app, text, said) {
  try {
    await navigator.clipboard.writeText(text);
    app.say(said);
  } catch {
    app.say('this browser would not take it - select the text and copy it');
  }
}

const copyButton = (app, label, text, said, klass = '') =>
  el('button', { class: klass, onclick: () => copy(app, text, said) }, label);

// `wrap`, because the schema at the end of the prompt is a single very long
// line and a box that scrolls sideways for a kilometre shows nothing at all.
const box = (text, rows, label) =>
  el('textarea', { class: 'json wrap', spellcheck: 'false', rows: String(rows), 'aria-label': label }, text);

export function SchemaTab(app) {
  const device = app.device;
  if (!device?.capabilities || !device.algorithms?.filter(Boolean).length) {
    return Panel('the patch format',
      Hint('the schema is read from a module, and none is attached — press “connect a module”, '
         + 'or wait for the built-in one to start'));
  }
  const { ui } = app.state;
  // Built once per render rather than once per button: three buttons and two
  // boxes on this page ask for it.
  const prompt = promptText(device, { patch: ui.schemaWithPatch ? app.patches.patchJson() : null });
  const schema = schemaText(device);
  const algorithms = device.algorithms.filter(Boolean).length;
  const size = (text) => `${Math.round(text.length / 1024)} kB`;
  const answer = el('textarea', { class: 'json', spellcheck: 'false', rows: '8', 'aria-label': 'the JSON that came back' });

  return el('div', {},
    Panel('ask for a patch',
      Hint('the module has been asked what it can do, and the answer is below as a JSON Schema inside a '
        + `prompt — ${size(prompt)} of it, because it describes every one of this firmware’s `
        + `${algorithms} algorithms. Copy it, paste it wherever you ask, and bring the JSON back to `
        + 'the box at the bottom of this page.'),
      Row(copyButton(app, 'copy the prompt', prompt, 'the prompt is on the clipboard', 'primary'),
          Switch({ checked: ui.schemaWithPatch, label: 'include the patch I have open',
                   onChange: (on) => { ui.schemaWithPatch = on; app.render(); } })),
      box(prompt, 14, 'the prompt to copy')),
    Panel('the schema',
      Hint('read from the module itself: every algorithm this firmware has, what each inlet, outlet and '
        + 'parameter means, and how many buses, jacks and nodes there are. A patch that validates '
        + 'against it is one this editor can load.'),
      Disclosure({ summary: `the schema on its own (${algorithms} algorithms, ${size(schema)})`,
                   ...remembered(ui, 'schema') },
        box(schema, 12, 'the schema'),
        Row(copyButton(app, 'copy', schema, 'the schema is on the clipboard'),
            el('button', { onclick: () => {
              app.patches.download('mmmc-patch.schema.json', schema, 'application/json');
              app.say('exported the schema');
            } }, 'download .json')))),
    Panel('paste the answer',
      Hint('the same door the library tab’s JSON box is: it is read, built, and sent to the module, which '
        + 'accepts it or says why not.'),
      answer,
      Row(el('button', { class: 'primary', onclick: () => app.patches.loadJson(answer.value, 'the JSON above') },
             'load it'))));
}
