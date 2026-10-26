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
    juce::Array<juce::PluginDescription> getInstrumentPlugins() const;

    // ---- MIDI input ---------------------------------------------------
    juce::MidiMessageCollector midiCollector;      // all enabled MIDI inputs feed this
    std::atomic<Track*> midiFocus { nullptr };      // the selected track also plays live MIDI
    std::atomic<int> midiActivity { 0 };            // bumps whenever MIDI arrives (for the UI light)
    void enableAllMidiInputs();

    /** Puts a plugin in a slot (nullptr empties it). Returns the plugin it replaced,
        which the caller must delete after closing its editor window. */
    std::shared_ptr<juce::AudioPluginInstance> setInsert (Track&, int slot, std::shared_ptr<juce::AudioPluginInstance>,
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

    std::atomic<double> tempo { 120.0 };   // quarter notes per minute
    int beatsPerBar = 4;                    // time signature numerator
    int beatUnit = 4;                       // time signature denominator (4 or 8, also 2/16)
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
    double secondsPerBeat() const   { return 60.0 / tempo.load() * 4.0 / beatUnit; }   // one beat unit
    double secondsPerBar() const    { return secondsPerBeat() * beatsPerBar; }
    juce::int64 getRecordingStart (const Track&) const;

    // Tempo and time signature changes keep MIDI regions on their bars
    // (audio stays where it is, as recordings can't follow tempo).
    void setTempo (double bpm);
    void setTimeSignature (int numerator, int denominator);

    // ---- Session Drummer ----------------------------------------------
    TrackPtr addDrummerTrack();
    ClipPtr addDrummerRegion (const TrackPtr&, int startBar, int bars);
    void regenerateDrummer (Clip&);                 // rebuild notes from its settings (call under lock if live)
    void conformDrummerRegion (const ClipPtr&);     // snap to whole bars after a move/resize, then rebuild
    void updateDrummer (const ClipPtr&, const DrummerSettings&);
    void convertToMidi (const ClipPtr&);

    // ---- undo / redo --------------------------------------------------
    struct TrackSnap
    {
        TrackPtr track;
        juce::String name;
        juce::Colour colour;
        float volume = 1.0f, pan = 0.0f;
        bool mute = false, solo = false, stereoInput = false;
        int inputChannel = 0;
        std::vector<Clip> clips;
        std::array<std::shared_ptr<juce::AudioPluginInstance>, numInserts> inserts;
        std::array<bool, numInserts> bypassed {};
        std::array<juce::PluginDescription, numInserts> insertInfo;
        std::shared_ptr<juce::AudioPluginInstance> instrument;
        juce::PluginDescription instrumentInfo;
    };

    struct Snapshot
    {
        juce::String label;
        std::vector<TrackSnap> tracks;
        double tempo = 120.0;
        int beatsPerBar = 4, beatUnit = 4;
        float master = 1.0f;
    };

    void commit (const juce::String& label);   // call after every finished edit
    bool undo();
    bool redo();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    juce::String undoLabel() const { return undoStack.empty() ? juce::String() : undoStack.back().label; }
    juce::String redoLabel() const { return redoStack.empty() ? juce::String() : redoStack.back().label; }
    void resetHistory();
    std::vector<juce::AudioProcessor*> livePlugins() const;

    // ---- editing ------------------------------------------------------
    TrackPtr addTrack (juce::String name = {}, bool instrument = false);
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
                        bool live, bool playClips, bool rec,
                        const juce::MidiBuffer* liveMidi = nullptr, int liveMidiOffset = 0, bool flushNotes = false);
    void renderInstrument (Track&, int num, juce::int64 pos, double sr, bool playClips, bool rec,
                           const juce::MidiBuffer* liveMidi, int liveMidiOffset, bool flushNotes, bool takesLive);
    ClipPtr buildMidiClip (Track&, double sr);
    void renderMetronome (juce::AudioBuffer<float>& out, int outOffset, int num, juce::int64 pos, double sr);
    bool startRecording();
    void finishRecording();
    juce::File newRecordingFile (const juce::String& trackName);
    void clearTracks();
    Snapshot capture() const;
    void restore (const Snapshot&);
    std::vector<Snapshot> undoStack, redoStack;
    std::unique_ptr<Snapshot> baseline;
    static constexpr size_t maxUndo = 100;

    std::atomic<juce::int64> positionSamples { 0 };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool> playing { false }, recording { false };

    juce::TimeSliceThread writerThread { "Tracklayer recorder" };
    juce::AudioBuffer<float> mixBuffer, trackBuffer, silence, pluginBuffer;
    juce::MidiBuffer pluginMidi, liveMidi;
    std::atomic<bool> jumped { true };     // transport moved: silence hanging notes
    bool wasPlaying = false;
    int currentBlockSize = 512;
    int recordLatencySamples = 0;

    int clickRemaining = 0, clickTotal = 1;
    double clickPhase = 0.0, clickInc = 0.0;
    float clickGain = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
