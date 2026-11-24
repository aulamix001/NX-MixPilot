cmake_minimum_required(VERSION 3.22)
project(NXMixPilot VERSION 0.3.6 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
option(NX_BUILD_PLUGINS "Build the JUCE VST3 plug-ins" ON)
set(JUCE_ROOT "" CACHE PATH "Path to a checkout of JUCE 8.0.12")
if(NX_BUILD_PLUGINS)
    if(JUCE_ROOT)
        add_subdirectory("${JUCE_ROOT}" "${CMAKE_BINARY_DIR}/JUCE")
    else()
        include(FetchContent)
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 29396c22c93392d6738e021b83196283d6e4d850)
        FetchContent_MakeAvailable(JUCE)
    endif()

    # Both roles share one VST3 module and one in-process registry.
    juce_add_plugin(NXMixPilot
        COMPANY_NAME "NX Audio"
        BUNDLE_ID com.nxaudio.mixpilot
        PLUGIN_MANUFACTURER_CODE NxAu
        PLUGIN_CODE NxMp
        FORMATS VST3
        VST3_CATEGORIES Fx Analyzer
        PRODUCT_NAME "NX Audio MixPilot")
    target_sources(NXMixPilot PRIVATE Source/Plugin.cpp Source/Analysis.h Source/GlassLook.h Source/GainMeter.h Source/InstrumentPresets.h
        Source/TonalDSP.h Source/SpectralWorker.h Source/TonalPanel.h
        Source/SmartGain.h Source/GainStyles.h Source/SmartPanel.h Source/ReferenceAudio.h Source/LoudnessMeter.h)
    target_compile_definitions(NXMixPilot PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(NXMixPilot PRIVATE
        juce::juce_audio_processors juce::juce_gui_basics juce::juce_audio_formats juce::juce_recommended_config_flags)
endif()

if(BUILD_TESTING)
    add_executable(NXAnalysisTests Tests/AnalysisTests.cpp)
    target_include_directories(NXAnalysisTests PRIVATE Source)
    add_test(NAME NXAnalysisTests COMMAND NXAnalysisTests)
    add_executable(NXTonalTests Tests/TonalTests.cpp)
    target_include_directories(NXTonalTests PRIVATE Source)
    add_test(NAME NXTonalTests COMMAND NXTonalTests)
    if(TARGET NXMixPilot)
        add_executable(NXPluginIntegrationTests Tests/PluginIntegrationTests.cpp)
        target_include_directories(NXPluginIntegrationTests PRIVATE Source "$<TARGET_PROPERTY:NXMixPilot,INCLUDE_DIRECTORIES>")
        target_compile_definitions(NXPluginIntegrationTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
        target_link_libraries(NXPluginIntegrationTests PRIVATE NXMixPilot)
        add_test(NAME NXPluginIntegrationTests COMMAND NXPluginIntegrationTests)
    endif()
endif()

if(MSVC AND TARGET NXMixPilot)
    target_compile_options(NXMixPilot PRIVATE /utf-8)
endif()
