#include "Engine.h"
#include "BuiltIn.h"

using namespace juce;

static void storeMax (std::atomic<float>& a, float v)
{
    float cur = a.load();
    while (v > cur && ! a.compare_exchange_weak (cur, v)) {}
}

//==============================================================================
Engine::Engine()
{
    formatManager.registerBasicFormats();
    pluginFormats.addDefaultFormats();
    pluginFormats.addFormat (new BuiltIn::Format());
    pluginBuffer.setSize (32, pluginMaxBlock);
    pluginMidi.ensureSize (4096);
    liveMidi.ensureSize (4096);
    midiCollector.reset (44100.0);
    writerThread.startThread();

    mixBuffer.setSize (2, maxBlock);
    trackBuffer.setSize (2, 8192);
    silence.setSize (2, 8192);
    silence.clear();

    newProject();
    deviceManager.addAudioCallback (this);
}

Engine::~Engine()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    {
        const ScopedLock sl (lock);
        for (auto& t : tracks)
            t->recWriter.reset();
    }
    writerThread.stopThread (3000);
}

//==============================================================================
// Transport
//==============================================================================
void Engine::play()
{
    if (playing)
        return;

    if (cycleOn && ! recording)
    {
        const auto pos = getPosition();
        if (pos < cycleStart || pos >= cycleEnd)
            setPosition (cycleStart);
    }
    jumped = true;
    playing = true;
}

void Engine::stop()
{
    const bool wasRecording = recording.load();
    playing = false;
    jumped = true;
    if (wasRecording)
        finishRecording();
}

void Engine::togglePlay()
{
    if (playing) stop();
    else         play();
}

void Engine::toggleRecord()
{
    if (recording)
        stop();
    else
        startRecording();
}

double Engine::getPosition() const
{
    return (double) positionSamples.load() / sampleRate.load();
}

void Engine::setPosition (double seconds)
{
    if (recording)
        return;
    positionSamples = (int64) (jmax (0.0, seconds) * sampleRate.load());
    jumped = true;
}

int64 Engine::getRecordingStart (const Track& t) const
{
    return recording ? t.recStart.load() : -1;
}

//==============================================================================
// Recording
//==============================================================================
File Engine::newRecordingFile (const String& trackName)
{
    auto folder = projectFolder.getChildFile ("Audio");
    folder.createDirectory();
    auto base = File::createLegalFileName (trackName.isEmpty() ? String ("Audio") : trackName)
                + " " + Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    return folder.getNonexistentChildFile (base, ".wav", false);
}

bool Engine::startRecording()
{
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr)
    {
        lastError = "No audio device is open. Choose one in Settings (Ctrl+,).";
        return false;
    }

    std::vector<TrackPtr> armed, armedMidi;
    for (auto& t : tracks)
        if (t->armed)
            (t->isInstrument ? armedMidi : armed).push_back (t);

    if (armed.empty() && armedMidi.empty())
    {
        lastError = "Arm a track for recording first (the R button on the track).";
        return false;
    }

    const int numIn = dev->getActiveInputChannels().countNumberOfSetBits();
    if (numIn == 0 && ! armed.empty())
    {
        lastError = "No input channels are enabled. Turn some on in Settings (Ctrl+,).";
        return false;
    }

    for (auto& t : armedMidi)
    {
        const ScopedLock sl (lock);
        t->recMidi.clear();
        t->recMidi.reserve (200000);
        t->recStart = -1;
    }

    const double sr = sampleRate.load();
    recordLatencySamples = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();

    for (auto& t : armed)
    {
        auto file = newRecordingFile (t->name);
        const int chans = t->stereoInput ? 2 : 1;

        std::unique_ptr<FileOutputStream> os (file.createOutputStream());
        if (os == nullptr)
        {
            lastError = "Couldn't create recording file:\n" + file.getFullPathName();
            continue;
        }

        WavAudioFormat wav;
        auto* writer = wav.createWriterFor (os.get(), sr, (unsigned int) chans, 24, {}, 0);
        if (writer == nullptr)
        {
            lastError = "Couldn't create a WAV writer for " + file.getFullPathName();
            continue;
        }
        os.release();   // the writer owns the stream now

        auto threaded = std::make_unique<AudioFormatWriter::ThreadedWriter> (writer, writerThread, 1 << 17);

        const ScopedLock sl (lock);
        t->recFile = file;
        t->recStart = -1;
        t->recWriter = std::move (threaded);
    }

    recording = true;
    playing = true;
    return true;
}

void Engine::finishRecording()
{
    recording = false;

    std::vector<std::pair<TrackPtr, std::unique_ptr<AudioFormatWriter::ThreadedWriter>>> finished;
    std::vector<std::pair<TrackPtr, ClipPtr>> midiTakes;
    const double sr = sampleRate.load();
    {
        const ScopedLock sl (lock);   // waits for any in-flight audio callback
        for (auto& t : tracks)
        {
            if (t->recWriter != nullptr)
                finished.emplace_back (t, std::move (t->recWriter));

            if (t->isInstrument && t->armed)
                if (auto clip = buildMidiClip (*t, sr))
                    midiTakes.emplace_back (t, clip);
        }

        for (auto& [t, clip] : midiTakes)
            t->clips.push_back (clip);
    }

    for (auto& [track, writer] : finished)
    {
        const auto start = track->recStart.load();
        writer.reset();   // flushes everything to disk

        if (start < 0)
        {
            track->recFile.deleteFile();
            continue;
        }

        String err;
        auto clip = loadClip (track->recFile, err);
        if (clip == nullptr)
            continue;

        // The input arrives late by the round-trip latency; trim it off.
        const double lat = recordLatencySamples / sr;
        clip->start  = (double) start / sr;
        clip->offset = jmin (lat, clip->fileLength());
        clip->length = clip->fileLength() - clip->offset;
        if (clip->length <= 0.001)
            continue;

        const ScopedLock sl (lock);
        track->clips.push_back (clip);
    }

    dirty = true;
    if (onRecordingFinished)
        onRecordingFinished();
}

ClipPtr Engine::buildMidiClip (Track& t, double sr)
{
    const auto startSample = t.recStart.load();
    if (startSample < 0)
        return {};

    const double start = (double) startSample / sr;
    const double end = getPosition();
    auto notes = std::make_shared<std::vector<MidiNote>>();

    // Pair each note-on with the next note-off for the same key and channel.
    std::array<std::array<double, 128>, 16> onTime;
    std::array<std::array<int, 128>, 16> onVel;
    for (auto& a : onTime) a.fill (-1.0);

    auto close = [&] (int ch, int key, double when)
    {
        if (onTime[(size_t) ch][(size_t) key] < 0.0)
            return;
        const double t0 = onTime[(size_t) ch][(size_t) key];
        notes->push_back ({ t0 - start, jmax (0.01, when - t0), key, onVel[(size_t) ch][(size_t) key] });
        onTime[(size_t) ch][(size_t) key] = -1.0;
    };

    for (auto& ev : t.recMidi)
    {
        const int status = ev.b0 & 0xf0, ch = ev.b0 & 0x0f, key = ev.b1 & 0x7f;
        if (status == 0x90 && ev.b2 > 0)
        {
            close (ch, key, ev.time);
            onTime[(size_t) ch][(size_t) key] = ev.time;
            onVel[(size_t) ch][(size_t) key] = ev.b2;
        }
        else if (status == 0x80 || (status == 0x90 && ev.b2 == 0))
        {
            close (ch, key, ev.time);
        }
    }
    for (int ch = 0; ch < 16; ++ch)
        for (int key = 0; key < 128; ++key)
            close (ch, key, end);   // keys still held when recording stopped

    t.recMidi.clear();
    t.recMidi.shrink_to_fit();
    if (notes->empty())
        return {};

    std::sort (notes->begin(), notes->end(), [] (const MidiNote& a, const MidiNote& b) { return a.time < b.time; });

    auto clip = std::make_shared<Clip>();
    clip->isMidi = true;
    clip->name = t.name;
    clip->notes = notes;
    clip->start = start;
    clip->offset = 0.0;
    clip->length = jmax (end - start, notes->back().time + notes->back().length);
    return clip;
}

//==============================================================================
// Editing
//==============================================================================
TrackPtr Engine::addTrack (String name, bool instrument)
{
    auto t = std::make_shared<Track>();
    t->isInstrument = instrument;
    t->name = name.isNotEmpty() ? name : (instrument ? "Inst " : "Audio ") + String ((int) tracks.size() + 1);
    t->colour = Theme::trackColour ((int) tracks.size());

    // Default each new track to the next mono input.
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        const int numIn = dev->getActiveInputChannels().countNumberOfSetBits();
        if (numIn > 0)
            t->inputChannel = (int) tracks.size() % numIn;
    }

    const ScopedLock sl (lock);
    tracks.push_back (t);
    dirty = true;
    return t;
}

void Engine::removeTrack (const TrackPtr& t)
{
    if (recording || t == nullptr)
        return;

    const ScopedLock sl (lock);
    tracks.erase (std::remove (tracks.begin(), tracks.end(), t), tracks.end());
    dirty = true;
}

int Engine::indexOf (const TrackPtr& t) const
{
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i] == t)
            return (int) i;
    return -1;
}

TrackPtr Engine::findOwner (const ClipPtr& c) const
{
    for (auto& t : tracks)
        for (auto& tc : t->clips)
            if (tc == c)
                return t;
    return {};
}

ClipPtr Engine::loadClip (const File& file, String& error)
{
    std::unique_ptr<AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Can't read " + file.getFileName() + " (unsupported or damaged file).";
        return {};
    }

    if (reader->lengthInSamples <= 0)
    {
        error = file.getFileName() + " is empty.";
        return {};
    }

    if (reader->lengthInSamples > (int64) (reader->sampleRate * 60.0 * 40.0))
    {
        error = file.getFileName() + " is longer than 40 minutes, which this version doesn't support.";
        return {};
    }

    const int chans = jlimit (1, 2, (int) reader->numChannels);
    const int len   = (int) reader->lengthInSamples;

    auto buffer = std::make_shared<AudioBuffer<float>> (chans, len);
    reader->read (buffer.get(), 0, len, 0, true, chans > 1);

    // Waveform overview
    auto peaks = std::make_shared<std::vector<std::pair<float, float>>>();
    peaks->reserve ((size_t) (len / Clip::peakBlock + 1));
    for (int i = 0; i < len; i += Clip::peakBlock)
    {
        const int n = jmin (Clip::peakBlock, len - i);
        float mn = 0.0f, mx = 0.0f;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto r = buffer->findMinMax (ch, i, n);
            mn = jmin (mn, r.getStart());
            mx = jmax (mx, r.getEnd());
        }
        peaks->emplace_back (mn, mx);
    }

    auto clip = std::make_shared<Clip>();
    clip->file = file;
    clip->audio = buffer;
    clip->peaks = peaks;
    clip->fileSampleRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;
    clip->start = 0.0;
    clip->offset = 0.0;
    clip->length = clip->fileLength();
    return clip;
}

bool Engine::importFile (const File& file, const TrackPtr& track, double startSeconds, String& error)
{
    if (track == nullptr)
        return false;

    auto clip = loadClip (file, error);
    if (clip == nullptr)
        return false;

    clip->start = jmax (0.0, startSeconds);
    const ScopedLock sl (lock);
    track->clips.push_back (clip);
    dirty = true;
    return true;
}

void Engine::deleteClip (const ClipPtr& c)
{
    const ScopedLock sl (lock);
    for (auto& t : tracks)
        t->clips.erase (std::remove (t->clips.begin(), t->clips.end(), c), t->clips.end());
    dirty = true;
}

bool Engine::splitClip (const ClipPtr& c, double at)
{
    if (c == nullptr || at <= c->start + 0.001 || at >= c->end() - 0.001)
        return false;

    auto owner = findOwner (c);
    if (owner == nullptr)
        return false;

    auto right = std::make_shared<Clip> (*c);
    right->start  = at;
    right->offset = c->offset + (at - c->start);
    right->length = c->end() - at;

    const ScopedLock sl (lock);
    c->length = at - c->start;
    owner->clips.push_back (right);
    dirty = true;
    return true;
}

void Engine::moveClipToTrack (const ClipPtr& c, const TrackPtr& dest)
{
    auto owner = findOwner (c);
    if (owner == nullptr || dest == nullptr || owner == dest || (c->isMidi && ! dest->isInstrument))
        return;

    const ScopedLock sl (lock);
    owner->clips.erase (std::remove (owner->clips.begin(), owner->clips.end(), c), owner->clips.end());
    dest->clips.push_back (c);
    dirty = true;
}

double Engine::getProjectEnd() const
{
    double end = 0.0;
    for (auto& t : tracks)
        for (auto& c : t->clips)
            end = jmax (end, c->end());
    return end;
}

StringArray Engine::getActiveInputNames() const
{
    StringArray result;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        auto names  = dev->getInputChannelNames();
        auto active = dev->getActiveInputChannels();
        for (int i = 0; i < names.size(); ++i)
            if (active[i])
                result.add (names[i]);
    }
    return result;
}

//==============================================================================
// Project files (.tlproj = JSON)
//==============================================================================
void Engine::clearTracks()
{
    const ScopedLock sl (lock);
    tracks.clear();
}

void Engine::newProject()
{
    stop();
    clearTracks();
    projectFile = File();
    projectFolder = File::getSpecialLocation (File::userDocumentsDirectory)
                        .getChildFile ("Tracklayer Projects")
                        .getChildFile ("Untitled " + Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M"));
    tempo = 120.0;
    cycleOn = false;
    cycleStart = 0.0;
    cycleEnd = secondsPerBar() * 4.0;
    masterVolume = 1.0f;
    positionSamples = 0;

    addTrack();
    addTrack();
    dirty = false;
}

String Engine::getProjectName() const
{
    return projectFile.existsAsFile() ? projectFile.getFileNameWithoutExtension() : String ("Untitled");
}

bool Engine::saveProject (const File& file, String& error)
{
    auto dir = file.getParentDirectory();
    auto root = std::make_unique<DynamicObject>();
    root->setProperty ("app", "Tracklayer");
    root->setProperty ("version", 1);
    root->setProperty ("tempo", tempo.load());
    root->setProperty ("beatsPerBar", beatsPerBar);
    root->setProperty ("cycleOn", cycleOn.load());
    root->setProperty ("cycleStart", cycleStart.load());
    root->setProperty ("cycleEnd", cycleEnd.load());
    root->setProperty ("masterVolume", masterVolume.load());
    root->setProperty ("position", getPosition());

    Array<var> trackList;
    for (auto& t : tracks)
    {
        auto to = std::make_unique<DynamicObject>();
        to->setProperty ("name", t->name);
        to->setProperty ("colour", t->colour.toString());
        to->setProperty ("volume", t->volume.load());
        to->setProperty ("pan", t->pan.load());
        to->setProperty ("mute", t->mute.load());
        to->setProperty ("solo", t->solo.load());
        to->setProperty ("inputChannel", t->inputChannel.load());
        to->setProperty ("stereoInput", t->stereoInput.load());

        Array<var> clipList;
        for (auto& c : t->clips)
        {
            auto co = std::make_unique<DynamicObject>();
            if (c->isMidi)
            {
                Array<var> noteList;
                if (c->notes != nullptr)
                    for (auto& nt : *c->notes)
                        noteList.add (Array<var> { nt.time, nt.length, nt.note, nt.velocity });
                co->setProperty ("name", c->name);
                co->setProperty ("midi", noteList);
            }
            else
            {
                co->setProperty ("file", c->file.isAChildOf (dir) ? c->file.getRelativePathFrom (dir)
                                                                    : c->file.getFullPathName());
            }
            co->setProperty ("start", c->start);
            co->setProperty ("offset", c->offset);
            co->setProperty ("length", c->length);
            clipList.add (var (co.release()));
        }
        to->setProperty ("clips", clipList);


        Array<var> pluginList;
        for (size_t s = 0; s < (size_t) numInserts; ++s)
        {
            auto* plug = t->inserts[s].get();
            if (plug == nullptr)
                continue;

            MemoryBlock state;
            plug->getStateInformation (state);
            auto po = std::make_unique<DynamicObject>();
            po->setProperty ("slot", (int) s);
            po->setProperty ("bypassed", t->bypassed[s].load());
            auto desc = t->insertInfo[s].fileOrIdentifier.isNotEmpty() ? t->insertInfo[s] : plug->getPluginDescription();
            po->setProperty ("description", desc.createXml()->toString (XmlElement::TextFormat().singleLine()));
            po->setProperty ("state", state.toBase64Encoding());
            pluginList.add (var (po.release()));
        }
        to->setProperty ("plugins", pluginList);

        to->setProperty ("instrumentTrack", t->isInstrument);
        if (t->instrument != nullptr)
        {
            MemoryBlock state;
            t->instrument->getStateInformation (state);
            auto io = std::make_unique<DynamicObject>();
            auto desc = t->instrumentInfo.fileOrIdentifier.isNotEmpty() ? t->instrumentInfo : t->instrument->getPluginDescription();
            io->setProperty ("description", desc.createXml()->toString (XmlElement::TextFormat().singleLine()));
            io->setProperty ("state", state.toBase64Encoding());
            to->setProperty ("instrument", var (io.release()));
        }
        trackList.add (var (to.release()));
    }
    root->setProperty ("tracks", trackList);

    dir.createDirectory();
    if (! file.replaceWithText (JSON::toString (var (root.release()))))
    {
        error = "Couldn't write " + file.getFullPathName();
        return false;
    }

    // Recordings made before the first save live in the temporary project
    // folder; that's fine, the project stores their full paths.
    projectFile = file;
    projectFolder = dir;
    dirty = false;
    return true;
}

bool Engine::loadProject (const File& file, String& error)
{
    auto json = JSON::parse (file.loadFileAsString());
    if (! json.isObject() || json["app"].toString() != "Tracklayer")
    {
        error = file.getFileName() + " isn't a Tracklayer project.";
        return false;
    }

    stop();
    auto dir = file.getParentDirectory();
    std::vector<TrackPtr> loaded;
    StringArray missing;

    if (auto* list = json["tracks"].getArray())
    {
        for (auto& tv : *list)
        {
            auto t = std::make_shared<Track>();
            t->name = tv["name"].toString();
            t->colour = Colour::fromString (tv["colour"].toString());
            t->volume = (float) (double) tv.getProperty ("volume", 1.0);
            t->pan    = (float) (double) tv.getProperty ("pan", 0.0);
            t->mute   = (bool) tv.getProperty ("mute", false);
            t->solo   = (bool) tv.getProperty ("solo", false);
            t->inputChannel = (int) tv.getProperty ("inputChannel", 0);
            t->stereoInput  = (bool) tv.getProperty ("stereoInput", false);

            if (auto* clips = tv["clips"].getArray())
            {
                for (auto& cv : *clips)
                {
                    if (auto* noteList = cv["midi"].getArray())
                    {
                        auto notes = std::make_shared<std::vector<MidiNote>>();
                        for (auto& nv : *noteList)
                            if (nv.size() >= 4)
                                notes->push_back ({ (double) nv[0], (double) nv[1],
                                                    jlimit (0, 127, (int) nv[2]), jlimit (1, 127, (int) nv[3]) });
                        auto c = std::make_shared<Clip>();
                        c->isMidi = true;
                        c->name = cv["name"].toString();
                        c->notes = notes;
                        c->start  = (double) cv["start"];
                        c->offset = jmax (0.0, (double) cv["offset"]);
                        c->length = jmax (0.01, (double) cv["length"]);
                        t->clips.push_back (c);
                        continue;
                    }

                    auto f = dir.getChildFile (cv["file"].toString());
                    String err;
                    auto c = f.existsAsFile() ? loadClip (f, err) : ClipPtr();
                    if (c == nullptr)
                    {
                        missing.addIfNotAlreadyThere (f.getFileName());
                        continue;
                    }
                    c->start  = (double) cv["start"];
                    c->offset = jlimit (0.0, c->fileLength(), (double) cv["offset"]);
                    c->length = jlimit (0.001, c->fileLength() - c->offset, (double) cv["length"]);
                    t->clips.push_back (c);
                }
            }
            if (auto* plugins = tv["plugins"].getArray())
            {
                for (auto& pv : *plugins)
                {
                    const int slot = (int) pv["slot"];
                    PluginDescription desc;
                    auto xml = parseXML (pv["description"].toString());
                    if (slot < 0 || slot >= numInserts || xml == nullptr || ! desc.loadFromXml (*xml))
                        continue;

                    String err;
                    auto plug = createPlugin (desc, err);
                    if (plug == nullptr)
                    {
                        missing.addIfNotAlreadyThere ("Plugin: " + desc.name);
                        continue;
                    }

                    MemoryBlock state;
                    if (state.fromBase64Encoding (pv["state"].toString()) && state.getSize() > 0)
                        plug->setStateInformation (state.getData(), (int) state.getSize());

                    preparePlugin (*plug);
                    t->inserts[(size_t) slot] = std::move (plug);
                    t->insertInfo[(size_t) slot] = desc;
                    t->bypassed[(size_t) slot] = (bool) pv.getProperty ("bypassed", false);
                }
            }

            t->isInstrument = (bool) tv.getProperty ("instrumentTrack", false);
            if (t->isInstrument && tv["instrument"].isObject())
            {
                auto iv = tv["instrument"];
                PluginDescription desc;
                auto xml = parseXML (iv["description"].toString());
                if (xml != nullptr && desc.loadFromXml (*xml))
                {
                    String err;
                    if (auto synth = createPlugin (desc, err))
                    {
                        MemoryBlock state;
                        if (state.fromBase64Encoding (iv["state"].toString()) && state.getSize() > 0)
                            synth->setStateInformation (state.getData(), (int) state.getSize());
                        preparePlugin (*synth);
                        t->instrument = std::move (synth);
                        t->instrumentInfo = desc;
                    }
                    else
                    {
                        missing.addIfNotAlreadyThere ("Plugin: " + desc.name);
                    }
                }
            }

            loaded.push_back (t);
        }
    }

    {
        const ScopedLock sl (lock);
        tracks = std::move (loaded);
    }

    tempo = jlimit (20.0, 300.0, (double) json.getProperty ("tempo", 120.0));
    beatsPerBar = jlimit (1, 16, (int) json.getProperty ("beatsPerBar", 4));
    cycleOn = (bool) json.getProperty ("cycleOn", false);
    cycleStart = (double) json.getProperty ("cycleStart", 0.0);
    cycleEnd = (double) json.getProperty ("cycleEnd", secondsPerBar() * 4.0);
    masterVolume = (float) (double) json.getProperty ("masterVolume", 1.0);
    setPosition ((double) json.getProperty ("position", 0.0));

    projectFile = file;
    projectFolder = dir;
    dirty = false;

    if (missing.size() > 0)
        error = "These files or plugins couldn't be found and were skipped:\n" + missing.joinIntoString ("\n");
    return true;
}

bool Engine::exportMix (const File& file, String& error)
{
    stop();

    const double end = getProjectEnd();
    if (end <= 0.0)
    {
        error = "There's nothing to export yet.";
        return false;
    }

    const double sr = sampleRate.load();
    file.deleteFile();
    std::unique_ptr<FileOutputStream> os (file.createOutputStream());
    if (os == nullptr)
    {
        error = "Couldn't create " + file.getFullPathName();
        return false;
    }

    WavAudioFormat wav;
    std::unique_ptr<AudioFormatWriter> writer (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr)
    {
        error = "Couldn't create a WAV writer.";
        return false;
    }
    os.release();

    const int block = maxBlock;
    AudioBuffer<float> buf (2, block);
    const float mv = masterVolume.load();

    // Let effect tails (reverb, delay) ring out for up to 3 seconds.
    bool anyPlugins = false;
    for (auto& t : tracks)
    {
        for (auto& p : t->inserts)
            anyPlugins = anyPlugins || p != nullptr;
        anyPlugins = anyPlugins || t->instrument != nullptr;
    }
    const auto total = (int64) std::ceil ((end + (anyPlugins ? 3.0 : 0.05)) * sr);

    // Holding the lock pauses the live audio callback (it outputs silence).
    const ScopedLock sl (lock);

    auto forEachPlugin = [this] (auto fn)
    {
        for (auto& t : tracks)
        {
            for (auto& p : t->inserts)
                if (p != nullptr)
                    fn (*p);
            if (t->instrument != nullptr)
                fn (*t->instrument);
        }
    };
    forEachPlugin ([] (AudioPluginInstance& p) { p.setNonRealtime (true); p.reset(); });

    for (int64 pos = 0; pos < total; pos += block)
    {
        const int n = (int) jmin ((int64) block, total - pos);
        buf.clear();
        renderSegment (buf, 0, n, pos, sr, nullptr, 0, 0, false, true, false, nullptr, 0, pos == 0);
        buf.applyGain (0, n, mv);
        writer->writeFromAudioSampleBuffer (buf, 0, n);
    }

    forEachPlugin ([] (AudioPluginInstance& p) { p.setNonRealtime (false); p.reset(); });
    return true;
}

//==============================================================================
// Audio
//==============================================================================
void Engine::audioDeviceAboutToStart (AudioIODevice* device)
{
    const double oldRate = sampleRate.load();
    const double newRate = device->getCurrentSampleRate() > 0 ? device->getCurrentSampleRate() : 44100.0;

    positionSamples = (int64) ((double) positionSamples.load() * newRate / oldRate);
    sampleRate = newRate;

    const int bs = jmax (maxBlock, device->getCurrentBufferSizeSamples());
    mixBuffer.setSize (2, maxBlock);
    trackBuffer.setSize (2, jmax (8192, bs));
    silence.setSize (2, jmax (8192, bs));
    silence.clear();
    currentBlockSize = jmin (maxBlock, jmax (16, device->getCurrentBufferSizeSamples()));

    midiCollector.reset (newRate);

    for (auto& t : tracks)
    {
        for (auto& p : t->inserts)
            if (p != nullptr)
                preparePlugin (*p);
        if (t->instrument != nullptr)
            preparePlugin (*t->instrument);
    }
}

void Engine::audioDeviceStopped() {}

//==============================================================================
// Plugins
//==============================================================================
Array<PluginDescription> Engine::getInstrumentPlugins() const
{
    Array<PluginDescription> result;
    for (auto& d : BuiltIn::getTypes())
        if (d.isInstrument)
            result.add (d);
    for (auto& d : knownPlugins.getTypes())
        if (d.isInstrument)
            result.add (d);
    return result;
}

void Engine::enableAllMidiInputs()
{
    for (auto& d : MidiInput::getAvailableDevices())
    {
        // Skip DAW-control ports (e.g. "Komplete Kontrol DAW - 1"): they carry
        // controller-integration messages, not notes.
        if (d.name.containsIgnoreCase ("DAW"))
            continue;
        if (! deviceManager.isMidiInputDeviceEnabled (d.identifier))
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
    }
}

Array<PluginDescription> Engine::getEffectPlugins() const
{
    Array<PluginDescription> result;
    for (auto& d : BuiltIn::getTypes())
        if (! d.isInstrument)
            result.add (d);
    for (auto& d : knownPlugins.getTypes())
        if (! d.isInstrument)
            result.add (d);
    return result;
}

void Engine::preparePlugin (AudioPluginInstance& p)
{
    // Ask for stereo in/out; fall back to mono-in/stereo-out, then mono.
    using CS = AudioChannelSet;
    const std::pair<CS, CS> layouts[] = { { CS::stereo(), CS::stereo() },
                                          { CS::mono(),   CS::stereo() },
                                          { CS::mono(),   CS::mono() } };
    for (auto& [in, out] : layouts)
    {
        AudioProcessor::BusesLayout l = p.getBusesLayout();
        if (l.inputBuses.isEmpty() || l.outputBuses.isEmpty())
            break;
        l.inputBuses.getReference (0) = in;
        l.outputBuses.getReference (0) = out;
        if (p.checkBusesLayoutSupported (l) && p.setBusesLayout (l))
            break;
    }

    p.setRateAndBufferSizeDetails (sampleRate.load(), pluginMaxBlock);
    p.prepareToPlay (sampleRate.load(), pluginMaxBlock);
}

std::unique_ptr<AudioPluginInstance> Engine::createPlugin (PluginDescription& d, String& error)
{
    auto p = pluginFormats.createPluginInstance (d, sampleRate.load(), pluginMaxBlock, error);

    // Not where it was (e.g. the project came from another computer)?
    // Look for the same plugin among the ones scanned on this machine.
    if (p == nullptr)
    {
        for (auto& k : knownPlugins.getTypes())
        {
            if (k.name == d.name && k.manufacturerName == d.manufacturerName && k.pluginFormatName == d.pluginFormatName)
            {
                String err2;
                p = pluginFormats.createPluginInstance (k, sampleRate.load(), pluginMaxBlock, err2);
                if (p != nullptr)
                {
                    error.clear();
                    d = k;
                    break;
                }
            }
        }
    }

    if (p == nullptr && error.isEmpty())
        error = "Couldn't load " + d.name;
    return p;
}

std::unique_ptr<AudioPluginInstance> Engine::setInsert (Track& t, int slot, std::unique_ptr<AudioPluginInstance> p,
                                                      const PluginDescription& desc)
{
    if (slot < -1 || slot >= numInserts || (slot == -1 && ! t.isInstrument))
        return p;

    if (p != nullptr)
        preparePlugin (*p);

    if (slot == -1)
    {
        {
            const ScopedLock sl (lock);
            std::swap (t.instrument, p);
            t.instrumentInfo = desc;
            for (auto& ch : t.soundingNotes) ch.reset();
        }
        if (p != nullptr)
            p->releaseResources();
        dirty = true;
        return p;
    }

    {
        const ScopedLock sl (lock);
        std::swap (t.inserts[(size_t) slot], p);
        t.bypassed[(size_t) slot] = false;
        t.insertInfo[(size_t) slot] = desc;
    }

    if (p != nullptr)
        p->releaseResources();
    dirty = true;
    return p;   // the old plugin
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                               float* const* out, int numOut,
                                               int numSamples, const AudioIODeviceCallbackContext&)
{
    int done = 0;
    while (done < numSamples)
    {
        const int n = jmin (numSamples - done, maxBlock);
        processBlock (in, numIn, done, n);

        for (int ch = 0; ch < numOut; ++ch)
        {
            if (out[ch] == nullptr)
                continue;
            if (ch < 2)
                FloatVectorOperations::copy (out[ch] + done, mixBuffer.getReadPointer (ch), n);
            else
                FloatVectorOperations::clear (out[ch] + done, n);
        }
        done += n;
    }
}

void Engine::processBlock (const float* const* in, int numIn, int inOffset, int n)
{
    mixBuffer.clear (0, n);

    // Always drain incoming MIDI so it never piles up.
    liveMidi.clear();
    midiCollector.removeNextBlockOfMessages (liveMidi, n);
    if (! liveMidi.isEmpty())
        ++midiActivity;

    const ScopedTryLock sl (lock);
    if (! sl.isLocked())
        return;   // the UI is restructuring the project; output silence this block

    const double sr = sampleRate.load();
    const bool isPlay = playing.load();
    const bool isRec  = recording.load();
    auto startPos = positionSamples.load();
    auto pos = startPos;

    bool flush = jumped.exchange (false) || isPlay != wasPlaying;
    wasPlaying = isPlay;

    int done = 0;
    while (done < n)
    {
        int chunk = n - done;
        const auto cs = (int64) (cycleStart.load() * sr);
        const auto ce = (int64) (cycleEnd.load() * sr);
        const bool cyc = isPlay && ! isRec && cycleOn.load() && ce > cs + 64;

        if (cyc && pos < ce && pos + chunk > ce)
            chunk = (int) (ce - pos);

        renderSegment (mixBuffer, done, chunk, pos, sr, in, numIn, inOffset + done, true, isPlay, isRec,
                       &liveMidi, done, flush);
        flush = false;
        if (isPlay && metronomeOn.load())
            renderMetronome (mixBuffer, done, chunk, pos, sr);

        done += chunk;
        if (isPlay)
        {
            pos += chunk;
            if (cyc && pos >= ce)
            {
                pos = cs;
                flush = true;   // looping back: release notes from the end of the cycle
            }
        }
    }

    // If the UI moved the playhead during this block, keep the UI's position.
    if (isPlay)
        positionSamples.compare_exchange_strong (startPos, pos);

    mixBuffer.applyGain (0, n, masterVolume.load());
    storeMax (masterPeakL, mixBuffer.getMagnitude (0, 0, n));
    storeMax (masterPeakR, mixBuffer.getMagnitude (1, 0, n));
}

void Engine::renderSegment (AudioBuffer<float>& out, int outOffset, int num, int64 pos, double sr,
                            const float* const* in, int numIn, int inOffset,
                            bool live, bool playClips, bool rec,
                            const MidiBuffer* liveMidiIn, int liveMidiOffset, bool flushNotes)
{
    if (num <= 0)
        return;

    const double startSec = (double) pos / sr;
    const double fadeRate = 1.0 / 0.002;   // 2 ms fades at region edges avoid clicks

    bool anySolo = false;
    for (auto& t : tracks)
        anySolo = anySolo || t->solo.load();

    Track* focus = midiFocus.load();

    for (auto& tp : tracks)
    {
        auto& t = *tp;
        trackBuffer.clear (0, num);

        // ---- regions ----
        if (playClips)
        {
            for (auto& cp : t.clips)
            {
                const auto& c = *cp;
                if (c.audio == nullptr)
                    continue;

                const double a = (c.start - startSec) * sr;
                const double b = (c.end() - startSec) * sr;
                if (b <= 0.0 || a >= num)
                    continue;

                const int i0 = a <= 0.0 ? 0 : (int) std::ceil (a);
                const int i1 = b >= num ? num : (int) std::ceil (b);
                if (i0 >= i1)
                    continue;

                const auto& audio = *c.audio;
                const int fileLen = audio.getNumSamples();

                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* src = audio.getReadPointer (jmin (ch, audio.getNumChannels() - 1));
                    float* dst = trackBuffer.getWritePointer (ch);

                    for (int i = i0; i < i1; ++i)
                    {
                        const double rel = (double) (pos + i) / sr - c.start;
                        const double fp  = (rel + c.offset) * c.fileSampleRate;
                        const int idx = (int) fp;
                        if (idx < 0 || idx >= fileLen)
                            continue;

                        const float frac = (float) (fp - idx);
                        const float s0 = src[idx];
                        const float s1 = idx + 1 < fileLen ? src[idx + 1] : s0;
                        const float env = (float) jmin (1.0, rel * fadeRate, (c.length - rel) * fadeRate);
                        dst[i] += (s0 + (s1 - s0) * frac) * jmax (0.0f, env);
                    }
                }
            }
        }

        // ---- live input: recording and monitoring ----
        if (live && ! t.isInstrument && t.armed.load() && in != nullptr && numIn > 0)
        {
            const int ch = t.inputChannel.load();
            const bool stereo = t.stereoInput.load();
            const float* inL = (ch >= 0 && ch < numIn && in[ch] != nullptr) ? in[ch] + inOffset : nullptr;
            const float* inR = stereo ? ((ch + 1 < numIn && in[ch + 1] != nullptr) ? in[ch + 1] + inOffset : nullptr)
                                      : inL;

            if (rec && t.recWriter != nullptr)
            {
                if (t.recStart.load() < 0)
                    t.recStart = pos;

                const float* chans[2] = { inL != nullptr ? inL : silence.getReadPointer (0),
                                          inR != nullptr ? inR : silence.getReadPointer (1) };
                t.recWriter->write (chans, num);
            }

            if (t.monitor.load())
            {
                if (inL != nullptr) FloatVectorOperations::add (trackBuffer.getWritePointer (0), inL, num);
                if (inR != nullptr) FloatVectorOperations::add (trackBuffer.getWritePointer (1), inR, num);
            }
        }

        // ---- instrument (MIDI regions + live MIDI -> synth) ----
        if (t.isInstrument)
            renderInstrument (t, num, pos, sr, playClips, rec, live ? liveMidiIn : nullptr, liveMidiOffset,
                              flushNotes, live && (t.armed.load() || &t == focus));

        // ---- insert effects ----
        for (size_t s = 0; s < (size_t) numInserts; ++s)
        {
            auto* plug = t.inserts[s].get();
            if (plug == nullptr || t.bypassed[s].load())
                continue;

            const int ins  = plug->getTotalNumInputChannels();
            const int outs = plug->getTotalNumOutputChannels();
            const int chans = jlimit (1, pluginBuffer.getNumChannels(), jmax (ins, outs, 2));

            for (int done = 0; done < num; done += pluginMaxBlock)
            {
                const int n = jmin (pluginMaxBlock, num - done);
                AudioBuffer<float> view (pluginBuffer.getArrayOfWritePointers(), chans, n);
                view.clear();

                if (ins == 1)
                {
                    view.addFrom (0, 0, trackBuffer, 0, done, n, 0.5f);
                    view.addFrom (0, 0, trackBuffer, 1, done, n, 0.5f);
                }
                else if (ins >= 2)
                {
                    view.copyFrom (0, 0, trackBuffer, 0, done, n);
                    view.copyFrom (1, 0, trackBuffer, 1, done, n);
                }

                pluginMidi.clear();
                plug->processBlock (view, pluginMidi);

                if (outs >= 2)
                {
                    trackBuffer.copyFrom (0, done, view, 0, 0, n);
                    trackBuffer.copyFrom (1, done, view, 1, 0, n);
                }
                else if (outs == 1)
                {
                    trackBuffer.copyFrom (0, done, view, 0, 0, n);
                    trackBuffer.copyFrom (1, done, view, 0, 0, n);
                }
            }
        }

        // ---- volume / pan / mute / solo ----
        const float vol = t.volume.load();
        const float pan = t.pan.load();
        const float gL = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
        const float gR = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
        const bool audible = ! t.mute.load() && (! anySolo || t.solo.load());

        storeMax (t.peakL, audible ? trackBuffer.getMagnitude (0, 0, num) * gL : 0.0f);
        storeMax (t.peakR, audible ? trackBuffer.getMagnitude (1, 0, num) * gR : 0.0f);

        if (audible)
        {
            out.addFrom (0, outOffset, trackBuffer, 0, 0, num, gL);
            out.addFrom (1, outOffset, trackBuffer, 1, 0, num, gR);
        }
    }
}

void Engine::renderInstrument (Track& t, int num, int64 pos, double sr, bool playClips, bool rec,
                               const MidiBuffer* live, int liveOffset, bool flushNotes, bool takesLive)
{
    auto* synth = t.instrument.get();
    auto& mb = pluginMidi;
    mb.clear();

    auto noteOff = [&t, &mb] (int ch, int key, int at)
    {
        if (! t.soundingNotes[(size_t) ch][(size_t) key]) return;
        t.soundingNotes[(size_t) ch].reset ((size_t) key);
        mb.addEvent (MidiMessage::noteOff (ch + 1, key), at);
    };

    auto noteOn = [&t, &mb, &noteOff] (int ch, int key, int vel, int at)
    {
        noteOff (ch, key, at);   // retrigger cleanly
        t.soundingNotes[(size_t) ch].set ((size_t) key);
        mb.addEvent (MidiMessage::noteOn (ch + 1, key, (uint8) jlimit (1, 127, vel)), at);
    };

    // Transport moved or looped: release everything that's sounding.
    if (flushNotes)
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            if (t.soundingNotes[(size_t) ch].none()) continue;
            for (int key = 0; key < 128; ++key)
                noteOff (ch, key, 0);
            mb.addEvent (MidiMessage::controllerEvent (ch + 1, 64, 0), 0);   // sustain pedal up
        }
    }

    // ---- notes from MIDI regions ----
    if (playClips)
    {
        const double startSec = (double) pos / sr;
        const double endSec = (double) (pos + num) / sr;
        auto sampleAt = [startSec, sr] (double sec) { return (int) std::ceil ((sec - startSec) * sr - 1.0e-6); };

        for (int pass = 0; pass < 2; ++pass)   // note-offs first, then note-ons
        {
            for (auto& cp : t.clips)
            {
                const auto& c = *cp;
                if (! c.isMidi || c.notes == nullptr || c.end() <= startSec || c.start >= endSec)
                    continue;

                const double origin = c.start - c.offset;
                for (auto& nt : *c.notes)
                {
                    const double on = origin + nt.time;
                    if (on < c.start - 1.0e-9 || on >= c.end())
                        continue;   // trimmed away
                    const double off = jmin (on + nt.length, c.end());

                    if (pass == 0)
                    {
                        const int s = sampleAt (off);
                        if (s >= 0 && s < num) noteOff (0, nt.note, s);
                    }
                    else
                    {
                        const int s = sampleAt (on);
                        if (s >= 0 && s < num) noteOn (0, nt.note, nt.velocity, s);
                    }
                }
            }
        }
    }

    // ---- live MIDI from the keyboard ----
    if (takesLive && live != nullptr)
    {
        const bool recordThis = rec && t.armed.load();
        if (recordThis && t.recStart.load() < 0)
            t.recStart = pos;

        for (const auto meta : *live)
        {
            const int at = meta.samplePosition - liveOffset;
            if (at < 0 || at >= num)
                continue;

            const auto msg = meta.getMessage();
            if (msg.isNoteOn())
                noteOn (msg.getChannel() - 1, msg.getNoteNumber(), msg.getVelocity(), at);
            else if (msg.isNoteOff())
                noteOff (msg.getChannel() - 1, msg.getNoteNumber(), at);
            else if (! msg.isSysEx())
                mb.addEvent (msg, at);   // sustain, pitch bend, mod wheel...

            if (recordThis && (msg.isNoteOnOrOff()) && t.recMidi.size() < t.recMidi.capacity())
            {
                auto* raw = msg.getRawData();
                t.recMidi.push_back ({ (double) (pos + at) / sr, raw[0], raw[1], raw[2] });
            }
        }
    }
    else if (rec && t.armed.load() && t.recStart.load() < 0)
    {
        t.recStart = pos;
    }

    if (synth == nullptr || num > pluginMaxBlock)
        return;

    const int outs = synth->getTotalNumOutputChannels();
    const int chans = jlimit (1, pluginBuffer.getNumChannels(), jmax (outs, synth->getTotalNumInputChannels(), 2));
    AudioBuffer<float> view (pluginBuffer.getArrayOfWritePointers(), chans, num);
    view.clear();
    synth->processBlock (view, mb);

    if (outs >= 2)
    {
        trackBuffer.addFrom (0, 0, view, 0, 0, num);
        trackBuffer.addFrom (1, 0, view, 1, 0, num);
    }
    else if (outs == 1)
    {
        trackBuffer.addFrom (0, 0, view, 0, 0, num);
        trackBuffer.addFrom (1, 0, view, 0, 0, num);
    }
}

void Engine::renderMetronome (AudioBuffer<float>& out, int outOffset, int num, int64 pos, double sr)
{
    const double beatLen = sr * 60.0 / tempo.load();
    float* l = out.getWritePointer (0, outOffset);
    float* r = out.getWritePointer (1, outOffset);

    for (int i = 0; i < num; ++i)
    {
        const double s = (double) (pos + i);
        const auto beat = (int64) std::floor (s / beatLen);

        if (s - (double) beat * beatLen < 1.0)   // a beat starts on this sample
        {
            const bool downbeat = (beat % beatsPerBar) == 0;
            clickInc = MathConstants<double>::twoPi * (downbeat ? 1760.0 : 1320.0) / sr;
            clickPhase = 0.0;
            clickTotal = clickRemaining = (int) (0.03 * sr);
            clickGain = downbeat ? 0.45f : 0.3f;
        }

        if (clickRemaining > 0)
        {
            const float env = (float) clickRemaining / (float) clickTotal;
            const float v = (float) std::sin (clickPhase) * clickGain * env * env;
            l[i] += v;
            r[i] += v;
            clickPhase += clickInc;
            --clickRemaining;
        }
    }
}
