cmake_minimum_required(VERSION 3.22)
project(DaliStructure VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static MSVC runtime: the plugin must not depend on the Visual C++ Redistributable
# version installed on the user's machine (the build server uses the newest compiler).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(DALI_BUILD_TESTS "Build engine tests" ON)
option(DALI_BUILD_TOOLS "Build the dali_analyze CLI" ON)
option(DALI_BUILD_PLUGIN "Build the JUCE plugin (VST3 + Standalone)" OFF)

# ------------------------------------------------------------ core engine
# Pure C++17, no JUCE / DAW / UI dependency.
add_library(dali_core STATIC
    core/src/Model.cpp
    core/src/Json.cpp
    core/src/Serialization.cpp
    core/src/Edits.cpp
    core/src/Engine.cpp
    core/src/dsp/Dsp.cpp
    core/src/analysis/FrameFeatures.cpp
    core/src/analysis/Rhythm.cpp
    core/src/analysis/BarFeatures.cpp
    core/src/analysis/Structure.cpp
    core/src/analysis/Events.cpp
)
target_include_directories(dali_core PUBLIC core/include PRIVATE core/src)
set_target_properties(dali_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(dali_core PRIVATE /W3 /permissive- /utf-8)
else()
    target_compile_options(dali_core PRIVATE -Wall -Wextra)
endif()

if (DALI_BUILD_TESTS)
    enable_testing()
    add_executable(dali_tests tests/test_engine.cpp)
    target_include_directories(dali_tests PRIVATE tests core/src)
    target_link_libraries(dali_tests PRIVATE dali_core)
    if (MSVC)
        target_compile_options(dali_tests PRIVATE /utf-8)
    endif()
    add_test(NAME unit COMMAND dali_tests --unit)
    add_test(NAME end_to_end COMMAND dali_tests)
endif()

if (DALI_BUILD_TOOLS)
    add_executable(dali_analyze tools/dali_analyze.cpp)
    target_link_libraries(dali_analyze PRIVATE dali_core)
endif()

# ------------------------------------------------------------ plugin (JUCE)
if (DALI_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    juce_add_plugin(DaliStructure
        PRODUCT_NAME "DaliStructure"
        COMPANY_NAME "Dali Audio"
        BUNDLE_ID "com.daliaudio.dalistructure"
        PLUGIN_MANUFACTURER_CODE Dali
        PLUGIN_CODE DlSt
        FORMATS VST3 Standalone
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Instrument Tools)

    target_sources(DaliStructure PRIVATE
        plugin/PluginProcessor.cpp
        plugin/PluginEditor.cpp)

    target_compile_definitions(DaliStructure PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_FLAC=1)

    if (MSVC)
        target_compile_options(DaliStructure PRIVATE /utf-8)
    endif()

    target_link_libraries(DaliStructure
        PRIVATE
            dali_core
            juce::juce_audio_utils
            juce::juce_audio_formats
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
