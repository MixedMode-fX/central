# Developer entry points. Everything runs through the project-local PlatformIO
# in .venv/, so a clone needs nothing installed beyond python3.

PIO := .venv/bin/pio
FIRMWARE_ENV := teensy41
VERSION := $(shell cat VERSION)

.PHONY: help setup build test size upload clean distclean

help:
	@echo "make setup    - install PlatformIO and pre-fetch the toolchains"
	@echo "make build    - build firmware for $(FIRMWARE_ENV)"
	@echo "make test     - run the native unit tests"
	@echo "make size     - report firmware flash and RAM usage"
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

clean: $(PIO)
	$(PIO) run -t clean

distclean:
	rm -rf .pio .venv
