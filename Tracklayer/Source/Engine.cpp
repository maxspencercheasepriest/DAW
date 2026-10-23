#include "Engine.h"

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
    playing = true;
}

void Engine::stop()
{
    const bool wasRecording = recording.load();
    playing = false;
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

    const int numIn = dev->getActiveInputChannels().countNumberOfSetBits();
    if (numIn == 0)
    {
        lastError = "No input channels are enabled. Turn some on in Settings (Ctrl+,).";
        return false;
    }

    std::vector<TrackPtr> armed;
    for (auto& t : tracks)
        if (t->armed)
            armed.push_back (t);

    if (armed.empty())
    {
        lastError = "Arm a track for recording first (the R button on the track).";
        return false;
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
    {
        const ScopedLock sl (lock);   // waits for any in-flight audio callback
        for (auto& t : tracks)
            if (t->recWriter != nullptr)
                finished.emplace_back (t, std::move (t->recWriter));
    }

    const double sr = sampleRate.load();

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

//==============================================================================
// Editing
//==============================================================================
TrackPtr Engine::addTrack (String name)
{
    auto t = std::make_shared<Track>();
    t->name = name.isNotEmpty() ? name : "Audio " + String ((int) tracks.size() + 1);
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
    if (owner == nullptr || dest == nullptr || owner == dest)
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
            co->setProperty ("file", c->file.isAChildOf (dir) ? c->file.getRelativePathFrom (dir)
                                                                : c->file.getFullPathName());
            co->setProperty ("start", c->start);
            co->setProperty ("offset", c->offset);
            co->setProperty ("length", c->length);
            clipList.add (var (co.release()));
        }
        to->setProperty ("clips", clipList);
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
        error = "These audio files couldn't be found and were skipped:\n" + missing.joinIntoString ("\n");
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

    const int block = 4096;
    AudioBuffer<float> buf (2, block);
    const auto total = (int64) std::ceil ((end + 0.05) * sr);
    const float mv = masterVolume.load();

    // Holding the lock pauses the live audio callback (it outputs silence).
    const ScopedLock sl (lock);
    for (int64 pos = 0; pos < total; pos += block)
    {
        const int n = (int) jmin ((int64) block, total - pos);
        buf.clear();
        renderSegment (buf, 0, n, pos, sr, nullptr, 0, 0, false, true, false);
        buf.applyGain (0, n, mv);
        writer->writeFromAudioSampleBuffer (buf, 0, n);
    }
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
}

void Engine::audioDeviceStopped() {}

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

    const ScopedTryLock sl (lock);
    if (! sl.isLocked())
        return;   // the UI is restructuring the project; output silence this block

    const double sr = sampleRate.load();
    const bool isPlay = playing.load();
    const bool isRec  = recording.load();
    auto startPos = positionSamples.load();
    auto pos = startPos;

    int done = 0;
    while (done < n)
    {
        int chunk = n - done;
        const auto cs = (int64) (cycleStart.load() * sr);
        const auto ce = (int64) (cycleEnd.load() * sr);
        const bool cyc = isPlay && ! isRec && cycleOn.load() && ce > cs + 64;

        if (cyc && pos < ce && pos + chunk > ce)
            chunk = (int) (ce - pos);

        renderSegment (mixBuffer, done, chunk, pos, sr, in, numIn, inOffset + done, true, isPlay, isRec);
        if (isPlay && metronomeOn.load())
            renderMetronome (mixBuffer, done, chunk, pos, sr);

        done += chunk;
        if (isPlay)
        {
            pos += chunk;
            if (cyc && pos >= ce)
                pos = cs;
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
                            bool live, bool playClips, bool rec)
{
    if (num <= 0)
        return;

    const double startSec = (double) pos / sr;
    const double fadeRate = 1.0 / 0.002;   // 2 ms fades at region edges avoid clicks

    bool anySolo = false;
    for (auto& t : tracks)
        anySolo = anySolo || t->solo.load();

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
        if (live && t.armed.load() && in != nullptr && numIn > 0)
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
