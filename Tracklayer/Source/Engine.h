#pragma once

#include "Model.h"

// The audio engine: owns the device, the project data and the transport.
class Engine : public juce::AudioIODeviceCallback
{
public:
    Engine();
    ~Engine() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;

    // ---- plugins ------------------------------------------------------
    juce::AudioPluginFormatManager pluginFormats;
    juce::KnownPluginList knownPlugins;
    juce::Array<juce::PluginDescription> getEffectPlugins() const;

    /** Puts a plugin in a slot (nullptr empties it). Returns the plugin it replaced,
        which the caller must delete after closing its editor window. */
    std::unique_ptr<juce::AudioPluginInstance> setInsert (Track&, int slot, std::unique_ptr<juce::AudioPluginInstance>,
                                                         const juce::PluginDescription& = {});
    /** Creates a plugin. If found via the scanned list instead, `d` is updated to that entry. */
    std::unique_ptr<juce::AudioPluginInstance> createPlugin (juce::PluginDescription& d, juce::String& error);
    void preparePlugin (juce::AudioPluginInstance&);
    static constexpr int pluginMaxBlock = 4096;

    // ---- project data -------------------------------------------------
    // The message thread may read tracks/clips freely; it must hold `lock`
    // while changing them. The audio thread only reads, under a try-lock.
    juce::CriticalSection lock;
    std::vector<TrackPtr> tracks;

    juce::File projectFile;     // empty until first save
    juce::File projectFolder;   // recordings go to <projectFolder>/Audio
    bool dirty = false;

    std::atomic<double> tempo { 120.0 };
    int beatsPerBar = 4;
    std::atomic<bool>   cycleOn { false }, metronomeOn { false };
    std::atomic<double> cycleStart { 0.0 }, cycleEnd { 8.0 };
    std::atomic<float>  masterVolume { 1.0f };
    std::atomic<float>  masterPeakL { 0.0f }, masterPeakR { 0.0f };
    float masterDispL = 0.0f, masterDispR = 0.0f;

    // ---- transport ----------------------------------------------------
    void play();
    void stop();
    void togglePlay();
    void toggleRecord();
    bool isPlaying() const   { return playing.load(); }
    bool isRecording() const { return recording.load(); }
    double getPosition() const;
    void setPosition (double seconds);
    double getSampleRate() const    { return sampleRate.load(); }
    double secondsPerBeat() const   { return 60.0 / tempo.load(); }
    double secondsPerBar() const    { return secondsPerBeat() * beatsPerBar; }
    juce::int64 getRecordingStart (const Track&) const;

    // ---- editing ------------------------------------------------------
    TrackPtr addTrack (juce::String name = {});
    void removeTrack (const TrackPtr&);
    int indexOf (const TrackPtr&) const;
    TrackPtr findOwner (const ClipPtr&) const;
    ClipPtr loadClip (const juce::File&, juce::String& error);
    bool importFile (const juce::File&, const TrackPtr&, double startSeconds, juce::String& error);
    void deleteClip (const ClipPtr&);
    bool splitClip (const ClipPtr&, double atSeconds);
    void moveClipToTrack (const ClipPtr&, const TrackPtr&);
    double getProjectEnd() const;
    juce::StringArray getActiveInputNames() const;

    // ---- files --------------------------------------------------------
    void newProject();
    bool saveProject (const juce::File&, juce::String& error);
    bool loadProject (const juce::File&, juce::String& error);
    bool exportMix (const juce::File&, juce::String& error);
    juce::String getProjectName() const;

    juce::String lastError;
    std::function<void()> onRecordingFinished;

    // ---- AudioIODeviceCallback ----------------------------------------
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    static constexpr int maxBlock = 2048;

    void processBlock (const float* const* in, int numIn, int inOffset, int numSamples);
    void renderSegment (juce::AudioBuffer<float>& out, int outOffset, int num, juce::int64 pos, double sr,
                        const float* const* in, int numIn, int inOffset,
                        bool live, bool playClips, bool rec);
    void renderMetronome (juce::AudioBuffer<float>& out, int outOffset, int num, juce::int64 pos, double sr);
    bool startRecording();
    void finishRecording();
    juce::File newRecordingFile (const juce::String& trackName);
    void clearTracks();

    std::atomic<juce::int64> positionSamples { 0 };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool> playing { false }, recording { false };

    juce::TimeSliceThread writerThread { "Tracklayer recorder" };
    juce::AudioBuffer<float> mixBuffer, trackBuffer, silence, pluginBuffer;
    juce::MidiBuffer pluginMidi;
    int currentBlockSize = 512;
    int recordLatencySamples = 0;

    int clickRemaining = 0, clickTotal = 1;
    double clickPhase = 0.0, clickInc = 0.0;
    float clickGain = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
