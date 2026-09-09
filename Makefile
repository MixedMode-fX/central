# Developer entry points. Everything runs through the project-local PlatformIO
# in .venv/, so a clone needs nothing installed beyond python3.

PIO := .venv/bin/pio
FIRMWARE_ENV := teensy41
VERSION := $(shell cat VERSION)

.PHONY: help setup build test size upload app module emulator editor clean distclean

help:
	@echo "make setup    - install PlatformIO and pre-fetch the toolchains"
	@echo "make build    - build firmware for $(FIRMWARE_ENV)"
	@echo "make test     - run the native unit tests"
	@echo "make size     - report firmware flash and RAM usage"
	@echo "make module   - compile the firmware to WebAssembly (needs clang + lld) and build the app"
	@echo "make app      - build the module, then check the app against it"
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

# The firmware core compiled to WebAssembly with the browser as the hardware,
# and the app that runs it (emulator/README.md, app/README.md). PlatformIO is
# not involved: stock clang, lld and node.
module:
	emulator/build.sh
	node emulator/test/smoke.mjs

# The app is a client of the firmware's protocol, so it is checked against the
# firmware: the generated protocol module has to match the headers, and the
# app's own codec, validator, library and runtime seam are driven against the
# module compiled to WebAssembly.
app: module
	node app/tools/generate-protocol.mjs --check
	node app/test/protocol.test.mjs
	node app/test/app.test.mjs

# The names these two had before the editor and the emulator became one app.
emulator: module
editor: app

clean: $(PIO)
	$(PIO) run -t clean

distclean:
	rm -rf .pio .venv
