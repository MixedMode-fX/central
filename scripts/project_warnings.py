# PlatformIO post-script: C++-only warning flags for the project's own sources.
#
# build_src_flags in platformio.ini applies to every project source (src/ and
# test/), including the C sources the test framework contributes. -Weffc++
# is only valid for C++, so it is added here to CXXFLAGS instead.
Import("projenv")  # noqa: F821  (provided by PlatformIO)

projenv.Append(CXXFLAGS=["-Weffc++"])  # noqa: F821
