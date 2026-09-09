#ifndef MMMC_ALGORITHM_SEQUENCER_NOTE_SEQUENCER_H
#define MMMC_ALGORITHM_SEQUENCER_NOTE_SEQUENCER_H

#include "node/node.h"
#include "midi/sounding_notes.h"
#include "algorithm/sequencer/step_engine.h"

// Note sequencers (#13): the step engine writing a note bus.
//
// A step stores a **scale degree**, never an absolute note:
//
//     pitch = root + scale_degree_to_semitone(degree, scale)
//
// so changing the root transposes the whole pattern and keeps it in key, and
// changing the scale gives the same pattern a different character - both
// one-parameter operations on a pattern nobody touches. The root can come
// from a note bus (last note-on wins), so a keyboard transposes the running
// sequence; the scale is a 12-bit mask (midi/scale.h), two bytes that cover
// every named mode and any user scale.
//
// Inlet 0 (gate): advance. One step per rising edge, from a ClockDiv, a
//         logic gate, a jack, or another sequencer.
// Inlet 1 (gate, optional): reset. The next advance plays the first step, and
//         the record cursor goes back to step 0.
// Inlet 2 (note, optional): root. Note-ons set it; nothing else is read.
// Inlet 3 (note, optional): record (#22). Step-record: a note-on writes the
//         step under the record cursor and advances it. Works with any
//         keyboard patched to any port, needs no host, and is how a hardware
//         sequencer actually feels.
// Inlet 4 (gate, optional): record enable. While it is patched, recording only
//         happens when it is high; unpatched, a patched record inlet always
//         records.
// Outlet 0 (note): the notes.
//
// **Pitch to degree** is a real conversion, not a copy: a step stores a
// degree and a keyboard sends a pitch. A played note is quantised to the
// nearest tone of the current scale and stored as that tone's degree relative
// to the root - the same snap Quantise does, so a note outside the scale
// lands on the nearest one in it rather than being refused. Out-of-scale
// notes are counted (`snapped()`) so a user can tell it happened.
//
// **Rest and tie** have to be enterable or step-record is only good for
// continuous runs. Two note numbers are reserved for them: `rest key`
// (params[11], default MIDI note 0) and `tie key` (params[12], default note
// 1). Both are below anything a keyboard plays, and both are configurable if
// a controller does reach them.
//
// **Real-time record** - capturing against the running clock, quantised to
// the step grid - is a second stage and is deliberately not built. It needs
// the same period estimate the sub-step gate does, and that should prove
// itself on hardware first.
//
// Header, params[0..15]:
//   [0] length      1..MAX_SEQUENCE_LEN (0 -> 8)
//   [1] direction   StepEngine::Direction
//   [2] gate        0: note lengths are exact, in advance edges (below).
//                   1..100: the last edge-unit of every note is shortened to
//                   this percent of the measured step period - an estimate,
//                   see "Note length" below.
//   [3] scale low   scale mask bits 0..7   (both 0 -> chromatic)
//   [4] scale high  scale mask bits 8..11
//   [5] root        MIDI note when the root inlet is unpatched (0 -> 60)
//   [6] vel scale   percent applied to every velocity (0 -> 100)
//   [7] vel offset  int8 added after scaling
//   [8] channel     1..16 (0 -> 1)
//   [9] accent      velocity added by the accent flag (0 -> 30)
//  [10] stall       advance periods of silence after which sounding notes
//                   are released (0 -> 4, 255 -> never), so a clock that
//                   stops cannot leave a note held for ever
//  [11] rest key    the note that writes a rest when recording (0 -> note 0)
//  [12] tie key     the note that writes a tie when recording (0 -> note 1)
//  [13] rec velocity 0 keeps the velocity played, 1..127 forces one
//  [14..15]         reserved, zero
//
// Steps, from params[STEP_BASE], stride(voices) bytes each:
//   per voice: degree (int8), velocity (1..127; 0 -> the voice is silent)
//   then:      length (bits 0-4, in advance edges, 0 -> 1) | flags (bit 5
//              rest, bit 6 tie, bit 7 accent)
//              probability (percent, 0 -> always)
//
// Note length, the exact half: a note is released on the Nth advance edge
// after the one that played it, so ties and holds need nothing but the
// advance inlet and are exactly in time. A voice holds one note: a new note
// on a voice releases what that voice was playing first, so a length that
// reaches past the next played step is cut there - use a tie or a rest to
// hold through. A **tie** step extends every sounding note by its own length
// instead of retriggering; a tie with nothing sounding plays as a plain step.
// A **rest** plays nothing, and sounding notes keep counting down through it.
//
// Note length, the estimated half: a sub-step gate (`gate` percent) needs
// the *duration* of a step, and the sequencer only sees edges, so it
// measures the interval between the last two advances and extrapolates. It
// is wrong on the first step after a tempo change and on any irregular
// trigger, the same caveat as multiplying from a gate in ClockDiv. Until two
// edges have been seen there is no estimate and lengths are exact. If the
// edge that would have ended the note arrives before the estimate says so,
// the edge wins. Ratcheting has the identical problem and is deliberately
// not built: it would need the same estimate, and this one should prove
// itself on hardware first.
//
// Note-off ownership: the sequencer owns a note-off for every note-on it
// emits and releases it with the pitch it actually sent, from the ledger
// (midi/sounding_notes.h) - never recomputed from the current root or scale.
// The root moving under a held note, the scale changing, the pattern
// changing, the clock stopping (see `stall`) and the patch being swapped
// (silence(), called by the master before the node is destroyed) all
// release correctly, and there are tests for each.
class NoteSequencerBase : public Node{
    public:
        static constexpr uint8_t MAX_VOICES = NOTE_SEQ_VOICES;
        static constexpr uint16_t STEP_BASE = 16;
        static constexpr uint8_t P_LENGTH = 0, P_DIRECTION = 1, P_GATE = 2, P_SCALE_LO = 3, P_SCALE_HI = 4,
                                 P_ROOT = 5, P_VEL_SCALE = 6, P_VEL_OFFSET = 7, P_CHANNEL = 8, P_ACCENT = 9,
                                 P_STALL = 10, P_REST_KEY = 11, P_TIE_KEY = 12, P_REC_VELOCITY = 13;
        static constexpr uint8_t LENGTH_MASK = 0x1F, FLAG_REST = 0x20, FLAG_TIE = 0x40, FLAG_ACCENT = 0x80;
        static constexpr uint8_t NO_PITCH = 0xFF;
        static constexpr uint8_t DEFAULT_LENGTH = 8, DEFAULT_ROOT = 60, DEFAULT_ACCENT = 30, DEFAULT_STALL = 4;
        static constexpr uint8_t DEFAULT_REST_KEY = 0, DEFAULT_TIE_KEY = 1;
        static constexpr uint8_t STALL_NEVER = 255;
        // With no measured period, the stall timeout counts this per period.
        static constexpr uint32_t STALL_UNKNOWN_PERIOD_US = 1000000;

        static constexpr uint8_t stride(uint8_t voices){ return (uint8_t)(voices * 2u + 2u); }
        static constexpr uint16_t STEP_BYTES = MAX_SEQUENCE_LEN * (MAX_VOICES * 2u + 2u);
        static constexpr uint16_t param_count(uint8_t voices){ return (uint16_t)(STEP_BASE + MAX_SEQUENCE_LEN * stride(voices)); }

        NoteSequencerBase(const NodeConfig& config, uint8_t voices);

        void process(BusManager& bus, uint32_t now_us) override;
        void silence(BusManager& bus) override;
        // The whole parameter space (#20): the 16-byte header and every step
        // byte. A sounding note is released from the ledger at the pitch it
        // was sent at, so root, scale, length and the pattern itself can all
        // move underneath one without stranding it.
        bool set_param(uint16_t index, uint8_t value) override;
        uint8_t get_param(uint16_t index) const override;

        // The 16-byte header block, shared by the mono and poly variants.
        // The step block differs (its stride is the voice count) and lives
        // in the .cpp beside each descriptor.
        static const ParamDescriptor HEADER[16];

        // The typed seams, kept because they read better in a test.
        void set_root(uint8_t note){ root = note & 0x7F; }
        void set_scale_mask(uint16_t mask){ scale_mask = mask & 0x0FFF; }
        // Live length change: the cursor is left alone and clamped on the
        // next advance (StepEngine::set_length), so the pattern does not jump
        // under a running sequence.
        void set_length(uint8_t length){ engine.set_length(length, DEFAULT_LENGTH); }
        void set_step(uint8_t step, uint8_t voice, int8_t degree, uint8_t velocity);

        uint8_t voices() const { return n_voices; }
        uint8_t root_note() const { return root; }
        uint16_t scale() const { return scale_mask; }
        uint8_t channel_number() const { return channel; }
        uint8_t length() const { return engine.length(); }
        uint8_t position() const { return engine.position(); }
        uint32_t steps_taken() const { return engine.steps_taken(); }
        uint8_t sounding_count() const { return sounding.count(); }
        uint32_t refused() const { return sounding.refused(); }
        // Step-record diagnostics: where the next played note lands, and how
        // many played notes had to be snapped into the scale.
        uint8_t record_cursor() const { return rec_cursor; }
        uint32_t snapped() const { return snap_count; }
        bool recording() const { return rec_in != NO_BUS; }
        bool period_known() const { return have_period; }
        uint32_t period_us() const { return period; }

        // Step data as stored.
        int8_t degree(uint8_t step, uint8_t voice) const;
        uint8_t velocity(uint8_t step, uint8_t voice) const;
        uint8_t step_length(uint8_t step) const { return (uint8_t)((flags(step) & LENGTH_MASK) ? (flags(step) & LENGTH_MASK) : 1u); }
        uint8_t flags(uint8_t step) const;
        uint8_t probability(uint8_t step) const;
        // The pitch a voice of a step resolves to against the current root
        // and scale, or NO_PITCH if the voice is silent or out of range.
        uint8_t pitch(uint8_t step, uint8_t voice) const;
        // The velocity as it would be sent, after accent, scale and offset.
        uint8_t sent_velocity(uint8_t step, uint8_t voice) const;

    private:
        struct Voice {
            uint8_t edges_left;
            uint32_t release_at_us;
            bool sounding;
            bool timed;
        };

        const uint8_t* step_bytes(uint8_t step) const { return &steps[(uint16_t)step * stride(n_voices)]; }
        uint8_t* step_bytes(uint8_t step){ return &steps[(uint16_t)step * stride(n_voices)]; }
        bool any_sounding() const;
        void play_step(BusManager& bus, uint8_t step, uint32_t now_us);
        void release_voice(BusManager& bus, uint8_t v);
        void release_all(BusManager& bus);
        void time_last_unit(Voice& v, uint32_t now_us) const;

        // A played note, turned into the step it writes.
        void record_note(uint8_t pitch, uint8_t velocity);
        void write_rest();
        void write_tie();
        void advance_record_cursor();

        EdgeIn advance_in;
        EdgeIn reset_in;
        uint8_t root_in;
        uint8_t rec_in;
        EdgeIn rec_enable_in;
        uint8_t rec_enable_bus;    // NO_BUS when nothing gates recording
        uint8_t out;
        uint8_t n_voices;
        uint16_t scale_mask;
        uint8_t root;
        uint8_t gate_pct;
        uint8_t vel_scale;
        int8_t vel_offset;
        uint8_t channel;
        uint8_t accent;
        uint8_t stall_periods;
        uint8_t rest_key;
        uint8_t tie_key;
        uint8_t rec_velocity;
        uint8_t rec_cursor;
        uint32_t snap_count;
        uint32_t last_edge_us;
        uint32_t period;
        bool have_edge;
        bool have_period;
        StepEngine engine;
        Xorshift32 rng;
        SoundingNotes sounding;
        Voice voice[MAX_VOICES];
        // Copied from the preset at construction: the pool keeps no NodeConfig.
        uint8_t steps[STEP_BYTES];
};

// Monophonic: one voice per step, four bytes a step.
class NoteSequencer : public NoteSequencerBase{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit NoteSequencer(const NodeConfig& config) : NoteSequencerBase(config, 1) {}
};

// Up to NOTE_SEQ_VOICES voices per step, each with its own degree and
// velocity; length, flags and probability are per step. Ten bytes a step at
// four voices, which with the ledger makes this one of the two nodes that
// set NODE_SLOT_SIZE (the other is DrumSeqMidi).
class PolySequencer : public NoteSequencerBase{
    public:
        static const AlgorithmDescriptor descriptor;
        explicit PolySequencer(const NodeConfig& config) : NoteSequencerBase(config, NOTE_SEQ_VOICES) {}
};

#endif
