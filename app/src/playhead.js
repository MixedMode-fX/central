// The playhead: the one square a step display marks as playing.
//
// Every display in the editor that lays a pattern out in squares - the gate
// grid, the drum lanes, the note lanes, the chords of a harmony's loop, the
// chords on its circle - marks one of them as the one you are hearing. That
// is one rule, and it was written out four times: twice over the grids in
// perform.js and twice over the circle in views.js, each with its own idea of
// which square to mark and its own way of clearing the last one.
//
// **What it marks is the step that is sounding, never the step the next clock
// edge will play.** A display that marks the next one runs a step ahead of the
// MIDI leaving the jack, which is what a musician sees as "the picture is off
// by one" - and it is the bug a harmony's loop had, because the firmware
// reported the slot the next advance fell on and the page painted it. The
// running node answers with what is sounding (`StepEngine::position`,
// `Harmony::loop_position`) and this paints exactly that, so there is one
// place left where the two could ever disagree.
//
// Squares are addressed by id rather than held, because the patch tab is
// rebuilt wholesale on every edit and this is called from the frame loop: an
// id survives the rebuild and an element does not. A square that is not on the
// page - a closed card, a lane shorter than the pattern - is skipped, so a
// display costs nothing while it is out of sight.

// The firmware's "nothing": a byte that is not a step, a slot or a degree.
// Reported before the first advance, where there is no pattern to be on, and
// by a loop slot the walk has not reached yet.
export const NO_STEP = 0xFF;

// Marks `playing` and unmarks every other square of the same display. `idOf`
// names the square for a step; `count` is how many there are.
export function paintPlayhead(idOf, count, playing) {
  for (let step = 0; step < count; step++) {
    const cell = document.getElementById(idOf(step));
    if (cell) cell.classList.toggle('playing', step === playing);
  }
}
