#include "algorithm/sequencer/automaton.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[Automaton::LANES] = {
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate,
    Domain::Gate, Domain::Gate, Domain::Gate, Domain::Gate };

static_assert(Automaton::LANES == 8, "OUT lists one domain per cell");
static_assert(Automaton::LANES <= MAX_OUT, "Automaton has one outlet per cell");

static const char* const EDGE_NAMES[Automaton::CA_EDGES] = {"ring", "dead"};
static const char* const REVIVE_NAMES[Automaton::CA_REVIVES] = {"off", "on"};

static const ParamDescriptor PARAMS[7] = {
    {"rule",        0, 255, Automaton::DEFAULT_RULE, PARAM_NUMBER,   nullptr},
    {"seed",        0, 255, Automaton::DEFAULT_SEED, PARAM_BITFIELD, nullptr},
    {"edges",       Automaton::CA_RING,       Automaton::CA_EDGES,   Automaton::CA_RING,       PARAM_ENUM, EDGE_NAMES},
    {"revive",      Automaton::CA_REVIVE_OFF, Automaton::CA_REVIVES, Automaton::CA_REVIVE_ON,  PARAM_ENUM, REVIVE_NAMES},
    {"cells",       Automaton::MIN_CELLS, Automaton::LANES, Automaton::LANES, PARAM_NUMBER, nullptr},
    {"probability", 0, 100, 100, PARAM_PERCENT, nullptr},
    {"width",       0, 255, 0,   PARAM_MILLIS,  nullptr},
};
static const ParamGroup GROUPS[1] = {{0, 1, 7, PARAMS}};

static const char* const IN_NAMES[2] = {"advance", "reseed"};
static const char* const OUT_NAMES[Automaton::LANES] = {
    "cell 1", "cell 2", "cell 3", "cell 4", "cell 5", "cell 6", "cell 7", "cell 8"};

const AlgorithmDescriptor Automaton::descriptor = {
    ALGO_AUTOMATON, "Automaton", 2, 1, Automaton::LANES, 7, IN, OUT, sizeof(Automaton), false,
    construct_node<Automaton>, GROUPS, 1, IN_NAMES, OUT_NAMES,
    "A Wolfram rule over eight lanes: structure that never quite repeats. Try 90, 110, 30.",
    CATEGORY_SEQUENCER };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

Automaton::Automaton(const NodeConfig& config) :
    advance_in(config.in_bus[0]),
    reseed_in(config.in_bus[1]),
    out(),
    rule(config.params[0] ? config.params[0] : DEFAULT_RULE),
    seed(config.params[1] ? config.params[1] : DEFAULT_SEED),
    edges(clamp_enum(config.params[2], CA_EDGES, CA_RING)),
    revive(clamp_enum(config.params[3], CA_REVIVES, CA_REVIVE_ON)),
    n_cells(config.params[4] ? (config.params[4] > LANES ? LANES : config.params[4]) : LANES),
    chance(step_probability(config.params[5])),
    width_param(config.params[6]),
    cells(0), fired(0), count(0), revived(0),
    rng(entropy::seed()),
    pulse()
{
    for (uint8_t i = 0; i < LANES; i++) out[i] = config.out_bus[i];
    if (n_cells < MIN_CELLS) n_cells = MIN_CELLS;
    if (config.params[6]) pulse.set_width_us((uint32_t)config.params[6] * 1000u);
    reseed();
}

void Automaton::reseed(){
    // Cells outside the ring are not alive and never feed anything, so a seed
    // byte is masked rather than truncated: shortening the ring and
    // lengthening it again gives the seed back.
    cells = (uint8_t)(seed & (uint8_t)((1u << n_cells) - 1u));
}

uint8_t Automaton::next_row() const {
    const uint8_t mask = (uint8_t)((1u << n_cells) - 1u);
    uint8_t next = 0;
    for (uint8_t i = 0; i < n_cells; i++){
        uint8_t left, right;
        if (edges == CA_RING){
            left  = (uint8_t)((cells >> ((i + n_cells - 1u) % n_cells)) & 1u);
            right = (uint8_t)((cells >> ((i + 1u) % n_cells)) & 1u);
        } else {
            left  = i == 0 ? 0u : (uint8_t)((cells >> (i - 1u)) & 1u);
            right = (i + 1u >= n_cells) ? 0u : (uint8_t)((cells >> (i + 1u)) & 1u);
        }
        const uint8_t self = (uint8_t)((cells >> i) & 1u);
        const uint8_t neighbourhood = (uint8_t)((left << 2) | (self << 1) | right);
        if ((rule >> neighbourhood) & 1u) next = (uint8_t)(next | (uint8_t)(1u << i));
    }
    return (uint8_t)(next & mask);
}

void Automaton::process(BusManager& bus, uint32_t now_us){
    if (reseed_in.rising(bus)){
        reseed();
        fired = 0;
        pulse.clear();
    }

    if (advance_in.rising(bus)){
        const uint8_t next = next_row();
        // A row that does not change is a row that will never change again:
        // all-empty under most rules, all-full under some, and any other
        // stable configuration. A row that merely blinks between two states
        // is not this, and is left alone, because blinking is a rhythm.
        if (revive == CA_REVIVE_ON && next == cells){
            reseed();
            revived++;
        } else {
            cells = next;
        }
        count++;

        fired = 0;
        for (uint8_t i = 0; i < n_cells; i++){
            if (!((cells >> i) & 1u)) continue;
            if (!rng.chance(chance)) continue;
            fired = (uint8_t)(fired | (uint8_t)(1u << i));
        }
        // One timer for the row: every lane of a generation fires together,
        // so eight TriggerPulses would be eight copies of one number.
        if (fired) pulse.fire(now_us);
    }

    if (!pulse.level(now_us)) return;
    for (uint8_t i = 0; i < n_cells; i++){
        if ((fired >> i) & 1u) bus.gate_write(out[i], true);
    }
}

bool Automaton::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0: rule = value; return true;
        case 1:
            // The seed is what reseed and revive load. It does not disturb the
            // row that is running, for the reason RandomSequencer gives about
            // its own: a pattern that redrew itself whenever a control moved
            // would be unplayable.
            seed = value;
            return true;
        case 2: if (value == 0 || value > CA_EDGES) return false; edges = value; return true;
        case 3: if (value == 0 || value > CA_REVIVES) return false; revive = value; return true;
        case 4: {
            if (value < MIN_CELLS || value > LANES) return false;
            if (value == n_cells) return true;
            n_cells = value;
            // The cells that are left keep what they were doing; a ring that
            // grows finds the new cells empty, and the rule fills them from
            // their neighbours within a generation or two.
            cells = (uint8_t)(cells & (uint8_t)((1u << n_cells) - 1u));
            fired = (uint8_t)(fired & (uint8_t)((1u << n_cells) - 1u));
            return true;
        }
        case 5: if (value > 100) return false; chance = step_probability(value); return true;
        case 6:
            width_param = value;
            pulse.set_width_us(value ? (uint32_t)value * 1000u : (uint32_t)TRIGGER_WIDTH_US);
            return true;
        default: return false;
    }
}

uint8_t Automaton::get_param(uint16_t index) const {
    switch (index){
        case 0: return rule;
        case 1: return seed;
        case 2: return edges;
        case 3: return revive;
        case 4: return n_cells;
        case 5: return chance;
        case 6: return width_param;
        default: return 0;
    }
}
