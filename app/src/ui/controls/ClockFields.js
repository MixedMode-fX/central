// The clock's source and tempo, as two fields: on the play tab, where the
// module is being run, and under MIDI, where the rest of the globals are.
// One control, so the two cannot disagree about what a tempo is.

import * as P from '../../protocol/generated.js';
import { CLOCK_SOURCES } from '../../protocol/names.js';
import { Field } from '../components/Field.js';
import { Select } from '../components/Select.js';
import { NumberField } from '../components/NumberField.js';

export function ClockFields(app) {
  const g = app.state.globals;
  return [
    Field({ label: 'clock source' }, Select({
      options: CLOCK_SOURCES, value: g.clockSource,
      onChange: (clockSource) => app.editor.setGlobals({ clockSource }),
    })),
    Field({ label: 'tempo', hint: `${P.CLOCK_MIN_BPM}–${P.CLOCK_MAX_BPM} BPM` }, NumberField({
      value: g.bpm, min: P.CLOCK_MIN_BPM, max: P.CLOCK_MAX_BPM, fallback: P.CLOCK_DEFAULT_BPM,
      wide: true, 'aria-label': 'tempo in BPM',
      onChange: (bpm) => app.editor.setGlobals({ bpm }),
    })),
  ];
}
