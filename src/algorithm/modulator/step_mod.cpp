#include "algorithm/modulator/step_mod.h"
#include "node/registry.h"

static const Domain IN[2] = {Domain::Gate, Domain::Gate};
static const Domain OUT[1] = {Domain::CV};

static const char* const POLARITY_NAMES[StepMod::STEP_POLARITIES] = {"bipolar", "unipolar"};

// `steps` is bounded by MAX_SEQUENCE_LEN rather than by 255 because the step
// engine's cursor is, and a modulator and a sequencer counting steps to
// different limits is a trap for anyone patching one into the other.
static const ParamDescriptor PARAMS[6] = {
    {"shape",     CV_SHAPE_SINE, CV_SHAPES, CV_SHAPE_SINE, PARAM_ENUM, CV_SHAPE_NAMES},
    {"steps",     1, MAX_SEQUENCE_LEN, StepMod::DEFAULT_STEPS, PARAM_NUMBER, nullptr},
    {"direction", 0, StepEngine::SEQ_DIRECTIONS - 1, 0, PARAM_ENUM, PARAM_DIRECTION_NAMES},
    {"depth",     1, 255, 255, PARAM_PERCENT, nullptr},
    {"offset",    0, 255, 0,   PARAM_SIGNED,  nullptr},
    {"polarity",  StepMod::STEP_BIPOLAR, StepMod::STEP_POLARITIES, StepMod::STEP_BIPOLAR, PARAM_ENUM, POLARITY_NAMES},
};
// The same three questions the Lfo's groups ask - what shape is it, how fast
// does it go, how far does it move - except that the middle one is answered
// in steps rather than in time, which is the whole node. `polarity` sits
// under `level` here rather than under `shape` as it does there, because
// without a `phase` control beside it there is no second shape group to put
// it in and it is a level control in any case.
static const ParamGroup GROUPS[3] = {
    {0, 1, 1, PARAMS,     "shape"},   // shape
    {1, 1, 2, PARAMS + 1, "steps"},   // steps, direction
    {3, 1, 3, PARAMS + 3, "level"},   // depth, offset, polarity
};

static const char* const IN_NAMES[2] = {"trigger", "reset"};
static const char* const OUT_NAMES[1] = {"cv"};

const AlgorithmDescriptor StepMod::descriptor = {
    ALGO_STEP_MOD, "StepMod", 2, 1, 1, 6, IN, OUT, sizeof(StepMod), false, construct_node<StepMod>,
    GROUPS, 3, IN_NAMES, OUT_NAMES,
    "A modulation shape cut into steps and clocked by its trigger inlet, not by a rate.",
    CATEGORY_MODULATOR };

static uint8_t clamp_enum(uint8_t stored, uint8_t max_value, uint8_t fallback){
    if (stored == 0 || stored > max_value) return fallback;
    return stored;
}

StepMod::StepMod(const NodeConfig& config) :
    engine(),
    rng(entropy::seed()),
    trigger_in(config.in_bus[0]),
    reset_in(config.in_bus[1]),
    out(config.out_bus[0]),
    shape(clamp_enum(config.params[0], CV_SHAPES, CV_SHAPE_SINE)),
    depth(config.params[3] ? config.params[3] : 255),
    offset_param(config.params[4]),
    polarity(clamp_enum(config.params[5], STEP_POLARITIES, STEP_BIPOLAR)),
    cursor(0), random_from(0), random_to(0), held(0)
{
    engine.configure(config.params[1], config.params[2], DEFAULT_STEPS);
    random_from = (uint16_t)(rng.next() & CV_MAX);
    random_to = (uint16_t)(rng.next() & CV_MAX);
    // The level of step zero, rather than nothing, until the first trigger:
    // a CV bus has a value every pass whether or not anything has clocked
    // this, and a modulator that read as silence until the clock arrived
    // would put a jump into the patch on the first edge.
    refresh();
}

uint16_t StepMod::phase_of(uint8_t i) const {
    // The centre of the step's slice: (2i + 1) / 2n of the way through the
    // shape. See the class comment for why the centre and not the start.
    const uint32_t n = engine.length();            // never zero
    const uint32_t at = ((uint32_t)i < n) ? (uint32_t)i : n - 1u;
    return (uint16_t)(((2u * at + 1u) * (uint32_t)CV_FULL) / (2u * n));
}

void StepMod::draw(){
    random_from = random_to;
    random_to = (uint16_t)(rng.next() & CV_MAX);
}

void StepMod::refresh(){
    held = cv_shape_scaled(cv_shape_at(shape, phase_of(cursor), random_from, random_to),
                           depth, offset_param, polarity == STEP_UNIPOLAR);
}

void StepMod::process(BusManager& bus, uint32_t){
    if (reset_in.rising(bus)) engine.reset();

    if (trigger_in.rising(bus)){
        cursor = engine.advance(rng);
        if (shape == CV_SHAPE_RANDOM_STEP){
            draw();                                // a fresh level every step
        } else {
            // One target per period for the glide, counted in triggers, so a
            // direction that does not visit the steps in order still redraws
            // once every `steps` of them.
            const uint32_t n = engine.length();
            if (n <= 1u || engine.steps_taken() % n == 1u) draw();
        }
        refresh();
    }

    // Written every pass, not only on a trigger: the level is held, and a bus
    // value is republished by the swap rather than latched.
    bus.cv_write(out, held);
}

bool StepMod::set_param(uint16_t index, uint8_t value){
    switch (index){
        case 0:
            if (value == 0 || value > CV_SHAPES) return false;
            if (value == shape) return true;
            shape = value;
            break;
        case 1: {
            const uint8_t want = value ? value : DEFAULT_STEPS;
            if (want == engine.length()) return true;
            engine.set_length(value, DEFAULT_STEPS);   // cursor clamps on the next trigger
            break;
        }
        case 2:
            if (value >= StepEngine::SEQ_DIRECTIONS) return false;
            engine.set_direction(value);
            return true;                               // the level does not move until a trigger does
        case 3:
            if (value == 0) return false;              // the descriptor's minimum is 1
            if (value == depth) return true;
            depth = value;
            break;
        case 4:
            if (value == offset_param) return true;
            offset_param = value;
            break;
        case 5:
            if (value == 0 || value > STEP_POLARITIES) return false;
            if (value == polarity) return true;
            polarity = value;
            break;
        default:
            return false;
    }
    // Everything that falls through here changes what the step being held
    // reads as, so the output moves now rather than on the next trigger.
    refresh();
    return true;
}

uint8_t StepMod::get_param(uint16_t index) const {
    switch (index){
        case 0: return shape;
        case 1: return engine.length();
        case 2: return engine.direction();
        case 3: return depth;
        case 4: return offset_param;
        case 5: return polarity;
        default: return 0;
    }
}
