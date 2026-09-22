# The module as a plugin: the firmware core over the plugin's HAL, in a DAW.
#
#   cmake -S plugin -B plugin/build -DMMMC_VST3=OFF && cmake --build plugin/build
#       the core, the engine and its test, with no plugin framework -
#       what `scripts/checks.sh plugin` builds and runs
#   cmake -S plugin -B plugin/build && cmake --build plugin/build
#       the above plus the VST3 (and a standalone app), which fetches JUCE
#
# The core is the same file set the native tests and the WebAssembly module
# compile - src/ minus main.cpp and hal/teensy/ - with the firmware's warning
# flags, so a line that would not build on the module does not build here
# either. The page the editor shows is emulator/build.sh's output, taken as a
# file: it is built once, by the tools that build it, and embedded here.

cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" MMMC_VERSION)
string(STRIP "${MMMC_VERSION}" MMMC_VERSION)

project(mmmc_plugin VERSION ${MMMC_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MMMC_VST3 "Build the VST3 and the standalone app (fetches JUCE)" ON)
set(MMMC_APP_PAGE "${CMAKE_CURRENT_SOURCE_DIR}/../emulator/dist/index.html"
    CACHE FILEPATH "The app as one file, which the editor shows (emulator/build.sh)")
set(MMMC_JUCE_TAG "8.0.15" CACHE STRING "The JUCE release the plugin is built with")

set(MMMC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Build identity, as scripts/version.py stamps it into the firmware.
find_package(Git QUIET)
set(MMMC_GIT_REV "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
                    WORKING_DIRECTORY "${MMMC_ROOT}"
                    OUTPUT_VARIABLE described OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE describe_failed ERROR_QUIET)
    if(NOT describe_failed AND described)
        set(MMMC_GIT_REV "${described}")
    endif()
endif()

# --- the firmware core --------------------------------------------------------

file(GLOB_RECURSE MMMC_CORE_SOURCES CONFIGURE_DEPENDS "${MMMC_ROOT}/src/*.cpp")
list(FILTER MMMC_CORE_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")
list(FILTER MMMC_CORE_SOURCES EXCLUDE REGEX "/src/hal/teensy/")

add_library(mmmc_core STATIC ${MMMC_CORE_SOURCES})
target_include_directories(mmmc_core PUBLIC "${MMMC_ROOT}/src")
target_compile_definitions(mmmc_core PUBLIC
    MMMC_VERSION="${MMMC_VERSION}"
    MMMC_GIT_REV="${MMMC_GIT_REV}"
    MMMC_BUILD="${MMMC_VERSION}+${MMMC_GIT_REV}")

# The flags the firmware is held to (platformio.ini, project_warnings.py).
# MSVC has no -Weffc++ and a different idea of a warning; it gets its
# highest level and the log is read, so the gate that fails a build stays
# the one every checkout runs.
set(MMMC_WARNINGS $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wshadow;-Weffc++;-Werror>)
target_compile_options(mmmc_core PRIVATE ${MMMC_WARNINGS})

# --- the engine: the module in the plugin, with no framework -------------

add_library(mmmc_engine STATIC src/engine.cpp)
target_include_directories(mmmc_engine PUBLIC src)
target_link_libraries(mmmc_engine PUBLIC mmmc_core)
target_compile_options(mmmc_engine PRIVATE ${MMMC_WARNINGS})

add_executable(engine_test test/engine_test.cpp)
target_link_libraries(engine_test PRIVATE mmmc_engine)
target_compile_options(engine_test PRIVATE ${MMMC_WARNINGS})
enable_testing()
add_test(NAME engine COMMAND engine_test)

# --- the plugin ------------------------------------------------------------

if(MMMC_VST3)
    if(NOT EXISTS "${MMMC_APP_PAGE}")
        message(FATAL_ERROR "the editor's page is missing: ${MMMC_APP_PAGE}\n"
                            "build it with emulator/build.sh, or point MMMC_APP_PAGE at a built index.html")
    endif()

    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${MMMC_JUCE_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)

    # The page, as bytes in the binary: BinaryData::index_html.
    juce_add_binary_data(mmmc_page NAMESPACE mmmc_page SOURCES "${MMMC_APP_PAGE}")

    juce_add_plugin(mmmc
        PRODUCT_NAME "MMMC"
        COMPANY_NAME "MixedMode fX"
        BUNDLE_ID "fx.mixedmode.mmmc"
        PLUGIN_MANUFACTURER_CODE MmfX
        PLUGIN_CODE Mmmc
        FORMATS VST3 Standalone
        # An instrument to the host: it makes MIDI from nothing (sequencers,
        # harmony) and a track that hosts it has somewhere to route what it
        # makes. The audio it outputs is silence.
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        VST3_CATEGORIES Instrument Tools
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        # The editor is the app in a web view: WebKit on Linux, WebView2
        # linked statically on Windows (the runtime is part of Windows).
        NEEDS_WEB_BROWSER TRUE
        NEEDS_WEBVIEW2 TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_AUTO_MANIFEST TRUE)

    target_sources(mmmc PRIVATE src/processor.cpp src/editor.cpp)
    target_compile_definitions(mmmc PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(mmmc PRIVATE
        mmmc_engine
        mmmc_page
        juce::juce_audio_utils
        juce::juce_gui_extra
        PUBLIC
        juce::juce_recommended_config_flags)
    # JUCE's module sources compile inside this target, with their own idea
    # of a warning, so the flags go on the two files that are ours; and
    # JUCE's headers are not -Weffc++ clean, so those two are held to the
    # rest of the set.
    set_source_files_properties(src/processor.cpp src/editor.cpp PROPERTIES COMPILE_OPTIONS
        "$<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wshadow;-Werror>")
endif()
