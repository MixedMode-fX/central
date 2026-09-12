# Developer entry points. Everything runs through the project-local PlatformIO
# in .venv/, so a clone needs nothing installed beyond python3, and a clang
# for the WebAssembly build.
#
# One entry point per verb — the same words a human types, an agent types and
# CI runs. A verb that only exists in someone's shell history is a verb that
# gets re-derived, differently, in every session.
#
# Nothing in the mobile loop depends on this file: the hooks and the skills
# call scripts/*.sh by path, because a hook cannot rely on make being
# installed. It exists so that a session at a desk and a run in CI reach those
# same scripts by the same name you do.

PIO := .venv/bin/pio
FIRMWARE_ENV := teensy41
VERSION := $(shell cat VERSION)

.PHONY: help setup ready build test size upload app module emulator editor \
        checks dev preview stop shots clean distclean

help:
	@echo "make setup    - install PlatformIO and pre-fetch the toolchains"
	@echo "make ready    - wait for the dev environment to be prepared"
	@echo ""
	@echo "make checks   - the commit gate: firmware + app (silence = pass)"
	@echo "make build    - build firmware for $(FIRMWARE_ENV)"
	@echo "make test     - run the native unit tests"
	@echo "make size     - report firmware flash and RAM usage"
	@echo "make module   - compile the firmware to WebAssembly (needs clang + lld) and build the app"
	@echo "make app      - build the module, then check the app against it"
	@echo "make upload   - flash an attached Teensy"
	@echo ""
	@echo "make dev      - serve app/ as it is (detached, idempotent)"
	@echo "make preview  - serve the single-file build instead"
	@echo "make stop     - stop what dev/preview started"
	@echo "make shots    - photograph the app at phone and desktop widths"
	@echo ""
	@echo "make clean    - remove build output"
	@echo ""
	@echo "current version: $(VERSION)"

$(PIO):
	@scripts/bootstrap.sh

setup:
	@scripts/bootstrap.sh --warm

# Blocks until the environment is usable, returns instantly when it already is.
ready:
	bash scripts/await_ready.sh

# --- Quality -----------------------------------------------------------------

# The same script the commit hook runs on a phone, reached here by the name CI
# uses. There is no second list. Takes the same scope arguments it does:
# `make checks SCOPE=app`.
checks:
	bash scripts/checks.sh $(SCOPE)

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
app:
	bash scripts/checks.sh app

# The names these two had before the editor and the emulator became one app.
emulator: module
editor: app

# --- Run it ------------------------------------------------------------------

dev:
	bash scripts/start_app.sh

preview:
	bash scripts/start_app.sh --build

stop:
	bash scripts/stop_app.sh

# Widths, not one width: the shells this app crosses are 390 and 1440.
shots:
	node .claude/skills/app-screenshots/scripts/screenshot.mjs --width phone --width desktop --full-page

clean: $(PIO)
	$(PIO) run -t clean

distclean:
	rm -rf .pio .venv .dev emulator/dist
