#ifndef __VERSION_H_
#define __VERSION_H_

/*
 * Build identity.
 *
 * scripts/version.py defines these from the VERSION file and `git describe`
 * during a PlatformIO build. The fallbacks below keep the header usable when
 * something else compiles these sources — an editor's index, or a one-off
 * host compile — so nothing has to guess whether the macros exist.
 */

#ifndef MMMC_VERSION
#define MMMC_VERSION "0.0.0"
#endif

#ifndef MMMC_GIT_REV
#define MMMC_GIT_REV "unknown"
#endif

#ifndef MMMC_BUILD
#define MMMC_BUILD MMMC_VERSION "+" MMMC_GIT_REV
#endif

#endif
