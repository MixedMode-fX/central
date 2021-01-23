#ifndef __LOW_H_
#define __LOW_H_

#include "hardware.h"
#include "gpio.h"
#include "algorithm/algorithm.h"

#define N_NOTE_ON_LIST 8
#define FREE_NOTE 128

class Priority : public Algorithm{
    public:
        Priority(uint8_t midi_inputs, uint8_t midi_outputs, uint16_t gate_inputs, uint16_t gate_outputs) :
            Algorithm(midi_inputs, midi_outputs, gate_inputs, gate_outputs){};

        uint8_t index(uint8_t note);
        uint8_t next_free_slot(){ return index(FREE_NOTE); }
        uint8_t count();

        virtual uint8_t priority_note(){ return FREE_NOTE; };
        
        void note_on(uint8_t new_note);

    protected:
        uint8_t notes[N_NOTE_ON_LIST] = {FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE};
        uint8_t velocities[N_NOTE_ON_LIST] = {FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE, FREE_NOTE};
        int8_t transpose = 0;

    private:
        void _update(){};
};







void note_priority_on(byte channel, byte note, byte velocity){
    // Serial.print("ON = "); Serial.println(note);
    uint8_t prev_note = NOTE_PRIORITY();
    int8_t next_free_note = index_of_note(128);
    if (next_free_note < 0){ next_free_note = 0; } // if there is no free slot, remove the very first note TODO: add counter
    if (index_of_note(note) == -1){ notes[next_free_note] = note; }
    last_velocity = velocity;

    #ifndef LATEST
    if (note NOTE_PRIORITY_OPERATOR prev_note && prev_note < 128){
        PRIORITY_MIDI.sendNoteOn(prev_note + priority_transpose, 0, channel); // note off
        PRIORITY_MIDI.sendNoteOn(note + priority_transpose, velocity, channel);
        usbMIDI.sendNoteOn(prev_note + priority_transpose, 0, channel, 2); // note off
        usbMIDI.sendNoteOn(note + priority_transpose, velocity, channel, 2);
    } else if (count_note_on() == 1 && note != prev_note){
        PRIORITY_MIDI.sendNoteOn(note + priority_transpose, velocity, channel);
        usbMIDI.sendNoteOn(note + priority_transpose, velocity, channel, 2);
    }
    #else
    if (note != prev_note){
        PRIORITY_MIDI.sendNoteOn(note + priority_transpose, velocity, channel);
        usbMIDI.sendNoteOn(note + priority_transpose, velocity, channel, 2);
    }
    #endif

    #ifdef DEBUG
    for (uint8_t i=0; i<N_NOTE_ON_LIST; i++){
        Serial.print(notes[i]); Serial.print("\t");
    }
    Serial.print("ON\t");
    Serial.print(prev_note);
    Serial.print("\t");
    Serial.print(NOTE_PRIORITY());
    Serial.println();
    #endif
    

}

void note_priority_off(byte channel, byte note, byte velocity){
    // Serial.print("OFF = "); Serial.println(note);
    int8_t current_note_index = index_of_note(note);
    if (current_note_index > -1) { notes[current_note_index] = 128; }
    PRIORITY_MIDI.sendNoteOn(note + priority_transpose, 0, channel);
    usbMIDI.sendNoteOn(note + priority_transpose, 0, channel, 3);
    uint8_t next_note = NOTE_PRIORITY();
    if (count_note_on() > 0){ 
        PRIORITY_MIDI.sendNoteOn(next_note + priority_transpose, last_velocity, channel); 
        usbMIDI.sendNoteOn(next_note + priority_transpose, last_velocity, channel, 3);
    }

    #ifdef DEBUG
    for (uint8_t i=0; i<N_NOTE_ON_LIST; i++){
        Serial.print(notes[i]); Serial.print("\t");
    }
    Serial.print("OFF\t");
    Serial.print(NOTE_PRIORITY());
    Serial.print("\t");
    Serial.print(current_note_index);
    Serial.println();

    #endif
}


int8_t index_of_note(uint8_t note){
    for (uint8_t i=0; i<N_NOTE_ON_LIST; i++){
        if(notes[i] == note) return i;
    }
    return -1;
}

uint8_t lowest_note(){
    uint8_t lowest = notes[0];
    for (uint8_t i=1; i<N_NOTE_ON_LIST; i++){
        if(notes[i] < lowest) lowest = notes[i];
    }
    return lowest;
}

uint8_t highest_note(){
    int8_t highest = -1;
    for (uint8_t i=0; i<N_NOTE_ON_LIST; i++){
        if(notes[i] > highest && notes[i] < 128) highest = notes[i];
    }
    return highest;
}


uint8_t count_note_on(){
    uint8_t count = 0;
    for(uint8_t i=0; i<N_NOTE_ON_LIST; i++){
        if (notes[i] < 128) count += 1;
    }
    return count;
}

void reset_all_notes(uint8_t channel){
    for(uint8_t i=0; i<N_NOTE_ON_LIST; i++){
        if (notes[i] < 128){
            note_priority_off(channel, notes[i], 0);
        }
    }
}