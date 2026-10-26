#pragma once

#include "Theme.h"
#include <bitset>

// One note inside a MIDI region. Times are seconds from the region's source origin.
struct MidiNote
{
    double time = 0.0, length = 0.0;
    int note = 60, velocity = 100;
};

// Settings for a Session Drummer region. The region's notes are generated from these.
struct DrummerSettings
{
    int style = 0;
    float complexity = 0.4f;   // simple .. complex
    float loudness = 0.6f;     // soft .. loud
    float fills = 0.5f;        // none .. lots
    float swing = 0.0f;
    int cymbal = 0;            // 0 auto, 1 hi-hat, 2 ride
    int seed = 1;              // "variation"
    int startBar = 0;          // region position in bars (0-based)
    int bars = 8;
};

// A region on the timeline: either audio (the whole file is held in memory)
// or MIDI (a list of notes).
struct Clip
{
    bool isMidi = false;
    std::shared_ptr<DrummerSettings> drummer;                 // set for Session Drummer regions
    juce::String name;                                        // MIDI regions only
    std::shared_ptr<const std::vector<MidiNote>> notes;       // MIDI regions only

    juce::File file;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::shared_ptr<const std::vector<std::pair<float, float>>> peaks;   // min/max per peakBlock samples
    double fileSampleRate = 44100.0;

    double start  = 0.0;   // seconds on the timeline
    double offset = 0.0;   // seconds into the file where the region begins
    double length = 0.0;   // seconds

    static constexpr int peakBlock = 256;

    double end() const        { return start + length; }
    double fileLength() const
    {
        if (isMidi) return 1.0e7;   // MIDI regions can be stretched freely
        return audio != nullptr ? audio->getNumSamples() / fileSampleRate : 0.0;
    }
    juce::String displayName() const { return isMidi ? name : file.getFileNameWithoutExtension(); }
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
    std::array<std::shared_ptr<juce::AudioPluginInstance>, numInserts> inserts;   // shared so undo can keep removed ones
    std::array<std::atomic<bool>, numInserts> bypassed {};
    std::array<juce::PluginDescription, numInserts> insertInfo;   // what each slot was loaded from

    // Instrument tracks play MIDI regions and live MIDI through a synth plugin.
    bool isInstrument = false;
    bool isDrummer = false;    // a Session Drummer track (an instrument track with drummer regions)
    std::shared_ptr<juce::AudioPluginInstance> instrument;
    juce::PluginDescription instrumentInfo;

    // slot -1 = the instrument, 0.. = Audio FX inserts
    juce::AudioPluginInstance* pluginAt (int slot) const
    {
        if (slot < 0) return instrument.get();
        return slot < numInserts ? inserts[(size_t) slot].get() : nullptr;
    }

    // notes currently held down on the instrument, per MIDI channel (audio thread only)
    std::array<std::bitset<128>, 16> soundingNotes;

    // MIDI recording (audio thread appends while recording)
    struct RecordedMidi { double time; juce::uint8 b0, b1, b2; };
    std::vector<RecordedMidi> recMidi;

    // metering: audio thread writes peaks, UI turns them into decaying display values
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    float dispL = 0.0f, dispR = 0.0f;

    // recording state
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter;
    juce::File recFile;
    std::atomic<juce::int64> recStart { -1 };   // timeline sample where recording began
};

using TrackPtr = std::shared_ptr<Track>;
