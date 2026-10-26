#ifndef AUDIO_EFFECT_VST_H
#define AUDIO_EFFECT_VST_H

// VST 2.x-style effect plug-ins as Teensy Audio Library objects.
//
//   audioeffectx.h        the plug-in interface (AudioEffectX) they compile against
//   VSTEffect             one plug-in's control surface: parameters, bypass, mix, gain
//   VSTRack               plug-ins in stages, in series and in parallel, on float buffers
//   AudioEffectVST<P>     an Audio Library object that owns and runs plug-in P
//   AudioEffectVSTHost    the same around a plug-in instance of its own
//   AudioEffectVSTChain   a rack as one object, float from slot to slot
//   VSTPlugin<P>          a P a sketch can declare without the SDK's constructor argument
//
// A plug-in binary from a computer cannot be loaded: there is no operating
// system, no dynamic loader and the wrong processor. What runs is a plug-in's
// source, compiled into the sketch. examples/VSTHost does that with a few of
// the open-source Airwindows plug-ins.

#include "vst_host/audio_effect_vst.h"

#endif
