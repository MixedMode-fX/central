#include "control/midi_dispatch.h"
#include "hal/midi_types.h"

uint16_t dispatch_midi(MidiInputQueue& queue, SysexHandler& protocol, NrpnDecoder& nrpn,
                       CcMapper& cc, MixedModeMaster& master, uint32_t now_us){
    uint16_t taken = 0;
    SourcedMidiEvent in;
    while (queue.peek(in)){
        // A bus with no room this pass: leave this and everything behind it
        // for the next pass, in order.
        if (!master.has_room(in.source, in.event)) break;
        queue.pop(in);
        taken++;

        if (in.event.type == MIDI_PROGRAM_CHANGE &&
            protocol.program_change(in.source, in.event.channel, in.event.data1, now_us)) continue;
        if (in.event.type == MIDI_CONTROL_CHANGE &&
            nrpn.observe(in.source, in.event.channel, in.event.data1, in.event.data2, now_us)) continue;
        if (in.event.type == MIDI_CONTROL_CHANGE &&
            cc.observe(in.source, in.event.channel, in.event.data1, in.event.data2, now_us)) continue;
        master.deliver_midi(in.source, in.event, now_us);
    }
    return taken;
}
