# PlatformIO post-script: strict warnings for the project's own sources only.
#
# build_src_flags in platformio.ini applies -Wall -Wextra -Wshadow -Werror to
# every project source (src/ and test/). Two things are added here that the
# ini cannot express:
#
# 1. -Weffc++ is C++-only, so it goes into CXXFLAGS rather than being handed
#    to the C test-framework sources compiled alongside our code.
#
# 2. The Teensy core and its bundled libraries (SdFat, USBHost_t36, ...) are
#    not clean under these flags, and a warning raised inside their headers
#    would fail our build under -Werror. Every include directory that is not
#    src/, include/ or test/ is therefore passed as -isystem instead of -I,
#    which makes GCC treat those headers as system headers and suppress
#    diagnostics located in them (template instantiations included). Our own
#    headers keep -I and are held to the full set of flags.
#
#    Trade-off: SCons scans CPPPATH for implicit dependencies, so a change in
#    a framework header no longer triggers a rebuild of our objects. Those
#    headers only change with the pinned platform version, which is a clean
#    build anyway.
import os

Import("projenv")  # noqa: F821  (provided by PlatformIO)

projenv.Append(CXXFLAGS=["-Weffc++"])  # noqa: F821

own_dirs = [
    os.path.abspath(projenv.subst(d))  # noqa: F821
    for d in ("$PROJECT_SRC_DIR", "$PROJECT_INCLUDE_DIR", "$PROJECT_TEST_DIR")
]


def is_own(path):
    path = os.path.abspath(projenv.subst(str(path)))  # noqa: F821
    return any(path == d or path.startswith(d + os.sep) for d in own_dirs)


cpppath = list(projenv.get("CPPPATH", []))  # noqa: F821
system_dirs = [os.path.abspath(projenv.subst(str(p))) for p in cpppath if not is_own(p)]  # noqa: F821

projenv.Replace(CPPPATH=[p for p in cpppath if is_own(p)])  # noqa: F821
projenv.Append(CCFLAGS=[("-isystem", d) for d in system_dirs])  # noqa: F821
