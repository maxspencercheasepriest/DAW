cmake_minimum_required(VERSION 3.22)

project(Tracklayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if JUCE_SOURCE_DIR is given, otherwise download it.
#   cmake -B build -DJUCE_SOURCE_DIR=C:/path/to/JUCE
# ---------------------------------------------------------------------------
set(JUCE_SOURCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(JUCE_SOURCE_DIR)
    add_subdirectory(${JUCE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Optional ASIO support (Windows). Point this at the Steinberg ASIO SDK folder
# (the one containing common/iasiodrv.h) to enable ASIO drivers:
#   cmake -B build -DASIO_SDK_DIR=C:/SDKs/asiosdk
# Without it the app uses Windows Audio (WASAPI), including low-latency
# exclusive mode, which works with almost every interface.
# ---------------------------------------------------------------------------
set(ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (optional)")

juce_add_gui_app(Tracklayer
    PRODUCT_NAME "Tracklayer"
    COMPANY_NAME "Tracklayer"
    BUNDLE_ID "com.tracklayer.app"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Tracklayer needs microphone access to record audio."
    VERSION ${PROJECT_VERSION})

target_sources(Tracklayer PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/MainComponent.cpp
    Source/BuiltIn.cpp
    Source/Drummer.cpp)

# Piano samples for Tracklayer Keys, compiled into the app
# (Salamander Grand Piano by Alexander Holm, CC-BY 3.0)
juce_add_binary_data(TracklayerData SOURCES Resources/piano.zip)

target_compile_definitions(Tracklayer PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Tracklayer,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Tracklayer,JUCE_VERSION>")

if(WIN32 AND ASIO_SDK_DIR)
    target_compile_definitions(Tracklayer PRIVATE JUCE_ASIO=1)
    target_include_directories(Tracklayer PRIVATE "${ASIO_SDK_DIR}/common")
    message(STATUS "Tracklayer: ASIO enabled using ${ASIO_SDK_DIR}")
endif()

target_link_libraries(Tracklayer
    PRIVATE
        TracklayerData
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
