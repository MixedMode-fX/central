// Clouds' reverb from the unmodified Mutable Instruments sources next to
// this file. Only cumulus_reverb.cpp includes it, so the macros stmlib
// defines at file scope (CLIP, CONSTRAIN, TAIL, ...) stay in that one file.
//
// The sources are not written to this project's warning flags; the pragma
// has the compiler judge them, and everything they include, by their own.
// It has no effect in a primary source file, which is why this is a header.

#ifndef CUMULUS_MUTABLE_REVERB_H
#define CUMULUS_MUTABLE_REVERB_H
#pragma GCC system_header

// stmlib saturates with ARM instructions; away from ARM (the native tests)
// it has portable versions behind TEST, which is how Mutable's own desktop
// builds compile it.
#if !defined(__arm__) && !defined(TEST)
#define TEST
#endif

#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"

#endif
