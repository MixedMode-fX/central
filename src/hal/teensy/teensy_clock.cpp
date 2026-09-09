#include "hal/teensy/teensy_includes.h"

#include "hal/teensy/teensy_clock.h"
#include "hardware.h"

// The sync input goes through the same inverting input stage as the gate
// jacks, so a rising edge at the jack is a falling edge at the pin.
#if GATE_INPUT_ACTIVE_LOW
#define SYNC_EDGE FALLING
#else
#define SYNC_EDGE RISING
#endif

// Both interrupts run at the same priority, so neither can preempt the other
// and the clock's counter is only ever touched by one of them at a time.
static const uint8_t CLOCK_IRQ_PRIORITY = 128;

static IntervalTimer subtick_timer;
static MasterClock* bound_clock = nullptr;

static void subtick_isr(){
    if (bound_clock) bound_clock->advance();
}

static void sync_isr(){
    if (bound_clock) bound_clock->external_edge(micros());
}

void mm_clock_setup(MasterClock& clock){
    bound_clock = &clock;
    pinMode(SYNC_CLOCK, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SYNC_CLOCK), sync_isr, SYNC_EDGE);
    NVIC_SET_PRIORITY(IRQ_GPIO6789, CLOCK_IRQ_PRIORITY);
    mm_clock_service();
}

void mm_clock_service(){
    if (bound_clock == nullptr) return;
    if (!bound_clock->take_interval_change()) return;
    const uint32_t us = bound_clock->subtick_interval_us();
    if (us == 0) return;
    subtick_timer.begin(subtick_isr, us);   // begin() on a running timer reprograms it
    subtick_timer.priority(CLOCK_IRQ_PRIORITY);
}
