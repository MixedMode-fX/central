#ifndef MMMC_TEST_FAKE_CONSOLE_IO_H
#define MMMC_TEST_FAKE_CONSOLE_IO_H

#include <string>
#include "hal/iconsole_io.h"

// IConsoleIo for the native tests: input is a string the test queues, output
// is a string the test searches.
class FakeConsoleIo : public IConsoleIo {
    public:
        static constexpr size_t RESERVE = 1u << 18;

        FakeConsoleIo() : in(), out(), at(0) {
            out.reserve(RESERVE);   // so recording never allocates mid-test
            in.reserve(4096);
        }

        bool read(uint8_t& out) override {
            if (at >= in.size()) return false;
            out = (uint8_t)in[at++];
            return true;
        }
        void write(const char* text) override { out += text; }

        void type(const char* text){ in += text; }
        void clear(){ out.clear(); }
        bool said(const char* needle) const { return out.find(needle) != std::string::npos; }

        std::string in;
        std::string out;
        size_t at;
};

#endif
