// MMMC: one app for the module.
//
// The module has no encoder, no switches and no display (`src/hardware.h`),
// so this is not a companion to a panel - it is how a patch gets built, and
// it is a shipping deliverable. The module runs in the page, always: it is
// the firmware compiled to WebAssembly, and it is both the transport the
// editor talks to over the real SysEx protocol and the machine whose jacks,
// LEDs, sequencer positions and MIDI output the page can show.

import './styles/index.css';
import wasmUrl from '@module/mmmc.wasm?url';
import { createApp } from './services/app.js';
import { App } from './ui/App.js';

createApp({ root: document.getElementById('app'), view: App, wasmUrl }).boot();
