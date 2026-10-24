#ifndef MMMC_SURFACE_LAUNCHPAD_H
#define MMMC_SURFACE_LAUNCHPAD_H

#include <stdint.h>
#include "config.h"
#include "hal/isysex_in.h"
#include "hal/imidi_out.h"
#include "hal/midi_types.h"
#include "bus/domain.h"
#include "node/patch.h"
#include "master.h"
#include "patch/patch_manager.h"
#include "control/cc_mapper.h"

// A Novation Launchpad as the module's panel (node/patch.h, LaunchpadConfig).
//
// **It is a control surface, not a node.** It sits where a MIDI transport
// sits: in front of the graph, consuming everything that arrives on its
// cable, and what it emits enters the module by the paths a cable's own
// messages take - a note from the note page reaches `MidiInPort` exactly as a
// keyboard's would, a CC from a widget is offered to the binding table before
// the graph and so can be learned, and a key or a tempo written from a page
// goes through the one applier every other control-plane write goes through
// (control/cc_mapper.h, write_control). Nothing here reaches a bus directly
// and nothing here is a second way to move a parameter.
//
// **Programmer mode is the whole point.** In its factory modes a Launchpad
// decides for itself what a pad means and what it lights; in programmer mode
// every pad is a coordinate, every button is a CC and every LED is the
// host's, so the layout is this file's rather than Novation's. The three MK3
// models - Mini, X and Pro - share that protocol and differ in the device id
// in every SysEx message. The surface asks every INQUIRY_INTERVAL_US whether
// it is linked or not: the answer is also what says a Launchpad has been
// plugged in since boot, and silence for LINK_TIMEOUT_US is what says it has
// gone. Both are the device's own words, so the module never sends a frame of
// LEDs into a cable with nothing on it, and a Launchpad put back after being
// unplugged is put back into programmer mode without a reboot.
//
// **The answer does not say which model.** Novation gives the Mini and the
// Pro the same family in it, so `LP_MODEL_AUTO` asks each model's own
// layout readback in turn, and the one that answers names itself: every
// message a Launchpad sends carries its device id. Only then is it put into
// programmer mode - a Pro sent the Mini's switch ignores it and stays in its
// own modes. A patch that names the model links on the inquiry's answer.
//
// Four pages, on the top row's four right-hand buttons:
//
//   note     the grid is a neck: a semitone a column, a fourth a row, the
//            key painted on it - the roots pink, the scale blue, the notes
//            outside it unlit but playing - and the register moved with the
//            arrows. A pad owns the note-off of the
//            pitch it played, so the key may move under a held chord.
//   key      the root on a keyboard of twelve, the scales, the register.
//   widgets  eight columns of what the patch says they are
//            (LaunchpadWidget), two pages of them on the left and right
//            arrows. A fader whose CC is bound follows the module's own value
//            of its target rather than what was last pressed.
//   tempo    transport, nudges, a sixteen-step tempo bar and the beat.
//
// The right column is the same on every page: start/stop, continue, tap,
// panic. The logo lights when the surface is linked.
//
// **LEDs are a frame, diffed and rate-limited.** The page draws what it
// wants into a frame; once per LED interval the LEDs that differ from what
// the device is showing go out as one SysEx of at most LEDS_PER_MESSAGE, so
// a page change is a few messages rather than one long one, and a DIN
// cable (31250 baud, three kilobytes a second) is never asked for more than
// it can carry. Nothing here blocks or allocates.
class Launchpad : public ISysexIn {
    public:
        enum Page : uint8_t {
            PAGE_NOTE    = 0,
            PAGE_KEY     = 1,
            PAGE_WIDGETS = 2,
            PAGE_TEMPO   = 3,
            PAGES,
        };

        static constexpr uint8_t GRID = 8;
        static constexpr uint8_t PADS = GRID * GRID;
        // LED ids run 11..99 in programmer mode: (row + 1) * 10 + (column + 1)
        // for the grid, 91..98 for the top row, 19..89 for the right column
        // and 99 for the logo.
        static constexpr uint8_t LEDS = 100;
        static constexpr uint8_t WIDGET_COLUMNS = GRID;
        static constexpr uint8_t WIDGET_PAGES = LP_WIDGETS / WIDGET_COLUMNS;
        // How far the note page's register may be moved from the patch's.
        static constexpr int8_t OCTAVE_SHIFT_MAX = 4;
        // The bar the tempo page's beat row counts, as the swap timing does
        // (protocol/sysex_handler.h, BEATS_PER_BAR).
        static constexpr uint8_t BEATS_PER_BAR = 4;

        static constexpr uint32_t INQUIRY_INTERVAL_US = 2000000u;
        static constexpr uint32_t LINK_TIMEOUT_US = 6500000u;
        static constexpr uint32_t LED_INTERVAL_USB_US = 5000u;
        static constexpr uint32_t LED_INTERVAL_DIN_US = 40000u;
        static constexpr uint8_t LEDS_PER_MESSAGE = 32;
        // F0 00 20 29 02 <dev> 03, then three bytes an LED, then F7.
        static constexpr uint16_t LED_MESSAGE_MAX = 7u + 3u * LEDS_PER_MESSAGE + 1u;

        // Novation's manufacturer id, as the three bytes every Launchpad
        // message carries after F0.
        static constexpr uint8_t NOVATION_0 = 0x00, NOVATION_1 = 0x20, NOVATION_2 = 0x29;
        // The device byte of each model (LaunchpadModel), as its programmer's
        // reference gives it.
        static constexpr uint8_t DEVICE_MINI_MK3 = 0x0D;
        static constexpr uint8_t DEVICE_X = 0x0C;
        static constexpr uint8_t DEVICE_PRO_MK3 = 0x0E;
        // The command that selects a layout, and with no argument asks for
        // the current one: every model answers it, with its own device id.
        static constexpr uint8_t COMMAND_LAYOUT = 0x00;

        // The palette indices the surface paints with (0..63 of the
        // Launchpad's 128). The hues come in fours - light, full, dark,
        // darkest - from 4 up; `dim()` finds a colour's dark one.
        static constexpr uint8_t OFF = 0, DIM = 1, GREY = 2, WHITE = 3;
        static constexpr uint8_t RED = 5, ORANGE = 9, YELLOW = 13, GREEN = 21, CYAN = 37, BLUE = 45;
        static constexpr uint8_t VIOLET = 49, MAGENTA = 53, PINK = 57;
        static uint8_t dim(uint8_t colour);
        // The widget colour a zero byte means.
        static constexpr uint8_t WIDGET_DEFAULT_COLOUR = BLUE;

        Launchpad(PatchManager& patches, MixedModeMaster& master, CcMapper& mapper, IMidiOut& midi);
        Launchpad(const Launchpad&) = delete;
        Launchpad& operator=(const Launchpad&) = delete;

        // One channel-voice message from a transport. Consumed - translated
        // and acted on, or dropped - when it arrived on the surface's cable;
        // false for every other cable, where the caller dispatches it as
        // usual (control/midi_dispatch.h).
        bool receive(uint8_t source, const MidiEvent& event, uint32_t now_us);

        // SysEx that was not the protocol's, forwarded by the protocol
        // handler: the device's answer to the inquiry, and the answer to the
        // layout readback that says which model it is, are what link it.
        void deliver_sysex(uint8_t source, const uint8_t* data, uint16_t length, uint32_t now_us) override;

        // Once per pass, after the pass: follows the patch's configuration,
        // keeps the link alive, draws the page and sends the LEDs that moved.
        void service(uint32_t now_us);

        // What the protocol reports (SYSEX_LP_STATE).
        bool linked() const { return is_linked; }
        // The model the surface is talking to: the detected one, else the
        // configured one. LP_MODEL_AUTO while nothing has answered.
        uint8_t model() const;
        uint8_t page() const { return current_page; }
        int8_t octave_shift() const { return shift; }
        uint8_t widget_page() const { return widgets_page; }
        uint8_t port() const { return active_port; }
        // The frame as last drawn, by LED id, for the tests.
        uint8_t led(uint8_t id) const { return id < LEDS ? want[id] : 0; }

        // The grid as the device numbers it.
        static uint8_t led_id(uint8_t row, uint8_t col){ return (uint8_t)((row + 1u) * 10u + col + 1u); }
        static uint8_t top_id(uint8_t col){ return (uint8_t)(91u + col); }
        static uint8_t side_id(uint8_t row){ return (uint8_t)((row + 1u) * 10u + 9u); }
        static constexpr uint8_t LOGO_ID = 99;

        // The eight levels a fader pad stands for, bottom row first.
        static uint8_t fader_level(uint8_t row){ return (uint8_t)((row * 127u + 3u) / 7u); }
        // The tempo a step of the tempo bar sets, 0..15 across the clock's
        // whole range.
        static uint16_t bar_tempo(uint8_t step);

    private:
        // The top row, left to right: arrows, then the pages.
        enum TopButton : uint8_t { TOP_UP = 0, TOP_DOWN, TOP_LEFT, TOP_RIGHT,
                                   TOP_KEY, TOP_NOTE, TOP_WIDGETS, TOP_TEMPO };
        // The right column, top to bottom.
        enum SideButton : uint8_t { SIDE_PLAY = 7, SIDE_CONTINUE = 6, SIDE_TAP = 5, SIDE_PANIC = 4 };

        static constexpr uint8_t NO_NOTE = 0xFF;
        static constexpr uint8_t UNKNOWN = 0xFF;

        const LaunchpadConfig& config() const { return patches.active().launchpad; }
        uint8_t device_byte() const;
        bool is_din() const;
        uint32_t led_interval() const;

        void follow_config(uint32_t now_us);
        void unlink(uint32_t now_us);
        void enter_programmer_mode();
        void leave_programmer_mode();
        void send_inquiry(uint32_t now_us);
        void ask_model(uint8_t device);
        void link(uint32_t now_us);
        static uint8_t model_of(uint8_t device);
        void invalidate();

        // Input.
        void pad(uint8_t row, uint8_t col, uint8_t velocity, bool down, uint32_t now_us);
        void top(uint8_t index, bool down, uint32_t now_us);
        void side(uint8_t index, bool down, uint32_t now_us);
        void note_pad(uint8_t row, uint8_t col, uint8_t velocity, bool down, uint32_t now_us);
        void key_pad(uint8_t row, uint8_t col, uint32_t now_us);
        void widget_pad(uint8_t row, uint8_t col, bool down, uint32_t now_us);
        void tempo_pad(uint8_t row, uint8_t col, uint32_t now_us);
        void nudge_tempo(int16_t by, uint32_t now_us);
        void set_tempo(int32_t bpm, uint32_t now_us);
        void set_page(uint8_t page, uint32_t now_us);
        void release_all(uint32_t now_us);
        void emit_note(uint8_t type, uint8_t channel, uint8_t note, uint8_t velocity, uint32_t now_us);
        void emit_cc(uint8_t channel, uint8_t cc, uint8_t value, uint32_t now_us);
        // The pitch a pad of the note page plays, or NO_NOTE off the keyboard.
        uint8_t pitch_of(uint8_t row, uint8_t col) const;
        // A widget fader's level in pads, from the module's own value where
        // its CC is bound and from what was last pressed where it is not.
        uint8_t fader_shown(uint8_t slot, const LaunchpadWidget& g) const;

        // Output.
        void draw(uint32_t now_us);
        void draw_note();
        void draw_key();
        void draw_widgets();
        void draw_tempo(uint32_t now_us);
        void draw_frame(uint32_t now_us);
        void set(uint8_t id, uint8_t colour){ if (id < LEDS) want[id] = (uint8_t)(colour & 0x7F); }
        void flush(uint32_t now_us);

        PatchManager& patches;
        MixedModeMaster& mm;
        CcMapper& cc;
        IMidiOut& midi;

        uint8_t active_port;
        uint8_t active_model;       // the configured model the link was made for
        uint8_t seen_model;         // LaunchpadModel from the device id it sent, or LP_MODEL_AUTO
        bool is_linked;
        uint32_t last_inquiry_us;
        uint32_t last_seen_us;
        bool ever_asked;

        uint8_t current_page;
        int8_t shift;
        uint8_t widgets_page;
        // What each pad of the note page is holding: the pitch and the
        // channel it was played on, so the release is its own.
        uint8_t held_note[PADS];
        uint8_t held_channel[PADS];
        // Per widget: a fader's last level, the toggles that are on, the
        // buttons that are down.
        uint8_t fader_value[LP_WIDGETS];
        uint8_t toggles_on[LP_WIDGETS];
        uint8_t buttons_down[LP_WIDGETS];
        // Side buttons held, so a continue lights while it is pressed.
        uint8_t side_down;

        uint8_t want[LEDS];
        uint8_t shown[LEDS];       // UNKNOWN until sent
        uint32_t last_led_us;
        uint8_t tx[LED_MESSAGE_MAX];
};

static_assert(LP_WIDGETS % Launchpad::WIDGET_COLUMNS == 0, "the widget pages are whole pages");
static_assert(Launchpad::WIDGET_PAGES >= 1, "at least one page of widgets");

#endif
