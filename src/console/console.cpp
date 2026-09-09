#include "console/console.h"
#include "node/registry.h"
#include "version.h"
#include "hal/midi_types.h"

// A tiny formatting layer rather than snprintf: the Teensy's newlib printf
// pulls in a large chunk of flash and a reentrancy structure, and everything
// here is an unsigned decimal or a fixed string.

static bool str_eq(const char* a, const char* b){
    while (*a && *b){ if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

Console::Console(IConsoleIo& console_io, PatchManager& manager,
                 MixedModeMaster& master, PatchStore& patch_store, StatusLeds& status,
                 CcMapper& mapper) :
    io(console_io), patches(manager), mm(master), store(patch_store), leds(status), cc(mapper),
    line(), argv(), argc(0), length(0), overflowed(false)
{}

void Console::put_uint(uint32_t value){
    char buffer[11];
    uint8_t at = sizeof buffer;
    buffer[--at] = '\0';
    do { buffer[--at] = (char)('0' + (value % 10u)); value /= 10u; } while (value && at > 0);
    io.write(&buffer[at]);
}

void Console::put_int(int32_t value){
    if (value < 0){ io.write("-"); put_uint((uint32_t)(-(int64_t)value)); return; }
    put_uint((uint32_t)value);
}

void Console::put_kv(const char* key, uint32_t value){
    io.write(key);
    io.write(" ");
    put_uint(value);
    io.write("\r\n");
}

void Console::prompt(){ io.write("mmmc> "); }

void Console::greet(){
    io.write("\r\nMMMC " MMMC_BUILD "\r\n");
    io.write("no display, no knobs: type `help`\r\n");
    prompt();
}

void Console::service(uint32_t now_us){
    uint8_t incoming = 0;
    while (io.read(incoming)){
        if (incoming == '\r' || incoming == '\n'){
            if (length == 0 && !overflowed){ io.write("\r\n"); prompt(); continue; }
            line[length] = '\0';
            io.write("\r\n");
            if (overflowed){
                put_line("line too long");
            } else {
                dispatch(now_us);
            }
            length = 0;
            overflowed = false;
            prompt();
            continue;
        }
        if (incoming == 8 || incoming == 127){          // backspace
            if (length > 0){ length--; io.write("\b \b"); }
            continue;
        }
        if (incoming < 32 || incoming > 126) continue;
        if (length >= CONSOLE_LINE_MAX){ overflowed = true; continue; }
        line[length++] = (char)incoming;
        const char echo[2] = {(char)incoming, '\0'};
        io.write(echo);
    }
}

void Console::execute(const char* text, uint32_t now_us){
    length = 0;
    while (text[length] != '\0' && length < CONSOLE_LINE_MAX){ line[length] = text[length]; length++; }
    line[length] = '\0';
    dispatch(now_us);
    length = 0;
}

uint8_t Console::split(){
    argc = 0;
    uint8_t i = 0;
    while (line[i] != '\0' && argc < MAX_ARGS){
        while (line[i] == ' ') line[i++] = '\0';
        if (line[i] == '\0') break;
        argv[argc++] = &line[i];
        while (line[i] != '\0' && line[i] != ' ') i++;
    }
    return argc;
}

uint32_t Console::arg_uint(uint8_t index, bool& ok) const {
    ok = false;
    if (index >= argc) return 0;
    const char* s = argv[index];
    uint32_t value = 0;
    if (*s == '\0') return 0;
    for (; *s != '\0'; s++){
        if (*s < '0' || *s > '9') return 0;
        value = value * 10u + (uint32_t)(*s - '0');
    }
    ok = true;
    return value;
}

void Console::dispatch(uint32_t now_us){
    if (split() == 0) return;
    const char* cmd = argv[0];

    if (str_eq(cmd, "help"))       { cmd_help(); return; }
    if (str_eq(cmd, "info"))       { cmd_info(); return; }
    if (str_eq(cmd, "clock"))      { cmd_clock(argc, now_us); return; }
    if (str_eq(cmd, "patch"))      { cmd_patch(); return; }
    if (str_eq(cmd, "buses"))      { cmd_buses(); return; }
    if (str_eq(cmd, "errors"))     { cmd_errors(); return; }
    if (str_eq(cmd, "algos"))      { cmd_algorithms(argc); return; }
    if (str_eq(cmd, "params"))     { cmd_params(argc); return; }
    if (str_eq(cmd, "set"))        { cmd_set(argc, now_us); return; }
    if (str_eq(cmd, "get"))        { cmd_get(argc); return; }
    if (str_eq(cmd, "slots"))      { cmd_slots(); return; }
    if (str_eq(cmd, "save"))       { cmd_save(argc); return; }
    if (str_eq(cmd, "load"))       { cmd_load(argc, now_us); return; }
    if (str_eq(cmd, "erase"))      { cmd_erase(argc); return; }
    if (str_eq(cmd, "defaults"))   { cmd_defaults(now_us); return; }
    if (str_eq(cmd, "maps"))       { cmd_maps(); return; }
    if (str_eq(cmd, "map"))        { cmd_map(argc, now_us); return; }
    if (str_eq(cmd, "unmap"))      { cmd_unmap(argc, now_us); return; }
    if (str_eq(cmd, "learn"))      { cmd_learn(argc, now_us); return; }

    put("unknown command: ");
    put_line(cmd);
    put_line("type `help`");
}

void Console::cmd_help(){
    put_line("info                    firmware, patch and store summary");
    put_line("clock [bpm] [source]    show, or set tempo and source (0 int, 1 cv, 2 midi)");
    put_line("patch                   the running patch: ports, nodes, connections");
    put_line("buses                   live bus state");
    put_line("errors                  the counters behind the red LED");
    put_line("algos                   every algorithm this firmware has");
    put_line("params <node>           one node's parameters, with ranges");
    put_line("get <node> <param>      read one parameter");
    put_line("set <node> <param> <v>  write one parameter");
    put_line("slots                   what each preset slot holds");
    put_line("save <slot>             store the running patch");
    put_line("load <slot>             recall a stored patch");
    put_line("erase <slot>            forget a stored patch");
    put_line("defaults                back to the built-in patch");
    put_line("maps                    the controller bindings");
    put_line("map <slot> <cc> <node> <param>   bind a CC to a parameter");
    put_line("learn <slot> <node> <param>      bind the next CC seen");
    put_line("unmap <slot>            forget a binding");
}

void Console::cmd_info(){
    put("firmware  " MMMC_BUILD "\r\n");
    put_kv("nodes    ", mm.node_count());
    put("patch     ");
    put_line(patches.running_defaults() ? "defaults (nothing valid stored)" : "loaded");
    put_kv("eeprom   ", EEPROM_BYTES);
    put_kv("slot size", PATCH_SLOT_BYTES);
    put_kv("writes   ", store.writes());
    put("pending   ");
    put_line(store.dirty() ? "yes (autosave armed)" : "no");
}

void Console::cmd_clock(uint8_t n, uint32_t now_us){
    (void)now_us;
    if (n >= 2){
        bool ok = false;
        const uint32_t bpm = arg_uint(1, ok);
        if (!ok){ put_line("clock: bpm must be a number"); return; }
        mm.clock().set_bpm((uint16_t)bpm);
    }
    if (n >= 3){
        bool ok = false;
        const uint32_t src = arg_uint(2, ok);
        if (!ok || src > MasterClock::CLOCK_MIDI){ put_line("clock: source is 0, 1 or 2"); return; }
        mm.clock().set_source((uint8_t)src);
    }
    put_kv("bpm      ", mm.clock().bpm());
    put_kv("source   ", mm.clock().source());
    put_kv("cv ppqn  ", mm.clock().cv_ppqn());
    put("running   ");
    put_line(mm.clock().running() ? "yes" : "no");
    put_kv("subticks ", mm.clock().count());
}

void Console::cmd_patch(){
    const Patch& p = patches.active();

    put_line("jacks:");
    for (uint8_t i = 0; i < GPIO_N; i++){
        if (p.gate_ports[i].direction == GATE_PORT_UNUSED) continue;
        put("  jack "); put_uint(i + 1u);
        put(p.gate_ports[i].direction == GATE_PORT_IN ? " in  -> gate " : " out <- gate ");
        put_uint(p.gate_ports[i].bus);
        put_line("");
    }
    put_line("midi in:");
    for (uint8_t i = 0; i < N_MIDI_IN_NODES; i++){
        if (p.midi_in[i].source_mask == 0) continue;
        put("  port "); put_uint(i);
        put(" mask 0x"); put_uint(p.midi_in[i].source_mask);
        put(" ch "); put_uint(p.midi_in[i].channel);
        put(" -> note "); put_uint(p.midi_in[i].bus);
        put_line("");
    }
    put_line("midi out:");
    for (uint8_t i = 0; i < N_MIDI_OUT_NODES; i++){
        if (p.midi_out[i].target_mask == 0) continue;
        put("  port "); put_uint(i);
        put(" mask 0x"); put_uint(p.midi_out[i].target_mask);
        put(" ch "); put_uint(p.midi_out[i].channel);
        put(" <- note "); put_uint(p.midi_out[i].bus);
        put_line("");
    }

    put_line("nodes:");
    for (uint8_t n = 0; n < p.n_nodes; n++){
        const AlgorithmDescriptor* d = registry::find(p.nodes[n].algorithm_id);
        put("  "); put_uint(n); put(" ");
        put_line(d ? d->name : "<unknown>");
        if (d == nullptr) continue;
        // By name, not by index: "in 1 <- bus 3" does not say whether the
        // connection advances this node or resets it, and the descriptor has
        // known since the registry gained port names.
        for (uint8_t i = 0; i < d->n_in && i < MAX_IN; i++){
            if (p.nodes[n].in_bus[i] == NO_BUS) continue;
            put("      in  "); put_uint(i); put(" ");
            put(d->in_name != nullptr ? d->in_name[i] : "");
            put(" <- bus "); put_uint(p.nodes[n].in_bus[i]);
            put_line("");
        }
        for (uint8_t o = 0; o < d->n_out && o < MAX_OUT; o++){
            if (p.nodes[n].out_bus[o] == NO_BUS) continue;
            put("      out "); put_uint(o); put(" ");
            put(d->out_name != nullptr ? d->out_name[o] : "");
            put(" -> bus "); put_uint(p.nodes[n].out_bus[o]);
            put_line("");
        }
    }
}

void Console::cmd_buses(){
    const BusManager& b = mm.buses();
    put("gates ");
    for (uint8_t i = 0; i < N_GATE_BUS; i++) put(b.gate_read(i) ? "1" : "0");
    put_line("");
    for (uint8_t i = 0; i < N_NOTE_BUS; i++){
        const uint8_t n = b.note_count(i);
        const uint32_t dropped = b.note_overflows(i);
        if (n == 0 && dropped == 0) continue;
        put("note bus "); put_uint(i);
        put(": "); put_uint(n); put(" events");
        if (dropped){ put(", "); put_uint(dropped); put(" dropped"); }
        put_line("");
    }
}

void Console::cmd_errors(){
    // Everything the red LED can be reporting, in one place, on a module
    // running any patch - including one that is failing.
    put_kv("led flashes    ", leds.errors());
    const BusManager& b = mm.buses();
    uint32_t note_dropped = 0;
    for (uint8_t i = 0; i < N_NOTE_BUS; i++) note_dropped += b.note_overflows(i);
    put_kv("note overflows ", note_dropped);
    put_kv("rejected edges ", mm.clock().rejected_edges());
    put_kv("last load error", mm.last_error());
    put_kv("last node error", mm.last_node_error());
    put_kv("failing node   ", mm.last_node_index());
    put_kv("store error    ", store.last_error());
    put_kv("store writes   ", store.writes());
}

void Console::cmd_algorithms(uint8_t n){
    (void)n;
    for (uint8_t i = 0; i < registry::count(); i++){
        const AlgorithmDescriptor* d = registry::at(i);
        put_uint(d->id); put(" ");
        put(d->name);
        put(" in "); put_uint(d->n_in);
        put(" (need "); put_uint(d->min_in); put(")");
        put(" out "); put_uint(d->n_out);
        put(" params "); put_uint(d->n_params);
        if (d->wants_tick) put(" clocked");
        put_line("");
        // The console is the only discovery surface on a module with no
        // editor attached, so it says what the algorithm is for and what its
        // connections mean rather than only how many there are.
        if (d->summary != nullptr){ put("   "); put_line(d->summary); }
        if (d->in_name != nullptr && d->n_in){
            put("   reads: ");
            for (uint8_t k = 0; k < d->n_in && k < MAX_IN; k++){
                if (k) put(", ");
                put(d->in_name[k]);
                if (k >= d->min_in) put(" (optional)");
            }
            put_line("");
        }
        if (d->out_name != nullptr && d->n_out){
            put("   writes: ");
            for (uint8_t k = 0; k < d->n_out && k < MAX_OUT; k++){
                if (k) put(", ");
                put(d->out_name[k]);
            }
            put_line("");
        }
    }
}

void Console::cmd_params(uint8_t n){
    if (n < 2){ put_line("params <node>"); return; }
    bool ok = false;
    const uint32_t index = arg_uint(1, ok);
    if (!ok){ put_line("params: node must be a number"); return; }
    const AlgorithmDescriptor* d = mm.node_descriptor((uint8_t)index);
    if (d == nullptr){ put_line("params: no such node"); return; }

    put(d->name); put_line(":");
    for (uint16_t p = 0; p < d->n_params; p++){
        const ParamDescriptor* pd = registry::param(*d, p);
        if (pd == nullptr) continue;
        uint8_t value = 0;
        mm.get_node_param((uint8_t)index, p, value);
        // Reserved bytes are listed only when something has been written to
        // them, so a 336-parameter sequencer does not bury its header.
        if (pd->min == 0 && pd->max == 0 && value == 0) continue;
        put("  "); put_uint(p); put(" ");
        put(pd->name);
        put(" = "); put_uint(value);
        put(" ["); put_uint(pd->min); put(".."); put_uint(pd->max);
        put(", default "); put_uint(pd->def); put("]");
        if (pd->kind == PARAM_ENUM && value >= pd->min && value <= pd->max){
            put(" "); put(pd->options[value - pd->min]);
        }
        put_line("");
    }
}

void Console::cmd_set(uint8_t n, uint32_t now_us){
    if (n < 4){ put_line("set <node> <param> <value>"); return; }
    bool a = false, b = false, c = false;
    const uint32_t node = arg_uint(1, a);
    const uint32_t param = arg_uint(2, b);
    const uint32_t value = arg_uint(3, c);
    if (!a || !b || !c){ put_line("set: node, param and value must be numbers"); return; }
    if (node > 255 || param > 65535 || value > 255){ put_line("set: value out of range"); return; }

    switch (patches.set_param((uint8_t)node, (uint16_t)param, (uint8_t)value, now_us)){
        case PARAM_SET_OK: {
            uint8_t running = 0;
            mm.get_node_param((uint8_t)node, (uint16_t)param, running);
            put("ok, now "); put_uint(running); put_line("");
            return;
        }
        case PARAM_NO_SUCH_NODE:       put_line("set: no such node"); return;
        case PARAM_NO_SUCH_PARAM:      put_line("set: no such parameter"); return;
        case PARAM_VALUE_OUT_OF_RANGE: put_line("set: value outside the parameter's range"); return;
        default:                       put_line("set: the node refused it"); return;
    }
}

void Console::cmd_get(uint8_t n){
    if (n < 3){ put_line("get <node> <param>"); return; }
    bool a = false, b = false;
    const uint32_t node = arg_uint(1, a);
    const uint32_t param = arg_uint(2, b);
    if (!a || !b){ put_line("get: node and param must be numbers"); return; }
    uint8_t value = 0;
    if (!mm.get_node_param((uint8_t)node, (uint16_t)param, value)){
        put_line("get: no such node or parameter");
        return;
    }
    put_uint(value);
    put_line("");
}

void Console::cmd_slots(){
    for (uint8_t s = 0; s < PATCH_SLOTS; s++){
        put("slot "); put_uint(s);
        if (s == 0) put(" (current)");
        if (store.occupied(s)){ put(": "); put_uint(store.used(s)); put(" bytes"); }
        else put(": empty");
        put_line("");
    }
}

void Console::cmd_save(uint8_t n){
    bool ok = false;
    const uint32_t slot = n >= 2 ? arg_uint(1, ok) : 0;
    if (n >= 2 && !ok){ put_line("save: slot must be a number"); return; }
    switch (patches.save_slot((uint8_t)slot)){
        case APPLY_OK:           put_line("saved"); return;
        case APPLY_NO_SUCH_SLOT: put_line("save: no such slot"); return;
        case APPLY_TOO_LARGE:    put_line("save: the patch does not fit a slot"); return;
        default:                 put_line("save: failed"); return;
    }
}

void Console::cmd_load(uint8_t n, uint32_t now_us){
    if (n < 2){ put_line("load <slot>"); return; }
    bool ok = false;
    const uint32_t slot = arg_uint(1, ok);
    if (!ok){ put_line("load: slot must be a number"); return; }
    switch (patches.recall_slot((uint8_t)slot, now_us)){
        case APPLY_OK:           put_line("loaded"); return;
        case APPLY_NO_SUCH_SLOT: put_line("load: no such slot"); return;
        case APPLY_SLOT_EMPTY:   put_line("load: that slot is empty"); return;
        case APPLY_SLOT_CORRUPT: put_line("load: that slot did not check out"); return;
        default:
            put("load: the patch was rejected, error ");
            put_uint(mm.last_node_error());
            put(" at node ");
            put_uint(mm.last_node_index());
            put_line("");
            return;
    }
}

void Console::cmd_erase(uint8_t n){
    if (n < 2){ put_line("erase <slot>"); return; }
    bool ok = false;
    const uint32_t slot = arg_uint(1, ok);
    if (!ok || slot >= PATCH_SLOTS){ put_line("erase: no such slot"); return; }
    store.erase((uint8_t)slot);
    put_line("erased");
}

void Console::cmd_defaults(uint32_t now_us){
    if (patches.restore_defaults(now_us) == APPLY_OK) put_line("running the built-in patch");
    else put_line("defaults: failed");
}

// Controller bindings (#21). The console covers the same ground as the SysEx
// commands, so hardware testing is not blocked on an editor.

static const char* const CC_TARGET_NAMES[CC_TARGET_KINDS] = {
    "node", "clock", "transport", "port",
};

void Console::cmd_maps(){
    const Patch& p = patches.active();
    bool any = false;
    for (uint8_t i = 0; i < N_CC_MAP; i++){
        const CcMapping& m = p.cc_map[i];
        if (m.source_mask == 0) continue;
        any = true;
        put_uint(i); put(": cc "); put_uint(m.cc);
        put(" ch "); put_uint(m.channel);
        put(" mask "); put_uint(m.source_mask);
        put(" -> "); put(m.target_kind < CC_TARGET_KINDS ? CC_TARGET_NAMES[m.target_kind] : "?");
        put(" "); put_uint(m.target_index);
        put(" param "); put_uint(m.param);
        put(" ["); put_uint(m.min); put(".."); put_uint(m.max); put("]");
        if (m.flags & CC_FOURTEEN_BIT) put(" 14-bit");
        if (m.flags & CC_PASS_THROUGH) put(" thru");
        if ((m.flags & CC_RELATIVE_MASK) != CC_ABSOLUTE) put(" relative");
        put_line("");
    }
    if (!any) put_line("no bindings");
    if (cc.learning()) put_line("a learn is armed");
    put_kv("writes  ", cc.writes());
    put_kv("refused ", cc.refused());
}

void Console::cmd_map(uint8_t n, uint32_t now_us){
    if (n < 5){ put_line("map <slot> <cc> <node> <param> [min] [max]"); return; }
    bool ok[6] = {false, false, false, false, false, false};
    const uint32_t slot  = arg_uint(1, ok[0]);
    const uint32_t cc_no = arg_uint(2, ok[1]);
    const uint32_t node  = arg_uint(3, ok[2]);
    const uint32_t param = arg_uint(4, ok[3]);
    const uint32_t lo    = n > 5 ? arg_uint(5, ok[4]) : 0;
    const uint32_t hi    = n > 6 ? arg_uint(6, ok[5]) : 0;
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3]){ put_line("map: every argument must be a number"); return; }
    if (slot >= N_CC_MAP){ put_line("map: no such slot"); return; }

    CcMapping m = unused_mapping();
    // Every port but the control cable: a mapping cannot reach the protocol's
    // own port, and binding to one specific transport is what the editor is
    // for.
    m.source_mask = MIDI_MUSICAL_PORTS;
    m.channel = 0;                       // omni from the console
    m.cc = (uint8_t)cc_no;
    m.target_kind = CC_TARGET_NODE;
    m.target_index = (uint8_t)node;
    m.param = (uint16_t)param;
    m.min = (uint16_t)lo;
    m.max = (uint16_t)hi;

    patches.begin_edit();
    patches.staging().cc_map[slot] = m;
    if (patches.commit_cc_map((uint8_t)slot, now_us) != APPLY_OK){
        put_line("map: rejected - check the node and parameter exist");
        return;
    }
    cc.reset();
    put_line("bound");
}

void Console::cmd_unmap(uint8_t n, uint32_t now_us){
    if (n < 2){ put_line("unmap <slot>"); return; }
    bool ok = false;
    const uint32_t slot = arg_uint(1, ok);
    if (!ok || slot >= N_CC_MAP){ put_line("unmap: no such slot"); return; }
    patches.begin_edit();
    patches.staging().cc_map[slot] = unused_mapping();
    patches.commit_cc_map((uint8_t)slot, now_us);
    cc.reset();
    put_line("forgotten");
}

void Console::cmd_learn(uint8_t n, uint32_t now_us){
    if (n < 2){ cc.learn_cancel(); put_line("learn cancelled"); return; }
    if (n < 4){ put_line("learn <slot> <node> <param>, or `learn` alone to cancel"); return; }
    bool a = false, b = false, c = false;
    const uint32_t slot  = arg_uint(1, a);
    const uint32_t node  = arg_uint(2, b);
    const uint32_t param = arg_uint(3, c);
    if (!a || !b || !c || slot >= N_CC_MAP){ put_line("learn: bad arguments"); return; }
    cc.learn_arm((uint8_t)slot, CC_TARGET_NODE, (uint8_t)node, (uint16_t)param, now_us);
    put_line("armed: turn a controller");
}
