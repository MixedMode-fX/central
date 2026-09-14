// The playhead: the one square a step display marks as playing.
//
// Every display that lays a pattern out in squares - the gate grid, the drum
// lanes, the note lanes, the chords of a harmony's loop, the chords on its
// circle - marks one of them as the one you are hearing. That is one rule,
// written once.
//
// **What it marks is the step that is sounding, never the step the next
// clock edge will play.** A display that marks the next one runs a step
// ahead of the MIDI leaving the jack, which a musician reads as the module
// being wrong. The running node answers with what is sounding
// (`StepEngine::position`, `Harmony::loop_position`) and this paints exactly
// that, so there is one place left where the two could ever disagree.
//
// The squares are the elements the view built, closed over by the painter it
// registered with `Live`: the page is rebuilt wholesale on every edit and the
// registry emptied with it, so a painter never outlives its cells.

// The firmware's "nothing": a byte that is not a step, a slot or a degree.
// Reported before the first advance, where there is no pattern to be on, and
// by a loop slot the walk has not reached yet.
export const NO_STEP = 0xFF;

// Marks `playing` and unmarks every other square of the same display.
export function paintPlayhead(cells, playing) {
  cells.forEach((cell, step) => cell.classList.toggle('playing', step === playing));
}
