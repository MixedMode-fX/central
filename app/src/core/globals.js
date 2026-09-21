// What a global *is*, as against what the patch says it should be.
//
// Most settings have one writer and the two questions have one answer. The
// key and the clock do not: a Key node walks the root off a note bus, a CC,
// an NRPN or a modulation route reaches either of them, a tap moves the
// tempo, and a host being followed decides it outright. None of those touch
// the stored settings - that is deliberate, so a preset saved mid-performance
// captures the key the patch was written in rather than whichever bar it
// happened to be on (src/midi/global_key.h) - which leaves the editor drawing
// a number the module is not running.
//
// So an indicator asks. `LIVE_GLOBALS` (protocol/names.js) says which ones
// can be asked about and the session polls the ones on screen; this is the
// reading, and asking for it is what puts it on the poll list.

import { LIVE_GLOBALS } from '../protocol/names.js';

// `{ value, stored, moved }`: what to show, what the patch holds, and whether
// something else has taken it somewhere. With no module, no poll yet, or a
// module too old to answer, `value` is the stored one and `moved` is false -
// an indicator that cannot know is an indicator that says what the patch
// says, never a blank.
export function globalNow(app, name) {
  const stored = app.state.globals?.[name] ?? 0;
  if (!LIVE_GLOBALS[name]) return { value: stored, stored, moved: false };
  app.live?.watchGlobal(name);
  const live = app.session?.globalsLive?.get(name);
  if (live === undefined) return { value: stored, stored, moved: false };
  return { value: live, stored, moved: live !== stored };
}

// The key as the module is playing it: the one every node reads, which is not
// the one in the patch the moment a Key node or a bound controller has moved
// it. Read together, because a root from the live key and a scale from the
// stored one is a key nobody set.
export function keyNow(app) {
  const root = globalNow(app, 'root');
  const scale = globalNow(app, 'scale');
  return {
    root: root.value % 12,
    scale: scale.value,
    moved: root.moved || scale.moved,
  };
}
