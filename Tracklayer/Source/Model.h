#pragma once

#include "Theme.h"

// An audio region on the timeline. The whole file is held in memory.
struct Clip
{
    juce::File file;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::shared_ptr<const std::vector<std::pair<float, float>>> peaks;   // min/max per peakBlock samples
    double fileSampleRate = 44100.0;

    double start  = 0.0;   // seconds on the timeline
    double offset = 0.0;   // seconds into the file where the region begins
    double length = 0.0;   // seconds

    static constexpr int peakBlock = 256;

    double end() const        { return start + length; }
    double fileLength() const { return audio != nullptr ? audio->getNumSamples() / fileSampleRate : 0.0; }
};

using ClipPtr = std::shared_ptr<Clip>;

// Number of plugin insert slots on every track (like Logic's Audio FX slots).
static constexpr int numInserts = 6;

// Audio thread reads these fields; the message thread is the only writer.
// Structural changes (adding/removing tracks or clips, moving clips) are made
// while holding Engine::lock. Simple flags are atomics.
struct Track
{
    juce::String name;
    juce::Colour colour;

    std::atomic<float> volume { 1.0f };   // linear gain
    std::atomic<float> pan    { 0.0f };   // -1 .. +1 (balance)
    std::atomic<bool>  mute { false }, solo { false }, armed { false }, monitor { false };
    std::atomic<bool>  stereoInput { false };
    std::atomic<int>   inputChannel { 0 };   // index into the device's active inputs

    std::vector<ClipPtr> clips;

    // Effect plugins, processed top to bottom. Changed only under Engine::lock.
    std::array<std::unique_ptr<juce::AudioPluginInstance>, numInserts> inserts;
    std::array<std::atomic<bool>, numInserts> bypassed {};
    std::array<juce::PluginDescription, numInserts> insertInfo;   // what each slot was loaded from

    // metering: audio thread writes peaks, UI turns them into decaying display values
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    float dispL = 0.0f, dispR = 0.0f;

    // recording state
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter;
    juce::File recFile;
    std::atomic<juce::int64> recStart { -1 };   // timeline sample where recording began
};

using TrackPtr = std::shared_ptr<Track>;
