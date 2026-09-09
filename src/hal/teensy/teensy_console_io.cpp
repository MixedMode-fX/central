#include "hal/teensy/teensy_includes.h"
#include "hal/teensy/teensy_console_io.h"
#include "hardware.h"

void TeensyConsoleIo::begin(){
    SERIAL_UART.begin(SERIAL_BAUD_RATE);
}

bool TeensyConsoleIo::read(uint8_t& out){
    if (Serial.available()){ out = (uint8_t)Serial.read(); return true; }
    if (SERIAL_UART.available()){ out = (uint8_t)SERIAL_UART.read(); return true; }
    return false;
}

void TeensyConsoleIo::write(const char* text){
    Serial.print(text);
    // Only what fits: a UART with nothing draining it must not stall the
    // main loop waiting for room.
    for (const char* p = text; *p != '\0'; p++){
        if (SERIAL_UART.availableForWrite() <= 0) return;
        SERIAL_UART.write((uint8_t)*p);
    }
}
