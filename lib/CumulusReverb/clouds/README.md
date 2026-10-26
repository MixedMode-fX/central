# Clouds' reverb

The files under `clouds/` and `../stmlib/` are the unmodified sources of the
reverb in Mutable Instruments' Clouds, by Emilie Gillet, and the parts of
stmlib it needs. Each file carries its MIT license notice; `../stmlib/LICENSE`
is stmlib's.

| File                          | From                                                                 |
|-------------------------------|----------------------------------------------------------------------|
| `clouds/dsp/frame.h`          | https://github.com/pichenettes/eurorack at `f4cfa7697095a28d292bff758beb51deb49b5a86` |
| `clouds/dsp/fx/fx_engine.h`   | the same                                                             |
| `clouds/dsp/fx/reverb.h`      | the same                                                             |
| `stmlib/stmlib.h`             | https://github.com/pichenettes/stmlib at `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec`, the eurorack repository's submodule commit |
| `stmlib/dsp/dsp.h`            | the same                                                             |
| `stmlib/dsp/cosine_oscillator.h` | the same                                                          |

Their includes are relative to the library's directory (`"stmlib/stmlib.h"`),
which is on the include path. `../mutable_reverb.h` is where they are compiled
in, and `../cumulus_reverb.h` wraps the reverb as a plug-in.

Mutable Instruments' guidelines for derivative works ask that the company's
name, and the module's, not be used for them, which is why the plug-in has a
name of its own.
