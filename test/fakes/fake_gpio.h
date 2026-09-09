#ifndef MMMC_TEST_FAKE_GPIO_H
#define MMMC_TEST_FAKE_GPIO_H

#include <vector>
#include "config.h"
#include "hal/igpio.h"

// IGpio for the native tests: input levels are set by the test, every mode
// change and write is recorded, and the last written level per port is kept.
class FakeGpio : public IGpio {
    public:
        struct Write { uint8_t port; uint8_t state; };

        FakeGpio() : modes(), inputs(), outputs(), writes() {
            for (uint8_t i = 0; i < GPIO_N; i++) {
                modes[i] = GPIO_MODE_INPUT;
                inputs[i] = GPIO_LOW;
                outputs[i] = GPIO_LOW;
            }
        }

        void mode(uint8_t port, uint8_t m) override { if (port < GPIO_N) modes[port] = m; }
        void write(uint8_t port, uint8_t state) override {
            writes.push_back(Write{port, state});
            if (port < GPIO_N) outputs[port] = state;
        }
        uint8_t read(uint8_t port) override { return port < GPIO_N ? inputs[port] : 0xFF; }

        // Test-side controls
        void set_input(uint8_t port, uint8_t level) { inputs[port] = level; }
        void clear_writes() { writes.clear(); }

        uint8_t modes[GPIO_N];
        uint8_t inputs[GPIO_N];
        uint8_t outputs[GPIO_N];
        std::vector<Write> writes;
};

#endif
