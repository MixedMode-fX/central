# Developer entry points. Everything runs through the project-local PlatformIO
# in .venv/, so a clone needs nothing installed beyond python3.

PIO := .venv/bin/pio
FIRMWARE_ENV := teensy41
VERSION := $(shell cat VERSION)

.PHONY: help setup build test size upload emulator editor clean distclean

help:
	@echo "make setup    - install PlatformIO and pre-fetch the toolchains"
	@echo "make build    - build firmware for $(FIRMWARE_ENV)"
	@echo "make test     - run the native unit tests"
	@echo "make size     - report firmware flash and RAM usage"
	@echo "make emulator - build the browser emulator (needs clang + lld) and smoke-test it"
	@echo "make editor   - check the editor's protocol module and test it against the emulator"
	@echo "make upload   - flash an attached Teensy"
	@echo "make clean    - remove build output"
	@echo ""
	@echo "current version: $(VERSION)"

$(PIO):
	@scripts/bootstrap.sh

setup:
	@scripts/bootstrap.sh --warm

build: $(PIO)
	$(PIO) run -e $(FIRMWARE_ENV)

test: $(PIO)
	$(PIO) test -e native

size: $(PIO)
	$(PIO) run -e $(FIRMWARE_ENV) -t size

upload: $(PIO)
	$(PIO) run -e $(FIRMWARE_ENV) -t upload

# The firmware core compiled to WebAssembly with the browser as the hardware.
# See emulator/README.md. PlatformIO is not involved: stock clang and lld.
emulator:
	emulator/build.sh
	node emulator/test/smoke.mjs

# The editor is a client of the firmware's protocol, so it is checked against
# the firmware: the generated protocol module has to match the headers, and the
# editor's own codec and validator are driven against the module compiled to
# WebAssembly. Needs `make emulator` first for the wasm.
editor:
	node editor/tools/generate-protocol.mjs --check
	node editor/test/protocol.test.mjs

clean: $(PIO)
	$(PIO) run -t clean

distclean:
	rm -rf .pio .venv
