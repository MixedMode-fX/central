# MixedMode Modular Central (MMMC)

An expandable Eurorack CV & MIDI processor based around a Teensy 4.1

It provides `N_IO_PORT = 8` configurable input / output ports. Each port can be assigned to it's own independant algorithms. 

MMMC supports MIDI I/O over several transports : DIN Midi (3.5mm TRS) and USB MIDI and USB Host. The class compliant USB device shows up as 4 independant MIDI ports when connected to a host computer.



## Basics

The MixedMode Modular Central (MMMC) can clock off three sources:

- Internal
- CV
- MIDI

The behaviour of the rest of the module should be the same regardless of the clock source. The MMMC will operate under a `PPQN = 24` master clock derived from any of the sources listed above.

Each I/O channel can be configured as either an **input** or an **output**. They can be assigned to their own **algorithm** or be part of an algorithm that uses multiple inputs and/or outputs.

Each MIDI I/O can be routed to each other and MIDI Modifier Algorithms can be applied independently. 

## Output Algorithms

### Clock Dividers / Multipliers

Each output channel will have its own independant clock divider / multiplier. This will drive the refresh rate at which the algorithms will operate.

Each clock has its own independent `phase` which represents

### Gate Sequencers

The MMMC offers several types of gate sequencers:

- `Metronome` : every tick of the clock is outputted according to the clock multiplier/divider

- `StepSequencer` : a classic step sequencer with variable length (1-16). Each step can be ON/OFF.

- `EuclidianSequencer` : regular old Euclidian

- `RandomSequencer`: shred and load a random pattern

## Logic Algorithms

Configurable logic gates (all gates can also be inverted) & latches:

- NOT
- AND
- NAND
- OR
- NOR
- XOR
- NXOR
- ASTABLE
- LATCH

Logic algorithms are not clocked by the master clock and happen at a much higher sample rate

# Code structure

`Setters` and `Getters` are not represented in the diagram below. 

```mermaid
classDiagram

    class MixedModeMaster{
        + uint8_t clock_source
        + GateAlgorithm gate_algorithms[N_IO_PORT]
        + MIDIAlgorithm midi_algorithm[N_MIDI_SLOT]
        + void tick()
    }

    class Clock{
        + uint32_t counter
        + uint8_t modifier
        + uint8_t type
        + int phase
        + int delay
        + void tick()
    }

    class GateAlgorithm{
        + bool enable
        + uint8_t io_pins[]
        + uint8_t io_types[]
        + void setup()
        + void tick()
    }
    GateAlgorithm --|> GateSequencer

    class GateSequencer{
        + Clock clock
        + uint8_t length
        + uint8_t sequence[MAX_SEQUENCE_LEN]
    }

    GateSequencer --|> Metronome
    GateSequencer --|> StepSequencer
    GateSequencer --|> EuclidianSequencer
    GateSequencer --|> RandomSequencer

    GateAlgorithm --|> LogicAlgorithm
    LogicAlgorithm --|> NOT
    LogicAlgorithm --|> AND
    LogicAlgorithm --|> OR
    LogicAlgorithm --|> XOR
    LogicAlgorithm --|> LATCH
    LogicAlgorithm --|> ASTABLE

    MIDIAlgorithm

```

# Control & Feedback

The module has one encoder and two switches.
Each output has an RGB LED indicating the signal status and algorithm used.
A small square OLED is present to help with settings

A tight integration with Novation Launchpads using the DAW mode would be great. This would allow for additional control and feedback.